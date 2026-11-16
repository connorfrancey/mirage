#pragma once
// The game side of the mod API (Mirage, step 6): events, function hooks, calls and game objects for MirageApi
// (runtime/mods.cpp puts these functions in the table), and where the menu tells mods what happened.

#include <string>
#include <vector>
#include "mirage/api.h"

#include <string>

namespace mirage::mods::game_api
{
	MirageSubscription* MIRAGE_CALL EventSubscribe(MirageMod* mod, int32_t type, MirageEventFn fn, void* user);
	void MIRAGE_CALL EventUnsubscribe(MirageSubscription* subscription);
	int32_t MIRAGE_CALL NativeRegister(MirageMod* mod, const char* name, const char* signature, MirageNativeFn fn, void* user);
	int32_t MIRAGE_CALL NativeArg(MirageNativeCall* call, int32_t index, MirageValue* out);
	void MIRAGE_CALL NativeReturn(MirageNativeCall* call, const MirageValue* value);
	MirageObject* MIRAGE_CALL NativeSelf(MirageNativeCall* call);
	MirageObject* MIRAGE_CALL ClassLoad(MirageMod* mod, const char* path, char* error, uint32_t capacity);
	MirageObject* MIRAGE_CALL ClassDefine(MirageMod* mod, const char* spec, char* error, uint32_t capacity);
	uint32_t MIRAGE_CALL ObjectPath(MirageObject* object, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL ObjectKeep(MirageMod* mod, MirageObject* object, int32_t keep);
	MirageObject* MIRAGE_CALL LayerActor(MirageMod* mod, const char* modId, const char* name);
	int32_t MIRAGE_CALL ObjectHandle(MirageObject* object, MirageHandle* out);
	MirageObject* MIRAGE_CALL HandleObject(const MirageHandle* handle);
	int32_t MIRAGE_CALL ActorsQuery(const MirageActorQuery* query, MirageObject** out, int32_t capacity);
	int32_t MIRAGE_CALL ChangeBegin(MirageMod* mod, const char* name);
	void MIRAGE_CALL ChangeEnd(MirageMod* mod);
	int32_t MIRAGE_CALL ChangeRevert(MirageMod* mod, int32_t id);
	uint32_t MIRAGE_CALL ChangeList(MirageMod* mod, char* buffer, uint32_t capacity);
	void* MIRAGE_CALL SymbolFind(const char* name);
	MirageDetour* MIRAGE_CALL DetourSymbol(MirageMod* mod, const char* symbol, void* detour, int32_t priority, void** original);
	MirageMidHook* MIRAGE_CALL MidHookCreate(MirageMod* mod, void* at, MirageMidHookFn callback, void* user);
	void MIRAGE_CALL MidHookRemove(MirageMidHook* hook);
	int32_t MIRAGE_CALL TestRegister(MirageMod* mod, const char* name, MirageTestFn fn, void* user);
	int32_t MIRAGE_CALL MovementGet(MirageMovement* out);
	uint32_t MIRAGE_CALL KismetRead(const char* level, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL KismetLink(MirageMod* mod, MirageObject* from, const char* output, MirageObject* to, const char* input);
	int32_t MIRAGE_CALL KismetUnlink(MirageMod* mod, MirageObject* from, const char* output, MirageObject* to, const char* input);
	MirageObject* MIRAGE_CALL KismetCreate(MirageMod* mod, MirageObject* sequence, const char* class_name);
	int32_t MIRAGE_CALL KismetFire(MirageMod* mod, MirageObject* event, const char* output, MirageObject* instigator);
	void MIRAGE_CALL KismetTrace(MirageMod* mod, int32_t on);
	uint32_t MIRAGE_CALL KismetTraceRead(MirageMod* mod, char* buffer, uint32_t capacity);
	uint32_t MIRAGE_CALL MovementTransitions(char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL PhysicsBodyGet(MirageObject* actor, MirageBody* out);
	int32_t MIRAGE_CALL PhysicsBodySet(MirageMod* mod, MirageObject* actor, const MirageBody* in, uint32_t fields);
	int32_t MIRAGE_CALL PhysicsImpulse(MirageMod* mod, MirageObject* actor, const float* impulse, const float* at, int32_t velocityChange);
	int32_t MIRAGE_CALL PhysicsForce(MirageMod* mod, MirageObject* actor, const float* force, const float* at);
	MirageObject* MIRAGE_CALL PhysicsSpawn(MirageMod* mod, const char* mesh, const float* location, const int32_t* rotation, float massScale);
	int32_t MIRAGE_CALL PhysicsCollisionGet(MirageObject* actor);
	int32_t MIRAGE_CALL PhysicsCollisionSet(MirageMod* mod, MirageObject* actor, uint32_t flags);
	int32_t MIRAGE_CALL WorldTrace(MirageMod* mod, const float* start, const float* end, const float* extent, uint32_t flags,
								   MirageObject* ignore, MirageHit* out);
	int32_t MIRAGE_CALL WorldOverlap(const float* center, float radius, const char* className, MirageObject** out, int32_t capacity);
	MirageObject* MIRAGE_CALL PhysicsJoint(MirageMod* mod, MirageObject* a, MirageObject* b, const float* location, const int32_t* rotation,
										   int32_t kind, float breakThreshold);
	MirageObject* MIRAGE_CALL TriggerCreate(MirageMod* mod, const MirageTriggerDesc* desc, MirageTriggerFn fn, void* user);
	int32_t MIRAGE_CALL TriggerCount(MirageObject* trigger);
	MirageObject* MIRAGE_CALL PieceCreate(MirageMod* mod, const MiragePieceDesc* desc);
	// The change log's writer (runtime/changes): wired once at startup
	void WireChanges();
	MirageHook* MIRAGE_CALL InjectFunction(MirageMod* mod, const char* function, const char* point, int32_t priority,
											   MirageHookFn fn, void* user);
	MirageHook* MIRAGE_CALL HookFunction(MirageMod* mod, const char* function, int32_t when, int32_t priority,
										 MirageHookFn fn, void* user);
	void MIRAGE_CALL HookRemove(MirageHook* hook);
	MirageObject* MIRAGE_CALL CallObject(MirageCall* call);
	const char* MIRAGE_CALL CallFunction(MirageCall* call);
	void MIRAGE_CALL CallSkip(MirageCall* call);
	int32_t MIRAGE_CALL CallGet(MirageCall* call, const char* path, MirageValue* out);
	int32_t MIRAGE_CALL CallSet(MirageCall* call, const char* path, const MirageValue* in);
	MirageCall* MIRAGE_CALL CallBegin(MirageObject* object, const char* function);
	int32_t MIRAGE_CALL CallInvoke(MirageCall* call);
	void MIRAGE_CALL CallEnd(MirageCall* call);
	MirageObject* MIRAGE_CALL PlayerPawn();
	MirageObject* MIRAGE_CALL PlayerController();
	MirageObject* MIRAGE_CALL WorldInfo();
	MirageObject* MIRAGE_CALL ObjectFind(const char* fullName);
	int32_t MIRAGE_CALL ObjectGet(MirageObject* object, const char* path, MirageValue* out);
	int32_t MIRAGE_CALL ObjectSet(MirageObject* object, const char* path, const MirageValue* in);
	uint32_t MIRAGE_CALL ObjectName(MirageObject* object, char* buffer, uint32_t capacity);
	uint32_t MIRAGE_CALL ObjectClassName(MirageObject* object, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL ObjectIsA(MirageObject* object, const char* className);
	int32_t MIRAGE_CALL ObjectsOfClass(const char* className, uint32_t flags, MirageObject** out, int32_t capacity);
	void MIRAGE_CALL EventSkip(const MirageEvent* event);
	uint32_t MIRAGE_CALL PlayerStateCapture(char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL PlayerStateApply(const char* state);
	void MIRAGE_CALL PlayerResetMovement();
	int32_t MIRAGE_CALL MenuOpen();
	void MIRAGE_CALL InputKey(int32_t vk, int32_t down);
	void MIRAGE_CALL ShareSet(MirageMod* mod, const char* key, const char* value);
	uint32_t MIRAGE_CALL ShareGet(const char* modId, const char* key, char* buffer, uint32_t capacity);
	MirageDetour* MIRAGE_CALL DetourCreate(MirageMod* mod, void* target, void* detour, int32_t priority, void** original);
	void MIRAGE_CALL DetourRemove(MirageDetour* detour);
	void* MIRAGE_CALL FindPattern(const char* pattern, const char* mask);

	// Script patching (step 8): runtime/mod_script.cpp
	MirageScript* MIRAGE_CALL ScriptOpen(MirageMod* mod, const char* function);
	void MIRAGE_CALL ScriptClose(MirageScript* script);
	int32_t MIRAGE_CALL ScriptCount(MirageScript* script);
	int32_t MIRAGE_CALL ScriptGet(MirageScript* script, int32_t index, MirageToken* out);
	int32_t MIRAGE_CALL ScriptSet(MirageScript* script, int32_t index, const MirageToken* in);
	int32_t MIRAGE_CALL ScriptAdd(MirageScript* script, const MirageToken* in);
	int32_t MIRAGE_CALL ScriptChildren(MirageScript* script, int32_t index, int32_t* out, int32_t capacity);
	int32_t MIRAGE_CALL ScriptSetChildren(MirageScript* script, int32_t index, const int32_t* children, int32_t count);
	int32_t MIRAGE_CALL ScriptApply(MirageScript* script);
	int32_t MIRAGE_CALL ScriptRestore(MirageMod* mod, const char* function);
	uint32_t MIRAGE_CALL ScriptDump(MirageScript* script, char* buffer, uint32_t capacity);
	int32_t MIRAGE_CALL ScriptKind(const char* name);
	const char* MIRAGE_CALL ScriptKindName(int32_t kind);
}

namespace mirage::mods
{
	// A value the menu itself shares with mods (mod id "mirage"), like a mod's share_set. Any thread.
	void Share(const std::string& key, const std::string& value);

	// What happened, to the mods subscribed to it (the menu calls these where the game layer reports it)
	void DispatchTick(float seconds);								// Game thread

	// The mods' own tests (test_register): every one of the mods named (all mods when empty), on the game thread
	struct TestResult
	{
		std::string mod, name;
		bool ok = false;
		std::string message;		// Why it failed (or "didn't run": the mod isn't running)
		double ms = 0.0;
	};
	std::vector<TestResult> RunTests(const std::vector<std::string>& mods);
	void DispatchLevelLoad(bool finished, const std::string& map);	// Game thread
	void DispatchDeath(bool after);									// Game thread
	bool DispatchPlayerState(bool fromHook = true);										// Game thread: reads the move state itself; true: a mod
																	// skipped it (event_skip)
	void DispatchKey(int vk);										// The window's thread
	void DispatchStateRestored();									// Game thread: a TAS snapshot was put back
	void DispatchMemoryPressure(int level);							// Game thread: the memory manager's level changed (or holds high)
	// World lifecycle (step 14; runtime/world_events tells these). Game thread.
	void DispatchActor(bool added, void* actor, const std::string& className);	// actor: a Classes::AActor*
	void DispatchLevelStreamed(bool visible, const std::string& level);
	void DispatchRestart();
	// Whether any mod listens to MIRAGE_EVENT_<type> now (so the work to find out can be skipped). Any thread.
	bool HasSubscribers(int type);
}
