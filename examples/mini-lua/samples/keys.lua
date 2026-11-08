-- rawset and rawget take the same key the table syntax does: a float that is
-- a whole number is the integer, wherever the integer is in 64 bits.
local t = {}
rawset(t, 2^53, "a")
print(t[2^53], t[9007199254740992], rawget(t, 2^53), rawget(t, 9007199254740992))
t[2^60] = "b"
print(t[2^60], rawget(t, 2^60), rawget(t, 1152921504606846976))
rawset(t, 2^60, "c")
print(t[2^60], t[1152921504606846976])
rawset(t, -2^63, "d")
print(t[-2^63], rawget(t, -2^63), t[(-9223372036854775807 - 1)])
rawset(t, 2^63, "e")
print(t[2^63], rawget(t, 2^63))
rawset(t, -0.0, "f")
print(t[0], t[-0.0], rawget(t, 0), rawget(t, 0.0))
rawset(t, 1.5, "g")
print(t[1.5], rawget(t, 1.5))
rawset(t, 3.0, "h")
print(t[3], rawget(t, 3), rawget(t, 3.0))
local n = 0
for k in pairs(t) do n = n + 1 end
print(n)
-- a table with a metatable stores through the helper and is read by the VM
local m = setmetatable({}, {__newindex = function(tt, k, v) rawset(tt, k, v) end})
m[2^53] = "x"
m[2^60] = "y"
m[3.0] = "z"
print(m[2^53], m[9007199254740992], m[2^60], m[3], m[3.0])
print(rawget(m, 2^53), rawget(m, 2^60), rawget(m, 3))
-- table.insert takes a float position
local list = {10, 20, 30}
table.insert(list, 2.0, 15)
print(list[1], list[2], list[3], list[4], #list)
