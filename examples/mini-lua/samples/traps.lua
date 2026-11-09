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
