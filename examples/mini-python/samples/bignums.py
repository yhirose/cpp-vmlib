# The headline. `Value` holds an int64 or a double and nothing wider --
# "a decision rather than a gap", says the top-level README -- so a
# language whose integers are unbounded carries them itself, as a
# little-endian array of limbs in base 10^9. Python's `int` is exactly
# such a language, which makes python3 an oracle for the recipe.

# Powers, which is the shortest way past int64.
print(2 ** 10)
print(2 ** 62)
print(2 ** 63)
print(2 ** 64)
print(2 ** 100)
print(2 ** 200)
print(3 ** 100)
print(10 ** 30)

# Factorials, which is the shortest way past it by multiplication rather
# than by squaring -- the promotion rule has to fire in the middle of a
# loop, not at a literal.
def fact(n):
    acc = 1
    for i in range(1, n + 1):
        acc = acc * i
    return acc

print(fact(20))
print(fact(21))
print(fact(30))
print(fact(50))

# Addition and subtraction across the boundary, in both directions.
big = 2 ** 100
print(big + 1)
print(big - 1)
print(big + big)
print(big - big)
print(1 - big)
print(-big + big)
print(big * -1)

# A result that comes back *down* is a machine integer again, which is
# what keeps ordinary arithmetic ordinary.
print((2 ** 100) // 1 if False else big - (big - 7))
print(type(big - (big - 7)).__name__, type(big).__name__)

# Ordering and equality reach across the two representations.
print(big > 0, big > 2 ** 99, big < 2 ** 101, -big < 0)
print(big == 2 ** 100, big == big + 1, big != 0)
print(2 ** 64 == 18446744073709551616)

# A literal too wide for an int64 is built as a bignum at bind time.
print(123456789012345678901234567890)
print(123456789012345678901234567890 + 1)
print(123456789012345678901234567890 * 2)
print(-123456789012345678901234567890)

# Fibonacci, which is the classic reason to want them.
def fib(n):
    a = 0
    b = 1
    for i in range(n):
        a, b = b, a + b
    return a

print(fib(100))
print(fib(300))

# Digits, since printing is the operation base 10^9 was chosen to make
# free of division.
print(len(str(fact(100))))
print(str(2 ** 128))
