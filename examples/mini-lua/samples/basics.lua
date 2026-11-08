-- Numbers, operators, and the three rules Lua does not share with the VM's
-- defaults: what is true, how `%` and `//` round, and what `==` does
-- across two types.

print(1 + 2 * 3, 7 - 2, 3 * 4)
-- `/` is always a float, `//` floors, and `%` follows `//`. BinOp::Div on
-- two ints truncates and BinOp::Mod is C's, so both of these are
-- BinOp::FloorDiv and BinOp::FloorMod.
print(7 / 2, 7 // 2, -7 // 2, 7 % 3, -7 % 3, 7 % -3)
-- A float on either side floors too, and the answer is a float: the
-- quotient of the rounded division, the remainder on the divisor's side
-- (a zero one on the dividend's, and a tiny one whose product with the
-- divisor underflows all the same), and a zero divisor's inf or nan.
print(7.5 // 2, -7.5 // 2, 7 // 2.5, 1 // 0.2, 7.5 // 0, -7.5 // 0)
print(7.5 % 2, -7.5 % 2, 7 % 2.5, 7.5 % -2, -4.0 % 2, 4.0 % -2)
print(1e-320 % -1e-10)
-- A nan is asked about rather than printed: Lua spells it as the C
-- library does, "-nan" on one machine and "nan" on another.
print(7.5 % 0 ~= 7.5 % 0)
-- Which way to round is the signs' to say, not their product's, which
-- wraps past 2^63.
print(-3 % 4611686018427387904, -3 // 4611686018427387904)
-- The least integer over -1 wraps to itself, and leaves 0.
print((-9223372036854775807 - 1) // -1, (-9223372036854775807 - 1) % -1)
print(2 ^ 10, 2 ^ 0.5)
print(math.type(1), math.type(1.0), math.type(7 // 2), math.type(7 / 2))

print("a" .. "b", "n=" .. 1, 1 .. 2)
print(#"hello", #"")

-- Only nil and false are false. Value::truthy() calls 0 false, and its
-- comment says why it will not decide: "Lua calls neither" falsy.
local function t(v) if v then return "T" else return "F" end end
print(t(0), t(""), t({}), t(nil), t(false), t(1))

-- `==` across two types is false, where BinOp::Eq refuses the comparison.
print(1 == 1.0, 1 == "1", "a" == "a", nil == false, {} == {})
print(1 ~= 2, 1 < 2, 2 <= 2, "a" < "b")

-- and/or answer one of their operands, not a boolean.
print(1 and 2, nil and 2, false or "x", nil or "y", 0 or "z")
print(not nil, not 0, not "")

print(tostring(1), tostring(1.0), tostring(10 / 2), tostring(0.1 + 0.2))
print(type(1), type("s"), type({}), type(print), type(nil))

local n, i = 0, 0
while i < 5 do i = i + 1; n = n + i end
print(n, i)

local r = 0
repeat r = r + 1 until r == 3
print(r)

print(string.upper("abc"), ("xy"):rep(3), ("hello"):sub(2, 3), ("hello"):sub(-3))

-- Multiple assignment evaluates every target's table and key before the
-- values, so the i in arr[i] is the old one; a single target still
-- evaluates every value, and takes the first.
local function noisy(tag, v) io.write("[" .. tag .. "]") return v end
local arr, i = {}, 3
i, arr[i] = i + 1, 20
print(i, arr[3], arr[4])
local only = noisy("a", 1), noisy("b", 2)
print(only)
