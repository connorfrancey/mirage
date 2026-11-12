#include "runtime/mod_dotnet.h"

#include "paths.h"
#include "runtime/mem/heap.h"
#include "runtime/mods.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

namespace mirage::dotnet
{
	namespace
	{
		namespace fs = std::filesystem;

		// hostfxr's C API (hostfxr.h, coreclr_delegates.h): stable, declared here instead of downloading the headers
		using char_t = wchar_t;
		struct InitParams
		{
			std::size_t size;
			const char_t* host_path;
			const char_t* dotnet_root;
		};
		using InitForConfigFn = std::int32_t(__cdecl*)(const char_t* config, const InitParams* params, void** context);
		using GetDelegateFn = std::int32_t(__cdecl*)(void* context, int type, void** fn);
		using CloseFn = std::int32_t(__cdecl*)(void* context);
		using ErrorWriterFn = void(__cdecl*)(const char_t* message);
		using SetErrorWriterFn = ErrorWriterFn(__cdecl*)(ErrorWriterFn writer);
		constexpr int kLoadAssemblyAndGetFunctionPointer = 5;		// hdt_load_assembly_and_get_function_pointer
		using LoadAndGetFn = int(__stdcall*)(const char_t* assembly, const char_t* type, const char_t* method,
											  const char_t* delegateType, void* reserved, void** fn);
		const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

		// Mirage.Bridge's entry points ([UnmanagedCallersOnly], cdecl)
		using InitializeFn = int(__cdecl*)(const MirageApi* api, int apiSize, char* error, int capacity);
		using LoadModFn = int(__cdecl*)(MirageMod* mod, const char* path, char* error, int capacity);
		using UnloadModFn = void(__cdecl*)(MirageMod* mod);
		using StillLoadedFn = int(__cdecl*)();
		using ManagedBytesFn = std::int64_t(__cdecl*)();
		using CheckScriptsFn = int(__cdecl*)(const char* folder, const char* file, const char* text, char* output, int capacity);

		struct Runtime
		{
			std::mutex lock;
			bool tried = false;
			Status status;
			InitializeFn initialize = nullptr;
			LoadModFn loadMod = nullptr;
			UnloadModFn unloadMod = nullptr;
			StillLoadedFn stillLoaded = nullptr;
			ManagedBytesFn managedBytes = nullptr;
			CheckScriptsFn checkScripts = nullptr;
		};
		Runtime& R()
		{
			static Runtime* r = new Runtime();		// Never destroyed: the CLR can't be unloaded from a process anyway
			return *r;
		}

		std::string Narrow(const std::wstring& w)
		{
			if (w.empty()) return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
			std::string s(static_cast<std::size_t>(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
			return s;
		}

		void __cdecl ErrorWriter(const char_t* message) { spdlog::warn("C# runtime: {}", Narrow(message ? message : L"")); }

		std::uint64_t FreeAddressSpace()
		{
			MEMORYSTATUSEX ms{sizeof ms};
			return GlobalMemoryStatusEx(&ms) ? ms.ullAvailVirtual : 0;
		}

		// The newest host\fxr\<version>\hostfxr.dll under the bundled runtime
		fs::path FindHostFxr(std::string* version)
		{
			std::error_code ec;
			fs::path best;
			std::string bestName;
			for (const auto& e : fs::directory_iterator(Root() / "host" / "fxr", ec)) {
				const std::string name = e.path().filename().string();
				if (fs::exists(e.path() / "hostfxr.dll", ec) && name > bestName) {
					bestName = name;
					best = e.path() / "hostfxr.dll";
				}
			}
			if (version) *version = bestName;
			return best;
		}

		std::string Start()
		{
#if !MEMENU_DOTNET
			return "this build has no C# (built with MEMENU_DOTNET off: its scripting language is Lua)";
#endif
			Runtime& r = R();
			const auto t0 = std::chrono::steady_clock::now();
			r.status.addressBefore = FreeAddressSpace();
			std::string version;
			const fs::path fxr = FindHostFxr(&version);
			if (fxr.empty()) return "the .NET runtime isn't installed with the menu (no " + (Root() / "host" / "fxr").string() + ")";
			const fs::path config = Root() / "mirage" / "Mirage.runtimeconfig.json";
			const fs::path assembly = Root() / "mirage" / "Mirage.dll";
			if (!fs::exists(assembly)) return "Mirage.dll is missing (" + assembly.string() + ")";

			HMODULE lib = LoadLibraryExW(fxr.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			if (!lib) return "hostfxr.dll could not be loaded (error " + std::to_string(GetLastError()) + ")";
			auto init = reinterpret_cast<InitForConfigFn>(GetProcAddress(lib, "hostfxr_initialize_for_runtime_config"));
			auto getDelegate = reinterpret_cast<GetDelegateFn>(GetProcAddress(lib, "hostfxr_get_runtime_delegate"));
			auto close = reinterpret_cast<CloseFn>(GetProcAddress(lib, "hostfxr_close"));
			auto setWriter = reinterpret_cast<SetErrorWriterFn>(GetProcAddress(lib, "hostfxr_set_error_writer"));
			if (!init || !getDelegate || !close) return "hostfxr.dll lacks the hosting API";
			if (setWriter) setWriter(&ErrorWriter);

			const std::wstring root = Root().wstring();
			InitParams params{sizeof params, nullptr, root.c_str()};
			void* context = nullptr;
			const std::int32_t rc = init(config.c_str(), &params, &context);
			if (rc < 0 || !context) {		// 1 and 2 are success codes ("already initialized" variants)
				char hex[16];
				std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(rc));
				return std::string("the runtime could not start (hostfxr ") + hex + ")";
			}
			void* loadAndGetRaw = nullptr;
			const std::int32_t rd = getDelegate(context, kLoadAssemblyAndGetFunctionPointer, &loadAndGetRaw);
			close(context);
			if (rd < 0 || !loadAndGetRaw) return "the runtime gave no assembly loader";
			auto loadAndGet = reinterpret_cast<LoadAndGetFn>(loadAndGetRaw);

			const auto entry = [&](const char_t* method, void** out) {
				return loadAndGet(assembly.c_str(), L"Mirage.Bridge, Mirage", method, kUnmanagedCallersOnly, nullptr, out) == 0 && *out;
			};
			if (!entry(L"Initialize", reinterpret_cast<void**>(&r.initialize)) || !entry(L"LoadMod", reinterpret_cast<void**>(&r.loadMod)) ||
				!entry(L"UnloadMod", reinterpret_cast<void**>(&r.unloadMod)) ||
				!entry(L"StillLoaded", reinterpret_cast<void**>(&r.stillLoaded)) ||
				!entry(L"ManagedBytes", reinterpret_cast<void**>(&r.managedBytes)) ||
				!entry(L"CheckScripts", reinterpret_cast<void**>(&r.checkScripts)))
				return "Mirage.dll's entry points were not found (an older Mirage.dll?)";
			char error[512] = {};
			if (!r.initialize(mods::Api(), static_cast<int>(sizeof(MirageApi)), error, sizeof error))
				return std::string("Mirage.dll refused this runtime: ") + error;

			r.status.version = version;
			r.status.addressAfter = FreeAddressSpace();
			r.status.startMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			spdlog::info("C# runtime: .NET {} started in {:.0f} ms ({:.0f} MB of address space)", version, r.status.startMs,
						 (static_cast<double>(r.status.addressBefore) - static_cast<double>(r.status.addressAfter)) / 1048576.0);
			return {};
		}
	}

	fs::path Root() { return paths::Dir() / "dotnet"; }

	namespace
	{
		// Starts the runtime the first time it is needed. "" when it runs.
		std::string EnsureStarted()
		{
			Runtime& r = R();
			std::lock_guard<std::mutex> l(r.lock);
			if (!r.tried) {
				r.tried = true;
				static const auto kTag = mem::RegisterTag("C# runtime");
				mem::TagScope tag(kTag);
				r.status.problem = Start();
				r.status.started = r.status.problem.empty();
				if (!r.status.started) spdlog::error("C# runtime: {}: C# mods won't load", r.status.problem);
			}
			return r.status.started ? std::string() : "the C# runtime isn't available: " + r.status.problem;
		}

		std::string U8(const fs::path& p)
		{
			const auto u = p.u8string();
			return std::string(u.begin(), u.end());
		}
	}

	CheckResult CheckScripts(const fs::path& folder, const fs::path& file, const std::string& text)
	{
		CheckResult out;
		if (const std::string problem = EnsureStarted(); !problem.empty()) {
			out.output = problem;
			return out;
		}
		std::string buffer(16384, '\0');
		out.errors = R().checkScripts(U8(folder).c_str(), file.empty() ? nullptr : U8(file).c_str(), file.empty() ? nullptr : text.c_str(),
									  buffer.data(), static_cast<int>(buffer.size()));
		buffer.resize(std::strlen(buffer.c_str()));
		out.output = std::move(buffer);
		return out;
	}

	std::string LoadMod(MirageMod* mod, const fs::path& assembly)
	{
		Runtime& r = R();
		if (const std::string problem = EnsureStarted(); !problem.empty()) return problem;
		const auto u8 = assembly.u8string();
		const std::string path(u8.begin(), u8.end());
		char error[8192] = {};		// A script's compile errors
		if (!r.loadMod(mod, path.c_str(), error, sizeof error)) return error[0] ? std::string(error) : std::string("it didn't load");
		return {};
	}

	void UnloadMod(MirageMod* mod)
	{
		Runtime& r = R();
		if (r.status.started && r.unloadMod) r.unloadMod(mod);
	}

	Status GetStatus(bool measure)
	{
		Runtime& r = R();
		Status s;
		{
			std::lock_guard<std::mutex> l(r.lock);
			s = r.status;
		}
		if (s.started) {
			if (r.managedBytes) s.managedBytes = r.managedBytes();
			if (measure && r.stillLoaded) s.stillLoaded = r.stillLoaded();
		}
		return s;
	}
}
