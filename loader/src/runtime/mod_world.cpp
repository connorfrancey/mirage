// The mod API's 3D drawing in the world and the camera: the runtime's debug renderer (render/debug_renderer) as a service.
#include "runtime/mod_world.h"
#include <vector>

#include "game/game.h"
#include "render/debug_renderer.h"
#include "runtime/mod_internal.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace mirage::mods::world
{
	namespace
	{
		std::atomic<MirageMod*> g_owner{nullptr};		// The mod that switched the drawing on
		std::atomic<bool> g_active{false};
		std::atomic<bool> g_cameraWanted{false};		// A mod asked for the camera: it is followed from then on
		std::atomic<bool> g_modsDrawing{false};		// Some mod has lines of its own (world_draw_lines)

		// The renderer's moving layer is world_draw_set's MIRAGE_WORLD_MOVING plus every mod's own lines, merged: a mod
		// clearing its lines leaves the others' alone
		std::mutex g_movingLock;
		rmesh::MeshData g_moving;
		std::map<MirageMod*, std::vector<rmesh::Vertex>> g_modLines;

		void PublishMoving()	// g_movingLock held
		{
			rmesh::MeshData m = g_moving;
			for (const auto& [mod, lines] : g_modLines) m.lines.insert(m.lines.end(), lines.begin(), lines.end());
			debugrender::SetDynamic(std::move(m));
		}

		void Append(std::vector<rmesh::Vertex>& out, const MirageWorldVertex* v, uint32_t n)
		{
			if (!v) return;
			out.reserve(out.size() + n);
			for (uint32_t i = 0; i < n; ++i) out.push_back({v[i].x, v[i].y, v[i].z, v[i].color});
		}

		// Game thread, every tick while on: the renderer follows the game's camera
		void Follow(float)
		{
			if (!g_active.load(std::memory_order_relaxed) && !g_modsDrawing.load(std::memory_order_relaxed)) return;
			debugrender::SetCamera(game::GetCamera());
			debugrender::SetEngineNear(game::EngineNear());
		}
	}

	// The renderer on while the drawing's owner has it on or any mod has lines, and following the camera then
	void UpdateActive()
	{
		static const bool registered = [] {
			game::OnTick(&Follow);
			return true;
		}();
		(void)registered;
		const bool drawing = g_active.load() || g_modsDrawing.load();
		debugrender::SetActive(drawing);
		game::SetCameraTracking(drawing || g_cameraWanted.load());
	}

	void MIRAGE_CALL DrawActive(MirageMod* mod, int32_t on)
	{
		g_active = on != 0;
		g_owner = on ? mod : nullptr;
		if (!on) {
			debugrender::SetScene(nullptr);
			debugrender::SetStatic(nullptr);
			std::lock_guard lock(g_movingLock);
			g_moving = {};
			PublishMoving();		// The mods' own lines stay
		}
		UpdateActive();
	}

	void MIRAGE_CALL DrawLines(MirageMod* mod, const MirageWorldVertex* lines, uint32_t count)
	{
		if (!mod) return;
		{
			std::lock_guard lock(g_movingLock);
			if (lines && count >= 2) {
				std::vector<rmesh::Vertex>& mine = g_modLines[mod];
				mine.clear();
				Append(mine, lines, count - count % 2);
			} else if (!g_modLines.erase(mod)) {
				return;		// Had none: nothing changes (a mod clearing every tick costs nothing)
			}
			PublishMoving();
			g_modsDrawing = !g_modLines.empty();
		}
		UpdateActive();
	}

	void MIRAGE_CALL DrawSet(int32_t layer, const MirageWorldMesh* mesh)
	{
		const bool any = mesh && mesh->size >= sizeof(MirageWorldMesh);
		switch (layer) {
			case MIRAGE_WORLD_SCENE: {
				if (!any) {
					debugrender::SetScene(nullptr);
					return;
				}
				auto scene = std::make_shared<debugrender::Scene>();
				Append(scene->mesh.tris, mesh->triangles, mesh->triangle_vertices);
				Append(scene->mesh.lines, mesh->lines, mesh->line_vertices);
				if (mesh->labels)
					for (uint32_t i = 0; i < mesh->label_count; ++i) {
						const MirageWorldLabel& l = mesh->labels[i];
						scene->labels.push_back({{l.x, l.y, l.z}, l.text ? l.text : "", l.color});
					}
				scene->shapes = mesh->shapes;
				scene->buildMs = mesh->build_ms;
				debugrender::SetScene(std::move(scene));
				return;
			}
			case MIRAGE_WORLD_MOVING: {
				std::lock_guard lock(g_movingLock);
				g_moving = {};
				if (any) {
					Append(g_moving.tris, mesh->triangles, mesh->triangle_vertices);
					Append(g_moving.lines, mesh->lines, mesh->line_vertices);
				}
				PublishMoving();
				return;
			}
			case MIRAGE_WORLD_STATIC: {
				if (!any) {
					debugrender::SetStatic(nullptr);
					return;
				}
				auto m = std::make_shared<rmesh::MeshData>();
				Append(m->tris, mesh->triangles, mesh->triangle_vertices);
				Append(m->lines, mesh->lines, mesh->line_vertices);
				debugrender::SetStatic(std::move(m));
				return;
			}
			default: return;
		}
	}

	void MIRAGE_CALL DrawStyle(const MirageWorldStyle* style)
	{
		if (!style || style->size < sizeof(MirageWorldStyle)) return;
		debugrender::Style s;
		s.opacity = style->opacity;
		s.depth = style->overlay ? debugrender::Depth::Overlay : debugrender::Depth::Scene;
		s.fovOffset = style->fov_offset;
		s.aspect = style->aspect;
		s.nearPlane = style->near_plane;
		s.staticOpacity = style->static_opacity;
		debugrender::SetStyle(s);
	}

	void MIRAGE_CALL DrawStats(MirageWorldStats* out)
	{
		if (!out || out->size < sizeof(MirageWorldStats)) return;
		const debugrender::Stats st = debugrender::GetStats();
		*out = MirageWorldStats{sizeof(MirageWorldStats)};
		out->drawn = st.drawn ? 1 : 0;
		out->in_scene = st.path == debugrender::Path::InScene ? 1 : 0;
		out->overlay_fallback = st.overlayFallback ? 1 : 0;
		out->triangles = static_cast<uint32_t>(st.triangles);
		out->lines = static_cast<uint32_t>(st.lines);
		out->shapes = static_cast<uint32_t>(st.shapes);
		out->labels = static_cast<uint32_t>(st.labels);
		out->static_triangles = static_cast<uint32_t>(st.staticTriangles);
		out->static_lines = static_cast<uint32_t>(st.staticLines);
		out->build_ms = static_cast<float>(st.buildMs);
	}

	int32_t MIRAGE_CALL WorldToScreen(float x, float y, float z, float* sx, float* sy)
	{
		float a = 0.f, b = 0.f;
		const bool ok = debugrender::WorldToScreen({x, y, z}, a, b);
		if (sx) *sx = a;
		if (sy) *sy = b;
		return ok ? 1 : 0;
	}

	int32_t MIRAGE_CALL CameraGet(MirageCamera* out)
	{
		if (!out || out->size < sizeof(MirageCamera)) return 0;
		if (!g_cameraWanted.exchange(true)) game::SetCameraTracking(true);	// Valid from the next tick
		const rmath::Camera c = game::GetCamera();
		*out = MirageCamera{sizeof(MirageCamera), c.pos.x, c.pos.y, c.pos.z, c.pitch, c.yaw, c.roll, c.fov};
		return c.valid ? 1 : 0;
	}

	void Unloading(MirageMod* mod)
	{
		if (mod && g_owner.load() == mod) DrawActive(nullptr, 0);
		DrawLines(mod, nullptr, 0);
	}
}
