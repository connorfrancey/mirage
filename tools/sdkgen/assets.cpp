// mirage_assets: the game's textures, sounds, meshes, animations and materials, read with UnrealLib's asset readers.
// `mirage assets` builds the catalog once (into build/asset-catalog.sqlite) and browses it in a window; previews are
// made on demand.
//
//   mirage_assets catalog <CookedPC> <out.jsonl>             one JSON object per asset (kind, package, path, class,
//                                                            serialized size, a one-line summary, details, references)
//   mirage_assets preview <CookedPC> <package> <path> <dir>  a texture as PNG (its largest mip up to 1024 pixels), a
//                                                            mesh rendered to PNG, or a sound's file (Ogg); prints
//                                                            the file written
//
// Cooked packages repeat the assets they use (seek-free maps carry copies), so an asset is read once, where it is
// first seen, and its other copies are only counted. A Linker can't drop packages and this process is 32-bit: a fresh
// one is made after every few content packages.
#include <UnrealLib/Assets/Animation.hpp>
#include <UnrealLib/Assets/Material.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/Sound.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Assets/Texture.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Package/Package.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace UnrealLib;
using namespace UnrealLib::Assets;

namespace
{
	constexpr std::size_t kPackagesPerLinker = 6;	// Content packages read before the Linker is made again
	constexpr int kPreviewSize = 1024;

	std::string Json(std::string_view s)
	{
		std::string out = "\"";
		for (const char c : s) {
			switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof buf, "\\u%04x", c);
					out += buf;
				} else {
					out += c;
				}
			}
		}
		return out + "\"";
	}

	std::string Num(double v)
	{
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.6g", v);
		return buf;
	}

	std::string Fmt(const char* format, auto... args)
	{
		char buf[256];
		std::snprintf(buf, sizeof buf, format, args...);
		return buf;
	}

	// What one asset gives the catalog
	struct Entry
	{
		std::string kind, summary;
		std::vector<std::pair<std::string, std::string>> info;	// Shown as a table: name, value (JSON already)
		std::vector<std::string> refs;								// Other objects it uses, qualified paths
		void Add(std::string name, std::string value) { info.emplace_back(std::move(name), Json(value)); }
		void Add(std::string name, double value) { info.emplace_back(std::move(name), Num(value)); }
	};

	// The Linker and the schema that decodes struct arrays (material parameters), made again now and then
	struct Reader
	{
		fs::path cooked;
		std::unique_ptr<Linker> linker;
		std::unique_ptr<ReflectionSchema> schema;
		std::size_t used = 0;

		void Fresh()
		{
			schema.reset();
			linker = std::make_unique<Linker>();
			linker->add_search_path(cooked, true);
			schema = std::make_unique<ReflectionSchema>(*linker);
			for (const char* name : {"Core", "Engine", "GameFramework", "TdGame"})
				if (const Package* s = linker->find_package(name)) schema->add_package(*s);
			used = 0;
		}
	};

	std::string Path(Linker& linker, const Package& p, ObjectIndex index)
	{
		return index.is_null() ? std::string() : linker.qualified_path({&p, index});
	}

	// The asset's own name, the same in every package that has it: a cooked map holds its copies under a forced export
	// (EF_ForcedExport) named after their original package ("Bag.T_Bag_D"); the original package doesn't ("T_Bag_D" in
	// Bag). Groups are Package objects too, so the flag, not the class, tells the two apart.
	std::string FullPath(const Package& p, ObjectIndex idx, const std::string& file, const std::string& path)
	{
		ObjectIndex top = idx;
		while (top.is_export() && p.exports()[top.export_index()].outer.is_export()) top = p.exports()[top.export_index()].outer;
		const bool forced = top != idx && (p.exports()[top.export_index()].export_flags & ExportFlag::ForcedExport) != 0;
		return forced ? path : file + "." + path;
	}

	bool IsMaterialClass(const std::string& c)
	{
		// MaterialInstanceActor is a level actor, not a material
		return c == "Material" || c == "DecalMaterial" || (c.starts_with("MaterialInstance") && !c.ends_with("Actor"));
	}

	bool IsAnimSetClass(const std::string& c) { return c == "AnimSet" || c == "TdAnimSet"; }

	bool Wanted(const std::string& c)
	{
		return is_texture2d_class(c) || c == "TextureCube" || c == "SoundNodeWave" || c == "StaticMesh" ||
			   c == "SkeletalMesh" || c == "AnimSequence" || IsAnimSetClass(c) || IsMaterialClass(c);
	}

	Entry Read(Reader& r, const Package& p, ObjectIndex idx, const std::string& cls)
	{
		Linker& linker = *r.linker;
		const ObjectRef ref{&p, idx};
		Entry e;
		if (is_texture2d_class(cls)) {
			const Texture2D t = read_texture2d(linker, ref, r.schema.get());
			e.kind = "Texture";
			e.summary = Fmt("%dx%d %s, %zu mips", t.size_x, t.size_y, t.format_name.c_str(), t.mips.size());
			e.Add("Width", t.size_x);
			e.Add("Height", t.size_y);
			e.Add("Format", t.format_name);
			e.Add("Mips", static_cast<double>(t.mips.size()));
			e.Add("sRGB", t.srgb ? "yes" : "no");
			e.Add("Compression", t.compression_settings);
			e.Add("LOD group", t.lod_group);
			const auto inl = t.first_inline_mip();
			e.Add("Largest mip in this package", inl ? Fmt("%dx%d", t.mips[*inl].size_x, t.mips[*inl].size_y) : std::string("none"));
			if (!t.storage_package.empty() && t.storage_package != p.name()) e.Add("Mips stored in", t.storage_package);
		} else if (cls == "TextureCube") {
			const TextureCube t = read_texture_cube(linker, ref, r.schema.get());
			e.kind = "Texture";
			e.summary = "cube map";
			static const char* kFaces[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
			for (int i = 0; i < 6; ++i) {
				e.Add(std::string("Face ") + kFaces[i], Path(linker, p, t.faces[i]));
				if (!t.faces[i].is_null()) e.refs.push_back(Path(linker, p, t.faces[i]));
			}
		} else if (cls == "SoundNodeWave") {
			const SoundWave s = read_sound_wave(linker, ref, r.schema.get());
			e.kind = "Sound";
			e.summary = Fmt("%.2f s, %d Hz, %d ch", s.duration, s.sample_rate, s.channels);
			e.Add("Duration (s)", s.duration);
			e.Add("Sample rate", s.sample_rate);
			e.Add("Channels", s.channels);
			e.Add("PC data (bytes)", static_cast<double>(s.compressed_pc.element_count));
			if (!s.spoken_text.empty()) e.Add("Spoken text", s.spoken_text);
		} else if (cls == "StaticMesh") {
			const StaticMesh m = read_static_mesh(linker, ref, r.schema.get());
			e.kind = "Mesh";
			std::size_t tris = 0, verts = 0;
			std::set<std::string> materials;
			if (!m.lods.empty()) {
				verts = m.lods[0].positions.size();
				for (const auto& s : m.lods[0].sections) {
					tris += s.num_triangles;
					if (!s.material.is_null()) materials.insert(Path(linker, p, s.material));
				}
			}
			e.summary = Fmt("static, %zu verts, %zu tris, %zu LODs", verts, tris, m.lods.size());
			e.Add("LODs", static_cast<double>(m.lods.size()));
			e.Add("Vertices (LOD 0)", static_cast<double>(verts));
			e.Add("Triangles (LOD 0)", static_cast<double>(tris));
			e.Add("Sections (LOD 0)", static_cast<double>(m.lods.empty() ? 0 : m.lods[0].sections.size()));
			e.Add("UV sets", m.lods.empty() ? 0 : m.lods[0].num_uv_sets);
			e.Add("Collision triangles", m.kdop_triangle_count);
			e.Add("Bounds radius", m.bounds.sphere_radius);
			e.Add("Bounds extent", Fmt("%.1f x %.1f x %.1f", 2 * m.bounds.box_extent.x, 2 * m.bounds.box_extent.y, 2 * m.bounds.box_extent.z));
			e.refs.assign(materials.begin(), materials.end());
		} else if (cls == "SkeletalMesh") {
			const SkeletalMesh m = read_skeletal_mesh(linker, ref, r.schema.get());
			e.kind = "Mesh";
			const std::size_t verts = m.lods.empty() ? 0 : m.lods[0].num_vertices;
			const std::size_t tris = m.lods.empty() ? 0 : m.lods[0].indices.size() / 3;
			e.summary = Fmt("skeletal, %zu verts, %zu tris, %zu bones", verts, tris, m.skeleton.size());
			e.Add("LODs", static_cast<double>(m.lods.size()));
			e.Add("Vertices (LOD 0)", static_cast<double>(verts));
			e.Add("Triangles (LOD 0)", static_cast<double>(tris));
			e.Add("Bones", static_cast<double>(m.skeleton.size()));
			e.Add("Skeleton depth", m.skeletal_depth);
			e.Add("Bounds radius", m.bounds.sphere_radius);
			std::string bones;
			for (std::size_t i = 0; i < m.skeleton.size(); ++i) bones += (i ? ", " : "") + m.skeleton[i].name;
			e.Add("Bone names", bones);
			for (const ObjectIndex mat : m.materials)
				if (!mat.is_null()) e.refs.push_back(Path(linker, p, mat));
		} else if (cls == "AnimSequence") {
			const AnimSequence a = read_anim_sequence(linker, ref, r.schema.get());
			e.kind = "Animation";
			e.summary = Fmt("%s: %.2f s, %d frames", a.sequence_name.c_str(), a.sequence_length, a.num_frames);
			e.Add("Sequence", a.sequence_name);
			e.Add("Length (s)", a.sequence_length);
			e.Add("Frames", a.num_frames);
			e.Add("Frame rate", a.frame_rate());
			e.Add("Rate scale", a.rate_scale);
			e.Add("Additive", a.additive ? "yes" : "no");
			e.Add("Tracks", static_cast<double>(a.tracks.size()));
			for (const char* name : {"KeyEncodingFormat", "TranslationCompressionFormat", "RotationCompressionFormat"})
				if (const auto v = a.properties.get_name(name)) e.Add(name, *v);
			e.Add("Compressed bytes", static_cast<double>(a.stream.size()));
		} else if (IsAnimSetClass(cls)) {
			const AnimSet a = read_anim_set(linker, ref, r.schema.get());
			e.kind = "Animation";
			e.summary = Fmt("set: %zu sequences, %zu bones", a.sequences.size(), a.track_bone_names.size());
			e.Add("Sequences", static_cast<double>(a.sequences.size()));
			e.Add("Bones", static_cast<double>(a.track_bone_names.size()));
			e.Add("Rotation only", a.rotation_only ? "yes" : "no");
			std::string bones;
			for (std::size_t i = 0; i < a.track_bone_names.size(); ++i) bones += (i ? ", " : "") + a.track_bone_names[i];
			e.Add("Bone names", bones);
			for (const ObjectIndex s : a.sequences)
				if (!s.is_null()) e.refs.push_back(Path(linker, p, s));
		} else {
			const MaterialInfo m = read_material(linker, ref, r.schema.get());
			e.kind = "Material";
			const std::string parent = Path(linker, p, m.parent);
			e.summary = cls == "Material" ? Fmt("%s%s, %zu textures", m.blend_mode.c_str(), m.two_sided ? ", two-sided" : "", m.textures.size())
										  : Fmt("instance, %zu parameters, %zu textures", m.parameters.size(), m.textures.size());
			if (!parent.empty()) {
				e.Add("Parent", parent);
				e.refs.push_back(parent);
			}
			e.Add("Blend mode", m.blend_mode);
			e.Add("Two-sided", m.two_sided ? "yes" : "no");
			e.Add("Expressions", static_cast<double>(m.expressions.size()));
			for (const MaterialParameter& par : m.parameters) {
				std::string value;
				switch (par.kind) {
				case MaterialParameter::Kind::Scalar: value = Num(par.scalar); break;
				case MaterialParameter::Kind::Vector: value = Fmt("(%.3g, %.3g, %.3g, %.3g)", par.vector[0], par.vector[1], par.vector[2], par.vector[3]); break;
				case MaterialParameter::Kind::Texture: value = Path(linker, p, par.texture); break;
				}
				e.Add("Parameter " + par.name, value);
			}
			e.Add("Compiled shader bytes", static_cast<double>(m.native_bytes));
			for (const ObjectIndex t : m.textures)
				if (!t.is_null()) e.refs.push_back(Path(linker, p, t));
		}
		return e;
	}

	int Catalog(const fs::path& cooked, const fs::path& outFile)
	{
		std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
		if (!fs::is_directory(cooked) || !out) {
			std::fprintf(stderr, "mirage_assets: no folder %s, or can't write %s\n", cooked.string().c_str(), outFile.string().c_str());
			return 1;
		}
		std::vector<fs::path> files;
		for (const auto& entry : fs::recursive_directory_iterator(cooked)) {
			const std::string ext = entry.path().extension().string();
			if (entry.is_regular_file() && (ext == ".upk" || ext == ".u" || ext == ".me1")) files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());	// Same order every run: the same copy of an asset is read

		Reader r{cooked};
		r.Fresh();
		std::unordered_map<std::string, std::size_t> seen;	// "class path" -> the row's copies
		std::map<std::string, std::size_t> copies;
		std::size_t assets = 0, failed = 0, packages = 0;
		for (std::size_t n = 0; n < files.size(); ++n) {
			const std::string name = files[n].stem().string();
			try {
				if (r.used >= kPackagesPerLinker) r.Fresh();
				const Package* p = r.linker->find_package(name);
				if (!p) continue;
				bool content = false;
				const auto& exports = p->exports();
				for (std::size_t i = 0; i < exports.size(); ++i) {
					const auto idx = ObjectIndex::from_export(i);
					const std::string cls = p->class_name(idx);
					if (!Wanted(cls)) continue;
					const std::string path = p->object_path(idx);
					if (path.find("Default__") != std::string::npos) continue;
					content = true;
					const std::string full = FullPath(*p, idx, name, path);
					const std::string key = cls + " " + full;
					if (seen.contains(key)) {
						++copies[key];
						continue;
					}
					seen.emplace(key, 0);
					std::string line = "{\"package\":" + Json(name) + ",\"path\":" + Json(path) + ",\"full\":" + Json(full) +
									   ",\"class\":" + Json(cls) + ",\"bytes\":" + std::to_string(exports[i].serial_size);
					try {
						const Entry e = Read(r, *p, idx, cls);
						line += ",\"kind\":" + Json(e.kind) + ",\"summary\":" + Json(e.summary) + ",\"info\":[";
						for (std::size_t k = 0; k < e.info.size(); ++k)
							line += (k ? "," : "") + std::string("[") + Json(e.info[k].first) + "," + e.info[k].second + "]";
						line += "],\"refs\":[";
						for (std::size_t k = 0; k < e.refs.size(); ++k) line += (k ? "," : "") + Json(e.refs[k]);
						line += "]";
					} catch (const std::exception& ex) {
						++failed;	// Still listed, with why its details are missing
						const std::string kind = is_texture2d_class(cls) || cls == "TextureCube" ? "Texture"
											   : cls == "SoundNodeWave"						   ? "Sound"
											   : cls.ends_with("Mesh")							   ? "Mesh"
											   : IsMaterialClass(cls)							   ? "Material"
																								   : "Animation";
						line += ",\"kind\":" + Json(kind) + ",\"summary\":" + Json(std::string("unreadable: ") + ex.what()) +
								",\"info\":[],\"refs\":[]";
					}
					out << line << "}\n";
					++assets;
				}
				if (content && !p->name().empty() && name != "Core" && name != "Engine" && name != "GameFramework" && name != "TdGame")
					++r.used;
				++packages;
			} catch (const std::exception& ex) {
				std::fprintf(stderr, "mirage_assets: %s: %s\n", name.c_str(), ex.what());
				r.Fresh();
			}
			if (n % 100 == 0) std::fprintf(stderr, "mirage_assets: %zu / %zu packages, %zu assets\n", n, files.size(), assets);
		}
		// The copies of each asset, as a last line the loader reads
		out << "{\"copies\":{";
		bool first = true;
		for (const auto& [key, count] : copies) {
			out << (first ? "" : ",") << Json(key) << ":" << count;
			first = false;
		}
		out << "}}\n";
		std::printf("mirage_assets: %zu packages, %zu assets (%zu unreadable)\n", packages, assets, failed);
		return assets ? 0 : 1;
	}

	// A mesh's preview: its triangles from a three-quarter view above, orthographic, shaded by how each faces the light,
	// depth-tested, on a transparent background. Both sides lit (cooked meshes don't say which way they wind).
	Image RenderMesh(const std::vector<Vec3>& pos, const std::vector<std::uint32_t>& indices)
	{
		constexpr int kSize = 512;
		Image image = Image::create(kSize, kSize);
		if (pos.empty() || indices.size() < 3) throw std::runtime_error("the mesh has no triangles");
		auto sub = [](Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; };
		auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
		auto cross = [](Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
		auto norm = [&](Vec3 a) {
			const float l = std::sqrt(dot(a, a));
			return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
		};
		Vec3 lo = pos[0], hi = pos[0];
		for (const Vec3& v : pos) {
			lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
			hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
		}
		const Vec3 center{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
		const Vec3 forward = norm({-1.f, -1.f, -0.7f});		// Looking down at the mesh from a corner; Z is up in UE3
		const Vec3 right = norm(cross(forward, {0, 0, 1}));
		const Vec3 up = cross(right, forward);
		const Vec3 light = norm({-0.4f, 0.3f, 1.f});

		std::vector<std::array<float, 3>> screen(pos.size());	// x, y in pixels, depth
		float extent = 1e-6f;
		for (std::size_t i = 0; i < pos.size(); ++i) {
			const Vec3 d = sub(pos[i], center);
			screen[i] = {dot(d, right), dot(d, up), dot(d, forward)};
			extent = std::max({extent, std::abs(screen[i][0]), std::abs(screen[i][1])});
		}
		const float scale = kSize * 0.45f / extent;
		for (auto& s : screen) s = {kSize / 2.f + s[0] * scale, kSize / 2.f - s[1] * scale, s[2]};

		std::vector<float> depth(static_cast<std::size_t>(kSize) * kSize, std::numeric_limits<float>::max());
		for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
			const std::uint32_t ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
			if (ia >= pos.size() || ib >= pos.size() || ic >= pos.size()) continue;
			const auto& a = screen[ia];
			const auto& b = screen[ib];
			const auto& c = screen[ic];
			const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
			if (std::abs(area) < 1e-9f) continue;
			const float shade = 0.25f + 0.75f * std::abs(dot(norm(cross(sub(pos[ib], pos[ia]), sub(pos[ic], pos[ia]))), light));
			const auto tone = [&](float base) { return static_cast<std::uint8_t>(std::clamp(base * shade, 0.f, 255.f)); };
			const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]}))));
			const int x1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}))));
			const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], c[1]}))));
			const int y1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a[1], b[1], c[1]}))));
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x) {
					const float px = x + 0.5f, py = y + 0.5f;
					const float w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area;
					const float w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area;
					const float w2 = 1.f - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0) continue;
					const float z = w0 * a[2] + w1 * b[2] + w2 * c[2];
					float& d = depth[static_cast<std::size_t>(y) * kSize + x];
					if (z >= d) continue;
					d = z;
					std::uint8_t* pixel = image.at(x, y);
					pixel[0] = tone(200.f);
					pixel[1] = tone(210.f);
					pixel[2] = tone(225.f);
					pixel[3] = 255;
				}
		}
		return image;
	}

	int Preview(const fs::path& cooked, const std::string& package, const std::string& path, const fs::path& dir)
	{
		Reader r{cooked};
		r.Fresh();
		const Package* p = r.linker->find_package(package);
		if (!p) throw std::runtime_error(package + " wasn't found");
		const auto idx = p->find_export(path);
		if (!idx) throw std::runtime_error(path + " isn't an export of " + package);
		const std::string cls = p->class_name(*idx);
		fs::create_directories(dir);
		std::string file = package + "." + path;
		std::replace_if(file.begin(), file.end(), [](char c) { return c == ':' || c == '/' || c == '\\'; }, '_');
		if (is_texture2d_class(cls)) {
			const Texture2D t = read_texture2d(*r.linker, {p, *idx}, r.schema.get());
			// The largest mip that fits the preview and has its pixels somewhere, else the smallest with pixels
			std::optional<std::size_t> pick;
			for (std::size_t i = 0; i < t.mips.size(); ++i) {
				const bool has = t.mips[i].data.has_payload();
				if (has && std::max(t.mips[i].size_x, t.mips[i].size_y) <= kPreviewSize) {
					pick = i;
					break;
				}
				if (has) pick = i;
			}
			if (!pick) throw std::runtime_error("no mip of this texture has pixels");
			const fs::path out = dir / (file + ".png");
			decode_mip(*p, t, *pick, r.linker.get()).save_png(out);
			std::printf("%s\n", out.string().c_str());
			return 0;
		}
		if (cls == "SoundNodeWave") {
			const SoundWave s = read_sound_wave(*r.linker, {p, *idx}, r.schema.get());
			const auto sound = extract_sound(*p, s);
			if (!sound) throw std::runtime_error("the sound has no stored data");
			const fs::path out = dir / (file + "." + std::string(file_extension(sound->container)));
			write_file(out, sound->bytes);
			std::printf("%s\n", out.string().c_str());
			return 0;
		}
		if (cls == "StaticMesh" || cls == "SkeletalMesh") {
			std::vector<Vec3> positions;
			std::vector<std::uint32_t> indices;
			if (cls == "StaticMesh") {
				const StaticMesh m = read_static_mesh(*r.linker, {p, *idx}, r.schema.get());
				if (m.lods.empty()) throw std::runtime_error("the mesh has no LOD");
				positions = m.lods[0].positions;
				indices.assign(m.lods[0].indices.begin(), m.lods[0].indices.end());
			} else {
				const SkeletalMesh m = read_skeletal_mesh(*r.linker, {p, *idx}, r.schema.get());
				if (m.lods.empty()) throw std::runtime_error("the mesh has no LOD");
				for (const SkinnedVertex& v : m.lods[0].vertices) positions.push_back(v.position);
				indices = m.lods[0].indices;
			}
			const fs::path out = dir / (file + ".png");
			RenderMesh(positions, indices).save_png(out);
			std::printf("%s\n", out.string().c_str());
			return 0;
		}
		throw std::runtime_error("no preview for a " + cls);
	}
}

int main(int argc, char** argv)
{
	try {
		const std::string mode = argc > 1 ? argv[1] : "";
		if (mode == "catalog" && argc == 4) return Catalog(argv[2], argv[3]);
		if (mode == "preview" && argc == 6) return Preview(argv[2], argv[3], argv[4], argv[5]);
		std::fprintf(stderr, "usage: mirage_assets catalog <CookedPC> <out.jsonl>\n"
							 "       mirage_assets preview <CookedPC> <package> <path> <out dir>\n");
		return 2;
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "mirage_assets: %s\n", ex.what());
		return 1;
	}
}
