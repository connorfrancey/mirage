#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "crashhandler/analysis.h"

// The parts of reading crash memory that need no game: recognizing text, and disassembling code. The caller copies
// the bytes out of the process first (crashhandler/collect.cpp reads them guarded), so these only see buffers.
namespace crashhandler
{
	// The text at the start of `bytes` when it looks like a string: at least 4 printable characters ending in a NUL
	// (or filling the buffer), as 8-bit text or UTF-16. Quoted and cut to `maxChars` ("\"Edge_p\""); "" when it isn't.
	std::string TextAt(const std::uint8_t* bytes, std::size_t size, std::size_t maxChars = 48);

	// Instructions of 32-bit x86 code around `current`, which is `codeAddress + currentOffset`: decoding starts at an
	// offset before it that lands exactly on it (x86 can't be decoded backwards), and goes on `after` instructions
	// past it. `name` gives a call or jump target's name ("" for none). Empty when nothing decodes.
	std::vector<DisasmLine> Disassemble(const std::uint8_t* code, std::size_t size, std::uint32_t codeAddress,
										std::size_t currentOffset, int before, int after,
										const std::function<std::string(std::uint32_t)>& name = {});
}
