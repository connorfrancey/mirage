#include "runtime/class_source.h"

#include "core/clock.h"
#include "paths.h"
#include "runtime/classes.h"
#include "runtime/packages.h"

#include <UnrealLib/Compiler/Compiler.hpp>
#include <UnrealLib/Linker/Linker.hpp>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace mirage::classes
{
	namespace
	{
		// The game's script packages and the compiler over them
		struct Model
		{
			UnrealLib::Linker linker;
			std::vector<const UnrealLib::Package*> scripts;
			std::unique_ptr<UnrealLib::Compiler::ScriptCompiler> compiler;
		};
		std::mutex g_lock;
		std::unique_ptr<Model> g_model;

		Model& Get()
		{
			if (g_model) return *g_model;
			auto m = std::make_unique<Model>();
			const fs::path cooked = packages::CookedDir();
			m->linker.add_search_path(cooked, false);
			for (const char* name : {"Core", "Engine", "GameFramework", "IpDrv", "TdGame"}) {
				const UnrealLib::Package* p = m->linker.find_package(name);
				if (!p) throw std::runtime_error(std::string(name) + ".u wasn't found in " + cooked.string());
				m->scripts.push_back(p);
			}
			m->compiler = std::make_unique<UnrealLib::Compiler::ScriptCompiler>(m->linker, m->scripts);
			g_model = std::move(m);
			return *g_model;
		}

		// FNV-1a: the package name follows the source
		std::uint32_t Hash(const std::string& s)
		{
			std::uint32_t h = 2166136261u;
			for (const unsigned char c : s) h = (h ^ c) * 16777619u;
			return h;
		}
	}

	bool BuildSource(const fs::path& uc, BuiltClass& out, std::string* error)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		std::ifstream in(uc, std::ios::binary);
		if (!in) return fail("can't read " + uc.string());
		std::stringstream ss;
		ss << in.rdbuf();
		const std::string source = ss.str();

		out.className = uc.stem().string();
		const std::string package = fmt::format("{}_{:08x}", out.className, Hash(source));
		const fs::path dir = paths::Dir() / "classes";
		out.package = dir / (package + ".u");
		std::error_code ec;
		if (fs::is_regular_file(out.package, ec)) {
			out.cached = true;
			return true;
		}

		std::lock_guard lock(g_lock);
		const clk::Ticks start = clk::Now();
		UnrealLib::Compiler::BuiltPackage built;
		try {
			built = Get().compiler->build_class_package(package, source);
		} catch (const std::exception& e) {
			return fail(std::string("the game's script packages couldn't be read: ") + e.what());
		}
		out.seconds = clk::SecondsSince(start);
		if (!built.ok()) {
			std::string all;
			for (const std::string& e : built.errors) all += (all.empty() ? "" : "; ") + uc.filename().string() + " " + e;
			return fail(all.empty() ? "it didn't build" : all);
		}
		fs::create_directories(dir, ec);
		const fs::path temp = out.package.string() + ".tmp";
		{
			std::ofstream file(temp, std::ios::binary | std::ios::trunc);
			if (!file) return fail("can't write " + temp.string());
			file.write(reinterpret_cast<const char*>(built.bytes.data()), static_cast<std::streamsize>(built.bytes.size()));
			if (!file) return fail("can't write " + temp.string());
		}
		fs::rename(temp, out.package, ec);
		if (ec) return fail("can't write " + out.package.string() + ": " + ec.message());
		spdlog::info("Classes: built {} from {} in {:.2f} s ({} bytes)", out.package.filename().string(), uc.string(), out.seconds,
					 built.bytes.size());
		return true;
	}

	Classes::UClass* LoadSource(const fs::path& uc, std::string* error, BuiltClass* built)
	{
		BuiltClass b;
		if (!BuildSource(uc, b, error)) return nullptr;
		Classes::UClass* cls = LoadClassFrom(b.package, b.className, error);
		if (built) *built = b;
		return cls;
	}

	void Release()
	{
		std::lock_guard lock(g_lock);
		g_model.reset();
	}
}
