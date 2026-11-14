#include "crashhandler/memory_text.h"

#include <Zydis/Zydis.h>

#include <cstdio>

namespace crashhandler
{
	namespace
	{
		bool Printable(std::uint32_t c) { return (c >= 0x20 && c < 0x7F) || c == '\t'; }

		std::string Quote(const std::string& s, bool cut)
		{
			std::string out = "\"";
			for (char c : s) out += c == '"' ? std::string("\\\"") : c == '\t' ? std::string("\\t") : std::string(1, c);
			return out + (cut ? "...\"" : "\"");
		}
	}

	std::string TextAt(const std::uint8_t* bytes, std::size_t size, std::size_t maxChars)
	{
		if (!bytes || size < 4) return {};
		// 8-bit
		std::size_t n = 0;
		while (n < size && Printable(bytes[n])) ++n;
		if (n >= 4 && (n == size || bytes[n] == 0)) {
			const bool cut = n > maxChars;
			return Quote(std::string(reinterpret_cast<const char*>(bytes), cut ? maxChars : n), cut);
		}
		// UTF-16: printable code units with a zero high byte (the game's FStrings are wide)
		std::size_t chars = 0;
		while (chars * 2 + 1 < size && bytes[chars * 2 + 1] == 0 && Printable(bytes[chars * 2])) ++chars;
		const bool ended = chars * 2 + 1 >= size || (bytes[chars * 2] == 0 && bytes[chars * 2 + 1] == 0);
		if (chars >= 4 && ended) {
			std::string s;
			const bool cut = chars > maxChars;
			for (std::size_t i = 0; i < (cut ? maxChars : chars); ++i) s += static_cast<char>(bytes[i * 2]);
			return "L" + Quote(s, cut);
		}
		return {};
	}

	std::vector<DisasmLine> Disassemble(const std::uint8_t* code, std::size_t size, std::uint32_t codeAddress,
										std::size_t currentOffset, int before, int after,
										const std::function<std::string(std::uint32_t)>& name)
	{
		std::vector<DisasmLine> best;
		if (!code || currentOffset >= size) return best;
		// The earliest start that decodes straight onto the current instruction gives the most lines before it
		const std::size_t earliest = currentOffset > 48 ? currentOffset - 48 : 0;
		for (std::size_t start = earliest; start <= currentOffset; ++start) {
			std::vector<DisasmLine> lines;
			std::size_t at = start;
			bool landed = false;
			int afterLeft = after;
			while (at < size) {
				ZydisDisassembledInstruction ins;
				if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_COMPAT_32, codeAddress + at, code + at,
														size - at, &ins)))
					break;
				DisasmLine line;
				line.address = static_cast<std::uint32_t>(codeAddress + at);
				for (std::size_t i = 0; i < ins.info.length; ++i) {
					char b[4];
					std::snprintf(b, sizeof b, i ? " %02X" : "%02X", code[at + i]);
					line.bytes += b;
				}
				line.text = ins.text;
				if (name && (ins.info.meta.category == ZYDIS_CATEGORY_CALL || ins.info.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
							 ins.info.meta.category == ZYDIS_CATEGORY_COND_BR)) {
					ZyanU64 target = 0;
					if (ins.info.operand_count_visible > 0 &&
						ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins.info, &ins.operands[0], codeAddress + at, &target))) {
						const std::string n = name(static_cast<std::uint32_t>(target));
						if (!n.empty()) line.text += "  ; " + n;
					}
				}
				line.current = at == currentOffset;
				if (at == currentOffset) landed = true;
				if (at > currentOffset && !landed) break;
				lines.push_back(std::move(line));
				at += ins.info.length;
				if (landed && at > currentOffset && --afterLeft < 0) break;
			}
			if (!landed) continue;
			// Keep `before` lines before the current one
			std::size_t cur = 0;
			while (!lines[cur].current) ++cur;
			if (cur > static_cast<std::size_t>(before)) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(cur - before));
			return lines;
		}
		// Nothing lands on it: the current instruction alone
		ZydisDisassembledInstruction ins;
		if (ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_COMPAT_32, codeAddress + currentOffset,
											   code + currentOffset, size - currentOffset, &ins))) {
			DisasmLine line;
			line.address = static_cast<std::uint32_t>(codeAddress + currentOffset);
			line.text = ins.text;
			line.current = true;
			best.push_back(line);
		}
		return best;
	}
}
