-- A trap the program does not catch ends it with the error Lua would
-- raise: "trap.lua:5: attempt to perform arithmetic on a nil value" (lua
-- adds the variable's name when it knows one).
local function nothing() end
local x = nothing() + 1
