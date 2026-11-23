#include "runtime/manifest.h"

#include "core/enum_names.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace mirage::mods
{
	const char* PhaseName(Phase p) { return enums::Name(p); }
	const char* SettingTypeName(SettingDecl::Type t) { return enums::Name(t); }

	namespace
	{
		// Paths and UTF-8 text (C++20's u8string is std::u8string)
		std::string Utf8(const std::filesystem::path& p)
		{
			const std::u8string s = p.u8string();
			return std::string(s.begin(), s.end());
		}
		std::filesystem::path FromUtf8(const std::string& s)
		{
			return std::filesystem::path(std::u8string(s.begin(), s.end()));
		}

		bool ValidId(std::string_view id, std::size_t minSize = 2)
		{
			if (id.size() < minSize || id.size() > 64) return false;
			for (std::size_t i = 0; i < id.size(); ++i) {
				const char c = id[i];
				const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '-' || c == '_'));
				if (!ok) return false;
			}
			return true;
		}

		bool ValidModId(std::string_view id)
		{
			if (id.size() < 2 || id.size() > 64) return false;
			for (;;) {
				const auto dot = id.find('.');
				const auto part = id.substr(0, dot);
				if (!ValidId(part, 1)) return false;
				if (dot == id.npos) return true;
				id.remove_prefix(dot + 1);
			}
		}

		// A file name inside the mod's folder: relative, no "..", no drive or root
		bool InsideFolder(const std::string& path)
		{
			if (path.empty() || path.find(':') != path.npos ||
				std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32; })) return false;
			const std::filesystem::path p = FromUtf8(path);
			if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
			for (const auto& part : p)
				if (part == "..") return false;
			return true;
		}

		bool EndsWith(const std::string& s, std::string_view suffix)
		{
			if (s.size() < suffix.size()) return false;
			for (std::size_t i = 0; i < suffix.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) return false;
			return true;
		}
		using Table = toml::ordered_value;
		std::optional<double> Number(const Table& n)
		{
			if (n.is_integer()) return static_cast<double>(n.as_integer());
			if (n.is_floating() && std::isfinite(n.as_floating())) return n.as_floating();
			return std::nullopt;
		}

		bool NumericFormat(std::string_view format, bool integer)
		{
			int conversions = 0;
			for (std::size_t i = 0; i < format.size(); ++i) {
				if (format[i] != '%') continue;
				if (++i == format.size()) return false;
				if (format[i] == '%') continue;
				while (i < format.size() && std::string_view("-+ #0").find(format[i]) != format.npos) ++i;
				const auto digits = [&] {
					const auto begin = i;
					while (i < format.size() && format[i] >= '0' && format[i] <= '9') ++i;
					return i - begin <= 2;
				};
				if (!digits()) return false;
				if (i < format.size() && format[i] == '.') {
					++i;
					if (!digits()) return false;
				}
				const std::string_view expected = integer ? "di" : "fFeEgG";
				if (i == format.size() || expected.find(format[i]) == expected.npos || ++conversions != 1) return false;
			}
			return conversions == 1;
		}
		using SectionParser = void (*)(const Table&, Manifest&, Parsed&, const std::string&);

		void ParseSection(const Table& value, const char* name, std::filesystem::path& path, Manifest& m,
						  Parsed& out, const std::string& source, SectionParser parser)
		{
			if (!value.is_string()) return parser(value, m, out, source);
			const std::string file = value.as_string();
			if (!InsideFolder(file) || !EndsWith(file, ".toml")) {
				out.errors.push_back(source + ": " + name + " must reference a TOML file inside the mod folder");
				return;
			}
			path = FromUtf8(file).lexically_normal();
			const std::string local = Utf8(path);
			if (!path.has_parent_path() && ((local.size() == 11 && EndsWith(local, "config.toml")) ||
				(local.size() == 8 && EndsWith(local, "mod.toml"))))
				out.errors.push_back(source + ": " + name + " needs its own TOML file");
		}

		void LoadSection(const std::filesystem::path& path, Manifest& m, Parsed& out, SectionParser parser)
		{
			if (path.empty()) return;
			const auto file = m.dir / path;
			const std::string source = Utf8(file);
			std::error_code ec;
			const auto relative = std::filesystem::relative(file, m.dir, ec);
			if (ec || !InsideFolder(Utf8(relative)) || !std::filesystem::is_regular_file(file, ec) || ec) {
				out.errors.push_back(source + ": expected a regular TOML file inside the mod folder");
				return;
			}
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				out.errors.push_back(source + ": can't be read");
				return;
			}
			auto parsed = toml::try_parse<toml::ordered_type_config>(in, source);
			if (parsed.is_err()) {
				for (const auto& error : parsed.unwrap_err()) out.errors.push_back(toml::format_error(error));
				return;
			}
			parser(parsed.unwrap(), m, out, source);
		}

		void ParseActions(const Table& table, Manifest& m, Parsed& out, const std::string& source)
		{
			const auto error = [&](const std::string& what) { out.errors.push_back(source + ": " + what); };
			if (!table.is_table()) return error("actions must be tables or an external TOML path");
			for (const auto& [id, value] : table.as_table()) {
				const std::string where = "actions." + id;
				if (!ValidId(id)) {
					error(where + ": an action id must be 2–64 characters of a-z, 0-9, '-' or '_'");
					continue;
				}
				ActionDecl action;
				action.name = id;
				if (value.is_string()) action.key = value.as_string();
				else if (value.is_table()) {
					for (const auto& [key, field] : value.as_table()) {
						std::string* target = key == "key" ? &action.key : key == "label" ? &action.label :
							key == "description" ? &action.description : nullptr;
						if (!target) out.warnings.push_back(source + ": " + where + ": unknown key '" + key + "' (ignored)");
						else if (field.is_string()) *target = field.as_string();
						else error(where + "." + key + " must be a string");
					}
				} else {
					error(where + " must be a key string or a table");
					continue;
				}
				if (auto chord = keys::ParseChord(action.key)) {
					action.chord = std::move(*chord);
					m.actions.push_back(std::move(action));
				} else error(where + ".key must be a valid key or chord (such as F6 or Ctrl+F6)");
			}
		}

		void ParseSettings(const Table& table, Manifest& m, Parsed& out, const std::string& source)
		{
			const auto error = [&](const std::string& what) { out.errors.push_back(source + ": " + what); };
			const auto warn = [&](const std::string& what) { out.warnings.push_back(source + ": " + what); };
			if (!table.is_table()) {
				error("settings must be tables or an external TOML path");
				return;
			}
			const Table empty(Table::table_type{});
			for (const auto& [key, declaration] : table.as_table()) {
				const std::string where = "settings." + key;
				if (!ValidId(key) && !(key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))) {
					error(where + ": a setting key is a-z, 0-9, '-' and '_'");
					continue;
				}
				const auto& v = declaration.is_table() ? declaration : empty;
				if (declaration.is_table() && !v.contains("default")) {
					error(where + ".default is required");
					continue;
				}
				const auto& def = declaration.is_table() ? v.at("default") : declaration;
				static const std::set<std::string> settingKeys = {"type", "name", "label", "description", "default", "min",
					"max", "range", "step", "format", "options", "module"};
				for (const auto& [k, unused] : v.as_table())
					if (!settingKeys.count(k)) warn(where + ": unknown key '" + k + "' (ignored)");
				const auto textField = [&](const char* k) -> std::string {
					if (!v.contains(k)) return {};
					if (!v.at(k).is_string()) {
						error(where + "." + k + " must be a string");
						return {};
					}
					return v.at(k).as_string();
				};
				const auto number = [&](const char* k, double& out) {
					if (!v.contains(k)) return false;
					const auto n = Number(v.at(k));
					if (!n) {
						error(where + "." + k + " must be a finite number");
						return false;
					}
					out = *n;
					return true;
				};

				SettingDecl d;
				d.key = key;
				d.name = textField("name");
				if (d.name.empty()) d.name = textField("label");
				if (d.name.empty()) d.name = key;
				d.description = textField("description");
				d.module = textField("module");
				d.format = textField("format");
				const std::string type = textField("type");
				using T = SettingDecl::Type;
				if (type == "enum") d.type = T::Choice;
				else if (type == "keybind") d.type = T::Key;
				else if (const auto t = enums::Parse<T>(type)) d.type = *t;
				else if (!v.contains("type") && v.contains("options")) d.type = T::Choice;
				else if (!v.contains("type") && def.is_boolean()) d.type = T::Bool;
				else if (!v.contains("type") && def.is_integer()) d.type = T::Int;
				else if (!v.contains("type") && def.is_floating()) d.type = T::Float;
				else if (!v.contains("type") && def.is_string()) d.type = T::String;
				else {
					error(where + ".type must be " + enums::List<T>() + ", enum or keybind, not '" + type + "'");
					continue;
				}
				if ((d.type == T::Int || d.type == T::Float)) {
					if (!d.format.empty() && !NumericFormat(d.format, d.type == T::Int))
						error(where + ".format needs one numeric conversion of the setting's type");
					const double limit = d.type == T::Int ? INT32_MAX / 2 : std::numeric_limits<float>::max() / 2.0;
					d.min = -limit;
					d.max = limit;
					if (v.contains("range")) {
						const auto& range = v.at("range");
						if (v.contains("min") || v.contains("max") || !range.is_array() || range.as_array().size() != 2) {
							error(where + ".range must be [min, max] and cannot be combined with min or max");
						} else {
							const auto min = Number(range.as_array()[0]), max = Number(range.as_array()[1]);
							if (!min || !max) error(where + ".range must contain two finite numbers");
							else { d.min = *min; d.max = *max; d.hasRange = true; }
						}
					} else {
						const bool hasMin = number("min", d.min), hasMax = number("max", d.max);
						if (hasMin != hasMax) error(where + ": min and max go together");
						d.hasRange = hasMin && hasMax;
					}
					if (!(d.min < d.max)) error(where + ": min must be below max");
					if (d.min < -limit || d.max > limit ||
						(d.type == T::Int && (std::trunc(d.min) != d.min || std::trunc(d.max) != d.max)) ||
						(d.type == T::Float && static_cast<float>(d.min) == static_cast<float>(d.max)))
						error(where + ": range is outside the supported control limits");
					if (number("step", d.step) && (d.step <= 0 || d.step > limit || d.step > d.max - d.min ||
						(d.type == T::Int && std::trunc(d.step) != d.step) ||
						(d.type == T::Float && static_cast<float>(d.step) == 0)))
						error(where + ".step must be positive and fit the range and setting type");
				} else if (v.contains("min") || v.contains("max") || v.contains("range") || v.contains("step")) {
					warn(where + ": min and max are for int and float settings (ignored)");
				}
				if (v.contains("options") && d.type != T::Choice)
					error(where + ".options requires an enum");
				if (d.type == T::Choice) {
					if (!v.contains("options") || !v.at("options").is_array() || v.at("options").as_array().empty()) {
						error(where + ": a choice setting needs options = [\"...\", ...]");
						continue;
					}
					for (const auto& o : v.at("options").as_array()) {
						if (!o.is_string() || o.as_string().empty()) error(where + ".options must be nonempty strings");
						else if (std::ranges::find(d.options, o.as_string()) != d.options.end())
							error(where + ".options contains a duplicate");
						else d.options.push_back(o.as_string());
					}
				}
				switch (d.type) {
					case T::Bool:
						if (def.is_boolean()) d.boolDefault = def.as_boolean();
						else error(where + ".default must be true or false");
						break;
					case T::Int:
						if (def.is_integer()) d.intDefault = def.as_integer();
						else error(where + ".default must be an integer");
						if (d.intDefault < d.min || d.intDefault > d.max)
							error(where + ".default is outside min..max");
						break;
					case T::Key:
						if (def.is_integer() && def.as_integer() >= 0 && def.as_integer() <= 0xFFFF)
							d.intDefault = def.as_integer();
						else if (def.is_string()) {
							const auto chord = keys::ParseChord(def.as_string());
							if (chord && chord->vk) d.intDefault = chord->vk | chord->modifiers;
							else error(where + ".default must be a keyboard or mouse keybind");
						} else error(where + ".default must be a key code (0 = none) or keybind string");
						break;
					case T::Float:
						if (const auto n = Number(def)) d.floatDefault = *n;
						else error(where + ".default must be a number and finite");
						if (d.floatDefault < d.min || d.floatDefault > d.max)
							error(where + ".default is outside min..max");
						break;
					case T::String:
						if (def.is_string()) d.stringDefault = def.as_string();
						else error(where + ".default must be a string");
						break;
					case T::Color: {
						// "#RRGGBB" or "#RRGGBBAA"
						const std::string hexText = def.is_string() ? def.as_string() : std::string();
						const auto hex = [](char c) -> int {
							return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
						};
						bool ok = (hexText.size() == 7 || hexText.size() == 9) && hexText[0] == '#';
						for (std::size_t i = 1; ok && i < hexText.size(); ++i) ok = hex(hexText[i]) >= 0;
						if (!ok) {
							error(where + ".default must be a color like \"#E5332E\" or \"#E5332ECC\"");
							break;
						}
						for (std::size_t c = 0; c < 4; ++c)
							d.colorDefault[c] = c * 2 + 1 < hexText.size() ? (hex(hexText[c * 2 + 1]) * 16 + hex(hexText[c * 2 + 2])) / 255.f : 1.f;
						break;
					}
					case T::Choice: {
						if (!def.is_string()) {
							error(where + ".default must be one of the options");
							break;
						}
						const auto it = std::find(d.options.begin(), d.options.end(), def.as_string());
						if (it == d.options.end()) error(where + ".default '" + def.as_string() + "' is not one of the options");
						else d.intDefault = it - d.options.begin();
						break;
					}
				}
				if (std::any_of(m.settings.begin(), m.settings.end(), [&](const SettingDecl& o) { return o.name == d.name && o.module == d.module; }))
					error(where + ": another setting of the same module is also named '" + d.name + "'");
				m.settings.push_back(std::move(d));
			}
		}

	}

	Parsed Parse(std::string_view text, const std::string& source)
	{
		Parsed out;
		std::istringstream in{std::string(text)};
		auto parsed = toml::try_parse<toml::ordered_type_config>(in, source);	// Ordered: settings keep the file's order
		if (parsed.is_err()) {
			for (const auto& e : parsed.unwrap_err()) out.errors.push_back(toml::format_error(e));
			return out;
		}
		const toml::ordered_value root = parsed.unwrap();
		if (!root.is_table()) {
			out.errors.push_back(source + ": not a table");
			return out;
		}

		Manifest m;
		const auto error = [&](const std::string& what) { out.errors.push_back(source + ": " + what); };
		const auto warn = [&](const std::string& what) { out.warnings.push_back(source + ": " + what); };
		// A string value, or `fallback` when missing; a wrong type is an error
		const auto str = [&](const char* key, std::string fallback = {}) -> std::string {
			if (!root.contains(key)) return fallback;
			const toml::ordered_value& v = root.at(key);
			if (!v.is_string()) {
				error(std::string(key) + " must be a string");
				return fallback;
			}
			return v.as_string();
		};
		const auto strings = [&](const char* key) {
			std::vector<std::string> list;
			if (!root.contains(key)) return list;
			const toml::ordered_value& v = root.at(key);
			if (!v.is_array()) {
				error(std::string(key) + " must be a list of strings");
				return list;
			}
			for (const toml::ordered_value& e : v.as_array()) {
				if (e.is_string()) list.push_back(e.as_string());
				else error(std::string(key) + " must be a list of strings");
			}
			return list;
		};

		static const std::set<std::string> known = {"id", "name", "version", "description", "authors", "mirage", "game",
			"entry", "lua", "dotnet", "phase", "content", "dependencies", "optional", "incompatible", "settings", "hot_reload",
			"actions", "entrypoints"};
		for (const auto& [key, value] : root.as_table())
			if (!known.count(key)) warn("unknown key '" + key + "' (ignored)");

		m.id = str("id");
		if (m.id.empty()) error("id is required");
		else if (!ValidModId(m.id)) error("id '" + m.id + "' must be 2–64 characters with valid dot-separated components");

		const std::string version = str("version");
		if (version.empty()) {
			error("version is required (like \"1.0.0\")");
		} else {
			std::string why;
			if (auto v = semver::Version::Parse(version, &why)) m.version = *v;
			else error("version: " + why);
		}
		m.name = str("name", m.id);
		m.description = str("description");
		m.game = str("game");
		m.authors = strings("authors");
		if (root.contains("mirage")) {
			std::string why;
			if (auto r = semver::Range::Parse(str("mirage"), &why)) m.mirage = *r;
			else error("mirage: " + why);
		}

		m.entry = str("entry");
		m.lua = str("lua");
		m.dotnet = str("dotnet");
		if (root.contains("entrypoints")) {
			const auto& points = root.at("entrypoints");
			if (!points.is_table()) error("entrypoints must be a table");
			else for (const auto& [key, value] : points.as_table()) {
				const char* legacy = key == "native" ? "entry" : key == "lua" ? "lua" : key == "dotnet" ? "dotnet" : nullptr;
				if (!legacy) error("unknown entrypoints key '" + key + "'");
				else if (root.contains(legacy)) error("entrypoints." + key + " duplicates " + legacy);
				else if (!value.is_string() || value.as_string().empty()) error("entrypoints." + key + " must be a nonempty string");
				else (key == "native" ? m.entry : key == "lua" ? m.lua : m.dotnet) = value.as_string();
			}
		}
		if (!m.entry.empty() && (!InsideFolder(m.entry) || !EndsWith(m.entry, ".dll")))
			error("entry '" + m.entry + "' must be a .dll inside the mod's folder");
		if (!m.dotnet.empty() && (!InsideFolder(m.dotnet) || !(EndsWith(m.dotnet, ".dll") || EndsWith(m.dotnet, ".cs"))))
			error("dotnet '" + m.dotnet + "' must be a .dll (a built assembly) or a .cs file (a script: every .cs in the "
				  "folder is compiled) inside the mod's folder");
		if (!m.dotnet.empty() && !m.entry.empty() && m.dotnet == m.entry)
			error("dotnet and entry name the same file: a C# assembly is not a native DLL");
		if (!m.lua.empty() && (!InsideFolder(m.lua) || !EndsWith(m.lua, ".lua")))
			error("lua '" + m.lua + "' must be a .lua file inside the mod's folder");

		const std::string phase = str("phase", "engine");
		if (const auto p = enums::Parse<Phase>(phase)) m.phase = *p;
		else error("phase '" + phase + "' must be " + enums::List<Phase>());

		m.hotReload = m.entry.empty();
		if (root.contains("hot_reload")) {
			if (root.at("hot_reload").is_boolean()) m.hotReload = root.at("hot_reload").as_boolean();
			else error("hot_reload must be true or false");
		}

		m.content = strings("content");
		for (const std::string& c : m.content)
			if (!InsideFolder(c)) error("content '" + c + "' must be a folder inside the mod's folder");

		std::set<std::string> named;
		const auto deps = [&](const char* table, std::vector<Dependency>& list) {
			if (!root.contains(table)) return;
			const toml::ordered_value& t = root.at(table);
			if (!t.is_table()) {
				error(std::string("[") + table + "] must be a table of id = \"version range\"");
				return;
			}
			for (const auto& [id, value] : t.as_table()) {
				if (!ValidModId(id)) error(std::string("[") + table + "] '" + id + "' is not a mod id");
				if (id == m.id) error(std::string("[") + table + "] names the mod itself");
				if (!named.insert(id).second) error("'" + id + "' is listed twice in the dependencies");
				if (!value.is_string()) {
					error(std::string("[") + table + "] " + id + " must be a version range string (\"*\" for any)");
					continue;
				}
				std::string why;
				if (auto r = semver::Range::Parse(value.as_string(), &why)) list.push_back({id, *r});
				else error(std::string("[") + table + "] " + id + ": " + why);
			}
		};
		deps("dependencies", m.required);
		deps("optional", m.optional);
		deps("incompatible", m.incompatible);

		if (root.contains("actions"))
			ParseSection(root.at("actions"), "actions", m.actionsFile, m, out, source, ParseActions);
		if (root.contains("settings"))
			ParseSection(root.at("settings"), "settings", m.settingsFile, m, out, source, ParseSettings);

		if (out.errors.empty()) out.manifest = std::move(m);
		return out;
	}

	std::vector<std::filesystem::path> FindInstalledMods(const std::filesystem::path& root)
	{
		namespace fs = std::filesystem;
		std::vector<fs::path> mods;
		std::error_code error;
		for (auto entry = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, error);
			 !error && entry != fs::directory_iterator(); entry.increment(error)) {
			std::error_code ignored;
			if (entry->is_directory(ignored) && fs::exists(entry->path() / "mod.toml", ignored))
				mods.push_back(entry->path());
		}
		std::sort(mods.begin(), mods.end());
		return mods;
	}

	Parsed Load(const std::filesystem::path& dir)
	{
		const std::filesystem::path file = dir / "mod.toml";
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			Parsed out;
			out.errors.push_back(Utf8(file) + ": can't be read");
			return out;
		}
		std::stringstream text;
		text << in.rdbuf();
		Parsed out = Parse(text.str(), Utf8(file));
		if (!out.manifest) return out;

		Manifest& m = *out.manifest;
		m.dir = dir;
		LoadSection(m.actionsFile, m, out, ParseActions);
		LoadSection(m.settingsFile, m, out, ParseSettings);
		if (!out.errors.empty()) {
			out.manifest.reset();
			return out;
		}
		const auto fail = [&](const std::string& what) {
			out.errors.push_back(Utf8(file) + ": " + what);
			out.manifest.reset();
		};
		// Its code where a new mod's project has it (src/, `mirage new`) or at the top of its folder: main.lua, else a C#
		// script mod's main.cs (only when nothing else is its code)
		for (const char* code : {"main.lua", "src/main.lua"})
			if (m.lua.empty() && std::filesystem::is_regular_file(dir / code)) m.lua = code;
		for (const char* code : {"main.cs", "src/main.cs"})
			if (m.lua.empty() && m.entry.empty() && m.dotnet.empty() && std::filesystem::is_regular_file(dir / code)) m.dotnet = code;
		if (Utf8(dir.filename()) != m.id) fail("the folder is '" + Utf8(dir.filename()) + "' but the id is '" + m.id + "' (they must match)");
		else if (!m.entry.empty() && !std::filesystem::is_regular_file(dir / FromUtf8(m.entry)))
			fail("entry " + m.entry + " is not in the folder");
		else if (!m.lua.empty() && !std::filesystem::is_regular_file(dir / FromUtf8(m.lua)))
			fail("lua " + m.lua + " is not in the folder");
		else if (!m.dotnet.empty() && !std::filesystem::is_regular_file(dir / FromUtf8(m.dotnet)))
			fail("dotnet " + m.dotnet + " is not in the folder");
		else if (m.entry.empty() && m.lua.empty() && m.dotnet.empty() && m.content.empty() &&
				 !std::filesystem::is_directory(dir / "levels") &&	// Level additions, maps, routes (step 13), Kismet layers and models
				 !std::filesystem::is_directory(dir / "maps") && !std::filesystem::is_directory(dir / "routes") &&	// (step 31, other
				 !std::filesystem::is_directory(dir / "kismet") && !std::filesystem::is_directory(dir / "models"))	// mods' layers use them)
			out.warnings.push_back(Utf8(file) + ": no entry, no Lua, no C# and no content: the mod does nothing");
		return out;
	}

	std::set<std::string> ReadDisabled(const std::filesystem::path& folder)
	{
		std::set<std::string> out;
		const auto file = folder / "disabled.toml";
		std::error_code ec;
		if (!std::filesystem::exists(file, ec)) return out;
		try {
			const toml::value root = toml::parse(file);
			if (root.contains("off") && root.at("off").is_array())
				for (const auto& v : root.at("off").as_array())
					if (v.is_string()) out.insert(v.as_string());
		} catch (const std::exception&) {
		}
		return out;
	}

	bool WriteDisabled(const std::filesystem::path& folder, const std::set<std::string>& ids)
	{
		std::ofstream out(folder / "disabled.toml", std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << "# Mods switched off in the mod manager: not loaded at the next start. Remove an id (or switch it on in\n"
			   "# the menu) to load it again.\n\noff = [";
		bool first = true;
		for (const std::string& id : ids) {
			out << (first ? "" : ", ") << '"' << id << '"';
			first = false;
		}
		out << "]\n";
		return static_cast<bool>(out);
	}
}
