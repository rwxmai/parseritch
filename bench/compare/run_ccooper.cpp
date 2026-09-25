// charles-cooper/itch-order-book runner.  run_ccooper FILE book
//
// The upstream main.cpp loop and order_book.h, unchanged in logic, with three
// harness changes:
//   * buf_t refills from the in-memory file instead of read(2) on stdin
//     (same 1024-byte buffer and memmove-on-refill as upstream);
//   * unknown message types ('h' Operational Halt is not in upstream's enum)
//     are skipped by length; upstream would print and spin forever with NDEBUG;
//   * the clock covers the whole file, not "from the first Add Order".
#include "common.hpp"

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define be16toh(x) OSSwapBigToHostInt16(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#include <limits>
#include <memory>
#include "bufferedreader.h"
#include "itch.h"
#include "order_book.h"

namespace {
const uint8_t* g_src = nullptr;
std::size_t g_src_len = 0, g_src_pos = 0;
}

// --- buf_t (bufferedreader.cpp), refilling from memory -----------------------
read_t buf_t::ensure(unsigned const n) {
    if (this->available(n)) return read_t::OK;
    ssize_t bytes = this->read(n);
    return bytes > 0 ? read_t::OK : read_t::ERR;
}
ssize_t buf_t::read(unsigned const n) {
    if (pos + n > len) discard_to_pos();
    ssize_t bytes_read = 0;
    while (this->available() < n) {
        ssize_t bytes = this->read();
        if (bytes <= 0) return bytes;
        bytes_read += bytes;
    }
    return bytes_read;
}
ssize_t buf_t::read() {
    const std::size_t want = std::min<std::size_t>(len - limit, g_src_len - g_src_pos);
    std::memcpy(ptr + limit, g_src + g_src_pos, want);
    g_src_pos += want;
    limit += static_cast<unsigned>(want);
    return static_cast<ssize_t>(want);
}

// --- upstream main.cpp ---------------------------------------------------------
template <itch_t __code>
class PROCESS {
 public:
  static itch_message<__code> read_from(buf_t *__buf) {
    uint16_t const msglen = be16toh(*(uint16_t *)__buf->get(0));
    __buf->advance(2);
    assert(msglen == netlen<__code>);
    (void)msglen;
    __buf->ensure(netlen<__code>);
    itch_message<__code> ret = itch_message<__code>::parse(__buf->get(0));
    __buf->advance(netlen<__code>);
    return ret;
  }
};

static sprice_t mksigned(price_t price, BUY_SELL buy) {
  auto ret = MKPRIMITIVE(price);
  if (BUY_SELL::SELL == buy) ret = -ret;
  return sprice_t(ret);
}
#define DO_CASE(__itch_t)               \
  case (__itch_t): {                    \
    PROCESS<__itch_t>::read_from(&buf); \
    break;                              \
  }

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
  const cmp::Buffer file = cmp::load(argv[1]);
  g_src = file.data; g_src_len = file.size;

  buf_t buf(1024);
  size_t npkts = 0;
  order_book::oid_map.reserve(order_id_t(184118975 * 2));

  const auto t0 = cmp::Clock::now();
  while (is_ok(buf.ensure(3))) {
    ++npkts;
    itch_t const msgtype = itch_t(*buf.get(2));
    switch (msgtype) {
      DO_CASE(itch_t::SYSEVENT);
      DO_CASE(itch_t::STOCK_DIRECTORY);
      DO_CASE(itch_t::TRADING_ACTION);
      DO_CASE(itch_t::REG_SHO_RESTRICT);
      DO_CASE(itch_t::MPID_POSITION);
      DO_CASE(itch_t::MWCB_DECLINE);
      DO_CASE(itch_t::MWCB_STATUS);
      DO_CASE(itch_t::IPO_QUOTE_UPDATE);
      DO_CASE(itch_t::TRADE);
      DO_CASE(itch_t::CROSS_TRADE);
      DO_CASE(itch_t::BROKEN_TRADE);
      DO_CASE(itch_t::NET_ORDER_IMBALANCE);
      DO_CASE(itch_t::RETAIL_PRICE_IMPROVEMENT);
      DO_CASE(itch_t::PROCESS_LULD_AUCTION_COLLAR_MESSAGE);
      case (itch_t::ADD_ORDER): {
        auto const pkt = PROCESS<itch_t::ADD_ORDER>::read_from(&buf);
        order_book::add_order(order_id_t(pkt.oid), book_id_t(pkt.stock_locate),
                              mksigned(pkt.price, pkt.buy), pkt.qty);
        break;
      }
      case (itch_t::ADD_ORDER_MPID): {
        auto const pkt = PROCESS<itch_t::ADD_ORDER_MPID>::read_from(&buf);
        order_book::add_order(order_id_t(pkt.add_msg.oid), book_id_t(pkt.add_msg.stock_locate),
                              mksigned(pkt.add_msg.price, pkt.add_msg.buy), pkt.add_msg.qty);
        break;
      }
      case (itch_t::EXECUTE_ORDER): {
        auto const pkt = PROCESS<itch_t::EXECUTE_ORDER>::read_from(&buf);
        order_book::execute_order(order_id_t(pkt.oid), pkt.qty);
        break;
      }
      case (itch_t::EXECUTE_ORDER_WITH_PRICE): {
        auto const pkt = PROCESS<itch_t::EXECUTE_ORDER_WITH_PRICE>::read_from(&buf);
        order_book::execute_order(order_id_t(pkt.exec.oid), pkt.exec.qty);
        break;
      }
      case (itch_t::REDUCE_ORDER): {
        auto const pkt = PROCESS<itch_t::REDUCE_ORDER>::read_from(&buf);
        order_book::cancel_order(order_id_t(pkt.oid), pkt.qty);
        break;
      }
      case (itch_t::DELETE_ORDER): {
        auto const pkt = PROCESS<itch_t::DELETE_ORDER>::read_from(&buf);
        order_book::delete_order(order_id_t(pkt.oid));
        break;
      }
      case (itch_t::REPLACE_ORDER): {
        auto const pkt = PROCESS<itch_t::REPLACE_ORDER>::read_from(&buf);
        order_book::replace_order(order_id_t(pkt.oid), order_id_t(pkt.new_order_id), pkt.new_qty,
                                  mksigned(pkt.new_price, BUY_SELL::BUY));
        break;
      }
      default: {  // harness: skip types upstream doesn't know (e.g. 'h')
        const uint16_t msglen = be16toh(*(uint16_t *)buf.get(0));
        buf.advance(2);
        buf.ensure(msglen);
        buf.advance(msglen);
        break;
      }
    }
  }
  const double s = cmp::seconds_since(t0);
  cmp::result("ccooper", "book", npkts, s, file.size, 0);

  for (const auto& sym : cmp::digest_symbols()) {
    const int loc = cmp::locate_of(file, sym);
    if (loc < 0) continue;
    const order_book& b = order_book::s_books[loc];
    uint64_t bp = 0, bq = 0, ap = 0, aq = 0;
    if (!b.m_bids.empty()) {
      bp = uint64_t(int32_t(b.m_bids.back().m_price));
      bq = MKPRIMITIVE(order_book::s_levels[b.m_bids.back().m_ptr].m_qty);
    }
    if (!b.m_offers.empty()) {
      ap = uint64_t(-int64_t(int32_t(b.m_offers.back().m_price)));
      aq = MKPRIMITIVE(order_book::s_levels[b.m_offers.back().m_ptr].m_qty);
    }
    cmp::print_bbo("ccooper", sym, bp, bq, ap, aq);
  }
  return 0;
}
