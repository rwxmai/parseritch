#include "itch/simd/group.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>

namespace {

using namespace itch::simd;

template <class G>
class GroupTest : public ::testing::Test {};

using GroupTypes = ::testing::Types<GroupSwar, GroupSse2
#if defined(ITCH_HAVE_AVX2)
                                    , GroupAvx2
#endif
                                    >;
TYPED_TEST_SUITE(GroupTest, GroupTypes);

template <class Mask>
uint64_t slots_of(Mask m) {
    uint64_t bits = 0;
    for (uint32_t i : m) bits |= 1ULL << i;
    return bits;
}

TYPED_TEST(GroupTest, MatchesAgreeWithBytewiseReference) {
    using G = TypeParam;
    constexpr uint32_t W = G::kWidth;
    std::mt19937 rng(1234);
    alignas(64) std::array<uint8_t, 64> ctrl{};

    for (int trial = 0; trial < 20'000; ++trial) {
        const int mix = trial % 4;  // vary the density of empties/tombstones
        for (uint32_t i = 0; i < W; ++i) {
            const uint32_t r = rng() % 8;
            if (mix == 0 && r == 0)      ctrl[i] = kCtrlEmpty;
            else if (mix == 1 && r < 4)  ctrl[i] = kCtrlDeleted;
            else if (mix == 2 && r < 2)  ctrl[i] = (r == 0) ? kCtrlEmpty : kCtrlDeleted;
            else                         ctrl[i] = static_cast<uint8_t>(rng() % 128);
        }
        const auto h2 = static_cast<uint8_t>(rng() % 128);
        uint64_t exp_match = 0, exp_empty = 0, exp_free = 0;
        for (uint32_t i = 0; i < W; ++i) {
            if (ctrl[i] == h2)          exp_match |= 1ULL << i;
            if (ctrl[i] == kCtrlEmpty)  exp_empty |= 1ULL << i;
            if (ctrl[i] >= 0x80)        exp_free  |= 1ULL << i;
        }
        const G g(ctrl.data());
        const uint64_t got_match = slots_of(g.match(h2));
        // Every true match must be reported. SWAR may add false positives
        // (resolved later by the full-key compare); the SIMD groups are exact.
        ASSERT_EQ(got_match & exp_match, exp_match);
        if constexpr (!std::is_same_v<G, GroupSwar>) {
            ASSERT_EQ(got_match, exp_match);
        }
        ASSERT_EQ(slots_of(g.match_empty()), exp_empty);
        ASSERT_EQ(slots_of(g.match_empty_or_deleted()), exp_free);
    }
}

TEST(GroupSwar, FalsePositivesOnlyFromBorrowAboveARealMatch) {
    // 0x01 directly above a matching 0x00 byte is the known borrow case.
    alignas(8) std::array<uint8_t, 8> ctrl = {0x00, 0x01, 0x05, 0x7F, 0x10, 0x11, 0x00, 0x01};
    const GroupSwar g(ctrl.data());
    const uint64_t got = slots_of(g.match(0x00));
    EXPECT_EQ(got & 0b0100'0001u, 0b0100'0001u);  // both true matches present
    for (uint32_t i = 0; i < 8; ++i) {
        if (((got >> i) & 1) && ctrl[i] != 0x00) {
            EXPECT_TRUE(i > 0 && ctrl[i - 1] == 0x00) << i;
        }
    }
}

} // namespace
