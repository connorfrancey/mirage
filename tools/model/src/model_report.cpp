#include "model_report.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <sstream>

namespace model
{
	namespace
	{
		void CountNodes(const aiNode* n, unsigned& count)
		{
			if (!n) return;
			++count;
			for (unsigned i = 0; i < n->mNumChildren; ++i) CountNodes(n->mChildren[i], count);
		}

		std::string Escape(const std::string& s)
		{
			std::string out;
			for (const char c : s) {
				if (c == '"' || c == '\\') out += '\\';
				if (static_cast<unsigned char>(c) < 0x20) {
					char hex[8];
					std::snprintf(hex, sizeof hex, "\\u%04x", c);
					out += hex;
				} else out += c;
			}
			return out;
		}
	}

	Report Read(const std::string& path)
	{
		Report r;
		r.file = path;
		Assimp::Importer importer;
		// Triangles only (the game's meshes are triangle lists), shared vertices joined, normals where the file has none
		const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
														   aiProcess_GenSmoothNormals | aiProcess_SortByPType | aiProcess_ValidateDataStructure);
		if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
			r.error = importer.GetErrorString();
			if (r.error.empty()) r.error = "assimp read no scene from it";
			return r;
		}
		const auto dot = path.find_last_of('.');
		r.format = dot == std::string::npos ? std::string() : path.substr(dot + 1);
		CountNodes(scene->mRootNode, r.nodes);
		r.animations = scene->mNumAnimations;

		for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
			const aiMaterial* mat = scene->mMaterials[m];
			Material out;
			out.name = mat->GetName().C_Str();
			for (const aiTextureType type : {aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR}) {
				for (unsigned t = 0; t < mat->GetTextureCount(type); ++t) {
					aiString file;
					if (mat->GetTexture(type, t, &file) == AI_SUCCESS &&
						std::find(out.textures.begin(), out.textures.end(), file.C_Str()) == out.textures.end())
						out.textures.push_back(file.C_Str());
				}
			}
			r.materials.push_back(std::move(out));
		}

		std::fill(std::begin(r.min), std::end(r.min), FLT_MAX);
		std::fill(std::begin(r.max), std::end(r.max), -FLT_MAX);
		unsigned skipped = 0;
		for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
			const aiMesh* src = scene->mMeshes[i];
			if (!(src->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) {		// Points and lines: nothing a mesh can draw
				++skipped;
				continue;
			}
			Mesh m;
			m.name = src->mName.C_Str();
			m.vertices = src->mNumVertices;
			m.triangles = src->mNumFaces;
			m.uvChannels = src->GetNumUVChannels();
			m.normals = src->HasNormals();
			if (src->mMaterialIndex < r.materials.size()) m.material = r.materials[src->mMaterialIndex].name;
			r.bones += src->mNumBones;
			for (unsigned v = 0; v < src->mNumVertices; ++v) {
				const aiVector3D& p = src->mVertices[v];
				const float c[3] = {p.x, p.y, p.z};
				for (int k = 0; k < 3; ++k) {
					r.min[k] = std::min(r.min[k], c[k]);
					r.max[k] = std::max(r.max[k], c[k]);
				}
			}
			r.vertices += m.vertices;
			r.triangles += m.triangles;
			if (m.vertices > kMaxSectionVertices)
				r.problems.push_back("mesh '" + m.name + "' has " + std::to_string(m.vertices) + " vertices: a game mesh section takes " +
									 std::to_string(kMaxSectionVertices) + " (split it)");
			if (m.uvChannels == 0) r.problems.push_back("mesh '" + m.name + "' has no UVs: textures and lightmaps need them");
			r.meshes.push_back(std::move(m));
		}
		if (skipped) r.problems.push_back(std::to_string(skipped) + " mesh(es) of points or lines left out: the game draws triangles");
		if (r.meshes.empty()) {
			r.problems.push_back("no triangle mesh in it");
			std::fill(std::begin(r.min), std::end(r.min), 0.f);
			std::fill(std::begin(r.max), std::end(r.max), 0.f);
		}
		if (r.bones) r.problems.push_back("it has bones (" + std::to_string(r.bones) + "): static meshes don't animate; skeletal meshes aren't supported yet");
		return r;
	}

	std::string ToText(const Report& r)
	{
		std::ostringstream o;
		if (!r.error.empty()) {
			o << r.file << ": can't be read: " << r.error << "\n";
			return o.str();
		}
		o << r.file << " (" << r.format << "): " << r.meshes.size() << " mesh(es), " << r.vertices << " vertices, " << r.triangles
		  << " triangles, " << r.materials.size() << " material(s), " << r.nodes << " node(s)";
		if (r.animations) o << ", " << r.animations << " animation(s)";
		o << "\n  size " << (r.max[0] - r.min[0]) << " x " << (r.max[1] - r.min[1]) << " x " << (r.max[2] - r.min[2])
		  << " (the file's units; the game's are about 2 cm: Faith is ~192 units tall)\n";
		for (const Mesh& m : r.meshes)
			o << "  mesh '" << m.name << "': " << m.vertices << " vertices, " << m.triangles << " triangles, " << m.uvChannels
			  << " UV channel(s)" << (m.material.empty() ? "" : ", material '" + m.material + "'") << "\n";
		for (const Material& m : r.materials) {
			o << "  material '" << m.name << "'";
			for (const auto& t : m.textures) o << " " << t;
			o << "\n";
		}
		if (r.problems.empty()) o << "  ok for a game static mesh\n";
		for (const auto& p : r.problems) o << "  problem: " << p << "\n";
		return o.str();
	}

	std::string ToJson(const Report& r)
	{
		std::ostringstream o;
		o << "{\n  \"file\": \"" << Escape(r.file) << "\",\n";
		if (!r.error.empty()) {
			o << "  \"error\": \"" << Escape(r.error) << "\"\n}\n";
			return o.str();
		}
		o << "  \"format\": \"" << Escape(r.format) << "\",\n  \"vertices\": " << r.vertices << ",\n  \"triangles\": " << r.triangles
		  << ",\n  \"nodes\": " << r.nodes << ",\n  \"animations\": " << r.animations << ",\n  \"bones\": " << r.bones
		  << ",\n  \"min\": [" << r.min[0] << ", " << r.min[1] << ", " << r.min[2] << "],\n  \"max\": [" << r.max[0] << ", "
		  << r.max[1] << ", " << r.max[2] << "],\n  \"meshes\": [";
		for (std::size_t i = 0; i < r.meshes.size(); ++i) {
			const Mesh& m = r.meshes[i];
			o << (i ? ",\n" : "\n") << "    {\"name\": \"" << Escape(m.name) << "\", \"material\": \"" << Escape(m.material)
			  << "\", \"vertices\": " << m.vertices << ", \"triangles\": " << m.triangles << ", \"uvChannels\": " << m.uvChannels
			  << ", \"normals\": " << (m.normals ? "true" : "false") << "}";
		}
		o << "\n  ],\n  \"materials\": [";
		for (std::size_t i = 0; i < r.materials.size(); ++i) {
			o << (i ? ",\n" : "\n") << "    {\"name\": \"" << Escape(r.materials[i].name) << "\", \"textures\": [";
			for (std::size_t t = 0; t < r.materials[i].textures.size(); ++t)
				o << (t ? ", " : "") << "\"" << Escape(r.materials[i].textures[t]) << "\"";
			o << "]}";
		}
		o << "\n  ],\n  \"problems\": [";
		for (std::size_t i = 0; i < r.problems.size(); ++i) o << (i ? ", " : "") << "\"" << Escape(r.problems[i]) << "\"";
		o << "]\n}\n";
		return o.str();
	}
}
