-- t[k] and t[k] = v: a key the table has, one it lacks, a table with a
-- metatable (its misses are __index's and __newindex's, its hits are not),
-- and nil, which removes.
local t = {}
t.a = 1
t["b"] = 2
t[1] = "one"
t[2.0] = "two"
t[true] = "yes"
print(t.a, t.b, t[1], t[2], t[true], t.zzz, t[3])
print(#t)

-- nil removes the key, and assigning it again brings it back
t.a = nil
print(t.a)
local n = 0
for _ in pairs(t) do n = n + 1 end
print(n)
t.a = 5
print(t.a)

-- a float key that is a whole number is the integer's
t[3.0] = "three"
print(t[3], #t)

-- append, and overwrite
local xs = {}
for i = 1, 5 do xs[#xs + 1] = i * i end
xs[2] = 0
print(xs[1], xs[2], xs[5], xs[6], #xs)

-- a metatable: misses go to __index and __newindex, hits do not
local log = {}
local proxy = setmetatable({present = "mine"}, {
  __index = function(_, k) return "index:" .. tostring(k) end,
  __newindex = function(_, k, v) log[#log + 1] = tostring(k) .. "=" .. tostring(v) end,
})
print(proxy.present, proxy.missing)
proxy.present = "changed"
proxy.fresh = 1
proxy.fresh = 2
print(proxy.present, proxy.fresh, rawget(proxy, "fresh"))
print(table.concat(log, ","))

-- a key holding false is a hit, so __index is not asked
local d = setmetatable({flag = false}, {__index = function() return "default" end})
print(d.flag, d.other)

-- __index as a table, and a miss all the way down
local base = {x = 10}
local derived = setmetatable({}, {__index = base})
print(derived.x, derived.y)
derived.x = 20
print(derived.x, base.x)

print(({})[1], ({})[nil])

-- a stored value that is false or 0 is stored
local s = {}
s.f = false
s.z = 0
print(s.f, s.z, s.nope)
