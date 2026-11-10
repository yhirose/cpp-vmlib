-- What the VM itself traps on reaches pcall as the string Lua's own error
-- would be: the chunk and line, then "attempt to ...". Lua adds the name of
-- the variable when it knows one ("(local 't')"), which the VM does not, so
-- every operand here is the result of a call, which Lua does not name either.
local function tbl() return {} end
local function nothing() end
local function yes() return true end
local function five() return 5 end

local function show(f)
  print(select(2, pcall(f)))
end

show(function() return tbl() + 1 end)
show(function() return 1 - tbl() end)
show(function() return nothing() * 2 end)
show(function() return -tbl() end)
show(function() return tbl() < 1 end)
show(function() return 1 < tbl() end)
show(function() return tbl() > nothing() end)
show(function() return tbl() < tbl() end)
show(function() return tbl()() end)
show(function() return yes()() end)
show(function() return #five() end)
show(function() return nothing().x end)
show(function() return yes().x end)
show(function() nothing().x = 1 end)
show(function() return five() // 0 end)
show(function() return five() % 0 end)

-- A table is not an error object: pcall answers the value as it is.
local t = {}
print(select(2, pcall(function() error(t) end)) == t)

-- The error of a coroutine is the same string.
local co = coroutine.create(function() return tbl() + 1 end)
print(coroutine.resume(co))

-- A library function's complaint of an argument, and `..` of a value that
-- is neither a string nor a number.
show(function() return math.floor(tbl()) end)
show(function() return string.rep(nothing(), 2) end)
show(function() return string.rep("x", tbl()) end)
show(function() return string.sub(nothing(), 1) end)
show(function() return "a" .. tbl() end)
show(function() return nothing() .. "a" end)
show(function() return select(0, "x") end)
show(function() table.insert({}, 5, "x") end)

-- A numeric for whose step is zero fails before its first iteration, for an
-- integer or a float step, written or computed.
local zero = 0
show(function() for i = 1, 10, 0 do end end)
show(function() for i = 1.0, 10, 0 do end end)
show(function() for i = 1, 10, 0.0 do end end)
show(function() for i = 1, 10, zero do end end)

-- error's level: 0 adds no position, 1 (the default) the line of the call to
-- error, 2 the line of the call that entered the function holding it, which
-- is the function's caller's to blame. A level that is a whole float counts;
-- one that is not a number is an argument error. A value that is not a
-- string is raised as it is, whatever the level. (Level 2 where the caller
-- is pcall, after a tail call or in a coroutine's body, and levels past 2,
-- are where this front end differs: README.md.)
local function lvl2() error("lvl2", 2) end
local function lvl0() error("lvl0", 0) end
local function lvl(n) error("lvln", n) end
show(function() error("lvl0", 0) end)
show(function() lvl0() end)
show(function()
  lvl2()
end)
show(function() lvl(2) end)
show(function() lvl(2.0) end)
show(function() lvl(0) end)
show(function() lvl(-1) end)
show(function() error("lvl1", 1) end)
show(function() error("lvl1f", 1.0) end)
show(function() error("lvlnil", nil) end)
show(function()
  lvl2(
    1,
    2)
end)
show(function() error("lvl1.5", 1.5) end)
show(function() error("lvlstr", {}) end)
show(function() error("lvlbool", true) end)
local obj = {}
print(select(2, pcall(function() error(obj, 2) end)) == obj)
print(select(2, pcall(function() error(obj, 0) end)) == obj)
print(select(2, pcall(function() error(42, 2) end)))
print(select(2, pcall(function() error(nil, 2) end)))

-- A table has no nil or NaN key: storing at one is an error, whatever the
-- value (a nil value stores nothing, and still names a key), also in a
-- constructor. Reading at one answers nil. A metatable's __newindex is asked
-- first, and takes any key and any value; rawset has none to ask, and
-- stores nothing for a nil value. (rawset's own message has no position in
-- Lua, which is where this front end differs: README.md.)
local keys = {}
local nilkey
show(function() keys[nil] = 1 end)
show(function() keys[0/0] = 1 end)
show(function() keys[nilkey] = nil end)
show(function() keys[0/0] = nil end)
show(function() return {[nilkey] = 1} end)
show(function() return {[0/0] = 1} end)
show(function() local a, b = 1, 2; keys[a], keys[nilkey] = a, b end)
print(keys[nil], keys[0/0], keys[1])
print((pcall(function() rawset(keys, nilkey, 1) end)))
print((pcall(function() rawset(keys, 0/0, 1) end)))
show(function() rawset(1, 2, 3) end)
show(function() rawset("s", 2, 3) end)
local seen = setmetatable({}, {__newindex = function(_, k, v)
  print("newindex", k, v)
end})
seen[nilkey] = 1
seen.absent = nil
seen.absent = 2
rawset(keys, "q", 5)
rawset(keys, "q", nil)
rawset(keys, 1, 1)
rawset(keys, 2, 2)
rawset(keys, 2, nil)
local n = 0
for _ in pairs(keys) do n = n + 1 end
print(n, #keys)

-- table.concat joins strings and numbers, a number written as tostring
-- writes it, and the separator may be a number. Any other element names its
-- type and index; a non-table is an argument error. (Its i and j arguments
-- are not supported: README.md.)
show(function() return table.concat({1, {}}) end)
show(function() return table.concat({"a", "b", true}, ",") end)
show(function() return table.concat({"a", "b", nothing}, ",") end)
show(function() return table.concat({1.5, "x", 3}, 0) end)
show(function() return table.concat({1, 2}, 3) end)
show(function() return table.concat({1, 2}, {}) end)
show(function() return table.concat(1) end)
show(function() return table.concat("s", ",") end)
show(function() return table.concat({}, ",") .. "|" end)

-- A library function called without an argument it requires names it and its
-- number, in Lua's words; a method call counts after the receiver. An
-- expanding last argument that yields nothing is not told apart from nil
-- here (README.md).
show(function() return tostring() end)
show(function() return type() end)
show(function() return getmetatable() end)
show(function() for k in ipairs() do end end)
show(function() for k in pairs() do end end)
show(function() return math.floor() end)
show(function() return rawget({}) end)
show(function() return rawset({}, 1) end)
show(function() return setmetatable() end)
show(function() return string.upper() end)
show(function() return string.rep("x") end)
show(function() return ("x"):rep() end)
show(function() return ("x"):sub() end)
show(function() return string.len() end)
show(function() return table.concat() end)
show(function() return coroutine.create() end)
show(function() return pcall() end)
-- The arguments given run before the one that is missing.
local function mark() print("ran") return 1 end
show(function() return rawset({}, (mark())) end)
print(tostring(nil), type(nil), string.len(12))

-- setmetatable wants a table, then nil or a table, and says which it did not
-- get: the first argument first. string.upper wants a string.
local notmeta, meta = 5, {}
show(function() return setmetatable(1, {}) end)
show(function() return setmetatable("s", {}) end)
show(function() return setmetatable(nil, {}) end)
show(function() return setmetatable({}, 1) end)
show(function() return setmetatable({}, notmeta) end)
show(function() return setmetatable(1, notmeta) end)
show(function() return setmetatable(1, meta) end)
show(function() return setmetatable({}, "x") end)
show(function() return setmetatable({}, true) end)
show(function() return string.upper({}) end)
print(getmetatable(setmetatable({}, meta)) == meta)
print(getmetatable(setmetatable({}, nil)))
