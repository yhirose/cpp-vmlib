# An exception made from a failure of the language itself keeps its message
# as args, a tuple like any other.
def caught(thunk):
    try:
        thunk()
    except Exception as e:
        return e


e = caught(lambda: 1 / 0)
print(e.args, type(e.args).__name__, len(e.args))
print(repr(e), str(e))
t = caught(lambda: len(5))
print(t.args, type(t.args).__name__, len(t.args))
print(repr(t))
print(caught(lambda: [].pop()).args)
