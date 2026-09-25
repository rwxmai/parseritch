// chronoxor/CppTrader runner.  run_cpptrader FILE (parse|book)
//   parse  ITCHHandler with every onMessage folding locate+timestamp
//   book   ITCHHandler -> Matching::MarketManager (matching off), the same
//          wiring and counting MarketHandler as upstream performance/market_manager.cpp
// Upstream reads the file through an 8 KiB buffer; here Process() gets the
// whole in-memory file in one call, like the other runners.
#include "common.hpp"

#include "trader/matching/market_manager.h"
#include "trader/providers/nasdaq/itch_handler.h"

#include <string>

using namespace CppTrader::ITCH;
using namespace CppTrader::Matching;

namespace {

class FoldHandler : public ITCHHandler {
public:
    uint64_t n = 0, sum = 0, errors = 0;
protected:
#define FOLD(T) bool onMessage(const T& m) override { ++n; sum += m.StockLocate + m.Timestamp; return true; }
    FOLD(SystemEventMessage) FOLD(StockDirectoryMessage) FOLD(StockTradingActionMessage) FOLD(RegSHOMessage)
    FOLD(MarketParticipantPositionMessage) FOLD(MWCBDeclineMessage) FOLD(MWCBStatusMessage) FOLD(IPOQuotingMessage)
    FOLD(AddOrderMessage) FOLD(AddOrderMPIDMessage) FOLD(OrderExecutedMessage) FOLD(OrderExecutedWithPriceMessage)
    FOLD(OrderCancelMessage) FOLD(OrderDeleteMessage) FOLD(OrderReplaceMessage) FOLD(TradeMessage)
    FOLD(CrossTradeMessage) FOLD(BrokenTradeMessage) FOLD(NOIIMessage) FOLD(RPIIMessage) FOLD(LULDAuctionCollarMessage)
#undef FOLD
    bool onMessage(const UnknownMessage&) override { ++errors; return true; }
};

// Upstream MyMarketHandler (performance/market_manager.cpp), trimmed to its counters.
class CountingMarketHandler : public MarketHandler {
public:
    size_t updates = 0;
protected:
    void onAddSymbol(const Symbol&) override { ++updates; }
    void onDeleteSymbol(const Symbol&) override { ++updates; }
    void onAddOrderBook(const OrderBook&) override { ++updates; }
    void onUpdateOrderBook(const OrderBook&, bool) override {}
    void onDeleteOrderBook(const OrderBook&) override { ++updates; }
    void onAddLevel(const OrderBook&, const Level&, bool) override { ++updates; }
    void onUpdateLevel(const OrderBook&, const Level&, bool) override { ++updates; }
    void onDeleteLevel(const OrderBook&, const Level&, bool) override { ++updates; }
    void onAddOrder(const Order&) override { ++updates; }
    void onUpdateOrder(const Order&) override { ++updates; }
    void onDeleteOrder(const Order&) override { ++updates; }
    void onExecuteOrder(const Order&, uint64_t, uint64_t) override { ++updates; }
};

// Upstream MyITCHHandler (performance/market_manager.cpp).
class BookHandler : public ITCHHandler {
public:
    explicit BookHandler(MarketManager& m) : _market(m) {}
    uint64_t n = 0, errors = 0;
protected:
#define COUNT(T) bool onMessage(const T&) override { ++n; return true; }
    COUNT(SystemEventMessage) COUNT(StockTradingActionMessage) COUNT(RegSHOMessage)
    COUNT(MarketParticipantPositionMessage) COUNT(MWCBDeclineMessage) COUNT(MWCBStatusMessage) COUNT(IPOQuotingMessage)
    COUNT(TradeMessage) COUNT(CrossTradeMessage) COUNT(BrokenTradeMessage) COUNT(NOIIMessage) COUNT(RPIIMessage)
    COUNT(LULDAuctionCollarMessage)
#undef COUNT
    bool onMessage(const StockDirectoryMessage& m) override { ++n; Symbol s(m.StockLocate, m.Stock); _market.AddSymbol(s); _market.AddOrderBook(s); return true; }
    bool onMessage(const AddOrderMessage& m) override { ++n; _market.AddOrder(Order::Limit(m.OrderReferenceNumber, m.StockLocate, (m.BuySellIndicator == 'B') ? OrderSide::BUY : OrderSide::SELL, m.Price, m.Shares)); return true; }
    bool onMessage(const AddOrderMPIDMessage& m) override { ++n; _market.AddOrder(Order::Limit(m.OrderReferenceNumber, m.StockLocate, (m.BuySellIndicator == 'B') ? OrderSide::BUY : OrderSide::SELL, m.Price, m.Shares)); return true; }
    bool onMessage(const OrderExecutedMessage& m) override { ++n; _market.ExecuteOrder(m.OrderReferenceNumber, m.ExecutedShares); return true; }
    bool onMessage(const OrderExecutedWithPriceMessage& m) override { ++n; _market.ExecuteOrder(m.OrderReferenceNumber, m.ExecutionPrice, m.ExecutedShares); return true; }
    bool onMessage(const OrderCancelMessage& m) override { ++n; _market.ReduceOrder(m.OrderReferenceNumber, m.CanceledShares); return true; }
    bool onMessage(const OrderDeleteMessage& m) override { ++n; _market.DeleteOrder(m.OrderReferenceNumber); return true; }
    bool onMessage(const OrderReplaceMessage& m) override { ++n; _market.ReplaceOrder(m.OriginalOrderReferenceNumber, m.NewOrderReferenceNumber, m.Price, m.Shares); return true; }
    bool onMessage(const UnknownMessage&) override { ++errors; return true; }
private:
    MarketManager& _market;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s FILE parse|book\n", argv[0]); return 2; }
    const std::string mode = argv[2];
    const cmp::Buffer buf = cmp::load(argv[1]);

    if (mode == "parse") {
        FoldHandler h;
        const auto t0 = cmp::Clock::now();
        h.Process(buf.data, buf.size);
        const double s = cmp::seconds_since(t0);
        cmp::result("cpptrader", "parse", h.n, s, buf.size, h.sum, "unknown=" + std::to_string(h.errors));
        return 0;
    }

    CountingMarketHandler mh;
    MarketManager market(mh);
    BookHandler h(market);
    const auto t0 = cmp::Clock::now();
    h.Process(buf.data, buf.size);
    const double s = cmp::seconds_since(t0);
    cmp::result("cpptrader", "book", h.n, s, buf.size, 0,
                "unknown=" + std::to_string(h.errors) + " updates=" + std::to_string(mh.updates));
    for (const auto& sym : cmp::digest_symbols()) {
        const int loc = cmp::locate_of(buf, sym);
        if (loc < 0) continue;
        const OrderBook* b = market.GetOrderBook(static_cast<uint32_t>(loc));
        if (b == nullptr) continue;
        const auto* bb = b->best_bid();
        const auto* ba = b->best_ask();
        cmp::print_bbo("cpptrader", sym, bb ? bb->Price : 0, bb ? bb->VisibleVolume : 0,
                       ba ? ba->Price : 0, ba ? ba->VisibleVolume : 0);
    }
    return 0;
}
