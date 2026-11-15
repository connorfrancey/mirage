#include "runtime/detour_guard.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace mirage::detours::guard
{
	namespace
	{
		struct Ctx
		{
			void* detour;
			void** original;
			int id;
			std::uintptr_t moduleBegin, moduleEnd;		// The detour's module: a fault there is the detour's
			std::atomic<bool> off{false};				// Disabled or faulted: straight to *original
		};

		// One call inside a detour on this thread: what the guard puts back at a fault
		struct Record
		{
			Ctx* ctx;
			std::uint32_t* retSlot;			// The return address's slot (esp at the entry); the exit stub is in it
			std::uint32_t realReturn;
			std::uint32_t eax, ecx, edx, ebx, ebp, esi, edi, eflags;
		};
		constexpr int kDepth = 64;
		thread_local std::array<Record, kDepth> t_records;
		thread_local int t_depth = 0;

		// The registers GuardEntry pushes (pushfd after pushad), then the thunk's ctx and the caller's return address
		struct Frame
		{
			std::uint32_t eflags, edi, esi, ebp, espAtPushad, ebx, edx, ecx, eax;
			Ctx* ctx;
			std::uint32_t ret;
		};
		static_assert(sizeof(Frame) == 44);

		// Faults for TakeFaults: a fixed ring written from the exception handler (no allocation, no lock there)
		constexpr int kFaults = 16;
		std::array<Fault, kFaults> g_faults;
		std::atomic<unsigned> g_faultWrite{0}, g_faultRead{0};

		void GuardExit();

		// Records whose frame is gone (a longjmp or an exception unwound past the detour) are dropped first: the frames
		// still running are higher on the stack than `esp`
		void DropStale(std::uintptr_t esp)
		{
			while (t_depth > 0 && reinterpret_cast<std::uintptr_t>(t_records[t_depth - 1].retSlot) < esp) --t_depth;
		}

		void* __cdecl GuardEnterImpl(Frame* f)
		{
			Ctx* ctx = f->ctx;
			DropStale(reinterpret_cast<std::uintptr_t>(&f->ret));
			if (ctx->off.load(std::memory_order_acquire) || t_depth >= kDepth) return *ctx->original;
			Record& r = t_records[t_depth++];
			r.ctx = ctx;
			r.retSlot = &f->ret;
			r.realReturn = f->ret;
			r.eax = f->eax;
			r.ecx = f->ecx;
			r.edx = f->edx;
			r.ebx = f->ebx;
			r.ebp = f->ebp;
			r.esi = f->esi;
			r.edi = f->edi;
			r.eflags = f->eflags;
			f->ret = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&GuardExit));
			return ctx->detour;
		}

		std::uint32_t __cdecl GuardLeaveImpl()
		{
			if (t_depth <= 0) return 0;		// Can't happen: GuardExit is only ever a recorded return address
			return t_records[--t_depth].realReturn;
		}

		// The thunk jumps here with its ctx pushed: [esp] ctx, [esp+4] the caller's return address
		__declspec(naked) void GuardEntry()
		{
			__asm {
				pushad
				pushfd
				mov eax, esp
				push eax
				call GuardEnterImpl
				add esp, 4
				mov [esp + 36], eax			// The ctx's slot: where to go
				popfd
				popad
				ret							// Into the detour (or *original), the arguments as the caller left them
			}
		}

		// The detour returned here (its ret, or ret n): back to the real caller, eax/edx and the FPU untouched
		__declspec(naked) void GuardExit()
		{
			__asm {
				sub esp, 4					// The real return address's slot
				push eax
				push ecx
				push edx
				call GuardLeaveImpl
				mov [esp + 12], eax
				pop edx
				pop ecx
				pop eax
				ret
			}
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
		{
			switch (ep->ExceptionRecord->ExceptionCode) {
				case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
				case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_IN_PAGE_ERROR:
					break;
				default:
					return EXCEPTION_CONTINUE_SEARCH;	// Not a fault the detour made (or one there's no recovering from)
			}
			if (t_depth <= 0) return EXCEPTION_CONTINUE_SEARCH;
			CONTEXT* c = ep->ContextRecord;
			DropStale(c->Esp);
			if (t_depth <= 0) return EXCEPTION_CONTINUE_SEARCH;
			Record& r = t_records[t_depth - 1];
			const std::uintptr_t eip = c->Eip;
			if (eip < r.ctx->moduleBegin || eip >= r.ctx->moduleEnd) return EXCEPTION_CONTINUE_SEARCH;	// Not its own code

			r.ctx->off.store(true, std::memory_order_release);
			const unsigned slot = g_faultWrite.fetch_add(1);
			if (slot - g_faultRead.load() < kFaults) g_faults[slot % kFaults] = Fault{r.ctx->id, eip, ep->ExceptionRecord->ExceptionCode};

			// As at the entry, the real return address back in its slot, and on into the function the detour would have called
			*r.retSlot = r.realReturn;
			c->Eax = r.eax;
			c->Ecx = r.ecx;
			c->Edx = r.edx;
			c->Ebx = r.ebx;
			c->Ebp = r.ebp;
			c->Esi = r.esi;
			c->Edi = r.edi;
			c->EFlags = r.eflags;
			c->Esp = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(r.retSlot));
			c->Eip = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(*r.ctx->original));
			--t_depth;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		std::once_flag g_installed;
		void Install()
		{
			// First in line: before the crash logger (diag.cpp) logs a fault the guard recovers from
			std::call_once(g_installed, [] { AddVectoredExceptionHandler(1, &Handler); });
		}

		// Thunks: 10 bytes each (push imm32; jmp rel32), from pages made executable once written
		std::mutex g_pageLock;
		std::uint8_t* g_page = nullptr;
		std::size_t g_used = 0;
		constexpr std::size_t kPage = 4096, kThunk = 16;

		std::uint8_t* NewThunk(Ctx* ctx)
		{
			std::lock_guard lock(g_pageLock);
			if (!g_page || g_used + kThunk > kPage) {
				g_page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
				g_used = 0;
				if (!g_page) return nullptr;
			}
			std::uint8_t* t = g_page + g_used;
			g_used += kThunk;
			DWORD old = 0;
			// Writable without ever leaving execution: another thread may be running a thunk on this page
			VirtualProtect(g_page, kPage, PAGE_EXECUTE_READWRITE, &old);
			t[0] = 0x68;		// push ctx
			const auto ctxValue = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(ctx));
			std::memcpy(t + 1, &ctxValue, 4);
			t[5] = 0xE9;		// jmp GuardEntry
			const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&GuardEntry) -
														reinterpret_cast<std::uintptr_t>(t + 10));
			std::memcpy(t + 6, &rel, 4);
			VirtualProtect(g_page, kPage, PAGE_EXECUTE_READ, &old);
			FlushInstructionCache(GetCurrentProcess(), t, kThunk);
			return t;
		}
	}

	void* Make(void* detour, void** original, int id)
	{
		Install();
		auto* ctx = new Ctx;		// Never freed: a thread may be in its thunk
		ctx->detour = detour;
		ctx->original = original;
		ctx->id = id;
		ctx->moduleBegin = ctx->moduleEnd = 0;
		HMODULE m = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   static_cast<LPCWSTR>(detour), &m) && m) {
			const auto base = reinterpret_cast<std::uintptr_t>(m);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			ctx->moduleBegin = base;
			ctx->moduleEnd = base + nt->OptionalHeader.SizeOfImage;
		}
		std::uint8_t* t = NewThunk(ctx);
		if (!t) {
			delete ctx;
			return nullptr;
		}
		return t;
	}

	void Disable(void* thunk)
	{
		if (!thunk) return;
		std::uint32_t ctx = 0;
		std::memcpy(&ctx, static_cast<std::uint8_t*>(thunk) + 1, 4);
		reinterpret_cast<Ctx*>(static_cast<std::uintptr_t>(ctx))->off.store(true, std::memory_order_release);
	}

	std::vector<Fault> TakeFaults()
	{
		std::vector<Fault> out;
		const unsigned end = g_faultWrite.load();
		unsigned at = g_faultRead.load();
		if (end - at > kFaults) at = end - kFaults;		// Overrun: the newest ones
		for (; at != end; ++at) out.push_back(g_faults[at % kFaults]);
		g_faultRead = end;
		return out;
	}
}
