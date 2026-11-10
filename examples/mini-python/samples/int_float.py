# int() of a float truncates exactly, past an int64 too: the double's own
# digits, not a rounded or refused value. infinity and NaN are errors.
# Each line is its own check.
def show(thunk):
    try:
        print(thunk())
    except Exception as e:
        print(type(e).__name__, e)


for x in [2.5, -2.5, 0.9, -0.9, 0.0, -0.0, 4.5e15, 123456789012345678.0]:
    show(lambda: int(x))
for x in [2.0 ** 62, 2.0 ** 63 - 1024, -2.0 ** 63, -2.0 ** 63 + 1024]:
    show(lambda: int(x))
for x in [2.0 ** 63, -2.0 ** 63 - 2048, 1e19, -1e19, 1e20, -1e20, 1e22,
          2.0 ** 70, 1e300, 2.0 ** 1023, 1.7976931348623157e308,
          -1.7976931348623157e308]:
    show(lambda: int(x))
show(lambda: int(float("inf")))
show(lambda: int(float("-inf")))
show(lambda: int(float("nan")))
show(lambda: type(int(1e19)).__name__)
show(lambda: int(1e19) + 1)
show(lambda: int(1e19) == 10 ** 19)
show(lambda: int(-1e19) < -(10 ** 18))
show(lambda: int(2.0 ** 64) * 3)
