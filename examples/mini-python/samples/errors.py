# raise / except / finally. `finally` is a Defer inside the Scope wrapping
# the try, so it runs however the block is left, and `except NAME`
# re-raises what it does not match.

def attempt(f):
    try:
        return "ok " + str(f())
    except Exception as e:
        return "caught " + type(e).__name__ + ": " + str(e)

print(attempt(lambda: 1 + 1))
print(attempt(lambda: 1 // 0))

def boom():
    raise ValueError("something broke")

print(attempt(boom))

order = []
try:
    order.append("try")
    raise TypeError("bad")
except Exception as e:
    order.append("except " + type(e).__name__)
finally:
    order.append("finally")
order.append("after")
print(order)

# finally on the way out through a return: the value is already decided
# when the deferred block runs.
trace = []

def through_return():
    try:
        trace.append("body")
        return "returned"
    finally:
        trace.append("finally on return")

print(through_return(), trace)

# finally on the way out through an exception nothing here catches.
unwound = []

def through_raise():
    try:
        raise RuntimeError("deep")
    finally:
        unwound.append("finally on raise")

print(attempt(through_raise), unwound)

# finally on the way out of a loop, through break.
loop = []
for i in range(3):
    try:
        if i == 1:
            break
        loop.append("body " + str(i))
    finally:
        loop.append("finally " + str(i))
print(loop)

# `except NAME` is selective: the wrong name re-raises, and the outer
# handler sees it.
def selective(kind):
    try:
        try:
            if kind == "value":
                raise ValueError("v")
            raise TypeError("t")
        except ValueError as e:
            return "inner caught " + str(e)
    except Exception as e:
        return "outer caught " + type(e).__name__
    return "unreachable"

print(selective("value"))
print(selective("type"))

# Container failures raise the exceptions Python names them by.
print(attempt(lambda: [1, 2][9]))
print(attempt(lambda: {"a": 1}["zz"]))
