# How a float prints: the shortest digits that round trip, in Python's
# layout. Fixed notation from 1e-4 up to (not including) 1e16, an exponent
# outside that, and a ".0" on a whole number in fixed notation. The same
# text from print, str, an f-string and the repr of a list or a dict.
big = 1e300 * 1e300
nan = big - big
values = [
    0.0, -0.0, 0.1, 0.5, 1.0, 100000.0, 123456789.0,
    0.0001, 0.00001, 0.00012345, 1e-7, 1.5e-10,
    999999999999999.0, 1e15, 1e16, 1e17, 1e20, 1e22,
    12345678901234567.0, 0.1 + 0.2, 1 / 3, 2.5e-5,
    5e-324, 1.7976931348623157e308, 2.0 ** 53, 2.0 ** 64,
    big, -big, nan, -1.5,
]
for v in values:
    print(v)
print(values[7:12])
print({"a": 1e16, "b": 0.00001})
print(str(1e16) + " " + str(0.0001) + " " + repr(1e-05))
print(f"{1e16} {0.00001} {100000.0} {-0.0}")
