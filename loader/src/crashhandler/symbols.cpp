#include "crashhandler/symbols.h"

#include "core/address_cache.h"
#include "paths.h"
#include "runtime/fhooks.h"
#include "runtime/maps.h"
#include "runtime/mods.h"

#include "sdk/SDK.hpp"

#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace crashhandler::symbols
{
	namespace
	{
		struct Name
		{
			std::uint32_t address;
			std::string name;
		};
		struct Table
		{
			std::vector<Name> names;			// Sorted by address
			std::size_t natives = 0;			// How many came from native functions
			std::uint32_t allocator[3] = {};	// GMalloc's Malloc, Realloc, Free
		};
		std::atomic<std::shared_ptr<const Table>> g_table;

		constexpr std::uint32_t kFuncNative = 0x400;
		constexpr std::uint32_t kGapLimit = 0x2000;	// Further than this past a known function: not named after it

		std::uintptr_t ExeBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }
		std::uintptr_t ExeEnd()
		{
			const auto base = ExeBase();
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return base + nt->OptionalHeader.SizeOfImage;
		}

		std::string Narrow(const wchar_t* w)
		{
			std::string s;
			for (; w && *w; ++w) s += *w < 0x80 ? static_cast<char>(*w) : '?';
			return s;
		}

		std::atomic<bool> g_dbghelp{false};		// A Session is open
	}

	void Refresh()
	{
		const int funcOffset = mirage::hooks::FuncOffset();
		auto table = std::make_shared<Table>();
		const std::uintptr_t base = ExeBase(), end = ExeEnd();
		const auto inExe = [&](std::uintptr_t a) { return a >= base && a < end; };

		if (funcOffset > 0) {
			Classes::UClass* functionClass = Classes::UFunction::StaticClass();
			const auto& objects = Classes::UObject::GetGlobalObjects();
			std::uint32_t common = 0;		// ProcessInternal: what every script function's Func holds
			for (std::size_t i = 0; i < objects.Num(); ++i) {
				auto* o = objects.GetByIndex(i);
				if (!o || o->Class != functionClass) continue;
				auto* fn = static_cast<Classes::UFunction*>(o);
				const std::uint32_t func = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(fn) + funcOffset);
				if (!(fn->FunctionFlags & kFuncNative)) {
					common = func;
					continue;
				}
				if (!inExe(func)) continue;
				const std::string owner = fn->Outer ? fn->Outer->GetName() : std::string();
				table->names.push_back({func, owner + ".exec" + fn->GetName() + " (native)"});
			}
			table->natives = table->names.size();
			if (common && inExe(common)) table->names.push_back({common, "UObject::ProcessInternal (the script VM)"});
		}
		for (const auto& v : addr::Game().Snapshot())
			if (inExe(v.address)) table->names.push_back({static_cast<std::uint32_t>(v.address), v.name});
		if (void** var = mirage::maps::GMallocVariable(); var && *var) {
			void** vtable = *reinterpret_cast<void***>(*var);
			const char* names[3] = {"GMalloc->Malloc", "GMalloc->Realloc", "GMalloc->Free"};
			for (int i = 0; i < 3; ++i) {
				const auto f = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(vtable[i + 1]));
				table->allocator[i] = f;
				if (inExe(f)) table->names.push_back({f, names[i]});
			}
		}
		std::sort(table->names.begin(), table->names.end(), [](const Name& a, const Name& b) { return a.address < b.address; });
		g_table.store(std::move(table));
	}

	Session::Session()
	{
		// Where the PDBs are: next to Mirage.dll, and next to each mod's DLL (their own build paths are tried too)
		std::wstring path = paths::Dir().wstring();
		HMODULE mods[512];
		DWORD needed = 0;
		if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
			for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++i) {
				wchar_t file[MAX_PATH] = {};
				GetModuleFileNameW(mods[i], file, MAX_PATH);
				if (!std::wcsstr(file, L"\\mods\\")) continue;
				if (wchar_t* slash = std::wcsrchr(file, L'\\')) *slash = 0;
				path += L";";
				path += file;
			}
		}
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
					  SYMOPT_NO_PROMPTS | SYMOPT_OMAP_FIND_NEAREST);
		ok_ = SymInitializeW(GetCurrentProcess(), path.c_str(), TRUE) != FALSE;
		g_dbghelp = ok_;
	}

	Session::~Session()
	{
		if (ok_) SymCleanup(GetCurrentProcess());
		g_dbghelp = false;
	}

	Place Describe(std::uint32_t address)
	{
		Place p;
		p.address = address;
		HMODULE m = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(address)), &m) || !m)
			return p;
		wchar_t file[MAX_PATH] = {};
		GetModuleFileNameW(m, file, MAX_PATH);
		const wchar_t* slash = std::wcsrchr(file, L'\\');
		p.module = Narrow(slash ? slash + 1 : file);
		p.offset = address - static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(m));
		p.mod = mirage::mods::ModOfModule(m);

		if (m == GetModuleHandleW(nullptr)) {
			// The game: the nearest known engine function before it, if it isn't too far
			if (const std::shared_ptr<const Table> table = g_table.load()) {
				const auto& names = table->names;
				auto it = std::upper_bound(names.begin(), names.end(), address,
										   [](std::uint32_t a, const Name& n) { return a < n.address; });
				if (it != names.begin()) {
					--it;
					if (address - it->address < kGapLimit) {
						char off[24];
						std::snprintf(off, sizeof off, "+0x%X", address - it->address);
						p.symbol = it->name + off + (address == it->address ? "" : " (nearest known)");
					}
				}
			}
			return p;
		}
		if (!g_dbghelp) return p;
		alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
		auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buffer);
		sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
		sym->MaxNameLen = 255;
		DWORD64 displacement = 0;
		if (SymFromAddrW(GetCurrentProcess(), address, &displacement, sym)) {
			char off[24];
			std::snprintf(off, sizeof off, "+0x%llX", static_cast<unsigned long long>(displacement));
			p.symbol = Narrow(sym->Name) + off;
		}
		IMAGEHLP_LINEW64 line{};
		line.SizeOfStruct = sizeof line;
		DWORD lineDisplacement = 0;
		if (SymGetLineFromAddrW64(GetCurrentProcess(), address, &lineDisplacement, &line) && line.FileName) {
			const wchar_t* name = std::wcsrchr(line.FileName, L'\\');
			p.source = Narrow(name ? name + 1 : line.FileName) + ":" + std::to_string(line.LineNumber);
		}
		return p;
	}

	bool InAllocator(std::uint32_t address)
	{
		const std::shared_ptr<const Table> table = g_table.load();
		if (!table) return false;
		for (std::uint32_t f : table->allocator)
			if (f && address >= f && address - f < 0x400) return true;
		return false;
	}
}
