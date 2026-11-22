#pragma once
/** @file
 * @ingroup mirage_scripting
 * @brief Verified reflected invocation and parameter buffers for UE3.
 * @note Game thread only.
 */
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/layout.h"

namespace Classes
{
	class UObject;
	class UFunction;
	class UClass;
}

// Calling the game's UnrealScript and native functions with their parameters laid out the way the engine laid them
// out (Mirage, step 2). The SDK's generated wrappers get this wrong: their parameter structs use 1-byte bools where the
// engine has 4-byte UBOOLs (so values and return values land in the wrong place), and ProcessEvent refuses every native
// function that has a native index (IsInState, SetLocation, ...), which the SDK then reports as an uninitialised
// return value.
//
//     mirage::call::Params p(pawn, "SetMove");
//     p.Set("NewMove", std::uint8_t{1});
//     p.Set("bViaReplication", false);
//     if (mirage::call::Call(pawn, p)) bool moved = p.GetBool("ReturnValue");
//
// Game thread only.
namespace mirage::call
{
	// The function `name` of `object`'s class (or a class it extends), nullptr if there is none.
	Classes::UFunction* FindFunction(Classes::UObject* object, std::string_view name);

	// The native index of `fn` (0: none, ProcessEvent runs it). -1 when the UFunction layout didn't check out.
	int NativeIndex(Classes::UFunction* fn);

	/// @ingroup mirage_scripting
	/// @brief Own a buffer laid out for a reflected function's input and output properties.
	/// @note The underlying UFunction is borrowed from the engine.
	class Params
	{
	public:
		Params(Classes::UObject* object, std::string_view function);
		explicit Params(Classes::UFunction* fn);

		bool Valid() const { return fn_ != nullptr; }
		Classes::UFunction* Function() const { return fn_; }
		const std::vector<layout::Property>& Parameters() const { return params_; }
		const std::string& Error() const { return error_; }

		// False (and Error() says why) when there is no such parameter or it has another type
		bool Set(std::string_view name, bool value);
		bool Set(std::string_view name, std::int32_t value);
		bool Set(std::string_view name, float value);
		bool Set(std::string_view name, std::uint8_t value);		// Byte or enum
		bool Set(std::string_view name, Classes::UObject* value);	// Object, Class, Component
		bool SetName(std::string_view name, const char* value);		// Name
		bool Set(std::string_view name, const char* value) = delete;	// Would be a bool: SetName for names
		bool SetRaw(std::string_view name, const void* data, std::uint32_t size);	// A struct, whole

		bool GetBool(std::string_view name) const;
		std::int32_t GetInt(std::string_view name) const;
		float GetFloat(std::string_view name) const;
		std::uint8_t GetByte(std::string_view name) const;
		Classes::UObject* GetObject(std::string_view name) const;
		bool GetRaw(std::string_view name, void* out, std::uint32_t size) const;

		std::uint8_t* Data() { return buffer_.data(); }

	private:
		const layout::Property* Find(std::string_view name, std::initializer_list<layout::Kind> kinds, bool forWrite);
		const layout::Property* Find(std::string_view name, std::initializer_list<layout::Kind> kinds) const;

		Classes::UFunction* fn_ = nullptr;
		std::vector<layout::Property> params_;
		std::vector<std::uint8_t> buffer_;
		mutable std::string error_;
	};

	/// @brief Invoke the reflected function on the game thread.
	/// @param object Borrowed engine object to receive the call.
	/// @param[in,out] params Parameter buffer receiving the out values and return value.
	/// @param[out] error Optional reason for a rejected invocation.
	/// @return True when the engine call ran.
	bool Call(Classes::UObject* object, Params& params, std::string* error = nullptr);

	// The UFunction layout is what this code expects (native indexes where they belong). Logged once. Game thread.
	bool LayoutChecked(std::string* why = nullptr);
}
