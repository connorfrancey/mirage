// Models from mods in the game (models.h)
#include "runtime/models.h"

#include "game/game.h"
#include "game/uobject.h"
#include "paths.h"
#include "runtime/classes.h"
#include "runtime/model_files.h"
#include "runtime/packages.h"
#include "sdk/SDK.hpp"

#include <UnrealLib/Linker/Linker.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace mirage::models
{
	namespace
	{
		struct Build
		{
			fs::file_time_type stamp;		// The .mmesh's, when the build started
			std::future<std::pair<std::string, fs::path>> result;	// The error ("" when built) and the package
			bool done = false;
			std::string error;
			fs::path package;
			std::string meshName;
			Classes::UStaticMesh* mesh = nullptr;
			int slot = -1;
		};
		std::map<std::string, Build> g_builds;		// By the model file's path
		std::mutex g_buildLock;						// One build at a time (UnrealLib reads the game's packages)

		// A name the engine takes: letters, digits and underscores, not starting with a digit
		std::string Sanitize(const std::string& s)
		{
			std::string out;
			for (const char c : s) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
			if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(0, "M");
			return out;
		}

		// The package for `file`'s converted model, built unless there is one for the same bytes. "" or the error.
		std::string BuildFor(const fs::path& file, const std::string& meshName, fs::path& package)
		{
			const fs::path mmesh = file.string() + ".mmesh";
			MeshData data;
			std::string error;
			if (!ReadMeshFile(mmesh.string(), data, &error)) return error;
			if (data.sourceHash != HashFile(file.string()))
				return "it changed since it was converted: run `mirage model " + file.filename().string() + " --convert` again";
			// Named by the converted model and the builder's version: a package of an older layout is built again
			constexpr const char* kBuilderVersion = "mirage-model-package 3";		// 2: RB_BodySetup; 3: its hull points
			std::ifstream in(mmesh, std::ios::binary);
			char hash[16];
			std::snprintf(hash, sizeof hash, "%08x",
						  HashBytes(kBuilderVersion + std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())));
			package = paths::Dir() / "models" / (meshName + "_" + hash + ".upk");
			std::error_code ec;
			if (fs::is_regular_file(package, ec)) return {};
			const auto t0 = std::chrono::steady_clock::now();
			std::lock_guard lock(g_buildLock);
			UnrealLib::Linker linker;
			linker.add_search_path(packages::CookedDir(), true);
			if (!BuildPackage(linker, data, meshName, package, &error)) return error;
			spdlog::info("Models: {} built ({} vertices, {} triangles) in {:.2f} s", package.filename().string(), data.vertices.size(),
						 data.indices.size() / 3, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
			return {};
		}
	}

	Classes::UStaticMesh* MeshOf(const fs::path& file, std::string* why, bool* pending)
	{
		const auto fail = [&](const std::string& reason) -> Classes::UStaticMesh* {
			if (why) *why = reason;
			return nullptr;
		};
		const fs::path mmesh = file.string() + ".mmesh";
		std::error_code ec;
		if (!fs::is_regular_file(file, ec)) return fail("no model " + file.string());
		if (!fs::is_regular_file(mmesh, ec))
			return fail(file.filename().string() + " isn't converted: run `mirage model " + file.filename().string() + " --convert`");
		// Built again when either file changes (BuildFor checks the converted model is the model's)
		const auto stamp = std::max(fs::last_write_time(mmesh, ec), fs::last_write_time(file, ec));
		const std::string key = file.string();
		auto it = g_builds.find(key);
		if (it != g_builds.end() && it->second.done && it->second.stamp != stamp) {
			g_builds.erase(it);
			it = g_builds.end();
		}
		if (it == g_builds.end()) {
			Build b;
			b.stamp = stamp;
			b.meshName = Sanitize(file.stem().string());
			b.result = std::async(std::launch::async, [file, name = b.meshName] {
				std::pair<std::string, fs::path> r;
				r.first = BuildFor(file, name, r.second);
				return r;
			});
			it = g_builds.emplace(key, std::move(b)).first;
		}
		Build& b = it->second;
		if (!b.done) {
			if (b.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				*pending = true;
				return nullptr;
			}
			auto r = b.result.get();
			b.error = r.first;
			b.package = r.second;
			b.done = true;
		}
		if (!b.error.empty()) return fail(file.filename().string() + ": " + b.error);
		auto& objects = Classes::UObject::GetGlobalObjects();
		if (b.mesh && b.slot >= 0 && b.slot < static_cast<int>(objects.Num()) && objects.GetByIndex(b.slot) == b.mesh) return b.mesh;
		std::string error;
		auto* o = classes::LoadFrom(b.package, b.package.stem().string() + "." + b.meshName, Classes::UStaticMesh::StaticClass(), &error);
		b.mesh = static_cast<Classes::UStaticMesh*>(o);
		b.slot = b.mesh ? game::IndexOf(b.mesh) : -1;
		if (!b.mesh) return fail(file.filename().string() + ": the engine didn't load it: " + error);
		return b.mesh;
	}
}
