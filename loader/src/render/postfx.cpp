// Post-processing from mods (see postfx.h): one copy of the back buffer and one full-screen quad an effect.
#include "render/postfx.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace postfx
{
	namespace
	{
		struct Effect
		{
			std::string owner, name, source;
			bool enabled = false;
			float params[15][4]{};
			IDirect3DPixelShader9* shader = nullptr;		// Survives a Reset (not pooled)
			bool tried = false;								// Compiled, or failed: not tried again
			std::string error;
			IDirect3DDevice9* device = nullptr;				// The shader's device
		};

		std::mutex g_lock;
		std::map<int, Effect> g_effects;					// In the order made
		int g_next = 1;
		std::vector<IDirect3DPixelShader9*> g_retired;		// Of removed effects: released on the render thread

		IDirect3DTexture9* g_scene = nullptr;				// The frame's copy
		D3DSURFACE_DESC g_sceneDesc{};
		IDirect3DSurface9* g_probeTarget = nullptr;			// 1x1, for the self-test's probe
		IDirect3DSurface9* g_probeCopy = nullptr;
		std::atomic<bool> g_probeWanted{false}, g_probeDone{false};
		std::atomic<std::uint32_t> g_probeColor{0};
		std::atomic<float> g_probeX{0.5f}, g_probeY{0.5f};
		Stats g_stats;

		using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
										   ID3DBlob**, ID3DBlob**);
		CompileFn Compiler()
		{
			static CompileFn fn = [] {
				HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
				return dll ? reinterpret_cast<CompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
			}();
			return fn;
		}

		void Compile(IDirect3DDevice9* dev, Effect& e)
		{
			e.tried = true;
			e.device = dev;
			CompileFn compile = Compiler();
			if (!compile) {
				e.error = "d3dcompiler_47.dll isn't there: shaders can't be compiled";
				return;
			}
			ID3DBlob *code = nullptr, *errors = nullptr;
			const HRESULT hr = compile(e.source.data(), e.source.size(), e.name.c_str(), nullptr, nullptr, "main", "ps_3_0",
									   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (errors) {
				e.error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
				while (!e.error.empty() && (e.error.back() == '\0' || e.error.back() == '\n')) e.error.pop_back();
				errors->Release();
			}
			if (FAILED(hr) || !code) {
				if (e.error.empty()) e.error = "the shader didn't compile";
				spdlog::warn("[mod {}] postfx {}: {}", e.owner, e.name, e.error);
				if (code) code->Release();
				return;
			}
			if (FAILED(dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &e.shader))) e.error = "the device refused the shader";
			code->Release();
			if (e.shader) e.error.clear();		// Warnings aren't errors
		}

		bool SceneFor(IDirect3DDevice9* dev, IDirect3DSurface9* back, D3DSURFACE_DESC& desc)
		{
			back->GetDesc(&desc);
			if (g_scene && g_sceneDesc.Width == desc.Width && g_sceneDesc.Height == desc.Height && g_sceneDesc.Format == desc.Format) return true;
			if (g_scene) g_scene->Release();
			g_scene = nullptr;
			if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &g_scene, nullptr))) {
				g_scene = nullptr;
				return false;
			}
			g_scene->GetLevelDesc(0, &g_sceneDesc);
			g_sceneDesc.Format = desc.Format;
			return true;
		}

		// The centre pixel of the back buffer, read back (the self-test's probe)
		void ReadProbe(IDirect3DDevice9* dev, IDirect3DSurface9* back, const D3DSURFACE_DESC& desc)
		{
			if (!g_probeTarget &&
				FAILED(dev->CreateRenderTarget(1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_probeTarget, nullptr)))
				return;
			if (!g_probeCopy && FAILED(dev->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &g_probeCopy, nullptr))) return;
			const LONG x = std::clamp(static_cast<LONG>(g_probeX.load() * static_cast<float>(desc.Width)), 0L, static_cast<LONG>(desc.Width) - 1);
			const LONG y = std::clamp(static_cast<LONG>(g_probeY.load() * static_cast<float>(desc.Height)), 0L, static_cast<LONG>(desc.Height) - 1);
			const RECT centre{x, y, x + 1, y + 1};
			if (FAILED(dev->StretchRect(back, &centre, g_probeTarget, nullptr, D3DTEXF_NONE))) return;
			if (FAILED(dev->GetRenderTargetData(g_probeTarget, g_probeCopy))) return;
			D3DLOCKED_RECT lr{};
			if (FAILED(g_probeCopy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
			std::uint32_t px = 0;
			std::memcpy(&px, lr.pBits, 4);		// X8R8G8B8 / A8R8G8B8: the back buffers this game makes
			g_probeCopy->UnlockRect();
			g_probeColor = px | 0xFF000000u;
			g_probeDone = true;
			g_probeWanted = false;
		}

		struct Vertex
		{
			float x, y, z, rhw, u, v;
		};
	}

	int Create(const std::string& owner, const std::string& name, std::string source)
	{
		std::lock_guard lock(g_lock);
		const int handle = g_next++;
		Effect& e = g_effects[handle];
		e.owner = owner;
		e.name = name;
		e.source = std::move(source);
		return handle;
	}

	bool Set(const std::string& owner, int handle, int index, const float value[4])
	{
		std::lock_guard lock(g_lock);
		const auto it = g_effects.find(handle);
		if (it == g_effects.end() || it->second.owner != owner || index < 1 || index > 15) return false;
		std::memcpy(it->second.params[index - 1], value, sizeof(float) * 4);
		return true;
	}

	bool Enable(const std::string& owner, int handle, bool on)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_effects.find(handle);
		if (it == g_effects.end() || it->second.owner != owner) return false;
		it->second.enabled = on;
		return true;
	}

	std::string Error(const std::string& owner, int handle)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_effects.find(handle);
		return it == g_effects.end() || it->second.owner != owner ? std::string("no such effect") : it->second.error;
	}

	bool Compiled(const std::string& owner, int handle)
	{
		std::lock_guard lock(g_lock);
		const auto it = g_effects.find(handle);
		return it != g_effects.end() && it->second.owner == owner && it->second.shader;
	}

	void RemoveAll(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		for (auto it = g_effects.begin(); it != g_effects.end();) {
			if (it->second.owner == owner) {
				if (it->second.shader) g_retired.push_back(it->second.shader);
				it = g_effects.erase(it);
			} else {
				++it;
			}
		}
	}

	void Render(IDirect3DDevice9* dev)
	{
		std::lock_guard lock(g_lock);
		for (IDirect3DPixelShader9* s : g_retired) s->Release();
		g_retired.clear();
		const bool probe = g_probeWanted.load();
		int enabled = 0;
		for (const auto& [h, e] : g_effects) enabled += e.enabled;
		g_stats.effects = static_cast<int>(g_effects.size());
		g_stats.drawn = 0;
		if (!enabled && !probe) return;

		IDirect3DSurface9* back = nullptr;
		if (FAILED(dev->GetRenderTarget(0, &back)) || !back) return;
		D3DSURFACE_DESC desc{};
		if (enabled && SceneFor(dev, back, desc)) {
			IDirect3DStateBlock9* saved = nullptr;
			dev->CreateStateBlock(D3DSBT_ALL, &saved);
			IDirect3DSurface9* sceneSurface = nullptr;
			g_scene->GetSurfaceLevel(0, &sceneSurface);
			dev->SetVertexShader(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			dev->SetRenderState(D3DRS_ZENABLE, FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0.f, 1.f};
			dev->SetViewport(&vp);
			const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
			const Vertex quad[4] = {{-0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f}, {w - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f},
									{-0.5f, h - 0.5f, 0.f, 1.f, 0.f, 1.f}, {w - 0.5f, h - 0.5f, 0.f, 1.f, 1.f, 1.f}};
			LARGE_INTEGER now{}, freq{};
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const float frame[4] = {static_cast<float>(static_cast<double>(now.QuadPart % (freq.QuadPart * 3600)) / static_cast<double>(freq.QuadPart)),
									w, h, w / h};
			for (auto& [handle, e] : g_effects) {
				if (!e.enabled) continue;
				if (!e.tried || e.device != dev) {
					if (e.shader) e.shader->Release();
					e.shader = nullptr;
					Compile(dev, e);
				}
				if (!e.shader || !sceneSurface) continue;
				if (FAILED(dev->StretchRect(back, nullptr, sceneSurface, nullptr, D3DTEXF_NONE))) break;
				dev->SetRenderTarget(0, back);
				dev->SetTexture(0, g_scene);
				dev->SetPixelShader(e.shader);
				dev->SetPixelShaderConstantF(0, frame, 1);
				dev->SetPixelShaderConstantF(1, &e.params[0][0], 15);
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
				++g_stats.drawn;
			}
			dev->SetTexture(0, nullptr);
			if (sceneSurface) sceneSurface->Release();
			if (saved) {
				saved->Apply();
				saved->Release();
			}
			if (g_stats.drawn) ++g_stats.frames;
		} else {
			back->GetDesc(&desc);
		}
		if (probe) ReadProbe(dev, back, desc);
		back->Release();
	}

	void OnLostDevice()
	{
		std::lock_guard lock(g_lock);
		if (g_scene) g_scene->Release();
		g_scene = nullptr;
		if (g_probeTarget) g_probeTarget->Release();
		g_probeTarget = nullptr;
		if (g_probeCopy) g_probeCopy->Release();		// System memory: kept would be fine, but the format may change
		g_probeCopy = nullptr;
	}

	Stats GetStats()
	{
		std::lock_guard lock(g_lock);
		return g_stats;
	}

	void Probe(float x, float y)
	{
		g_probeX = x;
		g_probeY = y;
		g_probeDone = false;
		g_probeWanted = true;
	}

	bool ProbeResult(std::uint32_t* argb)
	{
		if (!g_probeDone.load()) return false;
		if (argb) *argb = g_probeColor.load();
		return true;
	}
}
