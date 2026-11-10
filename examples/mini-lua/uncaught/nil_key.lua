-- A store at a nil key ends the program with the error Lua raises, line 5:
-- "nil_key.lua:5: table index is nil" (lua prints it after "lua: ", and a
-- traceback).
local t = {}
t[nil] = 1
