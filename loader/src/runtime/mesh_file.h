#pragma once
// A converted model (Mirage, step 31): what `mirage model <file> --convert` (tools/model, assimp) writes next to a
// mod's glTF, FBX or OBJ file as `<file>.mmesh`, and what the runtime builds a StaticMesh package from
// (runtime/model_files.h). Header-only and standard C++ only: the tool, a separate build without UnrealLib, includes it.
//
// Already in the game's space: Unreal units, Z up, left-handed (the file's Y and Z swapped, which also makes the front
// faces clockwise, as Unreal draws them), UVs with V down. Little-endian:
//
//   "MMSH" u32 version (4)
//   u32 source hash (HashBytes of the model file it was converted from: a changed model is converted again)
//   u32 vertices, u32 indices (3 a triangle), u32 sections
//   per section: u32 first index, u32 triangles, u16 material name length, the name (UTF-8), u32 image size, the image
//     (the file's diffuse or base-color texture as stored: PNG, JPEG, ...; size 0 for none), u32 normal map size, the
//     normal map (as stored, OpenGL's convention: green is up; size 0 for none), u32 specular map size, the specular
//     map (as stored; size 0 for none), u32 roughness map size, the roughness map (glTF's metallic-roughness image as
//     stored, roughness in its green; size 0 for none, and always 0 when the section has a specular map)
//   per vertex: f32 position[3], f32 normal[3], f32 tangent[4] (w: the binormal's sign, +1 or -1), f32 uv[2]
//   u32 index[indices]

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace mirage::models
{
	struct MeshVertex
	{
		float position[3]{}, normal[3]{}, tangent[4]{}, uv[2]{};
	};
	struct MeshSection
	{
		std::uint32_t firstIndex = 0, triangles = 0;
		std::string material;		// The file's material name ("" if none)
		std::string image;			// Its diffuse texture's file bytes (PNG, JPEG, ...; empty if none)
		std::string normal;			// Its normal map's file bytes (green up, as glTF's; empty if none)
		std::string specular;		// Its specular map's file bytes (empty if none)
		std::string roughness;		// Else its glTF metallic-roughness image's (roughness in green; empty if none)
	};
	struct MeshData
	{
		std::uint32_t sourceHash = 0;
		std::vector<MeshVertex> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<MeshSection> sections;
	};

	inline constexpr std::uint32_t kMeshFileVersion = 4;

	// FNV-1a
	inline std::uint32_t HashBytes(const std::string& bytes)
	{
		std::uint32_t h = 2166136261u;
		for (const char c : bytes) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
		return h;
	}
	// HashBytes of a file's contents (0 if it can't be read)
	inline std::uint32_t HashFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return 0;
		return HashBytes(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
	}

	inline bool WriteMeshFile(const std::string& path, const MeshData& m, std::string* error = nullptr)
	{
		std::ofstream out(path, std::ios::binary);
		if (!out) {
			if (error) *error = "can't write " + path;
			return false;
		}
		const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
		out.write("MMSH", 4);
		u32(kMeshFileVersion);
		u32(m.sourceHash);
		u32(static_cast<std::uint32_t>(m.vertices.size()));
		u32(static_cast<std::uint32_t>(m.indices.size()));
		u32(static_cast<std::uint32_t>(m.sections.size()));
		for (const MeshSection& s : m.sections) {
			u32(s.firstIndex);
			u32(s.triangles);
			const auto n = static_cast<std::uint16_t>(s.material.size() > 0xFFFF ? 0xFFFF : s.material.size());
			out.write(reinterpret_cast<const char*>(&n), 2);
			out.write(s.material.data(), n);
			for (const std::string* file : {&s.image, &s.normal, &s.specular, &s.roughness}) {
				u32(static_cast<std::uint32_t>(file->size()));
				out.write(file->data(), static_cast<std::streamsize>(file->size()));
			}
		}
		static_assert(sizeof(MeshVertex) == 48);
		out.write(reinterpret_cast<const char*>(m.vertices.data()), static_cast<std::streamsize>(m.vertices.size() * sizeof(MeshVertex)));
		out.write(reinterpret_cast<const char*>(m.indices.data()), static_cast<std::streamsize>(m.indices.size() * 4));
		if (!out && error) *error = "writing " + path + " failed";
		return static_cast<bool>(out);
	}

	// False and `error` if it isn't a mesh file of this version, or its counts don't hold (an index past the vertices, a
	// section past the indices, a size that isn't the file's).
	inline bool ReadMeshFile(const std::string& path, MeshData& m, std::string* error = nullptr)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = path + ": " + why;
			return false;
		};
		std::ifstream in(path, std::ios::binary);
		if (!in) return fail("can't open it");
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::size_t at = 0;
		const auto take = [&](void* out, std::size_t n) {
			if (bytes.size() - at < n) return false;
			std::memcpy(out, bytes.data() + at, n);
			at += n;
			return true;
		};
		char magic[4];
		std::uint32_t version = 0, source = 0, vertices = 0, indices = 0, sections = 0;
		if (!take(magic, 4) || std::memcmp(magic, "MMSH", 4) != 0) return fail("not a converted model (MMSH)");
		if (!take(&version, 4) || version != kMeshFileVersion)
			return fail("version " + std::to_string(version) + ", this build reads " + std::to_string(kMeshFileVersion) + ": convert it again");
		if (!take(&source, 4) || !take(&vertices, 4) || !take(&indices, 4) || !take(&sections, 4)) return fail("cut short");
		if (indices % 3 != 0) return fail("an index count that isn't triangles");
		m = {};
		m.sourceHash = source;
		m.sections.resize(sections > 4096 ? 0 : sections);
		if (m.sections.size() != sections) return fail("too many sections");
		for (MeshSection& s : m.sections) {
			std::uint16_t n = 0;
			if (!take(&s.firstIndex, 4) || !take(&s.triangles, 4) || !take(&n, 2)) return fail("cut short");
			s.material.resize(n);
			if (n && !take(&s.material[0], n)) return fail("cut short");
			for (std::string* file : {&s.image, &s.normal, &s.specular, &s.roughness}) {
				std::uint32_t size = 0;
				if (!take(&size, 4) || size > bytes.size() - at) return fail("cut short");
				file->assign(bytes.data() + at, size);
				at += size;
			}
			if (s.firstIndex > indices || s.triangles > (indices - s.firstIndex) / 3) return fail("a section past the indices");
		}
		if (bytes.size() - at != static_cast<std::size_t>(vertices) * sizeof(MeshVertex) + static_cast<std::size_t>(indices) * 4)
			return fail("its size doesn't match its counts");
		m.vertices.resize(vertices);
		m.indices.resize(indices);
		take(m.vertices.data(), m.vertices.size() * sizeof(MeshVertex));
		take(m.indices.data(), m.indices.size() * 4);
		for (const std::uint32_t i : m.indices)
			if (i >= vertices) return fail("an index past the vertices");
		return true;
	}
}
