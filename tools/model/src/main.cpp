// mirage_model <file> [--json] [--strict]: what a model file holds for the game (tools/model/CMakeLists.txt).
// Exit codes: 0 read (with or without problems), 1 problems with --strict, 2 not readable or wrong arguments.
#include "model_report.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
	std::string file;
	bool json = false, strict = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--json")) json = true;
		else if (!std::strcmp(argv[i], "--strict")) strict = true;
		else if (file.empty()) file = argv[i];
		else {
			std::fprintf(stderr, "mirage_model: one file at a time (got '%s' too)\n", argv[i]);
			return 2;
		}
	}
	if (file.empty()) {
		std::fprintf(stderr, "usage: mirage_model <file.gltf|.glb|.fbx|.obj> [--json] [--strict]\n");
		return 2;
	}
	const model::Report r = model::Read(file);
	std::fputs((json ? model::ToJson(r) : model::ToText(r)).c_str(), stdout);
	if (!r.error.empty()) return 2;
	return strict && !r.problems.empty() ? 1 : 0;
}
