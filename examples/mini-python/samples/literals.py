# Integer literals past int64 are bignums (nothing is clamped), and the
# smallest int64 is just a negated literal.
print(9223372036854775807, -9223372036854775808, 9223372036854775808)
print(-9223372036854775809, 99999999999999999999999 + 1)

# String escapes. repr shows what a string holds where it would not read.
print("\x41\x4a\x4A\u0042\U00000043\101\1234")
print(len("\a\b\f\n\r\t\v\\\'\""))
print(repr("\0\7\x7f\xe9\u00e9"))
print(repr("\400\777"))
print(repr("\u00e9\u20ac"), repr("\x00\x1f"))
print("a\
b")
print("a\tb\"c\\d\'e")
print(f"{1 + 1}\x41{{}}\t|")
print(f"\u0041{1}\102")
