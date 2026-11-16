#include "runtime/touch.h"

#include "runtime/manifest.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace mirage::touch
{
	namespace fs = std::filesystem;

	namespace
	{
		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		std::string Relative(const fs::path& p, const fs::path& base)
		{
			std::error_code ec;
			const fs::path r = fs::relative(p, base, ec);
			return (ec ? p : r).generic_string();
		}

		std::string ReadText(const fs::path& p)
		{
			std::ifstream in(p, std::ios::binary);
			std::ostringstream s;
			s << in.rdbuf();
			return s.str();
		}

		// "Class Package.Object" (a full name) or "Package.Object": the path
		std::string ObjectPath(const std::string& key)
		{
			const std::size_t space = key.find(' ');
			return space == std::string::npos ? key : key.substr(space + 1);
		}

		// defaults.toml: every value a table writes, by dotted path (a nested table is a struct's members)
		void Properties(const toml::value& table, const std::string& prefix, const std::string& object, const std::string& where,
						std::vector<Item>& out)
		{
			for (const auto& [key, v] : table.as_table()) {
				if (prefix.empty() && key == "instances") continue;		// Where it applies, not a property
				const std::string path = prefix.empty() ? key : prefix + "." + key;
				if (v.is_table()) Properties(v, path, object, where, out);
				else out.push_back({Kind::Property, object + "." + path, where});
			}
		}

		void Content(const mods::Manifest& m, Set& set)
		{
			for (const std::string& folder : m.content) {
				const fs::path dir = m.dir / folder;
				std::error_code ec;
				const fs::path defaults = dir / "defaults.toml";
				if (fs::is_regular_file(defaults, ec)) {
					try {
						const toml::value root = toml::parse(defaults);
						for (const auto& [object, table] : root.as_table())
							if (table.is_table()) Properties(table, "", ObjectPath(object), Relative(defaults, m.dir), set.items);
					} catch (const std::exception&) {
						// An unreadable file: the content loader reports it
					}
				}
				const fs::path textures = dir / "textures";
				if (fs::is_directory(textures, ec))
					for (const auto& e : fs::directory_iterator(textures, ec)) {
						if (!e.is_regular_file()) continue;
						const std::string ext = Lower(e.path().extension().string());
						if (ext == ".png" || ext == ".tga" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds")
							set.items.push_back({Kind::Texture, e.path().stem().string(), Relative(e.path(), m.dir)});
					}
			}
		}

		void Kismet(const mods::Manifest& m, Set& set)
		{
			std::error_code ec;
			const fs::path dir = m.dir / "kismet";
			if (!fs::is_directory(dir, ec)) return;
			for (const auto& e : fs::directory_iterator(dir, ec)) {
				if (!e.is_regular_file() || Lower(e.path().extension().string()) != ".toml") continue;
				const std::string map = Lower(e.path().stem().string()), where = Relative(e.path(), m.dir);
				try {
					const toml::value root = toml::parse(e.path());
					if (root.contains("set"))
						for (const toml::value& s : toml::find<toml::array>(root, "set")) {
							const std::string op = toml::find_or<std::string>(s, "op", "");
							if (op.empty() || !s.contains("properties")) continue;
							for (const auto& [prop, unused] : toml::find<toml::table>(s, "properties"))
								set.items.push_back({Kind::KismetSet, map + ": " + op + "." + prop, where});
						}
					if (root.contains("unlink"))
						for (const toml::value& u : toml::find<toml::array>(root, "unlink")) {
							const std::string from = toml::find_or<std::string>(u, "from", ""), to = toml::find_or<std::string>(u, "to", "");
							if (!from.empty()) set.items.push_back({Kind::KismetUnlink, map + ": " + from + " -> " + to, where});
						}
				} catch (const std::exception&) {
				}
			}
		}

		// The game functions a source file's API calls name: a string literal, or an identifier assigned one in the file
		// (`local FUNCTION = "TdGame.TdMove_Jump.StartMove"`, `const string F = "..."`)
		void Source(const fs::path& file, const fs::path& base, bool lua, Set& set)
		{
			const std::string text = ReadText(file), where = Relative(file, base);
			std::map<std::string, std::string> constants;
			static const std::regex assign(R"re((?:local\s+|const\s+string\s+|string\s+)?\b([A-Za-z_]\w*)\s*=\s*"([A-Za-z_]\w*(?:\.[A-Za-z_]\w*){2,})")re");
			for (auto it = std::sregex_iterator(text.begin(), text.end(), assign); it != std::sregex_iterator(); ++it)
				constants.emplace((*it)[1].str(), (*it)[2].str());
			struct Call
			{
				std::regex pattern;
				Kind kind;
			};
			static const std::vector<Call> luaCalls = {
				{std::regex(R"re(mirage\.script\.open\s*\(\s*("[^"]+"|[A-Za-z_]\w*))re"), Kind::Patch},
				{std::regex(R"re(mirage\.hook\s*\(\s*("[^"]+"|[A-Za-z_]\w*))re"), Kind::Hook},
				{std::regex(R"re(mirage\.inject\s*\(\s*("[^"]+"|[A-Za-z_]\w*))re"), Kind::Inject},
			};
			static const std::vector<Call> csCalls = {
				{std::regex(R"re(\bHook\s*\(\s*("[^"]+"|[A-Za-z_]\w*))re"), Kind::Hook},
				{std::regex(R"re(\bInject\s*\(\s*("[^"]+"|[A-Za-z_]\w*))re"), Kind::Inject},
			};
			for (const Call& c : lua ? luaCalls : csCalls)
				for (auto it = std::sregex_iterator(text.begin(), text.end(), c.pattern); it != std::sregex_iterator(); ++it) {
					std::string arg = (*it)[1].str();
					if (arg.front() == '"') arg = arg.substr(1, arg.size() - 2);
					else if (const auto k = constants.find(arg); k != constants.end()) arg = k->second;
					else continue;		// Computed at run time: not knowable here
					set.items.push_back({c.kind, arg, where});
				}
		}

		void Sources(const mods::Manifest& m, Set& set)
		{
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(m.dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec) break;
				const std::string name = Lower(it->path().filename().string());
				if (it->is_directory() && (name == "bin" || name == "obj" || name == "build" || name == ".git")) {
					it.disable_recursion_pending();
					continue;
				}
				if (!it->is_regular_file()) continue;
				const std::string ext = Lower(it->path().extension().string());
				if (ext == ".lua") Source(it->path(), m.dir, true, set);
				else if (ext == ".cs") Source(it->path(), m.dir, false, set);
			}
		}
	}

	const char* KindName(Kind k)
	{
		switch (k) {
			case Kind::Property: return "property";
			case Kind::Texture: return "texture";
			case Kind::KismetSet: return "Kismet property";
			case Kind::KismetUnlink: return "Kismet link";
			case Kind::Patch: return "script patch";
			case Kind::Hook: return "hook";
			case Kind::Inject: return "injection";
			case Kind::ActionKey: return "action key";
		}
		return "?";
	}

	Set Read(const mods::Manifest& m)
	{
		Set set;
		set.mod = m.id;
		for (const auto& a : m.actions) set.items.push_back({Kind::ActionKey, Lower(a.key), "mod.toml [actions." + a.name + "]"});
		Content(m, set);
		Kismet(m, set);
		Sources(m, set);
		return set;
	}

	std::vector<Overlap> Overlaps(const std::vector<Set>& sets)
	{
		// Who touches what: by kind and (case-insensitive) name, mods in load order, each once
		struct Users
		{
			std::string what;
			std::vector<std::string> mods;
		};
		std::map<std::pair<Kind, std::string>, Users> by;
		for (const Set& s : sets)
			for (const Item& i : s.items) {
				Users& u = by[{i.kind, Lower(i.what)}];
				if (u.what.empty()) u.what = i.what;
				if (std::find(u.mods.begin(), u.mods.end(), s.mod) == u.mods.end()) u.mods.push_back(s.mod);
			}
		const auto list = [](const std::vector<std::string>& mods) {
			std::string t;
			for (std::size_t i = 0; i < mods.size(); ++i) t += (i ? (i + 1 == mods.size() ? " and " : ", ") : "") + mods[i];
			return t;
		};
		std::vector<Overlap> out;
		for (const auto& [key, u] : by) {
			const Kind kind = key.first;
			if (u.mods.size() < 2) continue;
			Overlap o;
			o.kind = kind;
			o.what = u.what;
			o.mods = u.mods;
			const std::string last = u.mods.back();
			switch (kind) {
				case Kind::Property:
					o.conflict = true;
					o.text = u.what + ": written by " + list(u.mods) + " (" + last + "'s value stays: later in the load order)";
					break;
				case Kind::Texture:
					o.conflict = true;
					o.text = "texture " + u.what + ": replaced by " + list(u.mods) + " (" + last + "'s shows: later in the load order)";
					break;
				case Kind::KismetSet:
					o.conflict = true;
					o.text = "Kismet " + u.what + ": set by " + list(u.mods) + " (" + last + "'s value stays)";
					break;
				case Kind::Patch:
					o.conflict = true;
					o.text = u.what + ": script-patched by " + list(u.mods) + " (each patch rewrites the function: they fight)";
					break;
				case Kind::ActionKey:
					o.text = "key " + u.what + ": an input action of " + list(u.mods) + " (each gets it)";
					break;
				default: continue;		// Hooks, injections and unlinks on their own work together
			}
			out.push_back(std::move(o));
		}
		// A patch and injections into the same function: they work together, the injections over the patched code
		for (const auto& [key, patch] : by) {
			if (key.first != Kind::Patch) continue;
			const auto inj = by.find({Kind::Inject, key.second});
			if (inj == by.end()) continue;
			std::vector<std::string> mods = patch.mods;
			for (const auto& m : inj->second.mods)
				if (std::find(mods.begin(), mods.end(), m) == mods.end()) mods.push_back(m);
			if (mods.size() < 2) continue;
			Overlap o;
			o.kind = Kind::Inject;
			o.what = patch.what;
			o.mods = mods;
			o.text = patch.what + ": script-patched by " + list(patch.mods) + " and injected into by " + list(inj->second.mods) +
					 " (the injections go into the patched code)";
			out.push_back(std::move(o));
		}
		std::stable_sort(out.begin(), out.end(), [](const Overlap& a, const Overlap& b) { return a.conflict > b.conflict; });
		return out;
	}
}
