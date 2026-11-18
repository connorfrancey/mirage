#pragma once
// What a model file holds, as the game would get it (tools/model/CMakeLists.txt): read with assimp, triangulated and
// with identical vertices joined, then checked against the game's static meshes.
#include <string>
#include <vector>

namespace model
{
	struct Mesh
	{
		std::string name, material;
		unsigned vertices = 0, triangles = 0, uvChannels = 0;
		bool normals = false;
	};
	struct Material
	{
		std::string name;
		std::vector<std::string> textures;		// Diffuse / base color, as the file names them
	};
	struct Report
	{
		std::string file, format;
		std::vector<Mesh> meshes;
		std::vector<Material> materials;
		unsigned vertices = 0, triangles = 0, nodes = 0, animations = 0, bones = 0;
		float min[3]{}, max[3]{};				// Bounds of every vertex, in the file's units
		std::vector<std::string> problems;		// What the game's static meshes can't take as it is
		std::string error;						// Why it couldn't be read ("" when it was)
	};

	// The 16-bit index limit of one section of a game static mesh
	inline constexpr unsigned kMaxSectionVertices = 65535;

	Report Read(const std::string& path);
	std::string ToText(const Report& r);
	std::string ToJson(const Report& r);
}
