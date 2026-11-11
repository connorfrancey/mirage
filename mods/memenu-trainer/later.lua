-- Work for the game thread. on_trigger and on_disable are called from the menu's thread; anything that touches the
-- game is queued here and runs in the next tick event, on the game thread (in a level).
local M = { queue = {} }

function M.run(fn)
    M.queue[#M.queue + 1] = fn
end

mirage.on("tick", function()
    local queue = M.queue
    M.queue = {}
    for _, fn in ipairs(queue) do fn() end
end)

return M
