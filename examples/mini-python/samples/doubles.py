# How a float prints: the shortest digits that round trip, in Python's
# layout. Fixed notation from 1e-4 up to (not including) 1e16, an exponent
# outside that, and a ".0" on a whole number in fixed notation. The same
# text from print, str, an f-string and the repr of a list or a dict.
big = 1e300 * 1e300
nan = big - big
values = [
    -0.0, 0.1, 100000.0, 0.0001, 0.00001, 1e15, 1e16,
    12345678901234567.0, 5e-324, big, -big, nan,
]
for v in values:
    print(v)
print(values[3:7])
print({"a": 1e16, "b": 0.00001})
print(str(1e16) + " " + str(0.0001) + " " + repr(1e-05))
print(f"{1e16} {0.00001} {100000.0} {-0.0}")
