# repr of an exception is its class and its arguments, Name(arg, arg), and
# the class's own name for a class of the program's.
print(repr(ZeroDivisionError("division by zero")))
print(repr(KeyError("b")), repr(KeyError(3)), repr(KeyError()))
print(repr(AssertionError()), repr(AssertionError("m")))
print(repr(ValueError()), repr(ValueError("a", 2)), repr(ValueError(None)))
print(repr(TypeError([1, "x"])), repr(IndexError("it's")))


class Plain(Exception):
    pass


class Keyed(KeyError):
    pass


print(repr(Plain(5)), repr(Plain()), repr(Keyed("k")))
try:
    1 / 0
except ZeroDivisionError as e:
    print(repr(e))
try:
    {"a": 1}["zz"]
except KeyError as e:
    print(repr(e))
print([ValueError("in a list")])
