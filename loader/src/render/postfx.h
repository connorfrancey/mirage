#pragma once
// Post-processing from mods (the plan's step 31): full-screen effects written as HLSL pixel shaders. Each frame, in
// EndScene on the back buffer and before the menu draws, every enabled effect in the order made copies the frame to a
// texture and draws it back through its shader. So an effect sees the game's whole frame, its HUD included, and the
// menu, the debug renderer and later effects come on top.
//
// A shader is ps_3_0 with the entry point `main(float2 uv : TEXCOORD0) : COLOR0`, compiled when first drawn (with
// d3dcompiler_47.dll, part of Windows). Its inputs:
//
//     sampler2D Scene : register(s0);    // the frame so far (linear, clamped)
//     float4 Frame : register(c0);       // seconds (the render thread's clock), width, height, width / height
//     float4 Mine : register(c1);        // c1 to c15: the mod's parameters (Set)
//
// Effects are registered from any thread and drawn on the render thread.
#include <cstdint>
#include <string>

struct IDirect3DDevice9;

namespace postfx
{
	// An effect of `owner` from HLSL source (`name` for errors: its file). A handle above 0.
	int Create(const std::string& owner, const std::string& name, std::string source);
	// c`index` (1 to 15) of the effect's shader. False for another owner's effect or an index out of range.
	bool Set(const std::string& owner, int handle, int index, const float value[4]);
	bool Enable(const std::string& owner, int handle, bool on);
	// The compile error, "" when there is none (or it isn't compiled yet)
	std::string Error(const std::string& owner, int handle);
	bool Compiled(const std::string& owner, int handle);
	// Every effect of `owner` gone: the mod unloads
	void RemoveAll(const std::string& owner);

	// Render thread: the enabled effects, drawn on the back buffer (EndScene, before the menu)
	void Render(IDirect3DDevice9* dev);
	// Before the device's Reset: its textures go (they are made again on the next frame)
	void OnLostDevice();

	// Effects drawn on the last frame, and frames drawn with any
	struct Stats
	{
		int effects = 0, drawn = 0;
		unsigned long long frames = 0;
	};
	Stats GetStats();
	// For the self-test: the back buffer's centre pixel after the next frame's effects, as 0xAARRGGBB (before the menu
	// draws). Probe() asks for it; ProbeResult() is true once it came.
	void Probe();
	bool ProbeResult(std::uint32_t* argb);
}
