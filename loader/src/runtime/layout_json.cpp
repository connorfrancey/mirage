// mirage::layout::ToJson / FromJson: layouts saved to a file and read back. The in-game self-test saves what only the
// engine knows (the intrinsic classes no package declares), and the SDK generator reads it (Mirage, step 3).

#include "runtime/layout.h"

#include "util/glz_compat.h"

#include <optional>

namespace mirage::layout
{
	namespace detail	// The file's shape; members left empty (std::nullopt) are not written
	{
		struct FileProperty
		{
			std::string name, kind;
			std::uint32_t offset = 0, size = 0;
			std::optional<std::uint32_t> dim, mask;
			std::uint64_t flags = 0;
			std::optional<std::string> type, inner, innerType;
		};
		struct FileStruct
		{
			std::string path, kind, super;
			std::uint32_t size = 0;
			std::optional<std::uint32_t> functionFlags;
			std::vector<FileProperty> properties;
			std::optional<std::vector<std::string>> values;
		};
	}

	namespace
	{
		Kind KindFromName(const std::string& name)
		{
			for (int k = 0; k <= static_cast<int>(Kind::Unknown); ++k)
				if (name == KindName(static_cast<Kind>(k))) return static_cast<Kind>(k);
			return Kind::Unknown;
		}
	}

	const std::vector<std::string>& SdkPackages()
	{
		// The packages the game loads (the old generated SDK's set, without Editor, UnrealEd and TdEditor: the game
		// loads their classes with the properties stripped, so they hold nothing a mod could use). WinDrv, ALAudio and
		// OnlineSubsystemPC are native only (no .u file: everything about them comes from the engine).
		static const std::vector<std::string> packages{"Core", "Engine", "GameFramework", "IpDrv", "WinDrv", "ALAudio",
			"OnlineSubsystemPC", "Ts", "Tp", "Fp", "TdGame", "TdSharedContent", "TdTTContent", "TdTuContent"};
		return packages;
	}

	std::string ToJson(const Layouts& layouts, const EnumValues& enums)
	{
		std::vector<detail::FileStruct> out;
		out.reserve(layouts.size() + enums.size());
		for (const auto& [path, s] : layouts) {
			detail::FileStruct f{s.path, s.kind, s.super, s.size, {}, {}, {}};
			if (s.functionFlags) f.functionFlags = s.functionFlags;
			for (const Property& p : s.properties) {
				detail::FileProperty q{p.name, KindName(p.kind), p.offset, p.elementSize, {}, {}, p.flags, {}, {}, {}};
				if (p.arrayDim != 1) q.dim = p.arrayDim;
				if (p.boolMask) q.mask = p.boolMask;
				if (!p.typePath.empty()) q.type = p.typePath;
				if (p.innerKind != Kind::Unknown) q.inner = KindName(p.innerKind);
				if (!p.innerPath.empty()) q.innerType = p.innerPath;
				f.properties.push_back(std::move(q));
			}
			out.push_back(std::move(f));
		}
		for (const auto& [path, values] : enums)
			if (!layouts.contains(path))
				out.push_back({path, "Enum", "", 0, {}, {}, values});
		return jc::dump(out, true);
	}

	Layouts FromJson(const std::string& text, std::string* error)
	{
		Layouts out;
		std::vector<detail::FileStruct> file;
		if (const auto ec = glz::read<jc::kLenient>(file, text)) {
			if (error) *error = glz::format_error(ec, text);
			return out;
		}
		for (detail::FileStruct& f : file) {
			Struct s;
			s.path = std::move(f.path);
			s.kind = std::move(f.kind);
			s.super = std::move(f.super);
			s.size = f.size;
			s.functionFlags = f.functionFlags.value_or(0u);
			for (detail::FileProperty& q : f.properties) {
				Property p;
				p.name = std::move(q.name);
				p.kind = KindFromName(q.kind);
				p.offset = q.offset;
				p.elementSize = q.size;
				p.arrayDim = q.dim.value_or(1u);
				p.boolMask = q.mask.value_or(0u);
				p.flags = q.flags;
				p.typePath = q.type.value_or("");
				p.type = p.typePath.substr(p.typePath.find_last_of('.') + 1);
				p.innerKind = KindFromName(q.inner.value_or("Unknown"));
				p.innerPath = q.innerType.value_or("");
				p.innerType = p.innerPath.substr(p.innerPath.find_last_of('.') + 1);
				s.properties.push_back(std::move(p));
			}
			const std::string key = s.path;
			out.emplace(key, std::move(s));
		}
		return out;
	}
}
