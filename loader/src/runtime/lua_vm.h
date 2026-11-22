#pragma once
/** @file
 * @ingroup mirage_scripting
 * @brief Sandbox and execution budgets for Lua mod states.
 */
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

// Lua 5.4 for Mirage mods (built from source as C++: cmake/Libraries.cmake). One State per mod: sandboxed (no io, os,
// package, debug), with a memory limit and an instruction budget per call, so a broken script is stopped and named
// instead of taking the game with it.
namespace mirage::lua
{
	struct Result
	{
		bool ok = false;
		std::string value;		// What the chunk returned (first value, as text)
		std::string error;		// Lua's message, or why it was stopped
	};

	/// @ingroup mirage_scripting
	/// @brief Own an isolated Lua state, its memory budget, and its instruction budget.
	/// @note Serialize access to an instance externally. The mod runtime holds its Lua lock across callbacks.
	class State
	{
	public:
		/// @brief Create a Lua VM with the Mirage sandbox.
		/// @param memoryLimit Maximum Lua allocator usage in bytes.
		/// @param instructionBudget VM instructions per call. A value of zero disables the instruction budget.
		explicit State(std::size_t memoryLimit = 64u << 20, std::uint64_t instructionBudget = 50'000'000);
		~State();
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		bool Valid() const { return L_ != nullptr; }
		/// @return Borrowed Lua VM pointer, valid while this state is alive. May be null.
		lua_State* Get() const { return L_; }

		/// @brief Compile and execute a Lua chunk under the current budgets.
		/// @param code Lua source text.
		/// @param chunk Name used in Lua diagnostics.
		/// @return The first result as text, or a failed Result with a Lua error message.
		Result Run(std::string_view code, std::string_view chunk = "chunk");
		/// @brief Call the function below its arguments on the VM stack.
		/// @param nargs Number of arguments on the stack.
		/// @param nresults Number of results requested from Lua.
		/// @return Success leaves results on the stack. Failure removes the error and records it in Result.
		Result Call(int nargs, int nresults);

		std::size_t MemoryUsed() const { return used_; }
		std::size_t MemoryPeak() const { return peak_; }

	private:
		static void* Allocate(void* self, void* ptr, std::size_t oldSize, std::size_t newSize);
		static void CountHook(lua_State* L, lua_Debug* ar);

		lua_State* L_ = nullptr;
		std::size_t used_ = 0, peak_ = 0, limit_;
		std::uint64_t budget_, executed_ = 0;
	};

	// Runs the checks of Mirage step 1 in a fresh state: results, a caught error, the sandbox, C++ unwinding through a
	// Lua error, the memory limit and the instruction budget. Logged; `problems` empty when all passed.
	struct ProbeReport
	{
		bool ok = false;
		std::vector<std::string> problems;
		std::vector<std::string> notes;
		double seconds = 0.0;
	};
	ProbeReport Probe();

	const char* Version();		// "Lua 5.4.8"
}
