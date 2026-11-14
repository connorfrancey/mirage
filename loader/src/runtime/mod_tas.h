#pragma once
// The mod API's TAS engine service (mod_tas.cpp)
#include "mirage/api.h"

namespace mirage::mods::tasapi
{
	int32_t MIRAGE_CALL Available();
	uint32_t MIRAGE_CALL Status(char* buffer, uint32_t capacity);
	void MIRAGE_CALL SetOptions(const char* json);
	int32_t MIRAGE_CALL Command(int32_t command, int64_t arg);
	void MIRAGE_CALL Record(int32_t start, const char* command, const char* map, const char* checkpoint);
	uint64_t MIRAGE_CALL ReplayRevision();
	uint32_t MIRAGE_CALL ReplayGet(char* data, uint32_t capacity);
	int32_t MIRAGE_CALL ReplaySet(const char* data, uint32_t size);
	int32_t MIRAGE_CALL File(int32_t save, const char* path, char* error, uint32_t capacity);
	uint32_t MIRAGE_CALL Folder(int32_t autosaves, char* buffer, uint32_t capacity);
	uint32_t MIRAGE_CALL Preflight(char* buffer, uint32_t capacity);
}
