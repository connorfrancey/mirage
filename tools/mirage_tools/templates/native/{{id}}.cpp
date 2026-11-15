// {{name}}: a native Mirage mod. Built by its own CMake project (CMakeLists.txt) against the mod API headers only,
// with any compiler that targets 32-bit Windows: `mirage mod build`, or cmake from an x86 environment. The API:
// docs/api-reference.md and mirage/mirage.hpp.
#include <mirage/mirage.hpp>

#include <string>

namespace
{
	class {{class}} final : public mirage::Module
	{
	public:
		{{class}}() : Module("{{name}}", "Mods", "{{description}}")
		{
			// A test the game runs with `mirage mod test`: "" when it passed, else why it failed
			mirage::Test("ticks while on", [this] { return ticks.Get() > 0 ? std::string() : "no tick was counted with the module on"; });
		}

		void OnEnable() override { mirage::Info("on"); }
		// Undo here whatever OnEnable and OnTick changed in the game
		void OnDisable() override { mirage::Info("off"); }

		// The game thread, every tick while the module is on
		void OnTick() override
		{
			ticks.Set(ticks.Get() + 1);
			if (const mirage::Object pawn = mirage::Object::Pawn())
				height.Set(pawn.Get<float>("Location.Z") * mirage::Get<float>("speed"));
		}

	private:
		mirage::SliderInt ticks{this, "Ticks", 0, 0, 1 << 30, "Game ticks counted while on"};
		mirage::Slider height{this, "Height", 0.f, -1e6f, 1e6f, "%.0f", "The player's height times Speed"};
	};
	MIRAGE_MODULE({{class}})

	// More: mirage::Hook("Engine.Actor.TakeDamage", ...) runs code when a script function runs; mirage::DetourSymbol
	// detours the engine's native functions by name (runtime/symbols.toml lists them); mirage::Actors finds actors.
}

MIRAGE_MOD()
