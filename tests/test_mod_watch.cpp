#include "runtime/mod_watch.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <chrono>
#include <fstream>
#include <thread>

namespace
{
	namespace fs = std::filesystem;
	namespace watch = mirage::mods::watch;
	using namespace std::chrono_literals;

	class ModWatch : public testing::Test
	{
	protected:
		void SetUp() override
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = fs::temp_directory_path() /
				("mirage-mod-watch-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
			mod = root / "test-mod";
			ASSERT_TRUE(fs::create_directories(mod));
		}

		void TearDown() override
		{
			watch::Stop();
			std::error_code ec;
			fs::remove_all(root, ec);
		}

		fs::path root, mod;
	};
}

TEST_F(ModWatch, FingerprintIgnoresRuntimeOwnedFiles)
{
	std::ofstream(mod / "main.lua") << "return 1";
	const auto before = watch::Fingerprint(mod);
	std::ofstream(mod / "config.toml") << "setting = 1";
	for (const char* folder : {".mirage", "levels", "routes", "kismet"}) {
		fs::create_directory(mod / folder);
		std::ofstream(mod / folder / "test.toml") << "engine managed";
	}
	EXPECT_EQ(watch::Fingerprint(mod), before);

	// The configured entry can live in bin. A full scan still accounts for it.
	fs::create_directory(mod / "bin");
	std::ofstream(mod / "bin" / "entry.dll") << "new build";
	EXPECT_NE(watch::Fingerprint(mod), before);
}

TEST_F(ModWatch, UnwatchableRootKeepsFullScansEnabled)
{
	watch::Start({root, root / "missing"});
	EXPECT_TRUE(watch::TakeChanges().scanAll);
	EXPECT_TRUE(watch::TakeChanges().scanAll);
}

TEST_F(ModWatch, DeclarationEditsLeaveCodeFingerprintStable)
{
	fs::create_directory(mod / "config");
	fs::create_directory(mod / "levels");
	const fs::path actions = "config/actions.toml";
	const fs::path settings = "levels/settings.toml";
	std::ofstream(mod / actions) << "jump = \"J\"";
	std::ofstream(mod / settings) << "speed = 42";
	std::ofstream(mod / "main.lua") << "return 1";
	const auto code = watch::Fingerprint(mod, actions, settings, true);
	const auto before = watch::Fingerprint(mod, actions, settings);
	std::ofstream(mod / actions) << "jump = \"Ctrl+J\"";
	EXPECT_EQ(watch::Fingerprint(mod, actions, settings, true), code);
	const auto actionChanged = watch::Fingerprint(mod, actions, settings);
	EXPECT_NE(actionChanged, before);
	std::ofstream(mod / settings) << "speed = 123";
	EXPECT_NE(watch::Fingerprint(mod, actions, settings), actionChanged);
	EXPECT_EQ(watch::Fingerprint(mod, actions, settings, true), code);
	std::ofstream(mod / "main.lua") << "return 100";
	EXPECT_NE(watch::Fingerprint(mod, actions, settings, true), code);
}

TEST_F(ModWatch, NotificationNamesTheChangedModFolder)
{
	watch::Start({root});
	ASSERT_TRUE(watch::TakeChanges().scanAll);	// Initial check.
	ASSERT_FALSE(watch::TakeChanges().scanAll);
	std::ofstream(mod / "main.lua") << "return 1";

	const auto deadline = std::chrono::steady_clock::now() + 3s;
	do {
		if (watch::TakeChanges().folders.contains(L"test-mod")) return;
		std::this_thread::sleep_for(5ms);
	} while (std::chrono::steady_clock::now() < deadline);
	FAIL() << "The watcher did not report the changed mod folder";
}
