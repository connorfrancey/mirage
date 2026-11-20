// Models from mods in the game (models.h)
#include "runtime/models.h"

#include "game/game.h"
#include "game/uobject.h"
#include "paths.h"
#include "game/object_query.h"
#include "runtime/call.h"
#include "runtime/classes.h"
#include "runtime/file_redirect.h"
#include "runtime/model_files.h"
#include "runtime/packages.h"
#include "runtime/texture_files.h"
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
#include <memory>
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

	namespace
	{
		// A skeletal mesh replaced in memory: its package rewritten (the overlay's copy) under a name of its own, loaded
		// beside the original, and the components that drew the original drawing the copy's mesh
		struct Swap
		{
			std::string owner;
			textures::Replacement replacement;
			Classes::USkeletalMesh* original = nullptr;
			std::size_t originalSlot = 0;
			std::future<std::pair<std::string, fs::path>> build;
			bool done = false;
			Classes::USkeletalMesh* copy = nullptr;
			std::size_t copySlot = 0;
		};
		std::vector<std::unique_ptr<Swap>> g_swaps;		// Game thread

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// The renamed copy of the package holding `r`'s mesh, with the mesh rewritten. "" or the error.
		std::string BuildCopy(const textures::Replacement& r, fs::path& out)
		{
			files::Bypass self;		// The original's bytes, not the overlay's copy of its package
			const std::string packageName = r.path.substr(0, r.path.find('.'));
			fs::path original;
			std::error_code ec;
			for (fs::recursive_directory_iterator it(packages::CookedDir(), ec), end; it != end && !ec; it.increment(ec))
				if (it->is_regular_file(ec) && Lower(it->path().stem().string()) == packageName) {
					original = it->path();
					break;
				}
			if (original.empty()) return "no package " + packageName + " under CookedPC";
			// Named by the converted model, the original package and the builder's version
			constexpr const char* kCopyVersion = "mirage-skeletal-copy 1";
			std::ifstream in(r.file.string() + ".mmesh", std::ios::binary);
			char hash[16];
			std::snprintf(hash, sizeof hash, "%08x",
						  HashBytes(kCopyVersion + std::to_string(fs::file_size(original, ec)) +
									std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())));
			out = paths::Dir() / "models" / (Sanitize(original.stem().string()) + "_" + hash + ".upk");
			if (fs::is_regular_file(out, ec)) return {};
			const auto t0 = std::chrono::steady_clock::now();
			std::lock_guard lock(g_buildLock);
			try {
				const textures::Overlay overlay = textures::BuildOverlay(original, {r});
				if (!overlay.problems.empty()) return overlay.problems.front();
				if (overlay.replaced.empty()) return "no " + r.path + " in " + original.filename().string();
				fs::create_directories(out.parent_path(), ec);
				const fs::path tmp = out.string() + ".tmp";
				std::ofstream(tmp, std::ios::binary)
					.write(reinterpret_cast<const char*>(overlay.bytes.data()), static_cast<std::streamsize>(overlay.bytes.size()));
				fs::rename(tmp, out, ec);
				if (ec) return "couldn't write " + out.string();
			} catch (const std::exception& e) {
				return e.what();
			}
			spdlog::info("Models: {} built for {} in {:.2f} s", out.filename().string(), r.path,
						 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
			return {};
		}

		// Points every component drawing `from` at `to`: a live one through SetSkeletalMesh (its render data made again),
		// a template (a class's default subobject: new pawns copy it) by its property
		int Point(Classes::USkeletalMesh* from, Classes::USkeletalMesh* to)
		{
			int pointed = 0;
			for (auto* c : game::ObjectsOf<Classes::USkeletalMeshComponent>(game::Freshness::Exact)) {
				if (c->SkeletalMesh != from) continue;
				if (c->Owner) {
					mirage::call::Params p(c, "SetSkeletalMesh");
					p.Set("NewMesh", static_cast<Classes::UObject*>(to));
					if (!mirage::call::Call(c, p)) continue;
				}
				else c->SkeletalMesh = to;
				++pointed;
			}
			return pointed;
		}
	}

	void ActivateSkeletal(const std::string& owner, const std::vector<textures::Replacement>& replacements)
	{
		for (const textures::Replacement& r : replacements) {
			if (!textures::IsModelFile(r.file)) continue;
			auto it = std::find_if(g_swaps.begin(), g_swaps.end(),
								   [&](const auto& s) { return s->owner == owner && s->replacement.path == r.path; });
			if (it != g_swaps.end()) {
				// Again after a level load: components the level brought
				if ((*it)->copy && game::AliveAt((*it)->copy, (*it)->copySlot)) Point((*it)->original, (*it)->copy);
				continue;
			}
			Classes::USkeletalMesh* original = nullptr;
			for (auto* m : game::ObjectsOf<Classes::USkeletalMesh>())
				if (Lower(m->GetPathName()) == r.path) original = m;
			if (!original) continue;		// Not in memory: the overlay replaces it when its package opens
			auto s = std::make_unique<Swap>();
			s->owner = owner;
			s->replacement = r;
			s->original = original;
			s->originalSlot = game::IndexOf(original);
			s->build = std::async(std::launch::async, [r] {
				std::pair<std::string, fs::path> result;
				result.first = BuildCopy(r, result.second);
				return result;
			});
			g_swaps.push_back(std::move(s));
		}
	}

	void DeactivateSkeletal(const std::string& owner)
	{
		for (auto& s : g_swaps) {
			if (s->owner != owner) continue;
			if (!s->done) s->build.wait();		// Its files are written either way; nothing was swapped yet
			// Every component drawing the copy, the ones made from a swapped template since (a respawned pawn) too
			if (s->copy && game::AliveAt(s->copy, s->copySlot) && game::AliveAt(s->original, s->originalSlot)) {
				Point(s->copy, s->original);
				classes::Keep(s->copy, false);
			}
		}
		std::erase_if(g_swaps, [&](const auto& s) { return s->owner == owner; });
	}

	void TickSkeletal()
	{
		for (auto& s : g_swaps) {
			if (s->done || s->build.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
			s->done = true;
			const auto [error, package] = s->build.get();
			if (!error.empty()) {
				spdlog::warn("[mod {}] {}: {}", s->owner, s->replacement.path, error);
				continue;
			}
			if (!game::AliveAt(s->original, s->originalSlot)) continue;
			std::string why;
			const std::string inner = s->replacement.path.substr(s->replacement.path.find('.'));
			auto* o = classes::LoadFrom(package, package.stem().string() + inner, Classes::USkeletalMesh::StaticClass(), &why);
			if (!o) {
				spdlog::warn("[mod {}] {}: the engine didn't load {}: {}", s->owner, s->replacement.path, package.filename().string(), why);
				continue;
			}
			s->copy = static_cast<Classes::USkeletalMesh*>(o);
			s->copySlot = game::IndexOf(o);
			// Kept from the garbage collector while swapped in: a level change collected it from under the pawn's arms
			classes::Keep(o, true);
			const int pointed = Point(s->original, s->copy);
			spdlog::info("[mod {}] {} replaced in memory: {} component(s) draw {}", s->owner, s->replacement.path, pointed, o->GetPathName());
		}
	}

	bool SkeletalPending()
	{
		return std::any_of(g_swaps.begin(), g_swaps.end(), [](const auto& s) { return !s->done; });
	}
}
