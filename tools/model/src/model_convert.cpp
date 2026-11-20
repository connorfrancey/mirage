#include "model_convert.h"

#include "runtime/mesh_file.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace model
{
	namespace
	{
		// The file bytes of a material's first texture of the first of `types` it has: embedded in the model (a .glb's
		// PNG or JPEG) or a file beside it. Empty if it has none, or only raw texels assimp decoded itself.
		std::string Image(const aiScene& scene, const aiMaterial& material, const std::string& modelPath,
						  std::initializer_list<aiTextureType> types)
		{
			aiString name;
			bool found = false;
			for (const aiTextureType type : types)
				if (!found) found = material.GetTexture(type, 0, &name) == AI_SUCCESS;
			if (!found) return {};
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
		// Triangles only, shared vertices joined, normals and tangents made where the file has none, at most 4 bones a
		// vertex. Node transforms are baked in below (assimp's PreTransformVertices would drop the bones).
		const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
														   aiProcess_CalcTangentSpace | aiProcess_LimitBoneWeights | aiProcess_SortByPType |
														   aiProcess_ValidateDataStructure);
		if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
			return fail(importer.GetErrorString()[0] ? importer.GetErrorString() : "assimp read no scene from it");

		mirage::models::MeshData m;
		m.sourceHash = mirage::models::HashFile(path);
		// The game's space: the file's Y and Z swapped (Z up, left-handed; it also turns the file's counter-clockwise front
		// faces clockwise, as the game draws them), scaled to Unreal units, V flipped (the game's textures start at the top)
		const auto game = [&](const aiVector3D& v, float s) { return aiVector3D(v.x * s, v.z * s, v.y * s); };
		// Every mesh each node draws, with the node's transform from the root (its directions: the inverse transpose)
		std::vector<std::pair<const aiMesh*, aiMatrix4x4>> drawn;
		const auto walk = [&](auto&& self, const aiNode* node, const aiMatrix4x4& parent) -> void {
			const aiMatrix4x4 global = parent * node->mTransformation;
			for (unsigned i = 0; i < node->mNumMeshes; ++i)
				if (node->mMeshes[i] < scene->mNumMeshes) drawn.emplace_back(scene->mMeshes[node->mMeshes[i]], global);
			for (unsigned i = 0; i < node->mNumChildren; ++i) self(self, node->mChildren[i], global);
		};
		walk(walk, scene->mRootNode, aiMatrix4x4());
		for (const auto& [src, transform] : drawn) {
			if (!(src->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
			const aiMatrix3x3 directions = aiMatrix3x3(transform).Inverse().Transpose();
			const auto base = static_cast<std::uint32_t>(m.vertices.size());
			// Its bones' influences on each of its vertices, by the model's bone index
			std::vector<std::vector<std::pair<float, std::uint16_t>>> influences(src->mNumVertices);
			for (unsigned b = 0; b < src->mNumBones; ++b) {
				const aiBone* bone = src->mBones[b];
				const std::string name = bone->mName.C_Str();
				auto at = std::find(m.bones.begin(), m.bones.end(), name);
				if (at == m.bones.end()) at = m.bones.insert(m.bones.end(), name);
				const auto index = static_cast<std::uint16_t>(at - m.bones.begin());
				for (unsigned w = 0; w < bone->mNumWeights; ++w)
					if (bone->mWeights[w].mVertexId < src->mNumVertices && bone->mWeights[w].mWeight > 0.f)
						influences[bone->mWeights[w].mVertexId].emplace_back(bone->mWeights[w].mWeight, index);
			}
			for (unsigned v = 0; v < src->mNumVertices; ++v) {
				mirage::models::MeshVertex out;
				const aiVector3D p = game(transform * src->mVertices[v], scale);
				const aiVector3D n = src->HasNormals() ? game(directions * src->mNormals[v], 1.f).Normalize() : aiVector3D(0, 0, 1);
				aiVector3D t = src->HasTangentsAndBitangents() ? game(directions * src->mTangents[v], 1.f) : aiVector3D(1, 0, 0);
				// The bitangent follows V, which is flipped
				aiVector3D b = src->HasTangentsAndBitangents() ? game(directions * src->mBitangents[v], -1.f) : aiVector3D(0, 1, 0);
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
				// The 4 strongest influences (a static mesh among skinned ones gets none: a vertex without bones)
				auto& in = influences[v];
				std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				mirage::models::MeshSkin skin;
				for (std::size_t k = 0; k < 4 && k < in.size(); ++k) {
					skin.weight[k] = in[k].first;
					skin.bone[k] = in[k].second;
				}
				m.skin.push_back(skin);
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
				section.image = Image(*scene, *material, path, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE});
				section.normal = Image(*scene, *material, path, {aiTextureType_NORMALS});
				// A specular map (an OBJ's map_Ks, an FBX's); glTF has none, its roughness stands in for one
				section.specular = Image(*scene, *material, path, {aiTextureType_SPECULAR});
				if (section.specular.empty()) section.roughness = Image(*scene, *material, path, {aiTextureType_GLTF_METALLIC_ROUGHNESS});
			}
			m.sections.push_back(section);
		}
		if (m.indices.empty()) return fail("no triangles in it");
		if (m.bones.empty()) m.skin.clear();
		else
			for (std::size_t v = 0; v < m.skin.size(); ++v)
				if (m.skin[v].weight[0] <= 0.f)
					return fail("vertex " + std::to_string(v) + " has no bone: every vertex of a skinned model needs one");
		if (stats) {
			stats->vertices = static_cast<unsigned>(m.vertices.size());
			stats->triangles = static_cast<unsigned>(m.indices.size() / 3);
			stats->sections = static_cast<unsigned>(m.sections.size());
			stats->bones = static_cast<unsigned>(m.bones.size());
			for (const auto& s : m.sections) stats->textures += !s.image.empty() + !s.normal.empty() + !s.specular.empty() + !s.roughness.empty();
		}
		return mirage::models::WriteMeshFile(out, m, error);
	}
}
