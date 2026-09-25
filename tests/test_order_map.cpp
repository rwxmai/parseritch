#include "itch/order_map.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

using namespace itch;

template <class G>
class OrderMapTest : public ::testing::Test {};

using GroupTypes = ::testing::Types<simd::GroupSwar, simd::GroupSse2
#if defined(ITCH_HAVE_AVX2)
                                    , simd::GroupAvx2
#endif
                                    >;
TYPED_TEST_SUITE(OrderMapTest, GroupTypes);

struct Model {
    uint32_t price, shares;
};

/// Random insert/find/erase against std::unordered_map. `churn_bias` > 0.5
/// keeps the live set small with heavy delete traffic, like an ITCH session.
template <class Map>
void run_model(Map& map, uint64_t ops, double insert_p, uint64_t key_space, uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::unordered_map<uint64_t, Model> model;
    std::vector<uint64_t> live;
    uint64_t next_ref = 1;
    for (uint64_t i = 0; i < ops; ++i) {
        const double u = std::uniform_real_distribution<double>(0, 1)(rng);
        if (live.empty() || u < insert_p) {
            // Mostly sequential refs (like ITCH), sometimes random ones.
            const uint64_t ref = (rng() % 8 == 0) ? (rng() % key_space) + 1 : next_ref++;
            if (model.count(ref)) continue;
            Order* o = map.insert(ref);
            o->price = static_cast<uint32_t>(rng());
            o->shares = static_cast<uint32_t>(rng());
            model[ref] = {o->price, o->shares};
            live.push_back(ref);
        } else {
            const std::size_t idx = rng() % live.size();
            const uint64_t ref = live[idx];
            Order* o = map.find(ref);
            ASSERT_NE(o, nullptr) << "lost ref " << ref;
            ASSERT_EQ(o->ref, ref);
            ASSERT_EQ(o->price, model[ref].price);
            ASSERT_EQ(o->shares, model[ref].shares);
            map.erase(o);
            model.erase(ref);
            live[idx] = live.back();
            live.pop_back();
            ASSERT_EQ(map.find(ref), nullptr);
        }
        if (i % 4096 == 0) {
            ASSERT_EQ(map.size(), model.size());
            ASSERT_EQ(map.find(key_space * 4 + 12345), nullptr);  // never inserted
        }
    }
    ASSERT_EQ(map.size(), model.size());
    for (const auto& [ref, m] : model) {
        const Order* o = map.find(ref);
        ASSERT_NE(o, nullptr);
        ASSERT_EQ(o->price, m.price);
    }
    std::size_t seen = 0;
    map.for_each([&](const Order& o) { ++seen; ASSERT_TRUE(model.count(o.ref)); });
    ASSERT_EQ(seen, model.size());
}

TYPED_TEST(OrderMapTest, RandomOpsMatchModel) {
    BasicOrderMap<TypeParam> map(1 << 14);
    run_model(map, 400'000, 0.55, 1 << 22, 1);
}

TYPED_TEST(OrderMapTest, GrowsFromTinyCapacity) {
    BasicOrderMap<TypeParam> map(4);
    run_model(map, 200'000, 0.8, 1 << 24, 2);  // net growth forces several doublings
    EXPECT_GT(map.stats().grows, 3u);
}

// Steady churn at a near-constant live count: tombstones must not pile up
// until probes never terminate.
TYPED_TEST(OrderMapTest, ChurnAtConstantSizeStaysBounded) {
    BasicOrderMap<TypeParam> map(1 << 12);
    run_model(map, 1'000'000, 0.5, 1 << 20, 3);
    EXPECT_LE(map.tombstones(), map.capacity() / 8);
    EXPECT_EQ(map.stats().grows, 0u) << "same live size must not grow the table";
}

TYPED_TEST(OrderMapTest, ClearKeepsCapacity) {
    BasicOrderMap<TypeParam> map(1000);
    for (uint64_t r = 1; r <= 1000; ++r) map.insert(r)->shares = 1;
    const std::size_t cap = map.capacity();
    map.clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.capacity(), cap);
    for (uint64_t r = 1; r <= 1000; ++r) EXPECT_EQ(map.find(r), nullptr);
}

TEST(OrderMap, CapacityHonoursSevenEighthsLoad) {
    OrderMap map(1'000'000);
    EXPECT_GE(map.capacity() - map.capacity() / 8, 1'000'000u);
}

} // namespace
