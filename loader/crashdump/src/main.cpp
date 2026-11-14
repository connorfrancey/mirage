// mirage_crashdump.exe: writes a minidump of Mirror's Edge from outside it.
//
// dbghelp suspends every other thread of the process it dumps. Called from inside the game (the crash reporter's
// thread), it deadlocked when a suspended thread held a lock dbghelp needs: the heap's, the loader's (seen: a report
// written, its .dmp left at 0 bytes, the game frozen). From this process nothing of the game's is needed, so the dump
// always finishes. Mirage.dll carries this program as a resource and writes it next to itself (runtime/crash_reporter).
//
//   mirage_crashdump.exe <pid> <thread id> <EXCEPTION_POINTERS address in the game, or 0> <dump file>
//
// Exit code 0 when the dump was written, 1 with the reason on stderr otherwise.
#include <Windows.h>
#include <DbgHelp.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
	if (argc != 5) {
		std::fwprintf(stderr, L"usage: mirage_crashdump <pid> <thread id> <exception pointers or 0> <dump file>\n");
		return 1;
	}
	const DWORD pid = std::wcstoul(argv[1], nullptr, 10);
	const DWORD thread = std::wcstoul(argv[2], nullptr, 10);
	const auto pointers = static_cast<std::uintptr_t>(std::wcstoull(argv[3], nullptr, 16));
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
	if (!process) {
		std::fwprintf(stderr, L"can't open process %lu (error %lu)\n", pid, GetLastError());
		return 1;
	}
	HANDLE file = CreateFileW(argv[4], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		std::fwprintf(stderr, L"can't create %ls (error %lu)\n", argv[4], GetLastError());
		CloseHandle(process);
		return 1;
	}
	// The exception record stays in the game's memory: ClientPointers says the address is the game's
	MINIDUMP_EXCEPTION_INFORMATION info{thread, reinterpret_cast<PEXCEPTION_POINTERS>(pointers), TRUE};
	const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
												 MiniDumpWithThreadInfo);
	const BOOL ok = MiniDumpWriteDump(process, pid, file, type, pointers ? &info : nullptr, nullptr, nullptr);
	const DWORD error = GetLastError();
	CloseHandle(file);
	CloseHandle(process);
	if (!ok) {
		std::fwprintf(stderr, L"MiniDumpWriteDump failed (error 0x%08lX)\n", error);
		DeleteFileW(argv[4]);
		return 1;
	}
	return 0;
}
