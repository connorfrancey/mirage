#include "crashhandler/collect.h"

#include "crashhandler/analyzer/analyzer.h"
#include "crashhandler/memory_text.h"
#include "crashhandler/symbols.h"
#include "game/game.h"
#include "hooks.h"
#include "log.h"
#include "runtime/mods.h"
#include "runtime/script_patch.h"

#include "sdk/SDK.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crashhandler
{
	namespace
	{
		// SEH around a part: `fn(ctx)` may fault on bad memory. No C++ objects in this function (SEH can't unwind them).
		bool Safe(void (*fn)(void*), void* ctx)
		{
			__try {
				fn(ctx);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		template <class F>
		bool Guard(F&& f)
		{
			return Safe([](void* c) { (*static_cast<F*>(c))(); }, &f);
		}

		bool Readable(std::uintptr_t address, std::size_t bytes)
		{
			if (address < 0x10000) return false;
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi)) return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
			return address + bytes <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		bool Executable(std::uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (address < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
				return false;
			return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		// The object at `address`, if it is one: it is in GObjects at the index it says it has
		Classes::UObject* ObjectAt(std::uintptr_t address)
		{
			if ((address & 3) || !Readable(address, sizeof(Classes::UObject))) return nullptr;
			auto* o = reinterpret_cast<Classes::UObject*>(address);
			const auto& objects = Classes::UObject::GetGlobalObjects();
			const int index = o->ObjectInternalInteger;
			if (index < 0 || index >= static_cast<int>(objects.Num())) return nullptr;
			return objects.GetByIndex(static_cast<std::size_t>(index)) == o ? o : nullptr;
		}

		std::string ObjectName(Classes::UObject* o)
		{
			std::string s = o->GetName();
			if (o->Class) s += " (" + o->Class->GetName() + ")";
			return s;
		}

		// What a 32-bit value looks like: an object, code, a string, a pointer into memory
		std::string Note(std::uint32_t v)
		{
			std::string note;
			Guard([&] {
				if (Classes::UObject* o = ObjectAt(v)) {
					note = ObjectName(o);
					return;
				}
				if (Executable(v)) {
					const Place p = symbols::Describe(v);
					if (!p.module.empty()) note = p.Full();
					return;
				}
				if (Readable(v, 4)) {
					std::uint8_t bytes[96];
					std::size_t n = 4;
					while (n + 16 <= sizeof bytes && Readable(v + n, 16)) n += 16;		// As far as it can be read
					std::memcpy(bytes, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(v)), n);
					note = TextAt(bytes, n);
					if (note.empty()) {
						// A pointer to an object (a field holding one)
						if (Classes::UObject* o = ObjectAt(*reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(v))))
							note = "-> " + ObjectName(o);
					}
				}
			});
			return note;
		}

		const char* CodeName(DWORD code)
		{
			switch (code) {
				case EXCEPTION_ACCESS_VIOLATION: return "access violation";
				case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
				case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
				case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
				case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
				case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float division by zero";
				case EXCEPTION_BREAKPOINT: return "breakpoint";
				case 0xE06D7363: return "C++ exception not caught";
				case 0xC0000409: return "stack buffer overrun (/GS) or fail-fast";
				case 0xC0000374: return "heap corruption";
				default: return "exception";
			}
		}

		std::string ThreadName(DWORD thread)
		{
			if (thread && thread == game::GameThreadId()) return "the game thread";
			if (thread && thread == hooks::RenderThreadId()) return "the render thread";
			std::string name = "another thread";
			if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread)) {
				PWSTR description = nullptr;
				if (SUCCEEDED(GetThreadDescription(h, &description)) && description) {
					if (*description) {
						name = "the thread \"";
						for (const wchar_t* c = description; *c; ++c) name += *c < 0x80 ? static_cast<char>(*c) : '?';
						name += "\"";
					}
					LocalFree(description);
				}
				CloseHandle(h);
			}
			return name;
		}

		// The top of the committed stack esp is in
		std::uintptr_t StackTop(std::uintptr_t esp)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(esp), &mbi, sizeof mbi)) return esp;
			return reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		// The script frames on the native stack: an FFrame is {vtable, Node, Object, Code, ...}, Node a function or
		// state, Object an object, Code inside Node's bytecode (FFrame::Node at 4: runtime/inject.cpp finds it there)
		void ScriptStack(Analysis& a, std::uintptr_t esp)
		{
			Classes::UClass* functionClass = Classes::UFunction::StaticClass();
			Classes::UClass* stateClass = Classes::UState::StaticClass();
			const std::uintptr_t top = std::min(StackTop(esp), esp + 512 * 1024);
			for (std::uintptr_t p = esp & ~std::uintptr_t{3}; p + 16 <= top && a.script.size() < 32; p += 4) {
				const auto* f = reinterpret_cast<const std::uint32_t*>(p);
				Classes::UObject* node = ObjectAt(f[1]);
				if (!node || (node->Class != functionClass && node->Class != stateClass)) continue;
				Classes::UObject* object = ObjectAt(f[2]);
				if (!object) continue;
				auto* fn = static_cast<Classes::UStruct*>(node);
				const auto* script = reinterpret_cast<const std::uint32_t*>(&fn->Script);
				const std::uint32_t data = script[0], count = script[1];
				if (!data || f[3] < data || f[3] >= data + count) continue;
				ScriptFrame s;
				s.function = std::string(node->Class == stateClass ? "State " : "Function ") +
							 (node->Outer ? node->Outer->GetName() + "." : std::string()) + node->GetName();
				s.object = ObjectName(object);
				s.offset = f[3] - data;
				s.changedBy = mirage::scripts::Owner(fn);
				if (!a.script.empty() && a.script.back().function == s.function && a.script.back().object == s.object) continue;
				a.script.push_back(std::move(s));
			}
		}

		void NativeStack(Analysis& a, const CONTEXT& c)
		{
			a.frames.push_back(a.where);
			std::uintptr_t ebp = c.Ebp;
			for (int i = 0; i < 64 && Readable(ebp, 8); ++i) {
				const auto* frame = reinterpret_cast<const std::uintptr_t*>(ebp);
				const std::uintptr_t ret = frame[1], next = frame[0];
				if (!ret || !Executable(ret)) break;
				a.frames.push_back(symbols::Describe(static_cast<std::uint32_t>(ret)));
				if (next <= ebp) break;
				ebp = next;
			}
			if (a.frames.size() >= 4) return;
			// No frame pointers (optimized code): the code addresses on the stack instead
			a.framesFromScan = true;
			a.frames.resize(1);
			for (std::uintptr_t p = c.Esp; a.frames.size() < 40 && p < c.Esp + 16384 && Readable(p, 4); p += 4) {
				const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(p);
				if (!Executable(v)) continue;
				Place place = symbols::Describe(v);
				if (!place.module.empty()) a.frames.push_back(std::move(place));
			}
		}

		void Disasm(Analysis& a, std::uintptr_t eip)
		{
			std::uint8_t code[160];
			std::uintptr_t from = eip >= 64 ? eip - 64 : 0;
			while (from < eip && !Readable(from, 1)) ++from;		// Start where it can be read
			std::size_t size = 0;
			while (size < sizeof code && Readable(from + size, 1)) size = std::min(size + 16, sizeof code);
			if (size <= eip - from) return;
			std::memcpy(code, reinterpret_cast<const void*>(from), size);
			a.disasm = Disassemble(code, size, static_cast<std::uint32_t>(from), eip - from, 8, 10, [](std::uint32_t target) {
				const Place p = symbols::Describe(target);
				return p.module.empty() ? std::string() : p.Short() + (p.symbol.empty() ? "" : " " + p.symbol);
			});
		}

		std::uint64_t LargestFreeBlock()
		{
			std::uint64_t largest = 0;
			MEMORY_BASIC_INFORMATION mbi{};
			for (std::uintptr_t p = 0x10000; VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi);) {
				if (mbi.State == MEM_FREE) largest = std::max<std::uint64_t>(largest, mbi.RegionSize);
				const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (next <= p) break;
				p = next;
			}
			return largest;
		}
	}

	Analysis Collect(const EXCEPTION_RECORD& r, const CONTEXT& c, DWORD thread, const char* title, bool fatal)
	{
		Analysis a;
		a.title = title ? title : "crash";
		a.fatal = fatal;
		a.code = r.ExceptionCode;
		a.what = CodeName(r.ExceptionCode);
		a.thread = thread;
		const auto eip = reinterpret_cast<std::uintptr_t>(r.ExceptionAddress);
		symbols::Session session;		// dbghelp, for this report

		Guard([&] { a.where = symbols::Describe(static_cast<std::uint32_t>(eip)); });
		if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2) {
			a.accessKnown = true;
			a.accessKind = r.ExceptionInformation[0] == 0 ? "reading" : r.ExceptionInformation[0] == 1 ? "writing" : "executing";
			a.accessAddress = static_cast<std::uint32_t>(r.ExceptionInformation[1]);
		}
		if (!a.where.mod.empty()) a.whose = "the mod " + a.where.mod;
		else if (a.where.module == "Mirage.dll") a.whose = "Mirage";
		else if (a.where.module.empty()) a.whose = "no module (code that isn't loaded, or generated code)";
		else if (a.where.module == "MirrorsEdge.exe") a.whose = "the game";
		else a.whose = a.where.module + " (outside the game)";
		Guard([&] { a.threadName = ThreadName(thread); });

		const std::pair<const char*, DWORD> regs[] = {{"eax", c.Eax}, {"ebx", c.Ebx}, {"ecx", c.Ecx}, {"edx", c.Edx}, {"esi", c.Esi},
													  {"edi", c.Edi}, {"ebp", c.Ebp}, {"esp", c.Esp}, {"eip", c.Eip}};
		for (const auto& [name, value] : regs) a.registers.push_back({name, value, Note(value)});
		a.registers.push_back({"eflags", c.EFlags, {}});

		Guard([&] { NativeStack(a, c); });
		Guard([&] { ScriptStack(a, c.Esp); });
		Guard([&] { Disasm(a, eip); });
		for (std::uintptr_t p = c.Esp, i = 0; i < 96 && Readable(p, 4); p += 4, ++i) {
			const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(p);
			char label[16];
			std::snprintf(label, sizeof label, "esp+0x%03X", static_cast<unsigned>(p - c.Esp));
			a.stack.push_back({label, v, Note(v)});
		}

		Guard([&] { a.inAllocator = symbols::InAllocator(static_cast<std::uint32_t>(eip)); });
		Guard([&] { a.largestFreeBlock = LargestFreeBlock(); });
		Guard([&] {
			for (const mirage::mods::Info& m : mirage::mods::List())
				if (m.loaded) a.mods.push_back(m.id + " " + m.version + " (" + m.kind + ")");
		});
		Guard([&] {
			std::vector<logging::Entry> entries;
			logging::CopySince(0, entries);
			const std::size_t from = entries.size() > 30 ? entries.size() - 30 : 0;
			for (std::size_t i = from; i < entries.size(); ++i) a.log.push_back(entries[i].time + " " + entries[i].text);
		});
		a.findings = RunAnalyzers(a);
		return a;
	}
}
