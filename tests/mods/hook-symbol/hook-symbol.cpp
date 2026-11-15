// Self-test mod (step 2 of the next ten steps): while its module is on, it detours the engine's StaticLoadObject by
// symbol name (mirage::DetourSymbol) and counts the calls. With its "Crash" setting on, the next call faults inside the
// detour: the runtime's guard must let the load go on in the real function, and stop and name this mod.
// It also hooks an instruction inside StaticLoadObject (a mid-function hook, mirage::MidHook) where the function reads
// its Name argument, and counts the loads of "Engine.Actor" from the registers.
#include <mirage/mirage.hpp>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{
	using LoadFn = void*(__cdecl*)(void* cls, void* outer, const wchar_t* name, const wchar_t* file, unsigned long flags,
								   void* sandbox, int reconcile);
	LoadFn g_original = nullptr;
	volatile int* volatile g_nowhere = nullptr;

	void* __cdecl Detour(void* cls, void* outer, const wchar_t* name, const wchar_t* file, unsigned long flags, void* sandbox,
						 int reconcile)
	{
		mirage::Set("calls", mirage::Get<int>("calls") + 1);
		if (mirage::Get<bool>("crash")) *g_nowhere = 1;		// The fault, in this mod's own code
		return g_original(cls, outer, name, file, flags, sandbox, reconcile);
	}

	// At StaticLoadObject+0x2B, `mov eax, [ebp+10h]`: ebp is its frame, [ebp+10h] its Name argument (a wide string)
	constexpr std::uintptr_t kNameRead = 0x2B;
	void MIRAGE_CALL AtNameRead(void*, MirageRegisters* r)
	{
		const auto* name = *reinterpret_cast<const wchar_t* const*>(static_cast<std::uintptr_t>(r->ebp) + 0x10);
		if (name && std::wcscmp(name, L"Engine.Actor") == 0) mirage::Set("mid_calls", mirage::Get<int>("mid_calls") + 1);
	}

	class HookSymbol final : public mirage::Module
	{
	public:
		HookSymbol() : Module("Hook (symbol)", "Mods", "Self-test: detours StaticLoadObject by symbol name") {}

		void OnEnable() override
		{
			mirage::Log(MIRAGE_LOG_INFO, mirage::Symbol("StaticLoadObject") ? "StaticLoadObject is known" : "StaticLoadObject is NOT known");
			detour_ = mirage::DetourSymbol("StaticLoadObject", &Detour, 0, &g_original);
			if (auto* f = static_cast<std::uint8_t*>(mirage::Symbol("StaticLoadObject"))) {
				const std::uint8_t expected[] = {0x8B, 0x45, 0x10};		// mov eax, [ebp+10h]
				if (std::memcmp(f + kNameRead, expected, sizeof expected) == 0) mid_ = mirage::MidHook(f + kNameRead, &AtNameRead);
				else mirage::Log(MIRAGE_LOG_ERROR, "StaticLoadObject+0x2B isn't the instruction this mod expects");
			}
		}
		void OnDisable() override
		{
			if (detour_) mirage::RemoveDetour(detour_);
			detour_ = nullptr;
			if (mid_) mirage::RemoveMidHook(mid_);
			mid_ = nullptr;
		}

	private:
		MirageDetour* detour_ = nullptr;
		MirageMidHook* mid_ = nullptr;
	};
	MIRAGE_MODULE(HookSymbol)
}

MIRAGE_MOD()
