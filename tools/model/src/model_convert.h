#pragma once
// A model file converted for the game (`mirage model <file> --convert`): read with assimp, put in the game's space and
// written as a converted model (loader/src/runtime/mesh_file.h), which the runtime builds a StaticMesh package from.
#include <string>

namespace model
{
	struct ConvertStats
	{
		unsigned vertices = 0, triangles = 0, sections = 0;
	};

	// `scale`: Unreal units per file unit (glTF is in meters; the game's unit is about 2 cm, so 50). False and `error`
	// if it can't be read or written.
	bool Convert(const std::string& path, const std::string& out, float scale, std::string* error, ConvertStats* stats = nullptr);
}
