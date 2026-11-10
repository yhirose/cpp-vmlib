# The arguments belong to the exception before any __init__ runs: a class
# whose own __init__ never says super() still has them.
def show(thunk):
    try:
        print(thunk())
    except Exception as e:
        print("raised", type(e).__name__)


class E(Exception):
    def __init__(self, x):
        self.x = x


e = E(5)
show(lambda: e.x)
show(lambda: e.args)
show(lambda: str(e))
show(lambda: repr(e))


class Both(Exception):
    def __init__(self, a, b):
        self.total = a + b


b = Both(1, 2)
show(lambda: (b.total, b.args, str(b), repr(b)))
