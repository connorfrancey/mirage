#pragma once
// Numbers from text with fast_float: exact (the nearest float, like strtod), much faster, and independent of the C
// locale (std::strtof reads "1.5" as 1 where the locale's decimal point is a comma). Header only.

#include <fast_float/fast_float.h>

#include <optional>
#include <string_view>
#include <system_error>

namespace util
{
	// The whole of `text` (spaces around it allowed, a leading '+' too) as a number; nothing if it isn't one
	template <class T>
	std::optional<T> ParseNumber(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
			text.remove_suffix(1);
		if (!text.empty() && text.front() == '+') text.remove_prefix(1);
		T value{};
		const auto [end, ec] = fast_float::from_chars(text.data(), text.data() + text.size(), value);
		if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
		return value;
	}

	inline std::optional<float> ParseFloat(std::string_view text) { return ParseNumber<float>(text); }
	inline std::optional<double> ParseDouble(std::string_view text) { return ParseNumber<double>(text); }
	// `fallback` when it isn't a number
	inline float ParseFloatOr(std::string_view text, float fallback) { return ParseFloat(text).value_or(fallback); }
}
