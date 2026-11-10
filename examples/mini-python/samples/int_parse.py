# int() of a string reads digits with a single underscore between two of
# them, as float() does; what is not a string or a number is a TypeError
# that names its type.
def show(thunk):
    try:
        print(thunk())
    except Exception as e:
        print(type(e).__name__, e)


show(lambda: int("1_0"))
show(lambda: int(" -1_0_0 "))
show(lambda: int("0_0"))
show(lambda: int("99999999999999999999999"))
show(lambda: int("1__0"))
show(lambda: int("_1"))
show(lambda: int("1_"))
show(lambda: int("x"))
show(lambda: int(""))
show(lambda: int("1.5"))
show(lambda: int(None))
show(lambda: int([1]))
show(lambda: int((1, 2)))
show(lambda: int(True))
show(lambda: int(2.5))
show(lambda: int(-2.5))
