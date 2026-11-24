// The adaptive (ARC) and weak caches, the hash wrapper, and the buddy / small / stack allocators.
#include "core/adaptive_cache.h"
#include "core/hash.h"
#include "core/weak_cache.h"
#include "runtime/mem/allocators.h"

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

// ---- cache::Adaptive (ARC) -----------------------------------------------------------------------------------------------
TEST(AdaptiveCache, HitsAndCapacity)
{
    cache::Adaptive<int, int> c("test arc", 4);
    int computed = 0;
    for (int i = 0; i < 4; ++i) EXPECT_EQ(c.Get(i, [&] { ++computed; return i * 10; }), i * 10);
    EXPECT_EQ(computed, 4);
    EXPECT_EQ(c.Get(2, [&] { ++computed; return -1; }), 20);     // Hit: not computed again
    EXPECT_EQ(computed, 4);
    EXPECT_EQ(c.Frequent(), 1u);                                  // Used twice: in T2
    for (int i = 4; i < 20; ++i) c.Get(i, [&] { return i; });
    EXPECT_LE(c.Size(), 4u);
}

TEST(AdaptiveCache, SurvivesAScanThatWouldFlushAnLru)
{
    // A small set used again and again, then a long scan of one-off keys: ARC keeps the frequent set (in T2),
    // where an LRU of the same size would have lost all of it
    cache::Adaptive<int, int> c("test arc scan", 8);
    for (int round = 0; round < 3; ++round)
        for (int k = 0; k < 4; ++k) c.Get(k, [&] { return k; });
    for (int k = 1000; k < 1200; ++k) c.Get(k, [&] { return k; });
    int kept = 0;
    for (int k = 0; k < 4; ++k) kept += c.Contains(k);
    EXPECT_EQ(kept, 4);
}

TEST(AdaptiveCache, GhostHitsMoveTheTarget)
{
    // (ARC deletes from T1 outright while T1 fills the whole cache: ghosts only appear once there are frequent values)
    cache::Adaptive<int, int> c("test arc ghosts", 4);
    for (int k : {0, 1, 0, 1}) c.Get(k, [&] { return k; });      // 0 and 1 used twice: in T2
    for (int k = 10; k < 15; ++k) c.Get(k, [&] { return k; });   // One-offs: the oldest go from T1 to B1
    ASSERT_FALSE(c.Contains(12));                                 // 12 is in B1 now (10 went from B1 for good)
    const std::size_t before = c.RecencyTarget();
    c.Get(12, [&] { return 12; });                                // A B1 ghost hit: recency gets more room
    EXPECT_GT(c.RecencyTarget(), before);
    EXPECT_TRUE(c.Contains(12));
}

TEST(AdaptiveCache, RandomWorkloadStaysConsistent)
{
    cache::Adaptive<int, int> c("test arc random", 32);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> hot(0, 20), cold(0, 1000);
    for (int i = 0; i < 20000; ++i) {
        const int k = (i % 3) ? hot(rng) : cold(rng);
        ASSERT_EQ(c.Get(k, [&] { return k * 2; }), k * 2);
        ASSERT_LE(c.Size(), 32u);
        ASSERT_LE(c.RecencyTarget(), 32u);
    }
    EXPECT_GT(c.Trim(1 << 20), 0u);
    EXPECT_EQ(c.Size(), 0u);
}

// ---- cache::Weak -----------------------------------------------------------------------------------------------------------
TEST(WeakCache, SharesWhileAliveForgetsAfter)
{
    cache::Weak<std::string, int> c("test weak");
    int made = 0;
    auto a = c.Get("x", [&] { ++made; return std::make_shared<int>(5); });
    auto b = c.Get("x", [&] { ++made; return std::make_shared<int>(6); });
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(made, 1);
    a.reset();
    b.reset();
    EXPECT_EQ(c.Alive(), 0u);
    EXPECT_EQ(c.Find("x"), nullptr);
    auto d = c.Get("x", [&] { ++made; return std::make_shared<int>(7); });
    EXPECT_EQ(*d, 7);
    EXPECT_EQ(made, 2);
}

TEST(WeakCache, SweepsDeadEntries)
{
    cache::Weak<int, int> c("test weak sweep");
    std::vector<std::shared_ptr<int>> keep;
    for (int i = 0; i < 200; ++i) {
        auto v = c.Get(i, [&] { return std::make_shared<int>(i); });
        if (i % 10 == 0) keep.push_back(v);
    }
    c.Sweep();
    EXPECT_EQ(c.Size(), keep.size());
    EXPECT_EQ(c.Alive(), keep.size());
}

// ---- hash ------------------------------------------------------------------------------------------------------------------
TEST(Hash, SeedsAndOrderMatter)
{
    EXPECT_EQ(hash::Text("abc"), hash::Bytes("abc", 3));
    EXPECT_NE(hash::Text("a"), hash::Text("b"));
    EXPECT_NE(hash::Text("a", 1), hash::Text("a", 2));
    EXPECT_NE(hash::Combine(1, 2), hash::Combine(2, 1));
}

// ---- Buddy -------------------------------------------------------------------------------------------------------------------
using namespace mirage::mem;

TEST(BuddyAllocator, SplitsAndMergesBack)
{
    BuddyAllocator b("test buddy", 16, 6);      // 64 KB, 64 B blocks
    ASSERT_TRUE(b.Ok());
    void* x = b.Allocate(100);                  // A 128 B block
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(b.BlockSize(x), 128u);
    EXPECT_TRUE(b.Owns(x));
    EXPECT_GT(b.Stats().splits, 0u);
    b.Free(x);
    const BuddyStats s = b.Stats();
    EXPECT_EQ(s.usedBytes, 0u);
    EXPECT_EQ(s.free.largestFree, 65536u);      // All merged back into one block
    EXPECT_EQ(s.free.freeRegions, 1u);
}

TEST(BuddyAllocator, FillsExactlyAndFailsCleanly)
{
    BuddyAllocator b("test buddy fill", 14, 6); // 16 KB
    std::vector<void*> blocks;
    while (void* p = b.Allocate(1024)) blocks.push_back(p);
    EXPECT_EQ(blocks.size(), 16u);
    EXPECT_EQ(b.Allocate(64), nullptr);
    EXPECT_GT(b.Stats().failures, 0u);
    EXPECT_EQ(b.Allocate(1 << 20), nullptr);    // Larger than the region
    std::set<void*> unique(blocks.begin(), blocks.end());
    EXPECT_EQ(unique.size(), blocks.size());
    // Free every other one: fragmented (no 2 KB block), then the rest: whole again
    for (std::size_t i = 0; i < blocks.size(); i += 2) b.Free(blocks[i]);
    EXPECT_EQ(b.Stats().free.largestFree, 1024u);
    EXPECT_EQ(b.Allocate(2048), nullptr);
    for (std::size_t i = 1; i < blocks.size(); i += 2) b.Free(blocks[i]);
    EXPECT_EQ(b.Stats().free.largestFree, 16384u);
}

TEST(BuddyAllocator, RandomStress)
{
    BuddyAllocator b("test buddy stress", 20, 6);
    std::mt19937 rng(3);
    std::uniform_int_distribution<int> size(1, 9000), coin(0, 2);
    std::vector<std::pair<std::uint8_t*, std::size_t>> live;
    for (int i = 0; i < 20000; ++i) {
        if (coin(rng) || live.empty()) {
            const std::size_t n = static_cast<std::size_t>(size(rng));
            if (auto* p = static_cast<std::uint8_t*>(b.Allocate(n))) {
                std::memset(p, static_cast<int>(n & 0xFF), n);
                live.emplace_back(p, n);
            }
        } else {
            const std::size_t k = rng() % live.size();
            auto [p, n] = live[k];
            for (std::size_t j = 0; j < n; j += 97) ASSERT_EQ(p[j], static_cast<std::uint8_t>(n & 0xFF));   // Nobody else wrote here
            b.Free(p);
            live[k] = live.back();
            live.pop_back();
        }
    }
    for (auto [p, n] : live) b.Free(p);
    EXPECT_EQ(b.Stats().usedBytes, 0u);
    EXPECT_EQ(b.Stats().free.largestFree, 1u << 20);
}

// ---- Small objects -----------------------------------------------------------------------------------------------------------
TEST(SmallAllocator, ClassesAndFallback)
{
    EXPECT_EQ(SmallAllocator::ClassOf(1), 0);
    EXPECT_EQ(SmallAllocator::ClassOf(8), 0);
    EXPECT_EQ(SmallAllocator::ClassOf(9), 1);
    EXPECT_EQ(SmallAllocator::ClassOf(256), 9);
    EXPECT_EQ(SmallAllocator::ClassOf(257), -1);
    SmallAllocator a("test small");
    std::vector<std::pair<void*, std::size_t>> blocks;
    for (std::size_t n : {1, 7, 8, 20, 60, 100, 200, 256, 300, 4000}) {
        void* p = a.Allocate(n);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0xAB, n);
        blocks.emplace_back(p, n);
    }
    EXPECT_EQ(a.HeapFallbacks(), 2u);           // 300 and 4000
    for (auto [p, n] : blocks) a.Free(p, n);
    std::size_t used = 0;
    for (const PoolStats& s : a.Stats()) used += s.used;
    EXPECT_EQ(used, 0u);
}

namespace {
struct Tiny : SmallObject<Tiny> {
    int a = 1, b = 2;
};
}

TEST(SmallAllocator, SmallObjectsUseTheSharedOne)
{
    std::vector<Tiny*> v;
    for (int i = 0; i < 1000; ++i) v.push_back(new Tiny());
    for (Tiny* t : v) EXPECT_EQ(t->b, 2);
    for (Tiny* t : v) delete t;
}

TEST(SmallBuffer, InlineThenHeapKeepingTheContents)
{
    SmallBuffer<16> b(10);
    EXPECT_FALSE(b.OnHeap());
    std::memcpy(b.Data(), "0123456789", 10);
    ASSERT_TRUE(b.Resize(1000));
    EXPECT_TRUE(b.OnHeap());
    EXPECT_EQ(std::memcmp(b.Data(), "0123456789", 10), 0);
    EXPECT_GE(b.Capacity(), 1000u);
}

// ---- Stack -------------------------------------------------------------------------------------------------------------------
TEST(StackAllocator, LifoOnlyAndMarkers)
{
    StackAllocator s(4096);
    void* a = s.Allocate(100);
    void* b = s.Allocate(200, 64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 64, 0u);
    EXPECT_FALSE(s.Free(a));                    // Not the newest: refused
    EXPECT_EQ(s.Misuses(), 1u);
    EXPECT_TRUE(s.Free(b));
    EXPECT_TRUE(s.Free(a));
    EXPECT_EQ(s.Used(), 0u);

    const auto m = s.Mark();
    for (int i = 0; i < 10; ++i) ASSERT_NE(s.Allocate(100), nullptr);
    s.Rewind(m);
    EXPECT_EQ(s.Used(), 0u);
    void* c = s.Allocate(10);
    EXPECT_TRUE(s.Free(c));                     // The newest after a rewind is known again
    EXPECT_EQ(s.Allocate(5000), nullptr);       // Full: nullptr, nothing taken
    EXPECT_EQ(s.Used(), 0u);
    EXPECT_GE(s.Peak(), 1000u);
}
