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
