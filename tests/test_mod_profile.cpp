// The profiler per mod (loader/src/runtime/mod_profile.h): calls summed per second into ms per tick, by callback
#include "runtime/mod_profile.h"

#include <gtest/gtest.h>

namespace pr = mirage::mods::profile;

namespace
{
	const pr::Mod* Find(const std::vector<pr::Mod>& all, const std::string& id)
	{
		for (const auto& m : all)
			if (m.id == id) return &m;
		return nullptr;
	}
}

TEST(ModProfile, SumsASecondIntoMsPerTickSlowestFirst)
{
	pr::Reset();
	pr::Tick(10.0);								// Starts the first second
	for (int t = 0; t < 10; ++t) {
		pr::Record("a", "tick event", 0.5);
		pr::Record("a", "hook Engine.Actor.Touch", 0.1);
		pr::Record("b", "B on_tick", 0.05);
		pr::Tick(10.0 + 0.1 * (t + 1));
	}
	const auto all = pr::List();
	ASSERT_EQ(all.size(), 2u);
	EXPECT_EQ(all[0].id, "a");					// Highest per tick first
	const pr::Mod* a = Find(all, "a");
	ASSERT_NE(a, nullptr);
	EXPECT_NEAR(a->msPerTick, 0.6, 1e-9);		// 6 ms over the 10 ticks of that second
	EXPECT_TRUE(a->overBudget == (a->msPerTick > pr::kBudgetMs));
	ASSERT_EQ(a->callbacks.size(), 2u);
	EXPECT_EQ(a->callbacks[0].what, "tick event");
	EXPECT_EQ(a->callbacks[0].calls, 10u);
	EXPECT_NEAR(a->callbacks[0].lastSecondMs, 5.0, 1e-9);
	EXPECT_NEAR(a->callbacks[0].maxMs, 0.5, 1e-9);
}

TEST(ModProfile, OverBudgetUntilUnderAgainAndForget)
{
	pr::Reset();
	pr::Tick(0.0);
	pr::Record("slow", "tick event", 5.0);
	pr::Tick(1.0);								// One tick in that second: 5 ms per tick
	EXPECT_TRUE(Find(pr::List(), "slow")->overBudget);
	pr::Record("slow", "tick event", 0.1);
	pr::Tick(2.0);
	const auto after = pr::List();
	const auto* s = Find(after, "slow");
	ASSERT_NE(s, nullptr);
	EXPECT_FALSE(s->overBudget);
	EXPECT_NEAR(s->callbacks[0].totalMs, 5.1, 1e-9);	// Totals since load are kept across seconds
	pr::Forget("slow");
	EXPECT_EQ(Find(pr::List(), "slow"), nullptr);
}
