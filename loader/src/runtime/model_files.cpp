// A converted model built into a package holding one StaticMesh (model_files.h)
#include "runtime/model_files.h"

#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Objects/ObjectHeader.hpp>
#include <UnrealLib/Package/Package.hpp>
#include <UnrealLib/Properties/PropertyTag.hpp>
#include <UnrealLib/Properties/PropertyValue.hpp>
#include <UnrealLib/Serialization/BinaryWriter.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace mirage::models
{
	namespace
	{
		using namespace UnrealLib;
		using namespace UnrealLib::Assets;

		constexpr std::uint64_t kMeshFlags = 0x000F000400000000ull;		// As the game's StaticMesh exports store them
		constexpr std::uint32_t kPackageFlags = 0x00000009u;			// AllowDownload | Cooked
		constexpr std::size_t kMaxVertices = 32767;						// Twice that (the shadow copies) in 16-bit indices

		std::uint32_t Pack(float x, float y, float z, std::uint8_t w)
		{
			const auto c = [](float f) {
				return static_cast<std::uint32_t>(std::lround((std::clamp(f, -1.0f, 1.0f) + 1.0f) * 127.5f));
			};
			return c(x) | (c(y) << 8) | (c(z) << 16) | (static_cast<std::uint32_t>(w) << 24);
		}

		// An import of `path` ("Package.Group.Name", class `cls` of `clsPackage`), its outers as Package imports
		ObjectIndex ImportPath(PackageModel& model, const std::string& path, const std::string& clsPackage, const std::string& cls)
		{
			ObjectIndex outer{};
			std::size_t start = 0;
			for (;;) {
				const std::size_t dot = path.find('.', start);
				const std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
				const bool last = dot == std::string::npos;
				const std::string kind = last ? cls : "Package";
				const std::string kindPackage = last ? clsPackage : "Core";
				if (const auto found = model.find_import(kind, outer, part)) outer = *found;
				else outer = model.add_import(kindPackage, kind, outer, part);
				if (last) return outer;
				start = dot + 1;
			}
		}

		// The class ("Material", "MaterialInstanceConstant") of a game material, empty if there's none at `path`
		std::string MaterialClass(Linker& linker, const std::string& path)
		{
			const std::size_t dot = path.find('.');
			if (dot == std::string::npos) return {};
			try {
				const Package* p = linker.find_package(path.substr(0, dot));
				if (!p) return {};
				const auto e = p->find_export(path.substr(dot + 1));
				if (!e) return {};
				const std::string cls = p->class_name(*e);
				return cls.find("Material") != std::string::npos ? cls : std::string{};
			} catch (const std::exception&) {
				return {};
			}
		}

		Property BoolProperty(PackageModel& model, const char* name, bool value)
		{
			Property p;
			p.name = name;
			p.value.value = value;
			PropertyTag tag;
			tag.name = model.make_name(name);
			tag.type_name = model.make_name("BoolProperty");
			tag.type = PropertyType::Bool;
			tag.bool_value = value;
			p.tag = tag;
			return p;
		}

		// The shadow volume's edges: each pair of vertices once, with the triangle on each side (-1: an open edge)
		std::vector<std::byte> Edges(const std::vector<std::uint16_t>& indices)
		{
			struct Edge { std::int32_t v[2]; std::int32_t f[2]; };
			std::vector<Edge> edges;
			std::map<std::pair<int, int>, std::size_t> open;		// (from, to) -> the edge waiting for its reverse
			for (std::size_t t = 0; t < indices.size() / 3; ++t)
				for (int k = 0; k < 3; ++k) {
					const int a = indices[t * 3 + k], b = indices[t * 3 + (k + 1) % 3];
					if (const auto it = open.find({b, a}); it != open.end() && edges[it->second].f[1] < 0) {
						edges[it->second].f[1] = static_cast<std::int32_t>(t);
						open.erase(it);
						continue;
					}
					open[{a, b}] = edges.size();
					edges.push_back({{a, b}, {static_cast<std::int32_t>(t), -1}});
				}
			std::vector<std::byte> out(edges.size() * sizeof(Edge));
			std::memcpy(out.data(), edges.data(), out.size());
			return out;
		}

		StaticMesh Mesh(const MeshData& data, const std::string& name, const MaterialFor& material)
		{
			const std::size_t n = data.vertices.size();
			StaticMesh mesh;
			mesh.name = name;
			mesh.internal_version = 16;
			mesh.trailing_float = 0.0f;
			StaticMeshLod lod;
			lod.num_vertices = static_cast<std::int32_t>(n);
			lod.num_uv_sets = 2;		// The texture's and the light map's (LightMapCoordinateIndex 1): the same here
			lod.full_precision_uvs = true;
			lod.uvs.assign(2, std::vector<Vec2>(n * 2));
			for (int copy = 0; copy < 2; ++copy)		// The vertices, then their shadow-volume copies
				for (std::size_t i = 0; i < n; ++i) {
					const MeshVertex& v = data.vertices[i];
					lod.positions.push_back({v.position[0], v.position[1], v.position[2]});
					lod.packed_tangents.push_back(Pack(v.tangent[0], v.tangent[1], v.tangent[2], 128));
					lod.packed_normals.push_back(Pack(v.normal[0], v.normal[1], v.normal[2], v.tangent[3] < 0 ? 0 : 255));
					lod.colors.push_back(Color{255, 255, 255, 255});
					lod.shadow_extrusion.push_back(static_cast<float>(copy));
					for (auto& set : lod.uvs) set[copy * n + i] = {v.uv[0], v.uv[1]};
				}
			for (const std::uint32_t i : data.indices) lod.indices.push_back(static_cast<std::uint16_t>(i));
			lod.edges = Edges(lod.indices);
			lod.num_edges = static_cast<std::int32_t>(lod.edges.size() / 16);
			lod.shadow_triangle_double_sided.assign(data.indices.size() / 3, 0);

			std::vector<MeshSection> sections = data.sections;
			if (sections.empty()) sections.push_back({0, static_cast<std::uint32_t>(data.indices.size() / 3), ""});
			for (std::size_t s = 0; s < sections.size(); ++s) {
				const MeshSection& in = sections[s];
				StaticMeshSection out;
				out.material = material(s, in.material);
				out.enable_collision = out.old_enable_collision = out.enable_shadow_casting = true;
				out.first_index = static_cast<std::int32_t>(in.firstIndex);
				out.num_triangles = static_cast<std::int32_t>(in.triangles);
				std::uint32_t lo = UINT32_MAX, hi = 0;
				for (std::uint32_t i = in.firstIndex; i < in.firstIndex + in.triangles * 3; ++i) {
					lo = std::min(lo, data.indices[i]);
					hi = std::max(hi, data.indices[i]);
				}
				out.min_vertex = in.triangles ? static_cast<std::int32_t>(lo) : 0;
				out.max_vertex = in.triangles ? static_cast<std::int32_t>(hi) : 0;
				out.material_index = static_cast<std::int32_t>(s);
				if (in.triangles) out.fragments.push_back({out.first_index, out.num_triangles});
				lod.sections.push_back(out);
			}

			Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
			for (std::size_t i = 0; i < n; ++i) {
				const Vec3& p = lod.positions[i];
				lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
				hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
			}
			mesh.bounds.origin = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
			mesh.bounds.box_extent = {(hi.x - lo.x) / 2, (hi.y - lo.y) / 2, (hi.z - lo.z) / 2};
			float radius = 0;
			for (std::size_t i = 0; i < n; ++i) {
				const Vec3& p = lod.positions[i];
				const float dx = p.x - mesh.bounds.origin.x, dy = p.y - mesh.bounds.origin.y, dz = p.z - mesh.bounds.origin.z;
				radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
			}
			mesh.bounds.sphere_radius = radius;
			mesh.lods.push_back(std::move(lod));
			build_kdop(mesh);
			return mesh;
		}
	}

	UnrealLib::Assets::StaticMesh MakeStaticMesh(const MeshData& data, const std::string& name, const MaterialFor& material)
	{
		if (data.vertices.empty() || data.indices.empty()) throw std::runtime_error("the model has no triangles");
		if (data.vertices.size() > kMaxVertices)
			throw std::runtime_error("the model has " + std::to_string(data.vertices.size()) + " vertices; a game mesh holds at most " +
									 std::to_string(kMaxVertices) + " (split it)");
		return Mesh(data, name, material);
	}

	bool BuildPackage(UnrealLib::Linker& linker, const MeshData& data, const std::string& meshName, const std::filesystem::path& out,
					  std::string* error, const std::string& defaultMaterial)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		try {
			const Package* engine = linker.find_package("Engine");
			if (!engine) return fail("the game's Engine package wasn't found");
			const std::string packageName = out.stem().string();
			PackageModel model;
			model.name = packageName;
			model.summary = engine->summary();
			model.summary.package_flags = kPackageFlags;
			model.summary.name_count = 0;
			model.summary.export_count = 0;
			model.summary.import_count = 0;
			model.summary.generations = {GenerationInfo{}};
			const std::size_t h = std::hash<std::string>{}(packageName);
			model.summary.guid = Guid{static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(h >> 16) ^ 0x4D697261u,
									  static_cast<std::uint32_t>(packageName.size()) * 2654435761u, 0x6853654Du};
			const FName none = model.make_name("None");
			(void)model.make_name(packageName);
			const ObjectIndex meshClass = ImportPath(model, "Engine.StaticMesh", "Core", "Class");

			// Each section's material: the game material its file names, else the default
			StaticMesh mesh = MakeStaticMesh(data, meshName, [&](std::size_t, const std::string& name) {
				std::string material = name, cls = MaterialClass(linker, material);
				if (cls.empty()) {
					material = defaultMaterial;
					cls = MaterialClass(linker, material);
					if (cls.empty()) cls = "Material";
				}
				return ImportPath(model, material, "Engine", cls);
			});
			ExportEntry e;
			e.class_index = meshClass;
			e.object_name = model.make_name(meshName);
			e.object_flags = kMeshFlags;
			const ObjectIndex index = model.add_export(std::move(e), {});
			model.summary.generations.back().net_object_count = 1;
			const std::vector<Property> properties = {BoolProperty(model, "UseSimpleLineCollision", false),
													  BoolProperty(model, "UseSimpleBoxCollision", false),
													  BoolProperty(model, "UseSimpleRigidBodyCollision", false)};
			const SerializationContext ctx = model.summary.context();
			// Inline bulk data stores its package offset: written once to learn where the export lands, then again
			std::vector<std::byte> bytes;
			std::uint64_t base = 0;
			for (int pass = 0; pass < 2; ++pass) {
				BinaryWriter w(ctx);
				ObjectHeader header;
				header.net_index = 0;		// Cooked packages: the export's index
				ObjectHeaderInput input;
				input.object_flags = kMeshFlags;
				write_object_header(w, header, input);
				write_properties(w, properties, none);
				const std::size_t native = w.size();
				const std::vector<std::byte> payload = write_static_mesh(mesh, ctx, base + native);
				w.write_bytes(payload);
				model.export_data[index.export_index()] = w.take();
				bytes = write_package(model);
				Package check;
				check.load_from_memory(std::vector<std::byte>(bytes), packageName);
				base = static_cast<std::uint64_t>(check.exports()[0].serial_offset);
			}
			std::error_code ec;
			std::filesystem::create_directories(out.parent_path(), ec);
			const std::filesystem::path temp = out.string() + ".tmp";
			{
				std::ofstream file(temp, std::ios::binary);
				file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!file) return fail("can't write " + temp.string());
			}
			std::filesystem::rename(temp, out, ec);
			if (ec) return fail("can't write " + out.string() + ": " + ec.message());
			return true;
		} catch (const std::exception& ex) {
			return fail(ex.what());
		}
	}
}
