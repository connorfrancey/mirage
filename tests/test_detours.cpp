// Managed native detours (loader/src/runtime/detours.h): several detours on one function, in priority order, relinked as
// they come and go. On real code in this test executable, through MirageHook (core/hooking.h) like in the game.
#include "runtime/detours.h"

#include "core/hooking.h"
#include <gtest/gtest.h>

namespace
{
	// The function detoured: noinline, and called through a volatile pointer so the call isn't folded away
	extern "C" __declspec(noinline) int __cdecl MirageDetourTarget(int x) { return x + 1; }
	int (*volatile g_call)(int) = &MirageDetourTarget;

	int(__cdecl* g_origHigh)(int) = nullptr;
	int(__cdecl* g_origLow)(int) = nullptr;
	int(__cdecl* g_origMid)(int) = nullptr;
	int __cdecl High(int x) { return g_origHigh(x) * 2; }		// Priority 20
	int __cdecl Low(int x) { return g_origLow(x) + 100; }		// Priority 10
	int __cdecl Mid(int x) { return g_origMid(x) - 7; }			// Priority 15, added last

	// Each test leaves no hook behind, and no manager
	struct HooksDown
	{
		~HooksDown() { hooking::Shutdown(); }
	};
}

TEST(Detours, ChainRunsByPriorityAndRelinks)
{
	HooksDown down;
	void* target = reinterpret_cast<void*>(&MirageDetourTarget);
	ASSERT_EQ(g_call(1), 2);

	std::string error;
	const int low = mirage::detours::Add(target, reinterpret_cast<void*>(&Low), 10, reinterpret_cast<void**>(&g_origLow), "low", &error);
	ASSERT_GT(low, 0) << error;
	EXPECT_EQ(g_call(1), 102);			// Low -> real

	const int high = mirage::detours::Add(target, reinterpret_cast<void*>(&High), 20, reinterpret_cast<void**>(&g_origHigh), "high", &error);
	ASSERT_GT(high, 0) << error;
	EXPECT_EQ(g_call(1), 204);			// High -> Low -> real: (1 + 1 + 100) * 2
	EXPECT_EQ(mirage::detours::Chain(target), (std::vector<std::string>{"high (20)", "low (10)"}));

	const int mid = mirage::detours::Add(target, reinterpret_cast<void*>(&Mid), 15, reinterpret_cast<void**>(&g_origMid), "mid", &error);
	ASSERT_GT(mid, 0) << error;
	EXPECT_EQ(g_call(1), 190);			// High -> Mid -> Low -> real: (102 - 7) * 2

	EXPECT_EQ(mirage::detours::Add(target, reinterpret_cast<void*>(&Mid), 1, reinterpret_cast<void**>(&g_origMid), "mid", &error), 0);

	mirage::detours::Remove(high);
	EXPECT_EQ(g_call(1), 95);			// Mid -> Low -> real
	EXPECT_EQ(g_origHigh, nullptr);
	mirage::detours::Remove(low);
	EXPECT_EQ(g_call(1), -5);			// Mid -> real: 2 - 7
	mirage::detours::Remove(mid);
	EXPECT_EQ(g_call(1), 2);			// The function as it was
	EXPECT_TRUE(mirage::detours::Chain(target).empty());
}

TEST(Detours, RemoveAllPutsTheFunctionBack)
{
	HooksDown down;
	void* target = reinterpret_cast<void*>(&MirageDetourTarget);
	ASSERT_GT(mirage::detours::Add(target, reinterpret_cast<void*>(&Low), 10, reinterpret_cast<void**>(&g_origLow), "low"), 0);
	ASSERT_GT(mirage::detours::Add(target, reinterpret_cast<void*>(&High), 20, reinterpret_cast<void**>(&g_origHigh), "high"), 0);
	EXPECT_EQ(g_call(3), 208);
	mirage::detours::RemoveAll();
	EXPECT_EQ(g_call(3), 4);
}

TEST(Detours, RejectsBadArguments)
{
	HooksDown down;
	std::string error;
	EXPECT_EQ(mirage::detours::Add(nullptr, reinterpret_cast<void*>(&Low), 0, reinterpret_cast<void**>(&g_origLow), "x", &error), 0);
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(mirage::detours::Add(reinterpret_cast<void*>(&MirageDetourTarget), reinterpret_cast<void*>(&Low), 0, nullptr, "x"), 0);
}

// The loader's own hooks: one per target, made disabled with the original set, enabled together, removed.
namespace
{
	extern "C" __declspec(noinline) int __cdecl MirageHookTargetA(int x) { return x * 3; }
	extern "C" __declspec(noinline) int __cdecl MirageHookTargetB(int x) { return x - 1; }
	int (*volatile g_callA)(int) = &MirageHookTargetA;
	int (*volatile g_callB)(int) = &MirageHookTargetB;
	int(__cdecl* g_origA)(int) = nullptr;
	int(__cdecl* g_origB)(int) = nullptr;
	int __cdecl DetourA(int x) { return g_origA(x) + 1000; }
	int __cdecl DetourB(int x) { return g_origB(x) + 2000; }
}

TEST(Hooking, CreateEnableTogetherAndRemove)
{
	HooksDown down;
	void* a = reinterpret_cast<void*>(&MirageHookTargetA);
	void* b = reinterpret_cast<void*>(&MirageHookTargetB);
	ASSERT_TRUE(hooking::Create(a, &DetourA, &g_origA, "A"));
	ASSERT_TRUE(hooking::Create(b, &DetourB, &g_origB, "B"));
	ASSERT_NE(g_origA, nullptr);
	EXPECT_EQ(g_callA(2), 6);			// Made, not enabled
	EXPECT_EQ(g_origA(2), 6);			// The original runs the real function before the hook is live

	const hooking::Status again = hooking::Create(a, &DetourA, &g_origA, "A again");
	EXPECT_FALSE(again);
	EXPECT_TRUE(again.alreadyHooked);

	void* const both[] = {a, b};
	ASSERT_TRUE(hooking::Enable(both));
	EXPECT_EQ(g_callA(2), 1006);
	EXPECT_EQ(g_callB(2), 2001);

	ASSERT_TRUE(hooking::Disable(a));
	EXPECT_EQ(g_callA(2), 6);
	ASSERT_TRUE(hooking::Remove(b));
	EXPECT_EQ(g_callB(2), 1);
	EXPECT_FALSE(hooking::Hooked(b));
	EXPECT_FALSE(hooking::Remove(b));		// Gone already
}

TEST(Hooking, InstallAndShutdownPutTheFunctionBack)
{
	void* a = reinterpret_cast<void*>(&MirageHookTargetA);
	ASSERT_TRUE(hooking::Install(a, &DetourA, &g_origA, "A"));
	EXPECT_EQ(g_callA(1), 1003);
	hooking::Shutdown();
	EXPECT_EQ(g_callA(1), 3);
	EXPECT_FALSE(hooking::Hooked(a));
	EXPECT_FALSE(hooking::Install(nullptr, &DetourA, &g_origA, "nothing"));
}

// The guard (runtime/detour_guard.h): a detour that faults in its own code is skipped, the call goes on in the function
// it would have called, and the fault is reported once
namespace
{
	extern "C" __declspec(noinline) int __cdecl MirageGuardTarget(int x) { return x + 1; }
	int (*volatile g_guardCall)(int) = &MirageGuardTarget;
	int(__cdecl* g_origFaulty)(int) = nullptr;
	int(__cdecl* g_origAbove)(int) = nullptr;
	volatile bool g_faultNow = false;
	volatile int* volatile g_nowhere = nullptr;
	__declspec(noinline) int __cdecl Faulty(int x)
	{
		if (g_faultNow) *g_nowhere = x;		// An access violation in the detour's own code
		return g_origFaulty(x) + 5;
	}
	int __cdecl Above(int x) { return g_origAbove(x) * 2; }		// Calls the faulty one: goes on with what it gets
}

TEST(Detours, AFaultInADetourIsSkippedAndReported)
{
	HooksDown down;
	void* target = reinterpret_cast<void*>(&MirageGuardTarget);
	mirage::detours::TakeFaults();		// Nothing left over from another test
	const int id = mirage::detours::Add(target, reinterpret_cast<void*>(&Faulty), 10, reinterpret_cast<void**>(&g_origFaulty), "faulty");
	ASSERT_GT(id, 0);
	const int above = mirage::detours::Add(target, reinterpret_cast<void*>(&Above), 20, reinterpret_cast<void**>(&g_origAbove), "above");
	ASSERT_GT(above, 0);
	EXPECT_EQ(g_guardCall(1), 14);		// Above -> Faulty -> real: (1 + 1 + 5) * 2

	g_faultNow = true;
	EXPECT_EQ(g_guardCall(1), 4);		// Faulty faulted: the call went on in the real function, Above got 2
	g_faultNow = false;
	EXPECT_EQ(g_guardCall(1), 4);		// And it is skipped from then on
	const auto faults = mirage::detours::TakeFaults();
	ASSERT_EQ(faults.size(), 1u);
	EXPECT_EQ(faults[0].id, id);
	EXPECT_EQ(faults[0].owner, "faulty");
	EXPECT_EQ(faults[0].target, target);
	EXPECT_EQ(faults[0].code, 0xC0000005ul);
	EXPECT_TRUE(mirage::detours::TakeFaults().empty());

	mirage::detours::Remove(id);
	EXPECT_EQ(g_guardCall(1), 4);		// Above -> real
	mirage::detours::Remove(above);
	EXPECT_EQ(g_guardCall(1), 2);
}
