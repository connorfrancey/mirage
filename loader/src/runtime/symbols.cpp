#include "runtime/symbols.h"

#include "core/address_cache.h"

#include <MirageHook/scanner/ModuleScanner.hpp>
#include <MirageHook/scanner/Signature.hpp>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>

namespace mirage::symbols
{
	namespace
	{
		// symbols.toml as text (generated from runtime/symbols.toml by the build)
		constexpr std::string_view kBuiltIn =
#include "symbols_toml.inc"
			;

		std::mutex g_lock;
		std::map<std::string, Info, std::less<>> g_symbols;
		bool g_resolved = false;

		// A resolve step onto a signature: "offset 2", "deref", "rel32 1"
		bool AddStep(MirageHook::Signature& sig, const std::string& step, std::string& why)
		{
			std::istringstream in(step);
			std::string word;
			in >> word;
			long long n = 0;
			const bool hasNumber = static_cast<bool>(in >> n);
			if (word == "offset" && hasNumber) sig.offset(static_cast<std::ptrdiff_t>(n));
			else if (word == "deref" && !hasNumber) sig.deref();
			else if (word == "rel32") sig.follow_rel32(hasNumber ? static_cast<std::ptrdiff_t>(n) : 1);
			else {
				why = "unknown resolve step \"" + step + "\" (offset n, deref, rel32 n)";
				return false;
			}
			return true;
		}
	}

	std::string_view BuiltIn() { return kBuiltIn; }

	std::vector<Entry> Parse(std::string_view text, std::string* error)
	{
		std::vector<Entry> out;
		try {
			std::istringstream in{std::string(text)};
			const toml::value root = toml::parse(in, "symbols.toml");
			for (const auto& [name, v] : root.as_table()) {
				Entry e;
				e.name = name;
				e.kind = toml::find_or<std::string>(v, "kind", "code");
				e.pattern = toml::find_or<std::string>(v, "pattern", "");
				e.description = toml::find_or<std::string>(v, "description", "");
				e.resolve = toml::find_or<std::vector<std::string>>(v, "resolve", {});
				e.matches = toml::find_or<int>(v, "matches", 1);
				out.push_back(std::move(e));
			}
		} catch (const std::exception& ex) {
			if (error) *error = ex.what();
			out.clear();
		}
		return out;
	}

	int Resolve()
	{
		std::lock_guard lock(g_lock);
		if (g_resolved) {
			return static_cast<int>(std::count_if(g_symbols.begin(), g_symbols.end(),
												  [](const auto& kv) { return kv.second.address != 0; }));
		}
		g_resolved = true;
		const auto start = std::chrono::steady_clock::now();
		std::string error;
		const std::vector<Entry> entries = Parse(kBuiltIn, &error);
		if (!error.empty()) spdlog::error("Symbols: the built-in symbols.toml can't be read: {}", error);
		auto scanner = MirageHook::ModuleScanner::open("");		// The game's executable
		if (!scanner) {
			spdlog::error("Symbols: the game's executable can't be scanned: {}", scanner.error().message());
			return 0;
		}
		int found = 0, ambiguous = 0;
		for (const Entry& e : entries) {
			Info info;
			info.name = e.name;
			info.kind = e.kind;
			info.description = e.description;
			info.expected = e.matches;
			auto sig = MirageHook::Signature::parse(e.name, e.pattern);
			if (!sig) {
				info.problem = "its pattern can't be read: " + sig.error().message();
			} else {
				std::string why;
				bool stepsOk = true;
				for (const std::string& step : e.resolve) stepsOk = stepsOk && AddStep(*sig, step, why);
				if (!stepsOk) info.problem = why;
				else if (auto all = scanner->find_all(*sig); !all) {
					info.problem = all.error().message();
				} else if (all->empty()) {
					info.problem = "not found (another version of the game?)";
				} else {
					info.matches = static_cast<int>(all->size());
					info.address = all->front().value();
					if (static_cast<int>(all->size()) != e.matches) {
						info.problem = std::to_string(all->size()) + " places match, not " + std::to_string(e.matches) +
									   ": the first is used";
						++ambiguous;
					}
				}
			}
			if (info.address) {
				++found;
				addr::Game().Set(info.name, info.address);
			} else {
				spdlog::error("Symbols: {}: {}", info.name, info.problem);
			}
			g_symbols[info.name] = std::move(info);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		spdlog::info("Symbols: {} of {} found in {:.0f} ms{}", found, entries.size(), ms,
					 ambiguous ? fmt::format(" ({} match in another number of places than the table says)", ambiguous) : std::string());
		return found;
	}

	void Provide(std::string_view name, std::uintptr_t address, std::string_view kind, std::string_view description)
	{
		{
			std::lock_guard lock(g_lock);
			Info& info = g_symbols[std::string(name)];
			info.name = name;
			info.kind = kind;
			if (!description.empty()) info.description = description;
			info.address = address;
			info.matches = address ? 1 : 0;
			info.provided = true;
			info.problem = address ? std::string() : "its code didn't find it";
		}
		if (address) addr::Game().Set(std::string(name), address);
	}

	std::uintptr_t Find(std::string_view name)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_symbols.find(name);
		return it == g_symbols.end() ? 0 : it->second.address;
	}

	std::vector<Info> List()
	{
		std::lock_guard lock(g_lock);
		std::vector<Info> out;
		for (const auto& [name, info] : g_symbols) out.push_back(info);
		return out;
	}

	std::string NameOf(std::uintptr_t address)
	{
		std::lock_guard lock(g_lock);
		for (const auto& [name, info] : g_symbols)
			if (info.address == address && address) return name;
		return {};
	}
}
