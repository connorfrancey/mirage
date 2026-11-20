#include "model_convert.h"

#include "runtime/mesh_file.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace model
{
	namespace
	{
		// The file bytes of a material's base-color (glTF) or diffuse texture: embedded in the model (a .glb's PNG or
		// JPEG) or a file beside it. Empty if it has none, or only raw texels assimp decoded itself.
		std::string Image(const aiScene& scene, const aiMaterial& material, const std::string& modelPath)
		{
			aiString name;
			if (material.GetTexture(aiTextureType_BASE_COLOR, 0, &name) != AI_SUCCESS &&
				material.GetTexture(aiTextureType_DIFFUSE, 0, &name) != AI_SUCCESS)
				return {};
			if (const aiTexture* embedded = scene.GetEmbeddedTexture(name.C_Str()))
				return embedded->mHeight == 0 ? std::string(reinterpret_cast<const char*>(embedded->pcData), embedded->mWidth) : std::string{};
			std::ifstream in(std::filesystem::path(modelPath).parent_path() / std::filesystem::u8path(name.C_Str()), std::ios::binary);
			return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string{};
		}
	}

	bool Convert(const std::string& path, const std::string& out, float scale, std::string* error, ConvertStats* stats)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		Assimp::Importer importer;
		// Node transforms baked in (one static mesh), triangles only, shared vertices joined, normals and tangents made
		// where the file has none
		const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
														   aiProcess_CalcTangentSpace | aiProcess_PreTransformVertices | aiProcess_SortByPType |
														   aiProcess_ValidateDataStructure);
		if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
			return fail(importer.GetErrorString()[0] ? importer.GetErrorString() : "assimp read no scene from it");

		mirage::models::MeshData m;
		m.sourceHash = mirage::models::HashFile(path);
		// The game's space: the file's Y and Z swapped (Z up, left-handed; it also turns the file's counter-clockwise front
		// faces clockwise, as the game draws them), scaled to Unreal units, V flipped (the game's textures start at the top)
		const auto game = [&](const aiVector3D& v, float s) { return aiVector3D(v.x * s, v.z * s, v.y * s); };
		for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
			const aiMesh* src = scene->mMeshes[i];
			if (!(src->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
			const auto base = static_cast<std::uint32_t>(m.vertices.size());
			for (unsigned v = 0; v < src->mNumVertices; ++v) {
				mirage::models::MeshVertex out;
				const aiVector3D p = game(src->mVertices[v], scale);
				const aiVector3D n = src->HasNormals() ? game(src->mNormals[v], 1.f).Normalize() : aiVector3D(0, 0, 1);
				aiVector3D t = src->HasTangentsAndBitangents() ? game(src->mTangents[v], 1.f) : aiVector3D(1, 0, 0);
				// The bitangent follows V, which is flipped
				aiVector3D b = src->HasTangentsAndBitangents() ? game(src->mBitangents[v], -1.f) : aiVector3D(0, 1, 0);
				t = (t - n * (t * n));		// Square to the normal
				if (t.SquareLength() < 1e-12f) t = std::fabs(n.x) < 0.9f ? aiVector3D(1, 0, 0) ^ n : aiVector3D(0, 1, 0) ^ n;
				t.Normalize();
				const float sign = ((n ^ t) * b) < 0.f ? -1.f : 1.f;
				const float pc[3] = {p.x, p.y, p.z}, nc[3] = {n.x, n.y, n.z}, tc[3] = {t.x, t.y, t.z};
				for (int k = 0; k < 3; ++k) {
					out.position[k] = pc[k];
					out.normal[k] = nc[k];
					out.tangent[k] = tc[k];
				}
				out.tangent[3] = sign;
				if (src->HasTextureCoords(0)) {
					out.uv[0] = src->mTextureCoords[0][v].x;
					out.uv[1] = 1.f - src->mTextureCoords[0][v].y;
				}
				m.vertices.push_back(out);
			}
			mirage::models::MeshSection section;
			section.firstIndex = static_cast<std::uint32_t>(m.indices.size());
			for (unsigned f = 0; f < src->mNumFaces; ++f) {
				const aiFace& face = src->mFaces[f];
				if (face.mNumIndices != 3) continue;
				for (unsigned k = 0; k < 3; ++k) m.indices.push_back(base + face.mIndices[k]);
				++section.triangles;
			}
			if (src->mMaterialIndex < scene->mNumMaterials) {
				const aiMaterial* material = scene->mMaterials[src->mMaterialIndex];
				section.material = material->GetName().C_Str();
				section.image = Image(*scene, *material, path);
			}
			m.sections.push_back(section);
		}
		if (m.indices.empty()) return fail("no triangles in it");
		if (stats) {
			stats->vertices = static_cast<unsigned>(m.vertices.size());
			stats->triangles = static_cast<unsigned>(m.indices.size() / 3);
			stats->sections = static_cast<unsigned>(m.sections.size());
			for (const auto& s : m.sections) stats->textures += !s.image.empty();
		}
		return mirage::models::WriteMeshFile(out, m, error);
	}
}
