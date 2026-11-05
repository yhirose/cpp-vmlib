-- Varargs. A function's `...` is the IR's Func::rest_param, and the end of
-- an argument list that expands -- a call, or `...` itself -- is a
-- Tag::CallSpread: one at each end of a call whose count is decided when
-- it runs.

local function two() return 1, 2 end
local function third(a, b, c) return c end

-- The last argument expands to every value it has; any other is one.
print(third(0, two()))
print(third(two(), 0))
print(third(0, (two())))

-- `...` in each place a list of values can go.
local function count(...) return select('#', ...) end
print(count(), count(nil), count(1, nil, 3), count(two()))
local function pass(a, ...) return a, ... end
print(pass(1, 2, 3))
print((pass(1, 2, 3)))
local function first(...) local a = ... return a end
print(first(7, 8), first())
local function pair(...) local a, b = ... return b, a end
print(pair('x', 'y'))
local function tbl(...) return {...} end
print(#tbl(1, 2, 3), #tbl())
local function sum(...)
  local s = 0
  for _, v in ipairs({...}) do s = s + v end
  return s
end
print(sum(1, 2, 3, 4), sum())
-- The main chunk is a vararg function too, and is given nothing here.
print(select('#', ...))

-- select, table.pack and table.unpack.
print(select(2, 'a', 'b', 'c'))
print(select(-1, 'a', 'b', 'c'))
print(select(5, 'a', 'b'))
local function sel(...) return select(...) end
print(sel(2, 'a', 'b', 'c'), sel('#', 'a', 'b'))
local packed = table.pack(1, nil, 3)
print(packed.n, packed[1], packed[2], packed[3])
print(table.unpack({10, 20, 30}))
print(table.unpack({10, 20, 30}, 2))
print(table.unpack({10, 20, 30}, 2, 3))
print(third(table.unpack({1, 2, 3})))

-- Methods, __call and pcall expand their last argument too.
local obj = {k = 5}
function obj:m(...) return self.k, select('#', ...) end
print(obj:m(two()))
local callable = setmetatable({}, {
  __call = function(self, ...) return select('#', ...) end
})
print(callable(two()))
print(pcall(third, 0, two()))
local function guarded(...) return pcall(...) end
print(guarded(sel, -1, 'p', 'q'))

-- `return f(...)` is still a tail call: a call twice as deep as the
-- executor allows calls to nest.
local function down(n, ...)
  if n == 0 then return select('#', ...) end
  return down(n - 1, ...)
end
print(down(20000, 'a', 'b'))

-- A call with a string literal for its argument reads it like any other.
local function echo(s) return s end
print(echo"lit", type"lit")
print"lit"
