#include "itch/messages.hpp"
#include "itch/simd/add_order_decode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>

namespace {

using namespace itch;

TEST(AddOrderDecode, AllVariantsAgreeWithGenericDecoder) {
    std::mt19937_64 rng(99);
    std::array<uint8_t, 64> buf{};
    for (int trial = 0; trial < 100'000; ++trial) {
        for (auto& b : buf) b = static_cast<uint8_t>(rng());  // incl. the 12-byte over-read tail
        buf[0] = 'A';
        const auto generic = decode<MsgAddOrder>(buf.data());
        simd::AddOrderFields expect{};
        expect.ref = generic.ref;
        expect.timestamp = generic.timestamp;
        expect.shares = generic.shares;
        expect.price = generic.price;
        expect.locate = generic.locate;
        expect.tracking = generic.tracking;
        expect.side = generic.side;

        ASSERT_EQ(simd::scalar::decode_add_order(buf.data()), expect);
#if defined(ITCH_HAVE_SSSE3)
        ASSERT_EQ(simd::ssse3::decode_add_order(buf.data()), expect);
#endif
#if defined(ITCH_HAVE_AVX2)
        ASSERT_EQ(simd::avx2::decode_add_order(buf.data()), expect);
#endif
    }
}

// The shuffle masks are derived at compile time; spot-check that no output
// byte is sourced from two chunks, or from none when it should have a source.
TEST(AddOrderDecode, MasksPartitionTheSources) {
    using simd::detail::kAddOrderSrc;
    using simd::detail::kMask;
    const std::array<std::array<int8_t, 16>, 3> lo = {kMask<0, 0>, kMask<1, 0>, kMask<2, 0>};
    const std::array<std::array<int8_t, 16>, 3> hi = {kMask<0, 16>, kMask<1, 16>, kMask<2, 16>};
    for (std::size_t k = 0; k < 32; ++k) {
        const auto& masks = k < 16 ? lo : hi;
        int sources = 0;
        for (std::size_t chunk = 0; chunk < 3; ++chunk) {
            const int8_t sel = masks[chunk][k % 16];
            if (sel >= 0) {
                ++sources;
                EXPECT_EQ(static_cast<int>(chunk) * 16 + sel, kAddOrderSrc[k]) << "byte " << k;
            }
        }
        EXPECT_EQ(sources, kAddOrderSrc[k] >= 0 ? 1 : 0) << "byte " << k;
    }
}

} // namespace
