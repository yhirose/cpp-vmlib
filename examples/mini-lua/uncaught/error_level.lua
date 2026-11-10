-- error with level 2 blames the call that entered the function: the line
-- Lua reports is the call's, here line 6, not the error's own line 3.
local function check(v)
  error("bad value: " .. v, 2)
end
check("x")
