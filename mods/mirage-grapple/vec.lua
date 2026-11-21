-- Small 3D vector helpers on {x, y, z} tables (what mirage.* takes and gives). Loaded with require("vec").
local vec = {}

function vec.new(x, y, z) return { x = x or 0, y = y or 0, z = z or 0 } end
function vec.of(v) return { x = v.x or v.X or 0, y = v.y or v.Y or 0, z = v.z or v.Z or 0 } end  -- Also game structs
function vec.add(a, b) return { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z } end
function vec.sub(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
function vec.scale(a, s) return { x = a.x * s, y = a.y * s, z = a.z * s } end
function vec.dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
function vec.cross(a, b)
    return vec.new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
end
function vec.len(a) return math.sqrt(vec.dot(a, a)) end
function vec.dist(a, b) return vec.len(vec.sub(a, b)) end
function vec.norm(a)
    local l = vec.len(a)
    if l < 1e-6 then return vec.new(0, 0, 0), 0 end
    return vec.scale(a, 1 / l), l
end
function vec.lerp(a, b, t) return vec.add(a, vec.scale(vec.sub(b, a), t)) end
-- The engine's struct form ({X =, Y =, Z =}), for writing pawn.Location / pawn.Velocity
function vec.engine(a) return { X = a.x, Y = a.y, Z = a.z } end

return vec
