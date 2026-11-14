#include "runtime/class_source.h"

#include "core/clock.h"
#include "paths.h"
#include "runtime/classes.h"
#include "runtime/natives.h"
#include "runtime/packages.h"

#include <UnrealLib/Compiler/Compiler.hpp>
#include <UnrealLib/Decompiler/Decompiler.hpp>
#include <UnrealLib/Decompiler/NativeTable.hpp>
#include <UnrealLib/Linker/Linker.hpp>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cctype>
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
		// The game's script packages, the compiler and the decompiler over them
		struct Model
		{
			UnrealLib::Linker linker;
			std::vector<const UnrealLib::Package*> scripts;
			std::unique_ptr<UnrealLib::Compiler::ScriptCompiler> compiler;
			std::unique_ptr<UnrealLib::Decompiler::ScriptDecompiler> decompiler;
		};
		std::recursive_mutex g_lock;
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
			m->decompiler = std::make_unique<UnrealLib::Decompiler::ScriptDecompiler>(
				m->linker, UnrealLib::Decompiler::NativeTable::from_packages(m->linker, m->scripts));
			g_model = std::move(m);
			return *g_model;
		}

		// Bumped whenever UnrealLib's class builder changes what it writes (packages of older builds are rebuilt)
		constexpr const char* kBuilderVersion = "builder 2 (own component templates);";

		// FNV-1a: the package name follows the source
		std::uint32_t Hash(const std::string& s)
		{
			std::uint32_t h = 2166136261u;
			for (const unsigned char c : s) h = (h ^ c) * 16777619u;
			return h;
		}

		bool IEquals(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size()) return false;
			for (std::size_t i = 0; i < a.size(); ++i)
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
			return true;
		}

		// native("Name") -> native(<index>): the owner's registered native of that name
		bool ExpandNatives(std::string& source, const std::string& owner, std::string* error)
		{
			const std::string key = "native(\"";
			std::vector<natives::Info> list;
			for (std::size_t at = source.find(key); at != std::string::npos; at = source.find(key, at)) {
				const std::size_t end = source.find("\")", at + key.size());
				if (end == std::string::npos) break;
				const std::string name = source.substr(at + key.size(), end - at - key.size());
				if (list.empty()) list = natives::List();
				int index = 0;
				for (const natives::Info& n : list)
					if (n.owner == owner && n.name == name) index = n.index;
				if (!index) {
					if (error) *error = "native(\"" + name + "\"): the mod registered no native of that name (register it before loading the class)";
					return false;
				}
				const std::string with = "native(" + std::to_string(index) + ")";
				source.replace(at, end + 2 - at, with);
				at += with.size();
			}
			return true;
		}

		bool BuildText(const std::string& className, const std::string& sourceName, std::string source, BuiltClass& out,
					   std::string* error, const std::string& owner)
		{
			const auto fail = [&](const std::string& why) {
				if (error) *error = why;
				return false;
			};
			if (!ExpandNatives(source, owner, error)) return false;
			out.className = className;
			// The name follows the source and the builder: a package built by an older builder isn't reused
			const std::string package = fmt::format("{}_{:08x}", out.className, Hash(std::string(kBuilderVersion) + source));
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
				for (const std::string& e : built.errors) all += (all.empty() ? "" : "; ") + sourceName + " " + e;
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
			spdlog::info("Classes: built {} from {} in {:.2f} s ({} bytes)", out.package.filename().string(), sourceName, out.seconds,
						 built.bytes.size());
			return true;
		}

		std::string Trim(std::string t)
		{
			while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
			while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
			return t;
		}
	}

	bool BuildSource(const fs::path& uc, BuiltClass& out, std::string* error, const std::string& owner)
	{
		std::ifstream in(uc, std::ios::binary);
		if (!in) {
			if (error) *error = "can't read " + uc.string();
			return false;
		}
		std::stringstream ss;
		ss << in.rdbuf();
		return BuildText(uc.stem().string(), uc.filename().string(), ss.str(), out, error, owner);
	}

	Classes::UClass* LoadSource(const fs::path& uc, std::string* error, BuiltClass* built, const std::string& owner)
	{
		BuiltClass b;
		if (!BuildSource(uc, b, error, owner)) return nullptr;
		Classes::UClass* cls = LoadClassFrom(b.package, b.className, error);
		if (built) *built = b;
		return cls;
	}

	std::string ParentDeclaration(const std::string& parent, const std::string& function, std::string* error)
	{
		using namespace UnrealLib;
		std::lock_guard lock(g_lock);
		try {
			Model& m = Get();
			// The class, then up its supers: the first function of the name
			std::optional<ObjectRef> cls;
			for (const Package* p : m.scripts)
				for (std::size_t i = 0; i < p->exports().size() && !cls; ++i) {
					const ObjectIndex idx = ObjectIndex::from_export(i);
					if (p->outer(idx).is_null() && p->class_name(idx) == "Class" && IEquals(p->object_name(idx), parent)) cls = ObjectRef{p, idx};
				}
			if (!cls) {
				if (error) *error = "no class " + parent;
				return {};
			}
			for (int guard = 0; cls && guard < 64; ++guard) {
				const FieldObject f = m.linker.read_field(*cls);
				for (ObjectIndex c = f.structure ? f.structure->children : ObjectIndex{}; !c.is_null() && c.is_export();) {
					const ObjectRef child{cls->package, c};
					if (cls->package->class_name(c) == "Function" && IEquals(cls->package->object_name(c), function)) {
						const std::string source = m.decompiler->decompile(child).source;
						std::string declaration = Trim(source.substr(0, source.find('\n')));
						if (declaration.find("final ") != std::string::npos) {
							if (error) *error = function + " is final in " + cls->package->object_name(cls->index) + ": it can't be overridden";
							return {};
						}
						if (!declaration.empty() && declaration.back() == ';') declaration.pop_back();
						// A native's override is a script function
						for (const char* word : {"native ", "noexport "})
							for (std::size_t at = declaration.find(word); at != std::string::npos; at = declaration.find(word))
								declaration.erase(at, std::string(word).size());
						return Trim(declaration);
					}
					c = m.linker.read_field(child).field.next;
				}
				if (f.field.super.is_null()) break;
				const Resolution& r = m.linker.resolve(ObjectRef{cls->package, f.field.super});
				if (r.status != ResolveStatus::Export) break;
				cls = r.target;
			}
			if (error) *error = "no function " + function + " in " + parent + " or the classes above it";
		} catch (const std::exception& e) {
			if (error) *error = std::string("the game's script packages couldn't be read: ") + e.what();
		}
		return {};
	}

	bool GenerateSource(const ClassSpec& spec, std::string& out, std::string* error)
	{
		if (spec.name.empty() || spec.extends.empty()) {
			if (error) *error = "a class needs a name and a class to extend";
			return false;
		}
		std::string s = "// Written by mirage from a mod's class description (mirage.define_class)\n";
		s += "class " + spec.name + " extends " + spec.extends + (spec.placeable ? "\n    placeable" : "") + ";\n\n";
		for (std::string v : spec.vars) {
			v = Trim(v);
			if (!v.empty() && v.back() == ';') v.pop_back();
			s += (v.rfind("var", 0) == 0 && (v.size() > 3 && (v[3] == ' ' || v[3] == '(')) ? v : "var " + v) + ";\n";
		}
		for (const std::string& f : spec.functions) {
			std::string declaration = Trim(f);
			if (declaration.find('(') == std::string::npos) {		// An override: the parent's declaration
				declaration = ParentDeclaration(spec.extends, declaration, error);
				if (declaration.empty()) return false;
			}
			s += "\n" + declaration + "\n{\n}\n";
		}
		if (!spec.extra.empty()) s += "\n" + spec.extra + "\n";
		if (!spec.defaults.empty()) {
			s += "\ndefaultproperties\n{\n";
			for (const std::string& d : spec.defaults) s += "    " + Trim(d) + "\n";
			s += "}\n";
		}
		out = std::move(s);
		return true;
	}

	Classes::UClass* DefineClass(const ClassSpec& spec, const std::string& owner, std::string* error, BuiltClass* built)
	{
		std::string source;
		if (!GenerateSource(spec, source, error)) return nullptr;
		// Written where the mod's author can read it (what the runtime compiled), outside the mod's folder
		std::error_code ec;
		const fs::path dir = paths::Dir() / "classes" / "generated" / (owner.empty() ? "mirage" : owner);
		fs::create_directories(dir, ec);
		{
			std::ofstream file(dir / (spec.name + ".uc"), std::ios::binary | std::ios::trunc);
			file << source;
		}
		BuiltClass b;
		if (!BuildText(spec.name, spec.name + ".uc", source, b, error, owner)) return nullptr;
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
