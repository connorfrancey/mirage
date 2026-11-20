// A converted model built into a package holding one StaticMesh (model_files.h)
#include "runtime/model_files.h"

#include "runtime/texture_files.h"

#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Linker/Linker.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
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
		constexpr std::uint64_t kBodyFlags = 0x0007000000000000ull;		// ... and their RB_BodySetup subobjects
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

		// The mesh's farthest vertex along each of the 26 directions (x, y, z each -1, 0 or +1, not all 0), each place once
		std::vector<Vec3> HullPoints(const StaticMesh& mesh)
		{
			const StaticMeshLod& lod = mesh.lods.front();
			const std::size_t n = std::min(lod.positions.size(), static_cast<std::size_t>(lod.num_vertices));	// Not the shadow copies
			std::vector<std::size_t> picked;
			for (int d = 0; d < 27; ++d) {
				const float dx = static_cast<float>(d % 3 - 1), dy = static_cast<float>(d / 3 % 3 - 1), dz = static_cast<float>(d / 9 - 1);
				if (dx == 0 && dy == 0 && dz == 0) continue;
				std::size_t best = 0;
				float reach = -FLT_MAX;
				for (std::size_t i = 0; i < n; ++i) {
					const Vec3& p = lod.positions[i];
					const float along = p.x * dx + p.y * dy + p.z * dz;
					if (along > reach) reach = along, best = i;
				}
				picked.push_back(best);
			}
			std::vector<Vec3> points;		// Each place once (split vertices share places)
			for (const std::size_t i : picked) {
				const Vec3& p = lod.positions[i];
				if (std::none_of(points.begin(), points.end(), [&](const Vec3& q) { return q.x == p.x && q.y == p.y && q.z == p.z; }))
					points.push_back(p);
			}
			return points;
		}

		// The mesh's rigid body, a subobject of it as the game's are (BodyGeometry; the native data holds no cooked copy,
		// like the game's own: an empty PreCachedPhysData). The export's data, header to native data.
		std::vector<std::byte> BodySetupData(PackageModel& model, Linker& linker, const StaticMesh& mesh, const FName& none,
											 std::int32_t netIndex)
		{
			BinaryWriter w(model.summary.context());
			ObjectHeader header;
			header.net_index = netIndex;
			ObjectHeaderInput input;
			input.object_flags = kBodyFlags;
			write_object_header(w, header, input);
			write_properties(w, {BodyGeometry(model, linker, mesh)}, none);
			w.write_i32(0);		// PreCachedPhysData: none
			return w.take();
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

	Property BodyGeometry(PackageModel& model, Linker& linker, const StaticMesh& mesh)
	{
		ReflectionSchema schema(linker);
		for (const char* name : {"Core", "Engine"})
			if (const Package* p = linker.find_package(name)) schema.add_package(*p);
		const auto type = schema.find_property("RB_BodySetup", "AggGeom");
		if (!type) throw std::runtime_error("RB_BodySetup.AggGeom isn't in the game's Engine package");
		const std::vector<Vec3> hull = HullPoints(mesh);
		std::string corners;
		for (std::size_t i = 0; i < hull.size(); ++i) {
			char v[128];
			std::snprintf(v, sizeof v, "%s(X=%.6f,Y=%.6f,Z=%.6f)", i ? "," : "", hull[i].x, hull[i].y, hull[i].z);
			corners += v;
		}
		PropertyParseOptions options;
		options.make_name = [&](std::string_view s) { return model.make_name(s); };
		options.resolve_object = [](std::string_view) -> std::optional<ObjectIndex> { return std::nullopt; };
		options.schema = &schema;
		Property geometry;
		geometry.name = "AggGeom";
		geometry.value = parse_property_value("(ConvexElems=((VertexData=(" + corners + "))))", *type, options);
		geometry.tag = make_property_tag("AggGeom", 0, *type, options.make_name);
		return geometry;
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
			ReflectionSchema schema(linker);
			for (const char* name : {"Core", "Engine"})
				if (const Package* p = linker.find_package(name)) schema.add_package(*p);

			// The exports: the mesh first, its body, then each textured section's texture and material instance
			ExportEntry e;
			e.class_index = meshClass;
			e.object_name = model.make_name(meshName);
			e.object_flags = kMeshFlags;
			const ObjectIndex index = model.add_export(std::move(e), {});
			ExportEntry body;
			body.class_index = ImportPath(model, "Engine.RB_BodySetup", "Core", "Class");
			body.outer = index;
			body.object_name = model.make_name("RB_BodySetup_0");
			body.object_flags = kBodyFlags;
			const ObjectIndex bodyIndex = model.add_export(std::move(body), {});

			// A section the file gives no game material but textures of its own: a MaterialInstanceConstant of a lit
			// game material (kTexturedParent) with them as its diffuse and normal map
			struct Texture
			{
				ObjectIndex index;
				textures::Encoded encoded;
				std::size_t header = 0;		// The export's bytes before its native data
			};
			std::vector<Texture> textures;
			std::map<std::size_t, ObjectIndex> textured;					// Section -> its material instance
			std::map<std::string, ObjectIndex, std::less<>> refs;		// The object texts the properties below name
			PropertyParseOptions options;
			options.make_name = [&](std::string_view text) { return model.make_name(text); };
			options.resolve_object = [&](std::string_view text) -> std::optional<ObjectIndex> {
				const auto it = refs.find(text);
				return it == refs.end() ? std::nullopt : std::optional<ObjectIndex>(it->second);
			};
			options.schema = &schema;
			const auto property = [&](const char* owner, const char* name, const std::string& text, std::int32_t index = 0) {
				const auto type = schema.find_property(owner, name);
				if (!type) throw std::runtime_error(std::string(owner) + "." + name + " isn't in the game's Engine package");
				Property p;
				p.name = name;
				p.array_index = index;
				p.value = parse_property_value(text, *type, options);
				p.tag = make_property_tag(name, index, *type, options.make_name);
				return p;
			};
			const auto exportData = [&](const std::vector<Property>& properties) {
				BinaryWriter w(model.summary.context());
				ObjectHeader header;
				header.net_index = 0;		// Cooked packages: the export's index
				ObjectHeaderInput input;
				input.object_flags = kMeshFlags;
				write_object_header(w, header, input);
				write_properties(w, properties, none);
				return w.take();
			};
			// A texture export of `file` (the model's bytes) named `name`; a normal map is stored as the game's are
			// (DXT1, not sRGB, TC_Normalmap, unpacked from [0, 1] to [-1, 1])
			const auto addTexture = [&](const std::string& file, const std::string& name, textures::Pixels pixels) {
				const bool normal = pixels == textures::Pixels::FlipGreen;
				Texture t;
				const auto* bytes = reinterpret_cast<const std::byte*>(file.data());
				t.encoded = textures::EncodeImage(std::vector<std::byte>(bytes, bytes + file.size()), file.compare(0, 4, "DDS ") == 0,
												  PixelFormat::DXT1, pixels);
				ExportEntry tex;
				tex.class_index = ImportPath(model, "Engine.Texture2D", "Core", "Class");
				tex.object_name = model.make_name(name);
				tex.object_flags = kMeshFlags;
				t.index = model.add_export(std::move(tex), {});
				const textures::Mip& top = t.encoded.mips.front();
				std::vector<Property> properties = {property("Texture2D", "SizeX", std::to_string(top.width)),
													property("Texture2D", "SizeY", std::to_string(top.height)),
													property("Texture2D", "Format", std::string(to_string(t.encoded.format))),
													property("Texture2D", "MipTailBaseIdx", std::to_string(t.encoded.mips.size() - 1)),
													property("Texture2D", "NeverStream", "True")};
				if (normal) {
					properties.push_back(property("Texture2D", "SRGB", "False"));
					for (std::int32_t i = 0; i < 3; ++i) properties.push_back(property("Texture2D", "UnpackMin", "-1.0", i));
					properties.push_back(property("Texture2D", "CompressionSettings", "TC_Normalmap"));
					properties.push_back(property("Texture2D", "LODGroup", "TEXTUREGROUP_WorldNormalMap"));
				}
				model.export_data[t.index.export_index()] = exportData(properties);
				t.header = model.export_data[t.index.export_index()].size();
				refs["Texture2D'" + name + "'"] = t.index;
				textures.push_back(std::move(t));
				return "Texture2D'" + name + "'";
			};
			for (std::size_t s = 0; s < data.sections.size(); ++s) {
				const MeshSection& section = data.sections[s];
				if ((section.image.empty() && section.normal.empty() && section.specular.empty() && section.roughness.empty()) || !MaterialClass(linker, section.material).empty()) continue;
				if (MaterialClass(linker, kTexturedParent).empty())
					throw std::runtime_error(std::string("the game material textured models use, ") + kTexturedParent + ", wasn't found");
				const std::string suffix = "_" + std::to_string(s);
				std::string values;
				const auto value = [&](const char* parameter, const std::string& texture) {
					values += std::string(values.empty() ? "" : ",") + "(ParameterName=\"" + parameter + "\",ParameterValue=" + texture + ")";
				};
				try {
					using textures::Pixels;
					if (!section.image.empty()) value(kTexturedParameter, addTexture(section.image, meshName + "_Texture" + suffix, Pixels::AsStored));
					if (!section.normal.empty()) value(kNormalParameter, addTexture(section.normal, meshName + "_Normal" + suffix, Pixels::FlipGreen));
					if (!section.specular.empty()) value(kSpecularParameter, addTexture(section.specular, meshName + "_Specular" + suffix, Pixels::AsStored));
					else if (!section.roughness.empty())
						value(kSpecularParameter, addTexture(section.roughness, meshName + "_Specular" + suffix, Pixels::Gloss));
				} catch (const std::exception& ex) {
					throw std::runtime_error("material " + std::to_string(s) + " ('" + section.material + "')'s texture: " + ex.what());
				}
				ExportEntry mic;
				mic.class_index = ImportPath(model, "Engine.MaterialInstanceConstant", "Core", "Class");
				mic.object_name = model.make_name(meshName + "_Material" + suffix);
				mic.object_flags = kMeshFlags;
				const ObjectIndex material = model.add_export(std::move(mic), {});
				const std::string parentText = std::string("Material'") + kTexturedParent + "'";
				refs[parentText] = ImportPath(model, kTexturedParent, "Engine", "Material");
				model.export_data[material.export_index()] =
					exportData({property("MaterialInstanceConstant", "Parent", parentText),
								property("MaterialInstanceConstant", "TextureParameterValues", "(" + values + ")")});
				textured[s] = material;
			}

			// Each section's material: the game material its file names, else its own texture's, else the default
			StaticMesh mesh = MakeStaticMesh(data, meshName, [&](std::size_t s, const std::string& name) {
				if (const auto it = textured.find(s); it != textured.end()) return it->second;
				std::string material = name, cls = MaterialClass(linker, material);
				if (cls.empty()) {
					material = defaultMaterial;
					cls = MaterialClass(linker, material);
					if (cls.empty()) cls = "Material";
				}
				return ImportPath(model, material, "Engine", cls);
			});
			model.export_data[bodyIndex.export_index()] = BodySetupData(model, linker, mesh, none, 1);
			mesh.body_setup = bodyIndex;
			model.summary.generations.back().net_object_count = static_cast<std::int32_t>(model.exports.size());
			// The player and traces collide with the triangles (the kDOP tree), rigid bodies with the body's shape
			const std::vector<std::byte> meshHeader = exportData({BoolProperty(model, "UseSimpleLineCollision", false),
																  BoolProperty(model, "UseSimpleBoxCollision", false),
																  BoolProperty(model, "UseSimpleRigidBodyCollision", true)});
			const SerializationContext ctx = model.summary.context();
			// Inline bulk data stores its package offset: written once to learn where each export lands, then again
			std::vector<std::byte> bytes;
			std::vector<std::uint64_t> bases(model.exports.size(), 0);
			for (int pass = 0; pass < 2; ++pass) {
				auto& meshData = model.export_data[index.export_index()];
				meshData = meshHeader;
				const std::vector<std::byte> payload = write_static_mesh(mesh, ctx, bases[index.export_index()] + meshHeader.size());
				meshData.insert(meshData.end(), payload.begin(), payload.end());
				for (const Texture& t : textures) {
					auto& texData = model.export_data[t.index.export_index()];
					texData.resize(t.header);
					const std::vector<std::byte> native = textures::TextureData(ctx, t.encoded, bases[t.index.export_index()] + t.header);
					texData.insert(texData.end(), native.begin(), native.end());
				}
				bytes = write_package(model);
				Package check;
				check.load_from_memory(std::vector<std::byte>(bytes), packageName);
				for (std::size_t i = 0; i < bases.size(); ++i) bases[i] = static_cast<std::uint64_t>(check.exports()[i].serial_offset);
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
