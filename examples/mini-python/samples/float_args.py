# float() of what is not a string: a bool is a number, and anything else
# that is not one is a TypeError that names its type.
def show(thunk):
    try:
        print(thunk())
    except Exception as e:
        print(type(e).__name__, e)


show(lambda: float(True))
show(lambda: float(False))
show(lambda: float(3))
show(lambda: float(2 ** 70))
show(lambda: float(2.5))
show(lambda: float(None))
show(lambda: float([1]))
show(lambda: float((1, 2)))
