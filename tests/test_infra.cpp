// The event bus, background tasks, the resource cache and the config cache.
#include "core/compressed_file.h"
#include "core/events.h"
#include "core/resource_cache.h"
#include "core/tasks.h"
#include "runtime/config_cache.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
struct Ping { int n = 0; };
struct Other { std::string s; };

fs::path TempDir(const char* name)
{
    const fs::path d = fs::temp_directory_path() / ("mirage-tests-" + std::to_string(GetCurrentProcessId())) / name;
    fs::create_directories(d);
    return d;
}

void WriteFile(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}
}

// ---- Events --------------------------------------------------------------------------------------------------------------
TEST(Events, PriorityOrderAndUnsubscribe)
{
    events::EventBus bus;
    std::vector<int> order;
    auto low = bus.Subscribe<Ping>([&](const Ping& p) { order.push_back(p.n * 10 + 1); }, {.priority = 1});
    auto high = bus.Subscribe<Ping>([&](const Ping& p) { order.push_back(p.n * 10 + 9); }, {.priority = 9});
    auto other = bus.Subscribe<Other>([&](const Other&) { order.push_back(-1); });
    bus.Publish(Ping{1});
    EXPECT_EQ(order, (std::vector<int>{19, 11}));
    high.Reset();
    bus.Publish(Ping{2});
    EXPECT_EQ(order, (std::vector<int>{19, 11, 21}));
    EXPECT_EQ(bus.Handlers<Ping>(), 1u);
    {
        auto scoped = bus.Subscribe<Ping>([&](const Ping&) { order.push_back(99); });
    }   // Gone with its scope
    bus.Publish(Ping{3});
    EXPECT_EQ(order.back(), 31);
}

TEST(Events, AThrowingHandlerDoesntStopTheOthers)
{
    events::EventBus bus;
    int ran = 0;
    auto a = bus.Subscribe<Ping>([](const Ping&) { throw std::runtime_error("boom"); }, {.priority = 5, .owner = "test"});
    auto b = bus.Subscribe<Ping>([&](const Ping&) { ++ran; });
    EXPECT_NO_THROW(bus.Publish(Ping{}));
    EXPECT_EQ(ran, 1);
    bool counted = false;
    for (const auto& s : bus.Snapshot()) counted |= s.failures == 1 && s.published == 1;
    EXPECT_TRUE(counted);
}

TEST(Events, UnsubscribingFromInsideAHandler)
{
    events::EventBus bus;
    int calls = 0;
    events::Subscription self;
    self = bus.Subscribe<Ping>([&](const Ping&) { ++calls; self.Reset(); });
    bus.Publish(Ping{});
    bus.Publish(Ping{});
    EXPECT_EQ(calls, 1);
}

TEST(Events, QueuedDeliveryGoesThroughTheDispatcher)
{
    events::EventBus bus;
    std::vector<std::function<void()>> queue;
    events::SetDispatcher(events::Where::GameThread, [&](std::function<void()> fn) { queue.push_back(std::move(fn)); });
    int got = 0;
    auto s = bus.Subscribe<Ping>([&](const Ping& p) { got = p.n; }, {.where = events::Where::GameThread});
    bus.Publish(Ping{7});
    EXPECT_EQ(got, 0);              // Not yet: queued for the game thread
    ASSERT_EQ(queue.size(), 1u);
    queue[0]();
    EXPECT_EQ(got, 7);
    events::SetDispatcher(events::Where::GameThread, nullptr);
}

// ---- Tasks ---------------------------------------------------------------------------------------------------------------
TEST(Tasks, RunWaitAndResults)
{
    tasks::Start(2);
    auto t = tasks::Run([] { return 6 * 7; }, {.name = "answer"});
    ASSERT_TRUE(t.Wait(5.0));
    ASSERT_TRUE(t.Ok());
    EXPECT_EQ(*t.Result(), 42);
    auto v = tasks::Run([] {});
    EXPECT_TRUE(v.Wait(5.0));
    EXPECT_TRUE(v.Ok());
    tasks::Stop();
}

TEST(Tasks, FailuresAndContinuations)
{
    tasks::Start(2);
    auto bad = tasks::Run([]() -> int { throw std::runtime_error("nope"); });
    bad.Wait(5.0);
    EXPECT_EQ(bad.GetStatus(), tasks::Status::Failed);
    EXPECT_EQ(bad.Error(), "nope");
    std::atomic<int> got{0};
    std::atomic<bool> otherwise{false};
    bad.Then([&](const int&) { got = -1; }, tasks::On::Inline, [&](const std::string&) { otherwise = true; });
    EXPECT_TRUE(otherwise.load());

    auto good = tasks::Run([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return 5; });
    good.Then([&](const int& n) { got = n; }, tasks::On::Inline);       // Registered before it finished
    good.Wait(5.0);
    for (int i = 0; i < 100 && got.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(got.load(), 5);
    tasks::Stop();
}

TEST(Tasks, CancelAndStop)
{
    tasks::Start(1);
    std::atomic<bool> sawCancel{false};
    auto longOne = tasks::Run([&] {
        for (int i = 0; i < 500 && !tasks::Cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        sawCancel = tasks::Cancelled();
    });
    auto queued = tasks::Run([] { return 1; }, {.priority = tasks::Priority::Low});   // Behind the long one (1 worker)
    queued.Cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tasks::Stop();                          // Asks the running one to stop, drops the queue
    EXPECT_TRUE(sawCancel.load());
    EXPECT_EQ(queued.GetStatus(), tasks::Status::Cancelled);
    EXPECT_FALSE(tasks::Running());
    auto inlineRun = tasks::Run([] { return 3; });   // Without workers: inline
    EXPECT_TRUE(inlineRun.Ok());
}

TEST(Tasks, ManyTasksAllRun)
{
    tasks::Start(3);
    std::atomic<int> sum{0};
    std::vector<tasks::Task<bool>> all;
    for (int i = 1; i <= 200; ++i) all.push_back(tasks::Run([i, &sum] { sum += i; }));
    for (auto& t : all) ASSERT_TRUE(t.Wait(10.0));
    EXPECT_EQ(sum.load(), 200 * 201 / 2);
    EXPECT_GE(tasks::Snapshot().completed, 200u);
    tasks::Stop();
}

// ---- Resource cache ------------------------------------------------------------------------------------------------------
TEST(ResourceCache, LoadsSharesAndReloads)
{
    const fs::path dir = TempDir("res");
    const fs::path a = dir / "a.txt", b = dir / "b.txt";
    WriteFile(a, "hello");
    WriteFile(b, "hello");              // Same bytes, another path
    res::ResourceCache c("test resources", 0.0);      // Check the files on every Get
    auto ra = c.Get(a);
    ASSERT_TRUE(ra);
    EXPECT_EQ(ra->Text(), "hello");
    EXPECT_EQ(c.Get(a).get(), ra.get());               // Kept
    auto rb = c.Get(b);
    EXPECT_EQ(rb.get(), ra.get());                     // Shared by content
    EXPECT_EQ(c.Stats().shared, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    WriteFile(a, "changed!");
    auto ra2 = c.Get(a);
    ASSERT_TRUE(ra2);
    EXPECT_EQ(ra2->Text(), "changed!");
    EXPECT_EQ(ra->Text(), "hello");                    // Holders of the old one keep it as it was
    EXPECT_EQ(c.Get(dir / "missing.txt"), nullptr);
    fs::remove_all(dir);
}

TEST(ResourceCache, TrimKeepsWhatIsInUse)
{
    const fs::path dir = TempDir("res-trim");
    WriteFile(dir / "x.bin", std::string(10000, 'x'));
    WriteFile(dir / "y.bin", std::string(20000, 'y'));
    res::ResourceCache c("test resources trim", 60.0);
    auto x = c.Get(dir / "x.bin");
    c.Get(dir / "y.bin");
    EXPECT_EQ(c.Bytes(), 30000u);
    EXPECT_GE(c.Trim(1 << 20), 30000u);
    EXPECT_EQ(c.Bytes(), 0u);
    EXPECT_EQ(c.Get(dir / "x.bin").get(), x.get());   // Still found: someone holds it
    fs::remove_all(dir);
}

TEST(ResourceCache, LoadOnAWorker)
{
    const fs::path dir = TempDir("res-async");
    WriteFile(dir / "z.txt", "async");
    tasks::Start(1);
    res::ResourceCache c("test resources async");
    auto t = c.Load(dir / "z.txt");
    ASSERT_TRUE(t.Wait(5.0));
    ASSERT_TRUE(t.Ok() && *t.Result());
    EXPECT_EQ((*t.Result())->Text(), "async");
    tasks::Stop();
    fs::remove_all(dir);
}

// ---- Config cache --------------------------------------------------------------------------------------------------------
TEST(ConfigCache, ParsesOnceReadsAgainWhenChanged)
{
    const fs::path dir = TempDir("cfg");
    const fs::path f = dir / "mod.toml";
    WriteFile(f, "id = \"x\"\n[settings.speed]\ndefault = 3\n");
    const auto before = mirage::config::GetStats();
    auto d1 = mirage::config::Load(f, 0.0);
    ASSERT_TRUE(d1);
    EXPECT_EQ(mirage::config::Get<int>(f, "settings.speed.default", 0), 3);
    EXPECT_EQ(mirage::config::Get<std::string>(f, "id", ""), "x");
    EXPECT_EQ(mirage::config::Get<int>(f, "settings.nothing", -1), -1);
    EXPECT_EQ(mirage::config::Get<int>(f, "id", -2), -2);     // Another type: the fallback
    EXPECT_EQ(mirage::config::Load(f, 0.0).get(), d1.get());  // Unchanged: the same document
    EXPECT_EQ(mirage::config::GetStats().parses - before.parses, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    WriteFile(f, "id = \"y\"\n");
    auto d2 = mirage::config::Load(f, 0.0);
    ASSERT_TRUE(d2);
    EXPECT_EQ(d2->As<std::string>("id").value_or(""), "y");
    EXPECT_EQ(d2->version, d1->version + 1);

    WriteFile(f, "this is = = not toml");                    // Broken: the last good one stays
    mirage::config::Invalidate(f);
    auto d3 = mirage::config::Load(f, 0.0);
    EXPECT_EQ(d3.get(), d2.get());
    fs::remove(f);
    EXPECT_EQ(mirage::config::Load(f, 0.0), nullptr);
    fs::remove_all(dir);
}

// ---- The adaptive update scheduler's per-entry stretch -------------------------------------------------------------------
#include "core/scheduler.h"

TEST(AdaptiveScheduler, ExpensiveEntriesGiveWayFirst)
{
    // No pressure: nothing stretched
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(1.0, 5000.0, 100.0, 8.0), 1.0);
    // Pressure 3x: an average entry gets 3x, a cheap one less, an expensive one more, never past the cap
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(3.0, 100.0, 100.0, 8.0), 3.0);
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(3.0, 10.0, 100.0, 8.0), 1.5);       // A quarter of the stretch
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(3.0, 300.0, 100.0, 8.0), 7.0);
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(3.0, 100000.0, 100.0, 8.0), 8.0);   // Capped
    EXPECT_DOUBLE_EQ(sched::AdaptiveScale(3.0, 0.0, 0.0, 8.0), 1.5);          // No history yet: treated as cheap
}

// logs/.old and crashlogs/.old (core/compressed_file.h): a report compressed and read back is the same text, a plain one
// reads as itself, and maxBytes cuts both
TEST(CompressedFile, RoundTrip)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mirage_zfile_test";
    fs::create_directories(dir);
    const fs::path plain = dir / "crash-20261115-020101.miragecrashlog";
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "line " + std::to_string(i) + ": What: EXCEPTION_ACCESS_VIOLATION\n";
    std::ofstream(plain, std::ios::binary | std::ios::trunc) << text;
    const fs::path packed = dir / "crash-20261115-020101.miragecrashlog.zst";
    std::string error;
    ASSERT_TRUE(zfile::Compress(plain, packed, 9, error)) << error;
    EXPECT_LT(fs::file_size(packed), fs::file_size(plain) / 4);
    EXPECT_FALSE(fs::exists(dir / "crash-20261115-020101.miragecrashlog.zst.tmp"));
    EXPECT_EQ(zfile::ReadText(packed, 1 << 20), text);
    EXPECT_EQ(zfile::ReadText(plain, 1 << 20), text);
    EXPECT_EQ(zfile::ReadText(packed, 100), text.substr(0, 100));
    EXPECT_EQ(zfile::ReadText(plain, 100), text.substr(0, 100));
    EXPECT_EQ(zfile::ReadText(dir / "missing.zst", 100), "");
    fs::remove_all(dir);
}
