# Numbers, operators, and the rules Python does not share with the VM's
# defaults.

print(1 + 2 * 3, 7 - 2, 3 * 4, 2 ** 3)
# `//` and `%` floor, where BinOp::Div truncates and BinOp::Mod is C's.
print(7 // 2, -7 // 2, 7 % 3, -7 % 3, 7 % -3)
# `/` is always a float, even on two ints.
print(7 / 2, 6 / 3, 1 / 4)
print(-3, - -3, abs(-5), abs(5))

print("a" + "b", "ab" * 3, len("hello"))
print(str(1), str(1.0), str(10 / 2), str(0.1 + 0.2), str(None), str(True))
print(type(1).__name__, type(1.0).__name__, type("s").__name__)
print(type([]).__name__, type({}).__name__, type(None).__name__)

# Truthiness: 0, "", [], {} and None are false. Value::truthy() agrees
# about the first and the last and disagrees about the middle three, which
# its own comment gives as the reason it will not decide.
def t(v):
    if v:
        return "T"
    return "F"

print(t(0), t(""), t([]), t({}), t(None), t(False))
print(t(1), t("x"), t([0]), t({"a": 1}), t(True), t(0.5))
print(not 0, not 1, not "", not [1])

# `==` across two types is False rather than an error.
print(1 == 1.0, 1 == "1", "a" == "a", None == False, [1] == [1])
print(1 != 2, 1 < 2, 2 <= 2, "a" < "b", 3 > 2 >= 2)

# and/or answer one of their operands.
print(1 and 2, 0 and 2, "" or "x", None or "y", [] or 0)

# A conditional expression, which Python writes back to front.
print("yes" if 1 < 2 else "no", "yes" if 1 > 2 else "no")

n = 0
i = 0
while i < 5:
    i += 1
    n += i
print(n, i)

total = 0
for k in range(1, 6):
    if k == 3:
        continue
    if k == 5:
        break
    total += k
print(total)

print(list(range(4)), list(range(2, 6)), list(range(6, 0, -2)))
