#include "itch/order_book.hpp"

#include <algorithm>

namespace itch {

namespace {
constexpr uint32_t kInitialLevels = 16;
static_assert(kInitialLevels >= simd::kLevelSearchPad,
              "SIMD level search needs kLevelSearchPad readable keys");

template <class T>
std::unique_ptr<T[]> regrow(const std::unique_ptr<T[]>& old, uint32_t size, uint32_t cap) {
    auto fresh = std::make_unique<T[]>(cap);  // value-initialised: padding lanes are 0
    if (size != 0) std::copy_n(old.get(), size, fresh.get());
    return fresh;
}
} // namespace

void LevelSide::grow() {
    const uint32_t cap = cap_ == 0 ? kInitialLevels : cap_ * 2;
    keys_   = regrow(keys_, size_, cap);
    qty_    = regrow(qty_, size_, cap);
    orders_ = regrow(orders_, size_, cap);
    cap_    = cap;
}

BookEngine::BookEngine(std::size_t expected_orders)
    : orders_(expected_orders), books_(std::make_unique<OrderBook[]>(kMaxLocate)) {}

} // namespace itch
