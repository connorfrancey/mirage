#pragma once
// JSON through glaze: the dynamic value every file uses (jc::json) and the few helpers nlohmann had as members.
// jc::json is glz::generic_i64: object keys keep their insertion order (files written by hand stay readable) and
// integers stay int64 apart from doubles (a Lua mod gets 3, not 3.0).
#include <glaze/glaze.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace jc
{
	using json = glz::generic_i64;
	using array = json::array_t;
	using object = json::object_t;

	inline constexpr glz::opts kCompact{};
	struct PrettyOpts : glz::opts
	{
		std::uint8_t indentation_width = 2;   // As nlohmann's dump(2) wrote: existing files keep their layout
	};
	inline constexpr PrettyOpts kPretty = [] {
		PrettyOpts o{};
		o.prettify = true;
		return o;
	}();
	inline constexpr glz::opts kLenient{.comments = true, .error_on_unknown_keys = false};

	// The parsed value, or null when the text is not JSON (comments allowed)
	inline json parse(std::string_view text)
	{
		json j;
		if (glz::read<kLenient>(j, text)) return {};
		return j;
	}
	// The same, with the parse error's text in `why`
	inline json parse(std::string_view text, std::string& why)
	{
		json j;
		if (const auto ec = glz::read<kLenient>(j, text)) {
			why = glz::format_error(ec, text);
			return {};
		}
		return j;
	}

	// Structs glaze reflects need external linkage with MSVC: declare them at namespace scope in a named namespace
	// (a file's `detail`), not in a function or an anonymous namespace.

	// Text into a struct glaze reflects (unknown keys skipped); false when the text doesn't fit
	template <class T>
	bool read(std::string_view text, T& out)
	{
		return !glz::read<kLenient>(out, text);
	}

	// Any value glaze can write (a json, or a struct with glz::meta) as text
	template <class T>
	std::string dump(const T& v, bool pretty = false)
	{
		std::string out;
		if (pretty) (void)glz::write<kPretty>(v, out);
		else (void)glz::write<kCompact>(v, out);
		return out;
	}

	inline json new_object() { return object{}; }
	inline json new_array() { return array{}; }

	// Appends to an array; a null value becomes an array first
	inline json& push(json& arr, json v)
	{
		if (arr.is_null()) arr = array{};
		return arr.get_array().emplace_back(std::move(v));
	}

	// The member, or null when j is not an object or lacks it
	inline const json& member(const json& j, std::string_view key)
	{
		static const json kNull;
		if (!j.is_object()) return kNull;
		const auto& o = j.get_object();
		const auto it = o.find(key);
		return it == o.end() ? kNull : it->second;
	}

	inline bool contains(const json& j, std::string_view key) { return j.is_object() && j.contains(key); }

	// A json as T, or `fallback` when its type doesn't fit. Numbers convert between int and double.
	template <class T>
	T as(const json& j, T fallback = {})
	{
		if constexpr (std::is_same_v<T, json>) {
			return j;
		} else if constexpr (std::is_same_v<T, bool>) {
			const auto* b = j.get_if<bool>();
			return b ? *b : fallback;
		} else if constexpr (std::is_arithmetic_v<T>) {
			return j.is_number() ? j.as<T>() : fallback;
		} else if constexpr (std::is_constructible_v<T, const std::string&>) {
			const auto* s = j.get_if<std::string>();
			return s ? T(*s) : fallback;
		} else {
			static_assert(!sizeof(T), "jc::as: unsupported type");
		}
	}

	// nlohmann's j.value("key", fallback): the member as T, or the fallback when it is missing or of another type
	template <class T>
	T value(const json& j, std::string_view key, T fallback = {})
	{
		const json& m = member(j, key);
		if constexpr (std::is_same_v<T, json>) return m.is_null() ? fallback : m;
		else return as<T>(m, std::move(fallback));
	}
	inline std::string value(const json& j, std::string_view key, const char* fallback)
	{
		return value<std::string>(j, key, fallback);
	}

	// The member's array (empty when missing or not an array), for range-for loops
	inline const array& items(const json& j, std::string_view key)
	{
		static const array kEmpty;
		const json& m = member(j, key);
		return m.is_array() ? m.get_array() : kEmpty;
	}
	inline const array& items(const json& j)
	{
		static const array kEmpty;
		return j.is_array() ? j.get_array() : kEmpty;
	}
}
