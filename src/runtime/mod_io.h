#pragma once
// The mod API's game input and files in a mod's folder (mod_io.cpp)
#include "mirage/api.h"

namespace mirage::mods::io
{
	int32_t MIRAGE_CALL InputKeyState(const char* key, MirageKeyState* out);
	void MIRAGE_CALL InputMouse(float window, float* x, float* y);
	int32_t MIRAGE_CALL InputKps();
	double MIRAGE_CALL InputNow();
	void MIRAGE_CALL InputResetCounts();
	int32_t MIRAGE_CALL TasSession();
	int32_t MIRAGE_CALL FileRead(MirageMod* mod, const char* path, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL FileWrite(MirageMod* mod, const char* path, const char* data, uint32_t size);
	uint32_t MIRAGE_CALL FileList(MirageMod* mod, const char* folder, const char* extension, char* buffer, uint32_t capacity);
	uint64_t MIRAGE_CALL FileStamp(MirageMod* mod, const char* path);
	void MIRAGE_CALL FileOpenFolder(MirageMod* mod, const char* folder);
}
