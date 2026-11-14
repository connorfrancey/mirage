#pragma once
#include <optional>

#include "crashhandler/analysis.h"

// Analyzers: each looks at a crash's Analysis and may name a likely cause ("Possible causes" in the report and the
// crash window). They only read the Analysis (no game memory, no Windows calls): whatever they need is collected into
// it first (crashhandler/collect.cpp), which keeps them safe in a crashed process and testable without the game.
//
// The built-in ones are in builtin.cpp. Another file adds its own with a static registration:
//
//     namespace { const bool kRegistered = crashhandler::RegisterAnalyzer(&MyAnalyzer); }
namespace crashhandler
{
	using Analyzer = std::optional<Finding> (*)(const Analysis&);

	bool RegisterAnalyzer(Analyzer analyzer);	// Always true (for the static registration above)

	// Every analyzer's finding, in registration order (built-in ones first)
	std::vector<Finding> RunAnalyzers(const Analysis& a);
}
