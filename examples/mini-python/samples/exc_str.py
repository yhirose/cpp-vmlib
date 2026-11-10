# str of an exception: nothing for no arguments, the argument's str for one,
# the tuple's for several -- and, for a KeyError with one argument, that
# argument's repr.
print(repr(str(ValueError())), repr(str(ValueError(5))), repr(str(ValueError("a", 2))))
print(repr(str(ValueError(None))), repr(str(ValueError([1, "x"]))))
print(repr(str(KeyError("b"))), repr(str(KeyError(3))), repr(str(KeyError())))
print(repr(str(KeyError("a", "b"))))


class Keyed(KeyError):
    pass


print(repr(str(Keyed("k"))))
try:
    raise KeyError("k")
except KeyError as e:
    print("caught", e)
try:
    assert 1 == 2, 5
except AssertionError as e:
    print(repr(str(e)))
