// Lua mods: Physics, world queries, triggers, pieces, movers, moves, animation, sound, post-processing, the options menu (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
	// ---- Physics and world queries (step 6): vectors are {x, y, z} (or {1, 2, 3}) -------------------------------------
	namespace
	{
		bool ReadVec(lua_State* L, int index, float out[3])
		{
			if (!lua_istable(L, index)) return false;
			index = lua_absindex(L, index);
			static const char* keys[] = {"x", "y", "z"};
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, index, keys[i]);
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_rawgeti(L, index, i + 1);
				}
				out[i] = static_cast<float>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			return true;
		}
		void PushVec(lua_State* L, const float v[3]) { lua::PushVec(L, v[0], v[1], v[2]); }	// A mirage.vec
		void CheckVec(lua_State* L, int index, float out[3])
		{
			if (!ReadVec(L, index, out)) luaL_typeerror(L, index, "vector {x, y, z}");
		}
	}

	// mirage.body(actor): {location, rotation = {pitch, yaw, roll}, velocity, angular, mass_scale, gravity_scale, awake,
	// simulated}, or nil
	int LuaMod::BodyFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_body_get)) return luaL_error(L, "mirage.body: this runtime has no physics");
		MirageBody b{};
		b.size = sizeof b;
		if (!self.api_->physics_body_get(CheckObject(L, 1), &b)) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 8);
		PushVec(L, b.location);
		lua_setfield(L, -2, "location");
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, b.rotation[0]);
		lua_setfield(L, -2, "pitch");
		lua_pushinteger(L, b.rotation[1]);
		lua_setfield(L, -2, "yaw");
		lua_pushinteger(L, b.rotation[2]);
		lua_setfield(L, -2, "roll");
		lua_setfield(L, -2, "rotation");
		PushVec(L, b.linear_velocity);
		lua_setfield(L, -2, "velocity");
		PushVec(L, b.angular_velocity);
		lua_setfield(L, -2, "angular");
		lua_pushnumber(L, b.mass_scale);
		lua_setfield(L, -2, "mass_scale");
		lua_pushnumber(L, b.gravity_scale);
		lua_setfield(L, -2, "gravity_scale");
		lua_pushboolean(L, b.awake);
		lua_setfield(L, -2, "awake");
		lua_pushboolean(L, b.simulated);
		lua_setfield(L, -2, "simulated");
		return 1;
	}

	// mirage.set_body(actor, {location =, rotation =, velocity =, angular =, gravity_scale =, awake =}): only the fields
	// given. True when done. (Mass: mirage.spawn_prop's.)
	int LuaMod::SetBodyFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_body_set)) return luaL_error(L, "mirage.set_body: this runtime has no physics");
		MirageObject* actor = CheckObject(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		MirageBody b{};
		b.size = sizeof b;
		uint32_t fields = 0;
		lua_getfield(L, 2, "location");
		if (ReadVec(L, -1, b.location)) fields |= MIRAGE_BODY_LOCATION;
		lua_pop(L, 1);
		lua_getfield(L, 2, "rotation");
		if (lua_istable(L, -1)) {
			static const char* keys[] = {"pitch", "yaw", "roll"};
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, -1, keys[i]);
				b.rotation[i] = static_cast<int32_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
			fields |= MIRAGE_BODY_ROTATION;
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "velocity");
		if (ReadVec(L, -1, b.linear_velocity)) fields |= MIRAGE_BODY_LINEAR;
		lua_pop(L, 1);
		lua_getfield(L, 2, "angular");
		if (ReadVec(L, -1, b.angular_velocity)) fields |= MIRAGE_BODY_ANGULAR;
		lua_pop(L, 1);
		lua_getfield(L, 2, "mass_scale");		// Refused by the runtime (with the reason in the log): mass is spawn_prop's
		if (lua_isnumber(L, -1)) {
			b.mass_scale = static_cast<float>(lua_tonumber(L, -1));
			fields |= 16u;
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "gravity_scale");
		if (lua_isnumber(L, -1)) {
			b.gravity_scale = static_cast<float>(lua_tonumber(L, -1));
			fields |= MIRAGE_BODY_GRAVITY;
		}
		lua_pop(L, 1);
		lua_getfield(L, 2, "awake");
		if (!lua_isnil(L, -1)) {
			b.awake = lua_toboolean(L, -1);
			fields |= MIRAGE_BODY_AWAKE;
		}
		lua_pop(L, 1);
		lua_pushboolean(L, self.api_->physics_body_set(self.mod_, actor, &b, fields) != 0);
		return 1;
	}

	// mirage.impulse(actor, {x, y, z}, at, velocity_change) / mirage.force(actor, {x, y, z}, at): at nil: the centre
	int LuaMod::ImpulseFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_impulse)) return luaL_error(L, "mirage.impulse: this runtime has no physics");
		MirageObject* actor = CheckObject(L, 1);
		float v[3], at[3];
		CheckVec(L, 2, v);
		const bool hasAt = ReadVec(L, 3, at);
		lua_pushboolean(L, self.api_->physics_impulse(self.mod_, actor, v, hasAt ? at : nullptr, lua_toboolean(L, 4)) != 0);
		return 1;
	}
	int LuaMod::ForceFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_force)) return luaL_error(L, "mirage.force: this runtime has no physics");
		MirageObject* actor = CheckObject(L, 1);
		float v[3], at[3];
		CheckVec(L, 2, v);
		const bool hasAt = ReadVec(L, 3, at);
		lua_pushboolean(L, self.api_->physics_force(self.mod_, actor, v, hasAt ? at : nullptr) != 0);
		return 1;
	}

	// mirage.spawn_prop(mesh, {x, y, z}, {pitch, yaw, roll}, mass_scale): a physics prop (destroyed on revert and unload),
	// or nil. mass_scale: times the mesh's mass (default 1), given here only
	int LuaMod::SpawnPropFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_spawn)) return luaL_error(L, "mirage.spawn_prop: this runtime has no physics");
		const char* mesh = luaL_checkstring(L, 1);
		float at[3];
		CheckVec(L, 2, at);
		int32_t rot[3] = {};
		bool hasRot = false;
		if (lua_istable(L, 3)) {
			static const char* keys[] = {"pitch", "yaw", "roll"};
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, 3, keys[i]);
				rot[i] = static_cast<int32_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
			hasRot = true;
		}
		const float mass = static_cast<float>(luaL_optnumber(L, 4, 1.0));
		MirageObject* prop = self.api_->physics_spawn(self.mod_, mesh, at, hasRot ? rot : nullptr, mass);
		if (prop) PushObject(L, prop);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.model("models/x.glb"): the mod's model as a game StaticMesh (for an actor's SetStaticMesh), or nil and
	// "pending" while it is built (ask again on a later tick), or nil (it can't be: the log says why). Game thread.
	int LuaMod::ModelFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, model_mesh)) return luaL_error(L, "mirage.model: this runtime has no mod models");
		int32_t pending = 0;
		MirageObject* mesh = self.api_->model_mesh(self.mod_, luaL_checkstring(L, 1), &pending);
		if (mesh) {
			PushObject(L, mesh);
			return 1;
		}
		lua_pushnil(L);
		if (!pending) return 1;
		lua_pushstring(L, "pending");
		return 2;
	}

	// mirage.spawn(class, {x, y, z}, {pitch, yaw, roll}): an actor (destroyed on revert and unload), or nil. class: the
	// mod's "classes/X.uc", another mod's "mirage-ai:classes/MirageBot.uc", or "Engine.Trigger"
	int LuaMod::SpawnFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, actor_spawn)) return luaL_error(L, "mirage.spawn: this runtime can't spawn actors");
		const char* cls = luaL_checkstring(L, 1);
		float at[3];
		CheckVec(L, 2, at);
		int32_t rot[3] = {};
		bool hasRot = false;
		if (lua_istable(L, 3)) {
			static const char* keys[] = {"pitch", "yaw", "roll"};
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, 3, keys[i]);
				rot[i] = static_cast<int32_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
			hasRot = true;
		}
		MirageObject* a = self.api_->actor_spawn(self.mod_, cls, at, hasRot ? rot : nullptr);
		if (a) PushObject(L, a);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.sound_play("sounds/x.wav", {volume = 1, loop = false}): a handle, or nil (the log says why). A WAV of the
	// mod's folder; stopped when the mod unloads
	int LuaMod::SoundPlayFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, sound_play)) return luaL_error(L, "mirage.sound_play: this runtime has no sound");
		const char* path = luaL_checkstring(L, 1);
		float volume = 1.f;
		uint32_t flags = 0;
		if (lua_istable(L, 2)) {
			lua_getfield(L, 2, "volume");
			if (lua_isnumber(L, -1)) volume = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
			lua_getfield(L, 2, "loop");
			if (lua_toboolean(L, -1)) flags |= MIRAGE_SOUND_LOOP;
			lua_pop(L, 1);
		}
		const int32_t handle = self.api_->sound_play(self.mod_, path, volume, flags);
		if (handle > 0) lua_pushinteger(L, handle);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.sound_stop(handle); mirage.sound_playing(handle): true while it plays
	int LuaMod::SoundStopFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (MIRAGE_API_HAS(self.api_, sound_stop) && lua_isinteger(L, 1))
			self.api_->sound_stop(self.mod_, static_cast<int32_t>(lua_tointeger(L, 1)));
		return 0;
	}
	int LuaMod::SoundPlayingFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushboolean(L, MIRAGE_API_HAS(self.api_, sound_playing) && lua_isinteger(L, 1) &&
							   self.api_->sound_playing(self.mod_, static_cast<int32_t>(lua_tointeger(L, 1))) == 1);
		return 1;
	}

	// mirage.postfx("shaders/x.hlsl"): an effect's handle, or nil. mirage.postfx_set(h, index, x, y, z, w) sets c<index>
	// (1 to 15); mirage.postfx_enable(h, on); mirage.postfx_error(h): the compile error, "" when none
	int LuaMod::PostfxFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, postfx_create)) return luaL_error(L, "mirage.postfx: this runtime has no post-processing");
		const int32_t h = self.api_->postfx_create(self.mod_, luaL_checkstring(L, 1));
		if (h > 0) lua_pushinteger(L, h);
		else lua_pushnil(L);
		return 1;
	}
	int LuaMod::PostfxSetFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, postfx_set)) return 0;
		const float v[4] = {static_cast<float>(luaL_optnumber(L, 3, 0.0)), static_cast<float>(luaL_optnumber(L, 4, 0.0)),
							static_cast<float>(luaL_optnumber(L, 5, 0.0)), static_cast<float>(luaL_optnumber(L, 6, 0.0))};
		lua_pushboolean(L, self.api_->postfx_set(self.mod_, static_cast<int32_t>(luaL_checkinteger(L, 1)),
												 static_cast<int32_t>(luaL_checkinteger(L, 2)), v) == 1);
		return 1;
	}
	int LuaMod::PostfxEnableFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, postfx_enable)) return 0;
		lua_pushboolean(L, self.api_->postfx_enable(self.mod_, static_cast<int32_t>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2) ? 1 : 0) == 1);
		return 1;
	}
	int LuaMod::PostfxErrorFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, postfx_error)) return 0;
		const int32_t h = static_cast<int32_t>(luaL_checkinteger(L, 1));
		std::string text(self.api_->postfx_error(self.mod_, h, nullptr, 0) + 1, '\0');
		text.resize(self.api_->postfx_error(self.mod_, h, text.data(), static_cast<uint32_t>(text.size())));
		lua_pushlstring(L, text.data(), text.size());
		return 1;
	}

	// mirage.menu_slider{label = "Strength", min = 0, max = 1, value = 0.5, step = 0.1}: a slider on the "Mod settings"
	// page of the game's pause menu, its id (nil if it couldn't be made); mirage.menu_slider_value(id): its value now
	int LuaMod::MenuSliderFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, menu_slider)) return luaL_error(L, "mirage.menu_slider: this runtime has no options menu");
		luaL_checktype(L, 1, LUA_TTABLE);
		const auto number = [&](const char* key, float fallback) {
			lua_getfield(L, 1, key);
			const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
			lua_pop(L, 1);
			return v;
		};
		lua_getfield(L, 1, "label");
		const std::string label = luaL_optstring(L, -1, "Setting");
		lua_pop(L, 1);
		const float min = number("min", 0.f), max = number("max", 1.f);
		const int32_t id = self.api_->menu_slider(self.mod_, label.c_str(), min, max, number("step", 0.f), number("value", min));
		if (id > 0) lua_pushinteger(L, id);
		else lua_pushnil(L);
		return 1;
	}
	int LuaMod::MenuSliderValueFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, menu_slider_value)) return 0;
		float v = 0.f;
		if (!self.api_->menu_slider_value(self.mod_, static_cast<int32_t>(luaL_checkinteger(L, 1)), &v)) return 0;
		lua_pushnumber(L, v);
		return 1;
	}

	// mirage.collision(actor): {collide_actors, block_actors, block_rigid_body} or nil; mirage.set_collision(actor, same)
	int LuaMod::CollisionFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_collision_get)) return luaL_error(L, "mirage.collision: this runtime has no physics");
		const int32_t f = self.api_->physics_collision_get(CheckObject(L, 1));
		if (f < 0) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 3);
		lua_pushboolean(L, (f & MIRAGE_COLLIDE_ACTORS) != 0);
		lua_setfield(L, -2, "collide_actors");
		lua_pushboolean(L, (f & MIRAGE_BLOCK_ACTORS) != 0);
		lua_setfield(L, -2, "block_actors");
		lua_pushboolean(L, (f & MIRAGE_BLOCK_RIGID_BODY) != 0);
		lua_setfield(L, -2, "block_rigid_body");
		return 1;
	}
	int LuaMod::SetCollisionFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_collision_set)) return luaL_error(L, "mirage.set_collision: this runtime has no physics");
		MirageObject* actor = CheckObject(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		const auto flag = [&](const char* key) {
			lua_getfield(L, 2, key);
			const bool on = lua_toboolean(L, -1);
			lua_pop(L, 1);
			return on;
		};
		const uint32_t f = (flag("collide_actors") ? MIRAGE_COLLIDE_ACTORS : 0) | (flag("block_actors") ? MIRAGE_BLOCK_ACTORS : 0) |
						   (flag("block_rigid_body") ? MIRAGE_BLOCK_RIGID_BODY : 0);
		lua_pushboolean(L, self.api_->physics_collision_set(self.mod_, actor, f) != 0);
		return 1;
	}

	// mirage.camera(): the player's view, {location = {x, y, z}, rotation = {pitch, yaw, roll}, forward = {x, y, z}, fov},
	// or nil (no camera yet: the first call starts following it, valid from the next tick). Any thread.
	int LuaMod::CameraFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, camera_get)) return luaL_error(L, "mirage.camera: this runtime has no camera");
		MirageCamera c{};
		c.size = sizeof c;
		if (!self.api_->camera_get(&c)) {
			lua_pushnil(L);
			return 1;
		}
		const float at[3] = {c.x, c.y, c.z};
		const double turn = 6.283185307179586 / 65536.0;	// Unreal rotation units to radians
		const double pitch = c.pitch * turn, yaw = c.yaw * turn;
		const float forward[3] = {static_cast<float>(std::cos(pitch) * std::cos(yaw)), static_cast<float>(std::cos(pitch) * std::sin(yaw)),
								  static_cast<float>(std::sin(pitch))};
		lua_createtable(L, 0, 4);
		PushVec(L, at);
		lua_setfield(L, -2, "location");
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, c.pitch);
		lua_setfield(L, -2, "pitch");
		lua_pushinteger(L, c.yaw);
		lua_setfield(L, -2, "yaw");
		lua_pushinteger(L, c.roll);
		lua_setfield(L, -2, "roll");
		lua_setfield(L, -2, "rotation");
		PushVec(L, forward);
		lua_setfield(L, -2, "forward");
		lua_pushnumber(L, c.fov);
		lua_setfield(L, -2, "fov");
		return 1;
	}

	// mirage.world_to_screen({x, y, z}) -> x, y (pixels, with the last frame's view), or nil (behind the camera)
	int LuaMod::WorldToScreenFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, world_to_screen)) return luaL_error(L, "mirage.world_to_screen: this runtime can't project");
		float v[3], x = 0.f, y = 0.f;
		CheckVec(L, 1, v);
		if (!self.api_->world_to_screen(v[0], v[1], v[2], &x, &y)) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushnumber(L, x);
		lua_pushnumber(L, y);
		return 2;
	}

	// mirage.draw3d.lines({{a, b, color}, ...}): lines in the world, drawn by the runtime's renderer inside the game's
	// frame (behind walls where there is depth); a, b vectors, color as mirage.rgba gives it. Each call replaces the last
	// (the renderer's moving layer: call it every tick for what moves). Each mod's lines are its own: drawn with every
	// other mod's. mirage.draw3d.clear(): the mod's lines gone. Game thread.
	int LuaMod::Draw3dLines(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, world_draw_set)) return luaL_error(L, "mirage.draw3d: this runtime has no 3D drawing");
		luaL_checktype(L, 1, LUA_TTABLE);
		std::vector<MirageWorldVertex> lines;
		const lua_Integer n = luaL_len(L, 1);
		lines.reserve(static_cast<std::size_t>(n) * 2);
		for (lua_Integer i = 1; i <= n; ++i) {
			lua_rawgeti(L, 1, i);
			const int line = lua_gettop(L);
			if (lua_istable(L, line)) {
				float a[3], b[3];
				lua_rawgeti(L, line, 1);
				const bool okA = ReadVec(L, -1, a);
				lua_rawgeti(L, line, 2);
				const bool okB = ReadVec(L, -1, b);
				lua_rawgeti(L, line, 3);
				const auto abgr = static_cast<uint32_t>(luaL_optinteger(L, -1, 0xFFFFFFFF));
				lua_pop(L, 3);
				// mirage.rgba is ImGui's 0xAABBGGRR; the renderer takes Direct3D's 0xAARRGGBB
				const uint32_t argb = (abgr & 0xFF00FF00u) | ((abgr & 0xFFu) << 16) | ((abgr >> 16) & 0xFFu);
				if (okA && okB) {
					lines.push_back({a[0], a[1], a[2], argb});
					lines.push_back({b[0], b[1], b[2], argb});
				}
			}
			lua_settop(L, line - 1);
		}
		if (MIRAGE_API_HAS(self.api_, world_draw_lines)) {		// The mod's own lines: other mods' stay
			self.api_->world_draw_lines(self.mod_, lines.data(), static_cast<uint32_t>(lines.size()));
			return 0;
		}
		MirageWorldMesh mesh{};
		mesh.size = sizeof mesh;
		mesh.lines = lines.data();
		mesh.line_vertices = static_cast<uint32_t>(lines.size());
		mesh.shapes = static_cast<uint32_t>(lines.size() / 2);
		self.api_->world_draw_active(self.mod_, 1);
		self.api_->world_draw_set(MIRAGE_WORLD_MOVING, &mesh);
		return 0;
	}
	int LuaMod::Draw3dClear(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (MIRAGE_API_HAS(self.api_, world_draw_lines)) {
			self.api_->world_draw_lines(self.mod_, nullptr, 0);
			return 0;
		}
		if (!MIRAGE_API_HAS(self.api_, world_draw_set)) return 0;
		self.api_->world_draw_set(MIRAGE_WORLD_MOVING, nullptr);
		self.api_->world_draw_active(self.mod_, 0);
		return 0;
	}

	// mirage.trace(start, end, {actors = true, extent = {x, y, z}, ignore = actor}): {actor, component, location, normal,
	// time, bone} or nil (nothing hit); error when the trace can't be done
	int LuaMod::TraceFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, world_trace)) return luaL_error(L, "mirage.trace: this runtime has no world queries");
		float start[3], end[3], extent[3];
		CheckVec(L, 1, start);
		CheckVec(L, 2, end);
		uint32_t flags = 0;
		bool hasExtent = false;
		MirageObject* ignore = nullptr;
		if (lua_istable(L, 3)) {
			lua_getfield(L, 3, "actors");
			if (lua_toboolean(L, -1)) flags |= MIRAGE_TRACE_ACTORS;
			lua_pop(L, 1);
			lua_getfield(L, 3, "material");
			if (lua_toboolean(L, -1)) flags |= MIRAGE_TRACE_MATERIAL;
			lua_pop(L, 1);
			lua_getfield(L, 3, "extent");
			hasExtent = ReadVec(L, -1, extent);
			lua_pop(L, 1);
			lua_getfield(L, 3, "ignore");
			if (!lua_isnil(L, -1)) ignore = CheckObject(L, -1);
			lua_pop(L, 1);
		}
		MirageHit h{};
		h.size = sizeof h;
		const int32_t r = self.api_->world_trace(self.mod_, start, end, hasExtent ? extent : nullptr, flags, ignore, &h);
		if (r < 0) return luaL_error(L, "mirage.trace: the trace couldn't be done (the log says why)");
		if (r == 0) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 6);
		PushObject(L, h.actor);
		lua_setfield(L, -2, "actor");
		PushObject(L, h.component);
		lua_setfield(L, -2, "component");
		PushVec(L, h.location);
		lua_setfield(L, -2, "location");
		PushVec(L, h.normal);
		lua_setfield(L, -2, "normal");
		lua_pushnumber(L, h.time);
		lua_setfield(L, -2, "time");
		lua_pushstring(L, h.bone);
		lua_setfield(L, -2, "bone");
		if (h.material) {
			PushObject(L, h.material);
			lua_setfield(L, -2, "material");
		}
		if (h.phys_material) {
			PushObject(L, h.phys_material);
			lua_setfield(L, -2, "phys_material");
		}
		return 1;
	}

	// mirage.joint(a, b, {x, y, z}, {kind = "ball" | "hinge", rotation = {pitch, yaw, roll}, break_threshold = 0}): a joint
	// between two bodies (b nil: a held to the world), or nil (the log says why). Removed on revert and unload.
	int LuaMod::JointFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, physics_joint)) return luaL_error(L, "mirage.joint: this runtime has no joints");
		MirageObject* a = CheckObject(L, 1);
		MirageObject* b = lua_isnoneornil(L, 2) ? nullptr : CheckObject(L, 2);
		float at[3];
		CheckVec(L, 3, at);
		int32_t kind = MIRAGE_JOINT_BALL, rot[3] = {};
		bool hasRot = false;
		float breakAt = 0.f;
		if (lua_istable(L, 4)) {
			lua_getfield(L, 4, "kind");
			if (const char* k = lua_tostring(L, -1); k && std::strcmp(k, "hinge") == 0) kind = MIRAGE_JOINT_HINGE;
			lua_pop(L, 1);
			lua_getfield(L, 4, "break_threshold");
			breakAt = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
			lua_getfield(L, 4, "rotation");
			if (lua_istable(L, -1)) {
				static const char* keys[] = {"pitch", "yaw", "roll"};
				for (int i = 0; i < 3; ++i) {
					lua_getfield(L, -1, keys[i]);
					rot[i] = static_cast<int32_t>(lua_tointeger(L, -1));
					lua_pop(L, 1);
				}
				hasRot = true;
			}
			lua_pop(L, 1);
		}
		MirageObject* j = self.api_->physics_joint(self.mod_, a, b, at, hasRot ? rot : nullptr, kind, breakAt);
		if (j) PushObject(L, j);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.overlap({x, y, z}, radius, class): the actors whose collision bounds touch the sphere
	int LuaMod::OverlapFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, world_overlap)) return luaL_error(L, "mirage.overlap: this runtime has no world queries");
		float c[3];
		CheckVec(L, 1, c);
		const float radius = static_cast<float>(luaL_checknumber(L, 2));
		const char* cls = luaL_optstring(L, 3, "");
		std::vector<MirageObject*> found(256);
		int32_t n = self.api_->world_overlap(c, radius, cls, found.data(), static_cast<int32_t>(found.size()));
		if (n > static_cast<int32_t>(found.size())) {
			found.resize(static_cast<std::size_t>(n));
			n = self.api_->world_overlap(c, radius, cls, found.data(), n);
		}
		lua_createtable(L, n, 0);
		for (int32_t i = 0; i < n && i < static_cast<int32_t>(found.size()); ++i) {
			PushObject(L, found[static_cast<std::size_t>(i)]);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	// mirage.trigger{location = {x, y, z}, radius = 100, height = 100, class = "Pawn", tag = "", stay = false,
	// on_enter = function(trigger, other) end, on_leave = ..., on_stay = ...}: a trigger the engine checks (a cylinder),
	// or nil (the log says why). Removed on revert and unload.
	int LuaMod::TriggerFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, trigger_create)) return luaL_error(L, "mirage.trigger: this runtime has no triggers");
		luaL_checktype(L, 1, LUA_TTABLE);
		MirageTriggerDesc d{};
		d.size = sizeof d;
		d.shape = MIRAGE_SHAPE_CYLINDER;
		lua_getfield(L, 1, "shape");
		if (const char* shape = lua_tostring(L, -1); shape && std::strcmp(shape, "cylinder") != 0) d.shape = -1;
		lua_pop(L, 1);
		lua_getfield(L, 1, "location");
		if (!ReadVec(L, -1, d.location)) return luaL_error(L, "mirage.trigger: location = {x, y, z} is needed");
		lua_pop(L, 1);
		lua_getfield(L, 1, "radius");
		d.radius = static_cast<float>(luaL_optnumber(L, -1, 100.0));
		lua_pop(L, 1);
		lua_getfield(L, 1, "height");
		d.height = static_cast<float>(luaL_optnumber(L, -1, 100.0));
		lua_pop(L, 1);
		lua_getfield(L, 1, "stay");
		d.stay = lua_toboolean(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 1, "class");
		const std::string cls = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		lua_getfield(L, 1, "tag");
		const std::string tag = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		d.class_name = cls.c_str();
		d.tag = tag.c_str();
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = "trigger";
		lua_pushvalue(L, 1);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);		// The table: its on_enter / on_leave / on_stay, looked up per call
		MirageObject* t = self.api_->trigger_create(self.mod_, &d, &OnTrigger, c.get());
		if (!t) {
			luaL_unref(L, LUA_REGISTRYINDEX, c->fn);
			lua_pushnil(L);
			return 1;
		}
		self.callbacks_.push_back(std::move(c));
		PushObject(L, t);
		return 1;
	}

	void MIRAGE_CALL LuaMod::OnTrigger(void* user, MirageObject* trigger, MirageObject* other, int32_t kind)
	{
		auto* c = static_cast<Callback*>(user);
		if (c->failed) return;
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		lua_State* L = self.state_.Get();
		const int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		lua_getfield(L, -1, kind == MIRAGE_TRIGGER_ENTER ? "on_enter" : kind == MIRAGE_TRIGGER_LEAVE ? "on_leave" : "on_stay");
		if (!lua_isfunction(L, -1)) {
			lua_settop(L, top);
			return;
		}
		PushObject(L, trigger);
		PushObject(L, other);
		const lua::Result r = self.state_.Call(2, 0);
		lua_settop(L, top);
		if (!r.ok) self.Fail(*c, r.error);
	}

	// mirage.trigger_count(trigger): actors inside now
	int LuaMod::TriggerCountFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, trigger_count)) return luaL_error(L, "mirage.trigger_count: this runtime has no triggers");
		lua_pushinteger(L, self.api_->trigger_count(CheckObject(L, 1)));
		return 1;
	}

	// mirage.piece{kind = "swing", location = {x, y, z}, yaw = 0, length = 200, reach = 150, thin_grip = false,
	// shimmy = false} or {kind = "zipline", start = {x, y, z}, ["end"] = {x, y, z}, landing_strip = 500, reach = 120}:
	// the game's swing pole or zipline, or nil (the log says why). Removed on revert and unload.
	int LuaMod::PieceFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, piece_create)) return luaL_error(L, "mirage.piece: this runtime has no pieces");
		luaL_checktype(L, 1, LUA_TTABLE);
		MiragePieceDesc d{};
		d.size = sizeof d;
		lua_getfield(L, 1, "kind");
		const std::string kind = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		const auto number = [&](const char* key) {
			lua_getfield(L, 1, key);
			const float v = static_cast<float>(luaL_optnumber(L, -1, 0.0));
			lua_pop(L, 1);
			return v;
		};
		const auto flag = [&](const char* key) {
			lua_getfield(L, 1, key);
			const bool v = lua_toboolean(L, -1);
			lua_pop(L, 1);
			return v;
		};
		const auto vec = [&](const char* key, float (&out)[3]) {
			lua_getfield(L, 1, key);
			const bool ok = ReadVec(L, -1, out);
			lua_pop(L, 1);
			return ok;
		};
		if (kind == "swing") {
			d.kind = MIRAGE_PIECE_SWING;
			if (!vec("location", d.location)) return luaL_error(L, "mirage.piece: a swing needs location = {x, y, z}");
			d.yaw = static_cast<int32_t>(number("yaw"));
			d.length = number("length");
			d.flags = (flag("thin_grip") ? MIRAGE_PIECE_THIN_GRIP : 0u) | (flag("shimmy") ? MIRAGE_PIECE_SHIMMY : 0u);
		} else if (kind == "zipline") {
			d.kind = MIRAGE_PIECE_ZIPLINE;
			if (!vec("start", d.location) || !vec("end", d.end)) return luaL_error(L, "mirage.piece: a zipline needs start and end = {x, y, z}");
			d.landing_strip = number("landing_strip");
		} else {
			const bool geometry = kind == "springboard";
			self.Log(MIRAGE_LOG_ERROR, "mirage.piece: " + (geometry ? std::string("a springboard is geometry, not a piece: a step 64 units high, then a "
																			 "ledge 80 to 148 high 112 units further")
																 : "no piece kind '" + kind + "' (swing, zipline; ladders and balance beams aren't supported yet)"));
			lua_pushnil(L);
			return 1;
		}
		d.reach = number("reach");
		MirageObject* p = self.api_->piece_create(self.mod_, &d);
		if (p) PushObject(L, p);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.mover{mesh = "Package.Name", location = {x, y, z}, rotation = {pitch, yaw, roll}, scale = {x, y, z} or 2}:
	// a moving platform (or door, or lift), or nil (the log says why). Removed on revert and unload.
	int LuaMod::MoverFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, mover_create)) return luaL_error(L, "mirage.mover: this runtime has no movers");
		luaL_checktype(L, 1, LUA_TTABLE);
		MirageMoverDesc d{};
		d.size = sizeof d;
		lua_getfield(L, 1, "mesh");
		const std::string mesh = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		d.mesh = mesh.c_str();
		lua_getfield(L, 1, "location");
		if (!ReadVec(L, -1, d.location)) return luaL_error(L, "mirage.mover: location = {x, y, z} is needed");
		lua_pop(L, 1);
		lua_getfield(L, 1, "rotation");
		if (lua_istable(L, -1)) {
			static const char* keys[] = {"pitch", "yaw", "roll"};
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, -1, keys[i]);
				d.rotation[i] = static_cast<int32_t>(lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		lua_getfield(L, 1, "scale");
		if (lua_isnumber(L, -1)) d.scale[0] = d.scale[1] = d.scale[2] = static_cast<float>(lua_tonumber(L, -1));
		else if (lua_istable(L, -1)) ReadVec(L, -1, d.scale);
		lua_pop(L, 1);
		MirageObject* m = self.api_->mover_create(self.mod_, &d);
		if (m) PushObject(L, m);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.move_to(mover, {x, y, z}, seconds): sent there, arriving after `seconds`; true, or false if it isn't a mover
	int LuaMod::MoveToFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, mover_move)) return luaL_error(L, "mirage.move_to: this runtime has no movers");
		MirageObject* m = CheckObject(L, 1);
		float at[3];
		CheckVec(L, 2, at);
		lua_pushboolean(L, self.api_->mover_move(m, at, static_cast<float>(luaL_optnumber(L, 3, 0.0))) == 1);
		return 1;
	}

	// MIRAGE_ANIM_* by name ("fullbody", "FullBody", "CNT_FullBody"): -1 if none
	static int32_t AnimSlotByName(const char* name)
	{
		static const char* const kNames[] = {"canned", "cannedupperbody", "fullbody", "fullbody_dir", "upperbody", "lowerbody",
											 "camera", "weapon", "face"};
		std::string n = name ? name : "";
		for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		if (n.rfind("cnt_", 0) == 0) n = n.substr(4);
		for (int32_t i = 0; i < 9; ++i)
			if (n == kNames[i]) return i;
		return -1;
	}

	// mirage.move{name = "DoubleJump", as = "TdMove_Falling", on_start = function() end, on_tick = function(seconds) end,
	// on_stop = function() end}: a move of the mod's own; its EMovement number (what player_state reports), or nil (the
	// log says why). Removed on revert and unload.
	int LuaMod::MoveFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, move_register)) return luaL_error(L, "mirage.move: this runtime has no moves");
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_getfield(L, 1, "name");
		const std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "Move";
		lua_pop(L, 1);
		lua_getfield(L, 1, "as");
		const std::string as = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = "move " + name;
		lua_pushvalue(L, 1);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);		// The table: its on_start / on_tick / on_stop, looked up per call
		MirageMoveDesc d{};
		d.size = sizeof d;
		d.name = name.c_str();
		d.behaves_as = as.empty() ? nullptr : as.c_str();
		d.fn = &OnMove;
		d.user = c.get();
		// Its animation: anim = "SpringBoardLeftLeg", anim_slot = "fullbody", anim_rate, blend_in, blend_out
		lua_getfield(L, 1, "anim");
		const std::string anim = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
		lua_pop(L, 1);
		if (!anim.empty()) {
			d.anim = anim.c_str();
			d.anim_slot = MIRAGE_ANIM_FULL_BODY;
			lua_getfield(L, 1, "anim_slot");
			if (lua_isstring(L, -1) && !lua_isnumber(L, -1)) d.anim_slot = AnimSlotByName(lua_tostring(L, -1));
			else if (lua_isnumber(L, -1)) d.anim_slot = static_cast<int32_t>(lua_tointeger(L, -1));
			lua_pop(L, 1);
			const auto opt = [&](const char* key, float fallback) {
				lua_getfield(L, 1, key);
				const float v = static_cast<float>(luaL_optnumber(L, -1, fallback));
				lua_pop(L, 1);
				return v;
			};
			d.anim_rate = opt("anim_rate", 1.0f);
			d.anim_blend_in = opt("blend_in", 0.15f);
			d.anim_blend_out = opt("blend_out", 0.2f);
		}
		const int32_t move = self.api_->move_register(self.mod_, &d);
		if (!move) {
			luaL_unref(L, LUA_REGISTRYINDEX, c->fn);
			lua_pushnil(L);
			return 1;
		}
		self.callbacks_.push_back(std::move(c));
		lua_pushinteger(L, move);
		return 1;
	}

	void MIRAGE_CALL LuaMod::OnMove(void* user, int32_t phase, float seconds)
	{
		auto* c = static_cast<Callback*>(user);
		if (c->failed) return;
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		lua_State* L = self.state_.Get();
		const int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		lua_getfield(L, -1, phase == MIRAGE_MOVE_START ? "on_start" : phase == MIRAGE_MOVE_TICK ? "on_tick" : "on_stop");
		if (!lua_isfunction(L, -1)) {
			lua_settop(L, top);
			return;
		}
		lua_pushnumber(L, seconds);
		const lua::Result r = self.state_.Call(1, 0);
		lua_settop(L, top);
		if (!r.ok) self.Fail(*c, r.error);
	}

	// A slot argument: a MIRAGE_ANIM_* number or its name ("fullbody", "upperbody", ...); default full body
	static int32_t AnimSlotArg(lua_State* L, int index)
	{
		if (lua_isnoneornil(L, index)) return MIRAGE_ANIM_FULL_BODY;
		if (lua_isnumber(L, index)) return static_cast<int32_t>(lua_tointeger(L, index));
		const int slot = AnimSlotByName(luaL_checkstring(L, index));
		if (slot < 0) return luaL_error(L, "no animation slot '%s' (canned, cannedupperbody, fullbody, fullbody_dir, upperbody, "
										   "lowerbody, camera, weapon, face)", lua_tostring(L, index));
		return slot;
	}

	// mirage.anim_play(pawn, "Sequence", {slot = "fullbody", rate = 1, blend_in = 0.15, blend_out = 0.2, loop = false,
	// root_motion = false}): true, or false (the log says why)
	int LuaMod::AnimPlayFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, anim_play)) return luaL_error(L, "mirage.anim_play: this runtime has no animation");
		MirageObject* pawn = CheckObject(L, 1);
		const char* sequence = luaL_checkstring(L, 2);
		int32_t slot = MIRAGE_ANIM_FULL_BODY;
		float rate = 1.f, in = 0.15f, out = 0.2f;
		uint32_t flags = 0;
		if (lua_istable(L, 3)) {
			lua_getfield(L, 3, "slot");
			slot = AnimSlotArg(L, -1);
			lua_pop(L, 1);
			const auto num = [&](const char* key, float& v) {
				lua_getfield(L, 3, key);
				v = static_cast<float>(luaL_optnumber(L, -1, v));
				lua_pop(L, 1);
			};
			num("rate", rate);
			num("blend_in", in);
			num("blend_out", out);
			lua_getfield(L, 3, "loop");
			if (lua_toboolean(L, -1)) flags |= MIRAGE_ANIM_LOOP;
			lua_pop(L, 1);
			lua_getfield(L, 3, "root_motion");
			if (lua_toboolean(L, -1)) flags |= MIRAGE_ANIM_ROOT_MOTION;
			lua_pop(L, 1);
		}
		lua_pushboolean(L, self.api_->anim_play(pawn, slot, sequence, rate, in, out, flags) == 1);
		return 1;
	}

	// mirage.anim_stop(pawn, slot, blend_out)
	int LuaMod::AnimStopFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, anim_stop)) return luaL_error(L, "mirage.anim_stop: this runtime has no animation");
		lua_pushboolean(L, self.api_->anim_stop(CheckObject(L, 1), AnimSlotArg(L, 2), static_cast<float>(luaL_optnumber(L, 3, 0.2))) == 1);
		return 1;
	}

	// mirage.anim_state(pawn, slot): {playing, looping, sequence, position, length, rate} or nil
	int LuaMod::AnimStateFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, anim_state)) return luaL_error(L, "mirage.anim_state: this runtime has no animation");
		MirageAnimState s{};
		s.size = sizeof s;
		if (self.api_->anim_state(CheckObject(L, 1), AnimSlotArg(L, 2), &s) != 1) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 6);
		lua_pushboolean(L, s.playing);
		lua_setfield(L, -2, "playing");
		lua_pushboolean(L, s.looping);
		lua_setfield(L, -2, "looping");
		lua_pushstring(L, s.sequence);
		lua_setfield(L, -2, "sequence");
		lua_pushnumber(L, s.position);
		lua_setfield(L, -2, "position");
		lua_pushnumber(L, s.length);
		lua_setfield(L, -2, "length");
		lua_pushnumber(L, s.rate);
		lua_setfield(L, -2, "rate");
		return 1;
	}

	// mirage.anim_sets(actor): {{mesh = "Mesh1p", sets = {{slot, name, path, sequences = {{name, length, frames, rate,
	// notifies}}}}}}
	int LuaMod::AnimSetsFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, anim_sets)) return luaL_error(L, "mirage.anim_sets: this runtime has no animation");
		MirageObject* actor = CheckObject(L, 1);
		std::string text(self.api_->anim_sets(actor, nullptr, 0) + 1, '\0');
		text.resize(self.api_->anim_sets(actor, text.data(), static_cast<uint32_t>(text.size())));
		PushJson(L, jc::parse(text));
		return 1;
	}

	// mirage.move_start(move): the player put into it now (true), or false (the log says why)
	int LuaMod::MoveStartFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, move_start)) return luaL_error(L, "mirage.move_start: this runtime has no moves");
		lua_pushboolean(L, self.api_->move_start(self.mod_, static_cast<int32_t>(luaL_checkinteger(L, 1))) == 1);
		return 1;
	}

	// mirage.move_remove(move)
	int LuaMod::MoveRemoveFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, move_remove)) return luaL_error(L, "mirage.move_remove: this runtime has no moves");
		self.api_->move_remove(self.mod_, static_cast<int32_t>(luaL_checkinteger(L, 1)));
		return 0;
	}

	// mirage.mover_moving(mover): true while it is on its way
	int LuaMod::MoverMovingFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, mover_moving)) return luaL_error(L, "mirage.mover_moving: this runtime has no movers");
		lua_pushboolean(L, self.api_->mover_moving(CheckObject(L, 1)) == 1);
		return 1;
	}

	int LuaMod::Changes(lua_State* L)
	{
		LuaMod& self = Self(L);
		std::string text(self.api_->change_list(self.mod_, nullptr, 0) + 1, '\0');
		self.api_->change_list(self.mod_, text.data(), static_cast<uint32_t>(text.size()));
		const jc::json j = jc::parse(text.c_str());
		lua_newtable(L);
		int n = 0;
		for (const jc::json& t : jc::items(j)) {
			lua_newtable(L);
			lua_pushinteger(L, jc::value(t, "id", 0));
			lua_setfield(L, -2, "id");
			lua_pushstring(L, jc::value(t, "name", std::string()).c_str());
			lua_setfield(L, -2, "name");
			lua_pushboolean(L, jc::value(t, "reverted", false));
			lua_setfield(L, -2, "reverted");
			lua_newtable(L);
			int k = 0;
			for (const jc::json& v : jc::items(t, "values")) {
				lua_pushstring(L, jc::as<std::string>(v).c_str());
				lua_rawseti(L, -2, ++k);
			}
			lua_setfield(L, -2, "values");
			lua_rawseti(L, -2, ++n);
		}
		return 1;
	}

	// mirage.valid(object) -> whether it is still the object it was (false once it is gone, or for nil)
	int LuaMod::Valid(lua_State* L)
	{
		auto* p = static_cast<Proxy*>(luaL_testudata(L, 1, kProxyMeta));
		Refresh(p);
		lua_pushboolean(L, p && p->kind == kObject && p->handle != nullptr);
		return 1;
	}

	// mirage.actors{ class = "TdBotPawn", tag = "Door", name = "TdBotPawn_3", near = actor | {x, y, z}, radius = 2000 }
	// -> the actors in play that match, nearest first when a radius is given (every field optional)
	int LuaMod::Actors(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageActorQuery q{};
		q.size = sizeof q;
		std::string cls, tag, name;
		if (lua_istable(L, 1)) {
			const auto text = [&](const char* key, std::string& out) {
				if (lua_getfield(L, 1, key) == LUA_TSTRING) out = lua_tostring(L, -1);
				lua_pop(L, 1);
			};
			text("class", cls);
			text("tag", tag);
			text("name", name);
			if (lua_getfield(L, 1, "radius") == LUA_TNUMBER) q.radius = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
			lua_getfield(L, 1, "near");
			if (auto* p = static_cast<Proxy*>(luaL_testudata(L, -1, kProxyMeta)); p && p->kind == kObject) {
				Refresh(p);
				if (!p->handle) {		// Around an actor that is gone: nothing is near it
					lua_newtable(L);
					return 1;
				}
				q.near_object = static_cast<MirageObject*>(p->handle);
			} else if (lua_istable(L, -1)) {
				const auto coord = [&](int i, const char* key) {
					float v = 0.f;
					if (lua_rawgeti(L, -1, i) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L, -1));
					lua_pop(L, 1);
					if (lua_getfield(L, -1, key) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L, -1));
					lua_pop(L, 1);
					return v;
				};
				q.x = coord(1, "X");
				q.y = coord(2, "Y");
				q.z = coord(3, "Z");
			}
			lua_pop(L, 1);
		}
		q.class_name = cls.c_str();
		q.tag = tag.c_str();
		q.name = name.c_str();
		std::vector<MirageObject*> found(256);
		int32_t n = self.api_->actors_query(&q, found.data(), static_cast<int32_t>(found.size()));
		if (n > static_cast<int32_t>(found.size())) {
			found.resize(static_cast<std::size_t>(n));
			n = self.api_->actors_query(&q, found.data(), n);
		}
		lua_createtable(L, std::max(n, 0), 0);
		for (int32_t i = 0; i < n && i < static_cast<int32_t>(found.size()); ++i) {
			PushObject(L, found[static_cast<std::size_t>(i)]);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	// mirage.call(object, "Function", { Param = value, ... }): its return value (true if it returns nothing)
	int LuaMod::CallGame(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageObject* o = CheckObject(L, 1);
		const char* fn = luaL_checkstring(L, 2);
		MirageCall* call = self.api_->call_begin(o, fn);
		if (!call) return luaL_error(L, "mirage.call: no function %s on this object", fn);
		if (lua_istable(L, 3)) {
			lua_pushnil(L);
			while (lua_next(L, 3)) {
				if (lua_type(L, -2) == LUA_TSTRING) {
					const std::string param = lua_tostring(L, -2);
					MirageValue probe;
					if (!self.api_->call_get(call, param.c_str(), &probe)) {
						self.api_->call_end(call);
						return luaL_error(L, "mirage.call: %s has no parameter %s", fn, param.c_str());
					}
					self.Set(L, kInCall, call, param, lua_gettop(L));
				}
				lua_pop(L, 1);
			}
		}
		if (!self.api_->call_invoke(call)) {
			self.api_->call_end(call);
			return luaL_error(L, "mirage.call: %s didn't run", fn);
		}
		MirageValue ret;
		if (self.api_->call_get(call, "ReturnValue", &ret) && ret.type != MIRAGE_VALUE_STRUCT) PushValue(L, ret, kInCall, call, "", 0);
		else lua_pushboolean(L, 1);
		self.api_->call_end(call);
		return 1;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"sound_play", LuaMod::SoundPlayFn}, {"sound_stop", LuaMod::SoundStopFn},
		{"sound_playing", LuaMod::SoundPlayingFn}, {"postfx", LuaMod::PostfxFn}, {"postfx_set", LuaMod::PostfxSetFn},
		{"postfx_enable", LuaMod::PostfxEnableFn}, {"postfx_error", LuaMod::PostfxErrorFn},
		{"menu_slider", LuaMod::MenuSliderFn}, {"menu_slider_value", LuaMod::MenuSliderValueFn},
		{"call", LuaMod::CallGame}, {"valid", LuaMod::Valid}, {"actors", LuaMod::Actors}, {"changes", LuaMod::Changes},
		{"body", LuaMod::BodyFn}, {"set_body", LuaMod::SetBodyFn}, {"impulse", LuaMod::ImpulseFn},
		{"force", LuaMod::ForceFn}, {"spawn_prop", LuaMod::SpawnPropFn}, {"spawn", LuaMod::SpawnFn}, {"model", LuaMod::ModelFn},
		{"collision", LuaMod::CollisionFn}, {"set_collision", LuaMod::SetCollisionFn}, {"trace", LuaMod::TraceFn},
		{"camera", LuaMod::CameraFn}, {"world_to_screen", LuaMod::WorldToScreenFn}, {"overlap", LuaMod::OverlapFn},
		{"joint", LuaMod::JointFn}, {"trigger", LuaMod::TriggerFn}, {"trigger_count", LuaMod::TriggerCountFn},
		{"piece", LuaMod::PieceFn}, {"mover", LuaMod::MoverFn}, {"move_to", LuaMod::MoveToFn},
		{"mover_moving", LuaMod::MoverMovingFn}, {"move", LuaMod::MoveFn}, {"move_start", LuaMod::MoveStartFn},
		{"move_remove", LuaMod::MoveRemoveFn}, {"anim_play", LuaMod::AnimPlayFn}, {"anim_stop", LuaMod::AnimStopFn},
		{"anim_state", LuaMod::AnimStateFn}, {"anim_sets", LuaMod::AnimSetsFn}
	});
	// mirage.draw3d: this file's functions (Bind puts every list in)
	const LuaFunctions kDraw3dFunctions("draw3d", {
		{"lines", LuaMod::Draw3dLines}, {"clear", LuaMod::Draw3dClear}
	});
}
