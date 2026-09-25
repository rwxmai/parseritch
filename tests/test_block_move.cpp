#include "itch/simd/block_move.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace {

template <class T>
class BlockMoveTest : public ::testing::Test {};
using ElementTypes = ::testing::Types<uint32_t, uint64_t>;
TYPED_TEST_SUITE(BlockMoveTest, ElementTypes);

// Every (size, pos) combination up to 80 elements, against std::memmove on an
// identical copy; guard elements on both sides must stay untouched.
TYPED_TEST(BlockMoveTest, ShiftsMatchMemmoveIncludingOverlap) {
    using T = TypeParam;
    std::mt19937_64 rng(4);
    constexpr std::size_t kGuard = 4;
    for (uint32_t size = 1; size <= 80; ++size) {
        for (uint32_t pos = 0; pos < size; ++pos) {
            const uint32_t count = size - pos - 1;  // elements above pos (room for one more)
            std::vector<T> a(size + 2 * kGuard);
            for (auto& x : a) x = static_cast<T>(rng());
            auto ref = a;
            T* base = a.data() + kGuard;
            T* rbase = ref.data() + kGuard;

            itch::simd::shift_up(base, pos, count);
            std::memmove(rbase + pos + 1, rbase + pos, count * sizeof(T));
            ASSERT_EQ(a, ref) << "shift_up size=" << size << " pos=" << pos;

            itch::simd::shift_down(base, pos, count);
            std::memmove(rbase + pos, rbase + pos + 1, count * sizeof(T));
            ASSERT_EQ(a, ref) << "shift_down size=" << size << " pos=" << pos;
        }
    }
}

TEST(BlockMove, ZeroCountIsANoOp) {
    std::vector<uint32_t> a(8);
    std::iota(a.begin(), a.end(), 1u);
    const auto before = a;
    itch::simd::shift_up(a.data(), 3, 0);
    itch::simd::shift_down(a.data(), 3, 0);
    EXPECT_EQ(a, before);
}

} // namespace
