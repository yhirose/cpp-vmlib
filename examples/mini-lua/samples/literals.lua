-- Integer literals: one that does not fit an int64 is a float, read from
-- its digits (Lua 5.4 manual, 3.1). math.type tells the two apart.
print(9223372036854775807, math.type(9223372036854775807))
print(9223372036854775808, math.type(9223372036854775808))
print(18446744073709551616)
print(99999999999999999999999)
-- The minus is an operator, applied to the float: Lua has no -2^63 literal
-- (math.mininteger, or the subtraction below, is how it is written).
print(-9223372036854775808, math.type(-9223372036854775808))
print(-9223372036854775807 - 1)
print(9223372036854775808 == 2^63)

-- String escapes (manual 3.1). `bytes` shows what a literal holds, byte by
-- byte, so that a NUL or a byte past 127 reads as a number.
local function bytes(s)
  local out = #s .. ":"
  for i = 1, #s do
    out = out .. " " .. string.byte(s, i)
  end
  return out
end
print(bytes("\a\b\f\n\r\t\v\\\"\'"))
print(bytes('\a\"\''))
print(bytes("\65\x41\u{48}"))
print(bytes("\0\00\000\1\12\123"))
print(bytes("\0651"))
print(bytes("\1234"))
print(bytes("\12a"))
print(bytes("\8\9"))
print(bytes("\255\xff\xFF\xaB"))
print(bytes("\u{0}\u{7F}\u{80}\u{7FF}\u{800}\u{20AC}\u{FFFF}\u{10000}\u{10FFFF}"))
print(bytes("\u{200000}\u{3FFFFFF}\u{4000000}\u{7FFFFFFF}"))
print(bytes("\u{D800}\u{0000000041}"))
print(bytes("a\z

   b"))
print(bytes("\z"))
print(bytes("a\
b"))
print(bytes("x\z  y\z  z"))
print("tab\there", "quote\"s", 'it\'s')
