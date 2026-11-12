// The memory manager's parts that run without the game (runtime/mem): its reasoning, pools, arenas, the sampling profiler
// and the address space walk.
#include "runtime/mem/analysis.h"
#include "runtime/mem/arena.h"
#include "runtime/mem/heap.h"
#include "runtime/mem/pool.h"
#include "runtime/mem/process.h"
#include "runtime/mem/sampler.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <cstdint>
#include <set>
#include <vector>

using namespace mirage::mem;

// ---- Analysis ------------------------------------------------------------------------------------------------------------
TEST(MemoryTrend, SlopeAndFitOfALine)
{
    Trend t(10);
    for (int i = 0; i < 10; ++i) t.Add(i, 100.0 + 5.0 * i);
    EXPECT_NEAR(t.Slope(), 5.0, 1e-9);
    EXPECT_NEAR(t.R2(), 1.0, 1e-9);
    EXPECT_NEAR(t.Rise(), 45.0, 1e-9);
    t.Add(10, 150.0);                       // Window of 10: the first point is gone
    EXPECT_EQ(t.Count(), 10u);
    EXPECT_NEAR(t.Span(), 9.0, 1e-9);
}

TEST(MemoryTrend, NoiseFitsBadly)
{
    Trend t(20);
    for (int i = 0; i < 20; ++i) t.Add(i, (i % 2) ? 100.0 : 0.0);
    EXPECT_LT(t.R2(), 0.1);
}

TEST(MemoryAnomalies, SteadyGrowthIsReportedOnce)
{
    AnomalyRules rules;
    rules.warmup = 2;
    rules.growthWindow = 30;
    AnomalyDetector d("test", rules);
    int growth = 0;
    std::uint64_t allocs = 0;
    for (int i = 0; i < 100; ++i) {
        allocs += 100;
        // 1 MB a second, in a straight line
        for (const Finding& f : d.Add({static_cast<double>(i), allocs, allocs * 64, static_cast<std::int64_t>(i) << 20}))
            if (f.kind == Finding::Kind::SteadyGrowth) ++growth;
    }
    EXPECT_EQ(growth, 1);       // The cooldown (120 s) keeps it to one in 100 s
}

TEST(MemoryAnomalies, FlatMemoryIsNoLeak)
{
    AnomalyRules rules;
    rules.warmup = 2;
    rules.growthWindow = 30;
    AnomalyDetector d("test", rules);
    std::uint64_t allocs = 0;
    for (int i = 0; i < 100; ++i) {
        allocs += 100;
        const auto live = static_cast<std::int64_t>(50 << 20) + ((i % 7) << 16);
        for (const Finding& f : d.Add({static_cast<double>(i), allocs, allocs * 64, live}))
            EXPECT_NE(f.kind, Finding::Kind::SteadyGrowth);
    }
}

TEST(MemoryAnomalies, RateSpikeNeedsToHold)
{
    AnomalyRules rules;
    rules.warmup = 5;
    rules.spikeMinRate = 1000;
    AnomalyDetector d("test", rules);
    std::uint64_t allocs = 0;
    int spikes = 0;
    const auto feed = [&](double t, std::uint64_t perSecond) {
        allocs += perSecond;
        for (const Finding& f : d.Add({t, allocs, allocs * 32, 1 << 20}))
            if (f.kind == Finding::Kind::RateSpike) ++spikes;
    };
    double t = 0;
    for (int i = 0; i < 20; ++i) feed(t++, 500);
    feed(t++, 50000);           // One sample: not yet
    EXPECT_EQ(spikes, 0);
    feed(t++, 50000);           // Held for two
    EXPECT_EQ(spikes, 1);
}

TEST(MemoryFragmentation, IndexFromFreeRegions)
{
    Fragmentation f;
    f.AddFree(100u << 20);
    EXPECT_DOUBLE_EQ(f.Index(), 0.0);
    f.AddFree(100u << 20);
    EXPECT_DOUBLE_EQ(f.Index(), 0.5);
    f.AddFree(64 * 1024);
    EXPECT_EQ(f.smallRegions, 1u);
    EXPECT_EQ(f.largestFree, 100u << 20);
}

TEST(MemoryPressure, RisesAfterTwoSamplesCalmsAfterTen)
{
    PressurePolicy p;
    PressureInputs ok{2000ull << 20, 1000ull << 20, 4000ull << 20, 0, 0};
    PressureInputs high = ok;
    high.largestFree = 100ull << 20;                    // Under 128 MB: High
    EXPECT_EQ(p.Add(ok), Level::Normal);
    EXPECT_EQ(p.Add(high), Level::Normal);              // One sample isn't enough
    EXPECT_EQ(p.Add(high), Level::High);
    for (int i = 0; i < 9; ++i) EXPECT_EQ(p.Add(ok), Level::High);   // Slow to calm down
    EXPECT_EQ(p.Add(ok), Level::Normal);
    EXPECT_FALSE(p.Reason().empty());
}

TEST(MemoryPressure, CriticalAtOnce)
{
    PressurePolicy p;
    PressureInputs crit{2000ull << 20, 20ull << 20, 4000ull << 20, 0, 0};
    EXPECT_EQ(p.Add(crit), Level::Critical);
}

TEST(MemoryPressure, MenuOverItsBudgetIsElevated)
{
    PressureInputs in{2000ull << 20, 1000ull << 20, 4000ull << 20, 400ll << 20, 256ll << 20};
    EXPECT_EQ(PressurePolicy::Raw(in, {}), Level::Elevated);
    in.menuLive = 300ll << 20;
    EXPECT_EQ(PressurePolicy::Raw(in, {}), Level::Normal);
}

TEST(MemoryBudget, GrowsOnlyWhenFullTrimmedAndUsed)
{
    BudgetRules r;
    r.min = 4 << 20;
    r.max = 32 << 20;
    BudgetController b(16 << 20, r);
    EXPECT_EQ(b.Decide({0.0, Level::Normal, 15 << 20, 0, 0}), 16u << 20);          // Primes
    // Full, trimmed and used: grows by 12.5%
    EXPECT_GT(b.Decide({1.0, Level::Normal, 15 << 20, 100000, 5}), 16u << 20);
    const auto grown = b.Budget();
    // Again within the cooldown: no
    EXPECT_EQ(b.Decide({2.0, Level::Normal, 15 << 20, 200000, 10}), grown);
    // Not used (no hits since): no
    EXPECT_EQ(b.Decide({100.0, Level::Normal, 17 << 20, 200000, 15}), grown);
    // High: halved per decision, not below the floor
    EXPECT_EQ(b.Decide({101.0, Level::High, 17 << 20, 200000, 15}), grown / 2);
    for (int i = 0; i < 10; ++i) b.Decide({102.0 + i, Level::High, 0, 0, 0});
    EXPECT_EQ(b.Budget(), 4u << 20);
}

TEST(MemoryBudget, CriticalDropsToTheFloorAtOnce)
{
    BudgetController b(32 << 20);
    EXPECT_EQ(b.Decide({0.0, Level::Critical, 0, 0, 0}), 4u << 20);
}

TEST(MemoryBudget, NeverPastTheCeiling)
{
    BudgetRules r;
    r.min = 4 << 20;
    r.max = 18 << 20;
    BudgetController b(16 << 20, r);
    std::uint64_t hits = 0, trims = 0;
    for (int i = 0; i < 20; ++i) b.Decide({i * 100.0, Level::Normal, b.Budget(), hits += 1000000, ++trims});
    EXPECT_EQ(b.Budget(), 18u << 20);
}

TEST(MemoryBudget, RecoversAfterPressure)
{
    BudgetController b(16 << 20);
    b.Decide({0.0, Level::Critical, 0, 0, 0});
    EXPECT_EQ(b.Budget(), 4u << 20);
    std::size_t last = b.Budget();
    int steps = 0;
    while (b.Budget() < (16u << 20) && steps < 100) {
        b.Decide({1.0 + steps, Level::Normal, 0, 0, 0});     // Empty, unused caches: recovery doesn't need them full
        EXPECT_GT(b.Budget(), last);
        last = b.Budget();
        ++steps;
    }
    EXPECT_EQ(b.Budget(), 16u << 20);
    EXPECT_GT(steps, 3);                                     // In steps, not at once
    b.Decide({200.0, Level::Normal, 0, 0, 0});
    EXPECT_EQ(b.Budget(), 16u << 20);                        // And stays there
}

TEST(MemoryBudget, ElevatedShrinksGently)
{
    BudgetController b(16 << 20);
    EXPECT_EQ(b.Decide({0.0, Level::Elevated, 0, 0, 0}), 12u << 20);
}

TEST(MemoryPredictor, ProfilesAndTimeToLimit)
{
    Predictor p;
    p.Observe("edge_p", 900ull << 20, 300ull << 20);
    p.Observe("edge_p", 800ull << 20, 200ull << 20);
    p.EndVisit("edge_p");
    const auto prof = p.Profile("edge_p");
    ASSERT_TRUE(prof);
    EXPECT_EQ(prof->peakPrivate, 900ull << 20);
    EXPECT_EQ(prof->minLargestFree, 200ull << 20);
    EXPECT_EQ(prof->visits, 1);
    EXPECT_FALSE(p.Profile("other"));

    Trend t(10);
    for (int i = 0; i < 10; ++i) t.Add(i, 1000.0 - 10.0 * i);   // Falling 10 a second
    const auto secs = Predictor::TimeToLimit(t, 910.0, 500.0);
    ASSERT_TRUE(secs);
    EXPECT_NEAR(*secs, 41.0, 1e-6);
    Trend up(10);
    for (int i = 0; i < 10; ++i) up.Add(i, 1000.0 + i);
    EXPECT_FALSE(Predictor::TimeToLimit(up, 1009.0, 500.0));
}

// ---- Pools ---------------------------------------------------------------------------------------------------------------
TEST(MemoryPool, ReusesBlocksAndKnowsThem)
{
    FixedPool pool("test", 24, 16);
    std::set<void*> blocks;
    for (int i = 0; i < 40; ++i) {
        void* p = pool.Allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t), 0u);
        EXPECT_TRUE(pool.Owns(p));
        blocks.insert(p);
    }
    EXPECT_EQ(blocks.size(), 40u);
    int stack = 0;
    EXPECT_FALSE(pool.Owns(&stack));
    void* first = *blocks.begin();
    pool.Free(first);
    EXPECT_EQ(pool.Allocate(), first);      // The block just freed comes back first
    const PoolStats st = pool.Stats();
    EXPECT_EQ(st.used, 40u);
    EXPECT_EQ(st.peakUsed, 40u);
    for (void* p : blocks) pool.Free(p);
    EXPECT_EQ(pool.Stats().used, 0u);
}

TEST(MemoryPool, TrimKeepsOneEmptySlab)
{
    FixedPool pool("test", 64, 8, 0, 1);
    std::vector<void*> v;
    for (int i = 0; i < 200; ++i) v.push_back(pool.Allocate());
    const std::size_t slabs = pool.Stats().slabs;
    EXPECT_GT(slabs, 2u);
    for (void* p : v) pool.Free(p);
    EXPECT_GT(pool.Trim(), 0u);
    EXPECT_EQ(pool.Stats().slabs, 1u);
    EXPECT_GT(pool.Release(), 0u);
    EXPECT_EQ(pool.Stats().slabs, 0u);
}

TEST(MemoryPool, CapFallsBackToNew)
{
    struct Thing { int a[20]; explicit Thing(int x) { a[0] = x; } };
    ObjectPool<Thing> things("things", 4, 1);      // One slab of at least 4
    std::vector<Thing*> made;
    for (int i = 0; i < 200; ++i) made.push_back(things.Create(i));
    for (int i = 0; i < 200; ++i) EXPECT_EQ(made[i]->a[0], i);
    EXPECT_GT(things.Stats().fallbacks, 0u);
    EXPECT_EQ(things.Stats().slabs, 1u);
    for (Thing* t : made) things.Destroy(t);       // Pool blocks and heap objects alike
    EXPECT_EQ(things.Stats().used, 0u);
}

TEST(MemoryPool, RegistryAndTrimRequests)
{
    {
        SyncPool shared("shared test pool", 32, 8);
        FixedPool own("own test pool", 32, 8);
        std::vector<void*> a, b;
        for (int i = 0; i < 400; ++i) { a.push_back(shared.Allocate()); b.push_back(own.Allocate()); }   // Several slabs each
        for (void* p : a) shared.Free(p);
        for (void* p : b) own.Free(p);
        bool sawShared = false, sawOwn = false;
        for (const PoolStats& s : pools::Snapshot()) {
            sawShared |= s.name == "shared test pool";
            sawOwn |= s.name == "own test pool";
        }
        EXPECT_TRUE(sawShared);
        EXPECT_TRUE(sawOwn);
        const std::size_t ownSlabs = own.Stats().slabs;
        EXPECT_GT(pools::TrimAll(), 0u);            // The SyncPool at once
        EXPECT_EQ(shared.Stats().slabs, 1u);
        EXPECT_EQ(own.Stats().slabs, ownSlabs);     // The other on its own thread, when next used
        own.Free(own.Allocate());
        EXPECT_EQ(own.Stats().slabs, 1u);
    }
    for (const PoolStats& s : pools::Snapshot()) EXPECT_NE(s.name, "shared test pool");    // Gone with it
}

// ---- Arenas --------------------------------------------------------------------------------------------------------------
TEST(MemoryArena, BumpsAlignsAndRewinds)
{
    Arena a("test", 4096, 8192);
    auto* c = static_cast<char*>(a.Allocate(3, 1));
    auto* d = static_cast<double*>(a.Allocate(sizeof(double), alignof(double)));
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(d) % alignof(double), 0u);
    EXPECT_GT(reinterpret_cast<char*>(d), c);
    const auto mark = a.Mark();
    for (int i = 0; i < 100; ++i) ASSERT_NE(a.Allocate(200), nullptr);   // Several chunks
    EXPECT_GT(a.Stats().chunks, 1u);
    a.Rewind(mark);
    EXPECT_EQ(a.Stats().used, mark.total);
    // Chunks went to the spare list (up to 8 KB), the rest freed
    EXPECT_LE(a.Stats().spareBytes, 8192u);
    void* big = a.Allocate(100000);                 // Oversized: a chunk of its own
    ASSERT_NE(big, nullptr);
    EXPECT_EQ(a.Stats().oversized, 1u);
    a.Reset();
    EXPECT_EQ(a.Stats().used, 0u);
    EXPECT_GT(a.Trim() + 1, 0u);
}

TEST(MemoryArena, ScratchScopesNest)
{
    Arena& arena = ThreadScratch();
    const std::size_t before = arena.Stats().used;
    {
        Scratch outer;
        int* x = outer.Array<int>(100);
        x[99] = 7;
        {
            Scratch inner;
            std::vector<int, ScratchAllocator<int>> v(inner.Allocator<int>());
            for (int i = 0; i < 1000; ++i) v.push_back(i);
            EXPECT_EQ(v[999], 999);
        }
        EXPECT_EQ(x[99], 7);                        // The inner scope didn't take the outer's memory
        EXPECT_GE(arena.Stats().used, before + 400);
    }
    EXPECT_EQ(arena.Stats().used, before);
}

TEST(MemoryArena, PersistentStrings)
{
    const char* s = PersistentString("hello", 5);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(s, "hello");
    EXPECT_GE(PersistentStats().used, 6u);
}

// ---- Heap counters and tags ----------------------------------------------------------------------------------------------
TEST(MemoryHeap, SizeClasses)
{
    EXPECT_EQ(SizeClassOf(1), 0);
    EXPECT_EQ(SizeClassOf(16), 0);
    EXPECT_EQ(SizeClassOf(17), 1);
    EXPECT_EQ(SizeClassOf(32), 1);
    EXPECT_EQ(SizeClassOf(33), 2);
    EXPECT_EQ(SizeClassOf(SIZE_MAX / 2), kSizeClasses - 1);
    EXPECT_EQ(SizeClassLimit(0), 16u);
    EXPECT_EQ(SizeClassLimit(kSizeClasses - 1), SIZE_MAX);
}

TEST(MemoryHeap, TagsAreRegisteredOnceAndScoped)
{
    const TagId a = RegisterTag("test tag A");
    EXPECT_NE(a, 0);
    EXPECT_EQ(RegisterTag("test tag A"), a);
    EXPECT_STREQ(TagName(a), "test tag A");
    EXPECT_EQ(CurrentTag(), 0);
    {
        TagScope s(a);
        EXPECT_EQ(CurrentTag(), a);
    }
    EXPECT_EQ(CurrentTag(), 0);
}

// ---- The sampler ---------------------------------------------------------------------------------------------------------
namespace {
struct SamplerReset {
    ~SamplerReset()
    {
        sampler::SetMode(sampler::Mode::Off);
        sampler::Clear();
    }
};
}

TEST(MemorySampler, FullModeIsExactAndFreesMatch)
{
    SamplerReset reset;
    sampler::Clear();
    sampler::SetMode(sampler::Mode::Full);
    const TagId tag = RegisterTag("sampler test");
    const auto gen = sampler::Generation();
    static char blocks[100][64];
    for (int i = 0; i < 100; ++i) sampler::OnAlloc(blocks[i], 64, tag);
    auto rep = sampler::Snapshot(gen);
    EXPECT_EQ(rep.liveBytes, 6400u);
    EXPECT_EQ(rep.liveBlocks, 100u);
    EXPECT_EQ(rep.liveByTag[tag], 6400u);
    ASSERT_FALSE(rep.sites.empty());
    EXPECT_FALSE(rep.sites[0].frames.empty());     // Where it came from
    for (int i = 0; i < 60; ++i) sampler::OnFree(blocks[i]);
    rep = sampler::Snapshot(gen);
    EXPECT_EQ(rep.liveBytes, 40u * 64);
    EXPECT_GT(rep.lifetime[0] + rep.lifetime[1], 0u); // Freed young
    // Only what this thread allocated
    EXPECT_EQ(sampler::Snapshot(gen, 20, GetCurrentThreadId()).liveBytes, 40u * 64);
    EXPECT_EQ(sampler::Snapshot(gen, 20, GetCurrentThreadId() + 1).liveBytes, 0u);
    for (int i = 60; i < 100; ++i) sampler::OnFree(blocks[i]);
    EXPECT_EQ(sampler::Snapshot(gen).liveBytes, 0u);
}

TEST(MemorySampler, SampledEstimateIsClose)
{
    SamplerReset reset;
    sampler::Clear();
    sampler::SetMode(sampler::Mode::Sampled, 64 * 1024);
    const auto gen = sampler::Generation();
    // 32 MB in 8,192 blocks of 4 KB: about 512 samples
    std::vector<char> backing(8192 * 16);
    for (int i = 0; i < 8192; ++i) sampler::OnAlloc(backing.data() + i * 16, 4096, 0);
    const auto rep = sampler::Snapshot(gen);
    const double estimate = static_cast<double>(rep.liveBytes);
    EXPECT_GT(rep.samples - gen, 300u);
    EXPECT_NEAR(estimate, 32.0 * 1024 * 1024, 32.0 * 1024 * 1024 * 0.25);
}

TEST(MemorySampler, OffRecordsNothingButStillMatchesFrees)
{
    SamplerReset reset;
    sampler::Clear();
    sampler::SetMode(sampler::Mode::Full);
    static char block[32];
    sampler::OnAlloc(block, 32, 0);
    sampler::SetMode(sampler::Mode::Off);
    static char other[32];
    sampler::OnAlloc(other, 32, 0);
    EXPECT_EQ(sampler::Snapshot().liveBytes, 32u);
    sampler::OnFree(block);
    EXPECT_EQ(sampler::Snapshot().liveBytes, 0u);
}

// ---- The process ---------------------------------------------------------------------------------------------------------
TEST(MemoryProcess, ReadsAndWalksTheAddressSpace)
{
    const ProcessSample p = ReadProcess();
    EXPECT_TRUE(p.ok);
    EXPECT_GT(p.privateBytes, 0u);
    EXPECT_GT(p.addressTotal, 0u);
    AddressSpaceWalker w;
    int steps = 0;
    while (!w.Step(0.05) && steps < 100000) ++steps;
    ASSERT_TRUE(w.HasResult());
    const AddressSpace& a = w.Result();
    EXPECT_GT(a.regions, 10u);
    EXPECT_GT(a.free.largestFree, 0u);
    EXPECT_GT(a.image, 0u);                 // This executable at least
    EXPECT_LE(a.free.largestFree, a.free.totalFree);
}
