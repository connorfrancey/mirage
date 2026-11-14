// The report's text from an Analysis (crashhandler/analysis.h). No game or Windows code: the unit tests use it.
#include "crashhandler/analysis.h"

#include <cstdio>

namespace crashhandler
{
	namespace
	{
		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%08X", v);
			return b;
		}
	}

	std::string Place::Short() const
	{
		char b[160];
		if (module.empty()) std::snprintf(b, sizeof b, "0x%08X", address);
		else std::snprintf(b, sizeof b, "%s+0x%X", module.c_str(), offset);
		return b;
	}

	std::string Place::Full() const
	{
		std::string s = Short();
		if (!symbol.empty()) s += "  " + symbol;
		if (!source.empty()) s += "  (" + source + ")";
		if (!mod.empty()) s += "  [mod " + mod + "]";
		return s;
	}

	std::string ToText(const Analysis& a)
	{
		std::string t;
		if (!a.findings.empty()) {
			t += "\nPossible causes:\n";
			for (const Finding& f : a.findings) t += "  - " + f.title + ": " + f.detail + "\n";
		}
		t += "\nThread: " + a.threadName + " (" + std::to_string(a.thread) + ")\n";

		t += a.framesFromScan ? "\nCall stack (from code addresses on the stack: some may be stale):\n" : "\nCall stack:\n";
		for (std::size_t i = 0; i < a.frames.size(); ++i) {
			char n[8];
			std::snprintf(n, sizeof n, "#%-2zu", i);
			t += std::string("  ") + n + " " + a.frames[i].Full() + "\n";
		}
		if (a.frames.empty()) t += "  (none could be read)\n";

		t += "\nUnrealScript call stack:\n";
		for (const ScriptFrame& f : a.script) {
			char off[24];
			std::snprintf(off, sizeof off, " +0x%X", f.offset);
			t += "  " + f.function + off + "  on " + f.object;
			if (!f.changedBy.empty()) t += "  [bytecode changed by " + f.changedBy + "]";
			t += "\n";
		}
		if (a.script.empty()) t += "  (no script was running on this thread)\n";

		t += "\nRegisters:\n";
		for (const Slot& r : a.registers) t += "  " + r.label + "=" + Hex(r.value) + (r.note.empty() ? "" : "  " + r.note) + "\n";

		if (!a.disasm.empty()) {
			t += "\nCode at the crash:\n";
			for (const DisasmLine& l : a.disasm) {
				char head[48];
				std::snprintf(head, sizeof head, "%s %08X  %-24s ", l.current ? "=>" : "  ", l.address, l.bytes.c_str());
				t += head + l.text + "\n";
			}
		}

		if (!a.stack.empty()) {
			t += "\nStack memory:\n";
			for (const Slot& s : a.stack)
				if (!s.note.empty()) t += "  [" + s.label + "] " + Hex(s.value) + "  " + s.note + "\n";
		}

		if (!a.log.empty()) {
			t += "\nThe log before the crash:\n";
			for (const std::string& l : a.log) t += "  " + l + "\n";
		}
		return t;
	}
}
