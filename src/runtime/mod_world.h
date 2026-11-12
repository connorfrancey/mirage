#pragma once
// The mod API's 3D drawing in the world and the camera (mod_world.cpp)
#include "mirage/api.h"

namespace mirage::mods::world
{
	void MIRAGE_CALL DrawActive(MirageMod* mod, int32_t on);
	void MIRAGE_CALL DrawSet(int32_t layer, const MirageWorldMesh* mesh);
	void MIRAGE_CALL DrawStyle(const MirageWorldStyle* style);
	void MIRAGE_CALL DrawStats(MirageWorldStats* out);
	int32_t MIRAGE_CALL WorldToScreen(float x, float y, float z, float* sx, float* sy);
	int32_t MIRAGE_CALL CameraGet(MirageCamera* out);
	void Unloading(MirageMod* mod);		// A mod unloads: its drawing goes with it
}
