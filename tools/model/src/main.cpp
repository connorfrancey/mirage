// mirage_model <file> [--json] [--strict] [--convert [--scale N]]: what a model file holds for the game
// (tools/model/CMakeLists.txt); --convert also writes <file>.mmesh (model_convert.h).
// Exit codes: 0 read (with or without problems), 1 problems with --strict, 2 not readable, not converted or wrong arguments.
#include "model_convert.h"
#include "model_report.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
	std::string file;
	bool json = false, strict = false, convert = false;
	float scale = 50.f;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--json")) json = true;
		else if (!std::strcmp(argv[i], "--strict")) strict = true;
		else if (!std::strcmp(argv[i], "--convert")) convert = true;
		else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = static_cast<float>(std::atof(argv[++i]));
		else if (file.empty()) file = argv[i];
		else {
			std::fprintf(stderr, "mirage_model: one file at a time (got '%s' too)\n", argv[i]);
			return 2;
		}
	}
	if (file.empty()) {
		std::fprintf(stderr, "usage: mirage_model <file.gltf|.glb|.fbx|.obj> [--json] [--strict] [--convert [--scale N]]\n");
		return 2;
	}
	const model::Report r = model::Read(file);
	std::fputs((json ? model::ToJson(r) : model::ToText(r)).c_str(), stdout);
	if (!r.error.empty()) return 2;
	if (convert) {
		if (!(scale > 0.f)) {
			std::fprintf(stderr, "mirage_model: --scale must be above 0\n");
			return 2;
		}
		std::string error;
		model::ConvertStats stats;
		const std::string out = file + ".mmesh";
		if (!model::Convert(file, out, scale, &error, &stats)) {
			std::fprintf(stderr, "mirage_model: not converted: %s\n", error.c_str());
			return 2;
		}
		std::printf("converted: %s (%u vertices, %u triangles, %u section(s), %g game units a file unit)\n", out.c_str(), stats.vertices,
					stats.triangles, stats.sections, scale);
	}
	return strict && !r.problems.empty() ? 1 : 0;
}
