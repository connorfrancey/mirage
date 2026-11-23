-- Portal Gun: left click puts the blue portal on the surface you aim at, right click the orange one. Once both are up,
-- each shows what is behind the other, and the player who walks, runs or falls into one comes out of the other with
-- their speed turned with it.
--
-- Each portal is a subclass of Engine.PortalTeleporter. Its SceneCapturePortalComponent renders the view through the
-- sister portal into a render target. Player traversal uses the same portal frames and hands off at the camera plane.
-- The view is shown on a flattened sphere (Engine.EditorMeshes.TexPropSphere, an
-- oval from the front) with a material instance of EngineMaterials.ScreenMaterial, the engine's material that samples
-- a texture by screen position: through it the render target lines up with the world behind the other portal.
--
-- What it uses:
--   mirage.define_class    the portal class, its setup written in UnrealScript (`extra`)
--   mirage.spawn / call    the portals, and the engine's functions on them
--   mirage.camera / trace  aiming
--   [actions] in mod.toml  rebindable keys, reported by mirage.on("action")
--   mirage.draw3d          the blue and orange rims, as lines in the world
local V = mirage.vec
local function of(v) return V(v.X, v.Y, v.Z) end      -- A game struct (pawn.Location: live, not a table) as a vector

local HALF_W, HALF_H = 72, 150            -- Fit the cylinder when a standing player aims at eye height
local RANGE = 6000                        -- How far the gun reaches
-- Sphere meshes for the disc, the first that loads, with their radius (bounds are native data: measured with UnrealLib)
local SPHERES = {
    { path = "EditorMeshes.TexPropSphere", radius = 160 },
    { path = "TDLevelDesign.UI.PlayerSphere", radius = 128 },
}
local COLORS = { blue = mirage.rgba(40, 140, 255), orange = mirage.rgba(255, 140, 20) }
local OTHER = { blue = "orange", orange = "blue" }

local portals = {}                        -- blue / orange: { actor, at, normal, right, up }
local cls, material, param, sphere, sphereRadius
local overlap                             -- Pawn and opening whose collision cylinder intersects the wall
local cameraBlend                         -- Eye offset and roll left by traversing between differently tilted surfaces
local foreground                          -- Portal disc currently moved clear of the camera's near plane
local Status, rims

-- ---- The portal class (defined once, on the first tick with the module on) ---------------------------------------------
local PORTAL_SCRIPT = [[
var TextureRenderTarget2D TextureTarget;
var MaterialInstanceConstant PortalMaterial;

// Its look: the render target the capture draws into, shown with ScreenMaterial on a flattened sphere
function SetupPortal(Material Parent, string Param, StaticMesh Shape, float ScaleX, float ScaleY, float ScaleZ)
{
    local vector Scale;
    TextureTarget = CreatePortalTexture();
    PortalMaterial = new(self) class'MaterialInstanceConstant';
    PortalMaterial.SetParent(Parent);
    PortalMaterial.SetTextureParameterValue(name(Param), TextureTarget);
    Scale.X = ScaleX;
    Scale.Y = ScaleY;
    Scale.Z = ScaleZ;
    StaticMesh.SetStaticMesh(Shape);
    StaticMesh.SetMaterial(0, PortalMaterial);
    StaticMesh.SetScale3D(Scale);
    StaticMesh.SetHidden(false);
}

// The other portal: where this one's view comes from and where actors go
function Link(PortalTeleporter Other)
{
    SisterPortal = Other;
    SceneCapturePortalComponent(SceneCapture).SetCaptureParameters(TextureTarget, 1.0, Other);
}
]]

local failed = false

local function define()
    if cls then return true end
    if failed then return false end            -- Tried once: the log says why
    failed = true
    material = mirage.find("Material EngineMaterials.ScreenMaterial")
    local expr = mirage.find("MaterialExpressionTextureSampleParameter2D EngineMaterials.ScreenMaterial.MaterialExpressionTextureSampleParameter2D_0")
    param = expr and tostring(expr.ParameterName) or nil       -- "ScreenTex"
    -- The disc's shape: a sphere from a package of its own, loaded with the engine's static DynamicLoadObject (callable
    -- on any object). Engine.u's copy of TexPropSphere is in the file but the cooked game refuses to load it.
    local meshClass = mirage.find("Class Engine.StaticMesh")
    for _, s in ipairs(SPHERES) do
        sphere = material and meshClass and mirage.call(material, "DynamicLoadObject",
            { ObjectName = s.path, ObjectClass = meshClass, MayFail = true })
        if sphere then
            sphereRadius = s.radius
            break
        end
    end
    if not material or not sphere or not param then
        Status:set("the engine's ScreenMaterial or a sphere mesh isn't loaded")
        mirage.warn(string.format("portals: ScreenMaterial %s, its parameter %s, StaticMesh class %s, sphere %s",
            tostring(material), tostring(param), tostring(meshClass), tostring(sphere)))
        return false
    end
    local why
    local base = mirage.portal_class("PortalTeleporter")
    if not base then
        Status:set("the engine's portal class isn't loaded")
        return false
    end
    cls, why = mirage.define_class {
        name = "MiragePortal", extends = mirage.name(base), placeable = true, vars = {}, functions = {},
        defaults = {
            bNoDelete = false, bStatic = false, bMovablePortal = true,
            bCollideActors = false, bBlockActors = false, bWorldGeometry = false,   -- Shown, never in the way
            TextureResolutionX = 1024, TextureResolutionY = 576,
        },
        extra = PORTAL_SCRIPT,
    }
    if not cls then
        Status:set("the portal class didn't build (see the log)")
        mirage.warn("portals: define_class: " .. tostring(why))
        return false
    end
    failed = false
    mirage.log("portals: ready (ScreenMaterial's parameter " .. param .. ")")
    return true
end

-- ---- Placing --------------------------------------------------------------------------------------------------------
-- The portal's own axes: `normal` out of the surface, `right` and `up` along it (up is toward the sky on a wall)
-- The Unreal rotation whose forward is `n` (pitch and yaw in 65536ths of a turn)
local function rotation(n)
    local k = 32768 / math.pi
    return { pitch = math.floor(math.atan(n.z, math.sqrt(n.x * n.x + n.y * n.y)) * k + 0.5),
             yaw = math.floor(math.atan(n.y, n.x) * k + 0.5), roll = 0 }
end

local function set_vector(field, v)
    field.X, field.Y, field.Z = v.x, v.y, v.z
end

-- The support distance of the upright collision cylinder along a surface normal.
local function reach(pawn, n)
    local c = pawn.CylinderComponent
    return c.CollisionRadius * math.sqrt(math.max(0, 1 - n.z ^ 2)) + c.CollisionHeight * math.abs(n.z)
end

local function leave_opening(move_clear)
    local state = overlap
    overlap = nil
    if not state or not mirage.valid(state.pawn) then return end
    local pawn, p = state.pawn, state.portal
    if move_clear then
        local at = of(pawn.Location)
        local limit = reach(pawn, p.normal)
        local depth = (at - p.at):dot(p.normal)
        if depth < limit then set_vector(pawn.Location, at + p.normal * (limit - depth)) end
    end
    pawn.bCollideWorld = state.collideWorld
end

local function set_disc(p, eye)
    local scale = 1
    local offset = V.zero
    if eye then
        local rel = eye - p.at
        local depth = math.max(rel:dot(p.normal), 0.1)
        scale = 24 / depth
        offset = V(depth, rel:dot(p.right), rel:dot(p.up)) * (1 - scale)
    end
    local mesh = p.actor.StaticMesh
    mirage.call(mesh, "SetTranslation", { NewTranslation = { X = offset.x, Y = offset.y, Z = offset.z } })
    mirage.call(mesh, "SetScale3D", {
        NewScale3D = { X = 1.5 / sphereRadius, Y = HALF_W * scale / sphereRadius, Z = HALF_H * scale / sphereRadius },
    })
    local near = eye ~= nil
    if p.near ~= near then
        mirage.call(mesh, "SetDepthPriorityGroup", { NewDepthPriorityGroup = near and 3 or 1 })
        p.near = near
    end
end

local function update_disc(p, eye)
    if foreground and foreground ~= p and mirage.valid(foreground.actor) then set_disc(foreground) end
    foreground = p
    if p then set_disc(p, eye) end
end

local function remove(color)
    local p = portals[color]
    if p and mirage.valid(p.actor) then mirage.call(p.actor, "Destroy", {}) end
    portals[color] = nil
end

local function link()
    local b, o = portals.blue, portals.orange
    if not (b and o) then return end
    mirage.call(b.actor, "Link", { Other = o.actor })
    mirage.call(o.actor, "Link", { Other = b.actor })
end

-- A portal of `color` at `point` on a surface facing `normal`. shortcut: it may hang over the surface's edge, a fit
-- check (traces at its rim) when portals on small ledges matter
function portal_place(color, point, normal)
    if not define() then return false end
    local n = V(normal):normalized()
    local rot = rotation(n)
    local yaw = rot.yaw * math.pi / 32768
    local right = V(-math.sin(yaw), math.cos(yaw), 0)
    local up = n:cross(right)
    local at = V(point) + n * 2                    -- Off the surface: the disc isn't in the wall
    local actor = mirage.spawn(mirage.path(cls), at, rot)
    if not actor then
        Status:set("the portal didn't spawn (see the log)")
        return false
    end
    actor.TextureResolutionX, actor.TextureResolutionY = 640, 360
    -- The sphere's X is its thickness (along the normal), Y its width, Z its height
    mirage.call(actor, "SetupPortal", { Parent = material, Param = param, Shape = sphere,
        ScaleX = 1.5 / sphereRadius, ScaleY = HALF_W / sphereRadius, ScaleZ = HALF_H / sphereRadius })
    mirage.call(actor.SceneCapture, "SetFrameRate", { NewFrameRate = 120 })
    leave_opening(true)
    update_disc(nil)
    remove(color)
    portals[color] = { actor = actor, at = at, normal = n, right = right, up = up }
    link()
    rims()
    Status:set((portals.blue and "blue " or "") .. (portals.orange and "orange" or ""))
    return true
end

local function shoot(color)
    local cam, pawn = mirage.camera(), mirage.pawn()
    if not cam or not pawn then return end
    local hit = mirage.trace(cam.location, V(cam.location) + V(cam.forward) * RANGE, { ignore = pawn })
    if hit then portal_place(color, hit.location, hit.normal) end
end

local function close_all()
    leave_opening(true)
    update_disc(nil)
    remove("blue")
    remove("orange")
    rims()
    Status:set("closed")
end

-- ---- Going through ----------------------------------------------------------------------------------------------------
local function fits(pawn, p, at)
    local rel = at - p.at
    local w, h = HALF_W - reach(pawn, p.right), HALF_H - reach(pawn, p.up)
    return w > 0 and h > 0 and (rel:dot(p.right) / w) ^ 2 + (rel:dot(p.up) / h) ^ 2 <= 1
end

local function through(v, from, to)
    return to.normal * -v:dot(from.normal) + to.right * -v:dot(from.right) + to.up * v:dot(from.up)
end

local function turn(r, from, to)
    local k = math.pi / 32768
    local pitch, yaw, roll = r.Pitch * k, r.Yaw * k, r.Roll * k
    local forward = V(math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch))
    local right = V(-math.sin(yaw), math.cos(yaw), 0)
    local up = forward:cross(right)
    local out = through(forward, from, to)
    local outRight = through(right * math.cos(roll) - up * math.sin(roll), from, to)
    local horizontal = math.sqrt(out.x ^ 2 + out.y ^ 2)
    yaw = horizontal > 0.00001 and math.atan(out.y, out.x) or math.atan(-outRight.x, outRight.y)
    local neutralRight = V(-math.sin(yaw), math.cos(yaw), 0)
    roll = math.atan(-outRight:dot(out:cross(neutralRight)), outRight:dot(neutralRight))
    return math.floor(math.atan(out.z, horizontal) / k + 0.5), math.floor(yaw / k + 0.5), math.floor(roll / k + 0.5)
end

-- While the cylinder straddles an opening, let the pawn move into the surface. The view callback
-- constrains motion to the opening and hands off after movement, before the renderer reads the POV.
local function begin_movement(pawn, eye, seconds)
    if overlap and overlap.pawn ~= pawn then leave_opening(false) end
    if not (portals.blue and portals.orange) then return end
    local at, vel = of(pawn.Location), of(pawn.Velocity)
    if overlap then
        if overlap.moving then
            -- The controller skipped its camera update last frame, for example on a change of view target.
            leave_opening(true)
            return
        end
        overlap.previous = at
        overlap.moving = true
        pawn.bCollideWorld = false
        return
    end
    for color, p in pairs(portals) do
        local depth = (at - p.at):dot(p.normal)
        local limit = reach(pawn, p.normal)
        local motion = vel + of(pawn.Acceleration) * seconds
        local step = motion * seconds
        local nextDepth = depth + step:dot(p.normal)
        if depth >= -2 and nextDepth <= limit + 6 and (depth <= limit + 3 or nextDepth < depth)
            and fits(pawn, p, at) and fits(pawn, p, at + step) then
            overlap = { pawn = pawn, portal = p, color = color, previous = at, moving = true, collideWorld = pawn.bCollideWorld,
                eyeDepth = (eye - p.at):dot(p.normal) }
            pawn.bCollideWorld = false
            return
        end
    end
end

-- A wall portal still needs a floor while the cylinder intersects the wall. Sample that floor
-- on the open side so a trace begun inside the wall cannot mistake it for the supporting surface.
local function keep_support(pawn, p, previous, at)
    if math.abs(p.normal.z) >= 0.25 or at.z >= previous.z then return at end
    local open = p.normal * math.max(0, 4 - (at - p.at):dot(p.normal))
    local foot = at + open - V(0, 0, pawn.CylinderComponent.CollisionHeight)
    local hit = mirage.trace(foot + V(0, 0, previous.z - at.z + 4), foot - V(0, 0, 2), { ignore = pawn })
    if not hit or V(hit.normal).z < 0.7 then return at end
    local rise = V(hit.location).z - foot.z
    if rise <= 0 then return at end
    pawn.Velocity.Z = math.max(0, pawn.Velocity.Z)
    return at + V(0, 0, rise)
end

local function reframe(pawn, controller, pov, from, to)
    local at, eye = of(pawn.Location), of(pov.Location)
    local exitEye = to.at + through(eye - from.at, from, to)
    local out = exitEye - (eye - at)
    local velocity, acceleration = through(of(pawn.Velocity), from, to), through(of(pawn.Acceleration), from, to)
    local pitch, yaw, roll = turn(pov.Rotation, from, to)
    local aimPitch, aimYaw = turn(controller.Rotation, from, to)
    local _, bodyYaw = turn(pawn.Rotation, from, to)
    -- Keep the first-person camera continuous even if an upright pawn needs clearance on a tilted exit.
    local depth = (out - to.at):dot(to.normal)
    local shift = to.normal * math.max(0, -depth)
    set_vector(pawn.Location, out + shift)
    if math.abs(from.normal.z) > 0.25 or math.abs(to.normal.z) > 0.25 then
        mirage.call(pawn, "SetPhysics", { newPhysics = 2 })    -- PHYS_Falling
    end
    set_vector(pawn.Velocity, velocity)
    set_vector(pawn.Acceleration, acceleration)
    controller.Rotation.Pitch, controller.Rotation.Yaw = aimPitch & 0xFFFF, aimYaw & 0xFFFF
    pawn.Rotation.Yaw = bodyYaw & 0xFFFF
    pawn.bJustTeleported = true
    set_vector(pov.Location, exitEye)
    pov.Rotation.Pitch, pov.Rotation.Yaw, pov.Rotation.Roll = pitch & 0xFFFF, yaw & 0xFFFF, roll & 0xFFFF
    local rollDelta = ((roll + 32768) % 65536) - 32768
    cameraBlend = (shift:len2() > 0.01 or rollDelta ~= 0) and { offset = -shift, roll = rollDelta } or nil
    return out + shift
end

local function update_view(pawn, controller, pov, seconds)
    if cameraBlend then
        local remaining = math.exp(-12 * seconds)
        cameraBlend.offset = cameraBlend.offset * remaining
        cameraBlend.roll = cameraBlend.roll * remaining
        set_vector(pov.Location, of(pov.Location) + cameraBlend.offset)
        pov.Rotation.Roll = (pov.Rotation.Roll + math.floor(cameraBlend.roll + 0.5)) & 0xFFFF
        if math.abs(cameraBlend.roll) < 1 and cameraBlend.offset:dot(cameraBlend.offset) < 0.01 then cameraBlend = nil end
    end
    local state = overlap
    if not state or not state.moving or state.pawn ~= pawn then return end
    pawn.bCollideWorld = state.collideWorld
    state.moving = false
    local p, to = state.portal, portals[OTHER[state.color]]
    local at = of(pawn.Location)
    local next = keep_support(pawn, p, state.previous, at)
    if not fits(pawn, p, next) and (next - p.at):dot(p.normal) < reach(pawn, p.normal) then
        -- Block lateral movement into the rim while the cylinder is embedded in the surface.
        next = state.previous + p.normal * math.max(0, (next - state.previous):dot(p.normal))
        local vel = of(pawn.Velocity)
        set_vector(pawn.Velocity, p.normal * math.max(0, vel:dot(p.normal)))
    end
    if next ~= at then set_vector(pawn.Location, next) end
    local eye = of(pov.Location) + next - at
    set_vector(pov.Location, eye)
    local rel = eye - p.at
    local depth = rel:dot(p.normal)
    local previousDepth = state.eyeDepth or (state.previous - p.at):dot(p.normal)
    state.eyeDepth = depth
    local inside = (rel:dot(p.right) / HALF_W) ^ 2 + (rel:dot(p.up) / HALF_H) ^ 2 <= 1
    if to and previousDepth >= 0 and depth < 0 and inside then
        next = reframe(pawn, controller, pov, p, to)
        state.color = OTHER[state.color]
        state.portal, state.previous = to, next
        state.eyeDepth = (of(pov.Location) - to.at):dot(to.normal)
        p = to
    end
    if (next - p.at):dot(p.normal) >= reach(pawn, p.normal) + 3 then leave_opening(false) end
end

-- Move the disc away from the eye along its projection rays when the near plane would cut it.
-- Its image and silhouette stay at the same screen coordinates.
local function clip_disc(pov)
    local eye = of(pov.Location)
    for _, p in pairs(portals) do
        local rel = eye - p.at
        local depth = rel:dot(p.normal)
        if depth > 0 and depth < 24 and (rel:dot(p.right) / HALF_W) ^ 2 + (rel:dot(p.up) / HALF_H) ^ 2 < 1 then
            update_disc(p, eye)
            return
        end
    end
    if foreground then update_disc(nil) end
end

-- ---- The gun in the player's hands (optional) ------------------------------------------------------------------------
-- The player's own model in the installed mod folder (models/portalgun.fbx, converted with `mirage model --convert`),
-- held in front of the view like a first-person weapon. Not shipped with the mod: without the file there's no gun.
local GUN_FILE = "models/portalgun.fbx"
local GUN_AT = { forward = 30, right = 12, down = 14 }      -- From the camera, in Unreal units
local GUN_SCALE = 0.5                                       -- The model is ~46 units long: ~23 in the hand
local GUN_PITCH = -16384                                    -- The model's long axis is its Z: turned onto the view's X
local gun = {}                                              -- mesh, actor; failed once it can't be shown

local function gun_tick()
    if gun.failed then return end
    if not gun.mesh then
        local mesh, why = mirage.model(GUN_FILE)
        if not mesh then
            gun.failed = why ~= "pending"                   -- No file, or it didn't build: the log says why, once
            return
        end
        gun.mesh = mesh
    end
    local cam = mirage.camera()
    if not cam then return end
    if not (gun.actor and mirage.valid(gun.actor)) then
        gun.actor = mirage.spawn("Engine.DynamicSMActor_Spawnable", cam.location, cam.rotation)
        if not gun.actor then
            gun.failed = true
            return
        end
        mirage.call(gun.actor, "SetCollision", { bNewColActors = false, bNewBlockActors = false })
        local c = gun.actor.StaticMeshComponent
        mirage.call(c, "SetStaticMesh", { NewMesh = gun.mesh })
        mirage.call(c, "SetDepthPriorityGroup", { NewDepthPriorityGroup = 3 })   -- SDPG_Foreground: never in a wall
        -- The model's own size and axes, on the component (relative to the actor, which follows the view)
        mirage.call(c, "SetScale3D", { NewScale3D = { X = GUN_SCALE, Y = GUN_SCALE, Z = GUN_SCALE } })
        mirage.call(c, "SetRotation", { NewRotation = { Pitch = GUN_PITCH, Yaw = 0, Roll = 0 } })
    end
    -- The view's axes from its rotation (Unreal: X forward, Y right, Z up)
    local yaw = cam.rotation.yaw * math.pi / 32768
    local forward = V(cam.forward)
    local right = V(-math.sin(yaw), math.cos(yaw), 0)
    local up = forward:cross(right)
    local at = V(cam.location) + forward * GUN_AT.forward + right * GUN_AT.right - up * GUN_AT.down
    mirage.call(gun.actor, "SetLocation", { NewLocation = { X = at.x, Y = at.y, Z = at.z } })
    mirage.call(gun.actor, "SetRotation", { NewRotation = { Pitch = cam.rotation.pitch, Yaw = cam.rotation.yaw, Roll = 0 } })
end

-- ---- Drawing: the rims in the world and the crosshair --------------------------------------------------------------
rims = function()
    local lines = {}
    for color, p in pairs(portals) do
        local prev
        for i = 0, 32 do
            local a = i / 32 * 2 * math.pi
            local point = p.at + p.normal * 1 + p.right * (math.cos(a) * HALF_W) + p.up * (math.sin(a) * HALF_H)
            if prev then lines[#lines + 1] = { prev, point, COLORS[color] } end
            prev = point
        end
    end
    if #lines > 0 then mirage.draw3d.lines(lines) else mirage.draw3d.clear() end    -- Only this mod's lines
end

local function hud()
    local w, h = mirage.draw.screen_size()
    local cx, cy = w * 0.5, h * 0.5
    local blue = portals.blue and COLORS.blue or mirage.rgba(40, 140, 255, 90)
    local orange = portals.orange and COLORS.orange or mirage.rgba(255, 140, 20, 90)
    mirage.draw.line(cx - 14, cy - 8, cx - 14, cy + 8, blue, 3)       -- Portal's crosshair: two brackets
    mirage.draw.line(cx + 14, cy - 8, cx + 14, cy + 8, orange, 3)
end

-- ---- The module -------------------------------------------------------------------------------------------------------
local m = mirage.module {
    name = "Portal Gun",
    tab = "Mods",
    description = "Left click: blue portal, right click: orange portal, R: close both. Walk, run or fall through one to come out of the other",
    on_tick = function(seconds)
        if not define() then return end
        local pawn, controller = mirage.pawn(), mirage.controller()
        local camera = controller and controller.PlayerCamera
        if portals.blue and portals.orange then
            if pawn and camera and pawn.CylinderComponent then
                begin_movement(pawn, of(camera.CameraCache.POV.Location), seconds or 0)
            else
                leave_opening(false)
            end
        end
        gun_tick()
    end,
    on_render = hud,
    on_disable = function()
        close_all()
        cameraBlend = nil
        mirage.revert()                                     -- The gun goes with the other spawned actors
        gun = {}
    end,
}
Status = m:text("Status", "no portals", "Which portals are open")

mirage.hook("Engine.Camera.UpdateCamera", function(call)
    if not (overlap or cameraBlend or (portals.blue and portals.orange)) then return end
    local pawn, controller = mirage.pawn(), mirage.controller()
    if not pawn or not controller or call.object ~= controller.PlayerCamera then return end
    local camera = call.object
    local target = camera.ViewTarget.Target
    if target == pawn or target == controller then
        local pov = camera.CameraCache.POV
        update_view(pawn, controller, pov, call.params.DeltaTime)
        clip_disc(pov)
    end
end, { when = "after" })

mirage.on("action", function(name, pressed)
    if not pressed or not m:enabled() then return end
    if name == "blue" or name == "orange" then shoot(name)
    elseif name == "close" then close_all() end
end)
for _, event in ipairs({ "level_load", "restart" }) do
    mirage.on(event, function()                            -- The level took the actors with it
        leave_opening(false)
        cameraBlend = nil
        foreground = nil
        portals = {}
        rims()
        gun.actor = nil
    end)
end
mirage.on("death", function()
    leave_opening(false)
    cameraBlend = nil
    update_disc(nil)
end)

-- ---- The mod's own tests --------------------------------------------------------------------------------------------
mirage.test("engine portal classes resolve through the public API", function()
    for _, name in ipairs({
        "PortalTeleporter", "PortalMarker", "PortalVolume", "SceneCapturePortalActor", "SceneCapturePortalComponent",
        "SceneCaptureReflectActor", "SceneCaptureReflectComponent", "SceneCaptureActor", "SceneCaptureComponent",
    }) do
        assert(mirage.portal_class(name) == mirage.find("Class Engine." .. name) and mirage.portal_class(name),
            "missing engine class " .. name)
    end
    assert(mirage.portal_class("Actor") == nil, "a non-portal class was accepted")
end)

mirage.test("portals preserve camera pose and momentum across the opening", function()
    assert(define(), "the portal class isn't ready")
    local pawn, controller = mirage.pawn(), mirage.controller()
    assert(pawn and controller, "no player")
    local start, velocity, acceleration = of(pawn.Location), of(pawn.Velocity), of(pawn.Acceleration)
    local pitch, yaw, bodyYaw, teleported = controller.Rotation.Pitch, controller.Rotation.Yaw, pawn.Rotation.Yaw, pawn.bJustTeleported
    local ok, why = pcall(function()
        assert(portal_place("blue", start + V(300, 0, 0), V(-1, 0, 0)), "blue wasn't placed")
        assert(portal_place("orange", start + V(0, 900, 0), V(0, -1, 0)), "orange wasn't placed")
        local blue, orange = portals.blue, portals.orange
        assert(blue.actor.SisterPortal == orange.actor, "the portals aren't linked")
        assert(blue.actor.SceneCapture.ViewDestination == orange.actor, "blue's capture is linked to the wrong actor")
        assert(blue.actor.TextureTarget and blue.actor.PortalMaterial, "blue has no render target or material")
        assert(blue.actor.StaticMesh.StaticMesh == sphere, "blue's disc has no shape")

        local eyeOffset = V(0, 0, 45)
        local pov = { Location = {}, Rotation = { Pitch = 0, Yaw = 0, Roll = 0 } }
        local function frame(at)
            set_vector(pawn.Location, at)
            set_vector(pov.Location, at + eyeOffset)
            update_view(pawn, controller, pov, 0.016)
        end
        local approach = blue.at + blue.normal * reach(pawn, blue.normal)
        local pause = blue.at + blue.normal * 5
        set_vector(pawn.Location, approach)
        set_vector(pawn.Velocity, V(600, 0, 0))
        set_vector(pawn.Acceleration, V.zero)
        begin_movement(pawn, approach + eyeOffset, 0.016)
        assert(overlap and not pawn.bCollideWorld, "the cylinder couldn't enter the opening")
        frame(pause)
        assert(pawn.bCollideWorld, "collision was left off between camera updates")
        set_vector(pawn.Velocity, V.zero)
        for _ = 1, 16 do
            begin_movement(pawn, pause + eyeOffset, 0.016)
            frame(pause)
        end
        assert(overlap and overlap.portal == blue and of(pawn.Location):dist(pause) < 0.01,
            "standing in the opening moved the pawn")

        set_vector(pawn.Velocity, V(600, 0, 0))
        begin_movement(pawn, pause + eyeOffset, 0.016)
        local crossed = blue.at - blue.normal * 6
        frame(crossed)
        local expected = orange.at + orange.normal * 6
        assert(overlap and overlap.portal == orange, "the camera crossing didn't enter orange")
        assert(of(pawn.Location):dist(expected) < 0.01, "the exit changed the crossing depth")
        assert(of(pov.Location):dist(expected + eyeOffset) < 0.01, "the view snapped away from the captured view")
        assert(pov.Rotation.Yaw == 49152, "the view didn't turn with the portal")
        assert(of(pawn.Velocity):dist(V(0, -600, 0)) < 0.01, "the exit changed the transformed speed")

        set_vector(pawn.Velocity, V(0, 600, 0))
        begin_movement(pawn, expected + eyeOffset, 0.016)
        frame(orange.at - orange.normal * 3)
        assert(overlap and overlap.portal == blue, "the player couldn't reverse through the opening")
        begin_movement(pawn, of(pov.Location), 0.016)
        frame(blue.at + blue.normal * (reach(pawn, blue.normal) + 4))
        assert(not overlap and pawn.bCollideWorld, "world collision wasn't restored on leaving the opening")

        local camera = controller.PlayerCamera
        set_vector(pawn.Location, approach)
        set_vector(pawn.Velocity, V.zero)
        begin_movement(pawn, approach + eyeOffset, 0.016)
        assert(overlap and overlap.moving, "the live camera test didn't reach the opening")
        mirage.call(camera, "UpdateCamera", { DeltaTime = 0.016 })
        assert(pawn.bCollideWorld, "the live camera update did not finish the movement frame")
    end)
    close_all()
    cameraBlend = nil
    set_vector(pawn.Location, start)
    set_vector(pawn.Velocity, velocity)
    set_vector(pawn.Acceleration, acceleration)
    controller.Rotation.Pitch, controller.Rotation.Yaw, pawn.Rotation.Yaw, pawn.bJustTeleported = pitch, yaw, bodyYaw, teleported
    if not ok then return false, why end
end)
