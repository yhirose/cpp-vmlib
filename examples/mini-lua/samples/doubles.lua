-- How a float prints: "%.14g", so at most 14 significant digits (not the
-- shortest that round trip), plain digits from 1e-4 up to (not including)
-- 1e14 and an exponent outside that ("1e+14", "1e-05"), and a ".0" when
-- the result then looks like an integer. print, tostring and `..` agree.
-- (A nan is not printed: Lua spells it "-nan" on one machine, "nan" on
-- another.)
local big = 1e300 * 1e300
local values = {
  0.0, -0.0, 0.1, 100000.0, 0.0001, 0.00001, 99999999999999.0, 1e14,
  123456789012345.0, 12345678901234567.0, 0.1 + 0.2, 1 / 3,
  5e-324, 1.7976931348623157e308, 2.0 ^ 53, big, -big, -1.5,
}
for _, v in ipairs(values) do
  print(v)
end
print(tostring(1e15) .. " " .. 0.00001 .. " " .. 100000.0)
print(1e100, 10 / 2, 3 // 1.0, 2 ^ 63)
