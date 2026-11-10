# An exception keeps what it was made with, as `e.args`: a tuple, and the
# same one each time it is asked for. Each line is its own check: a line
# that raises says so and the next one still runs.
def show(thunk):
    try:
        print(thunk())
    except Exception as e:
        print("raised", type(e).__name__)


def caught(thunk):
    try:
        thunk()
    except Exception as e:
        return e


def assertion():
    try:
        assert 1 == 2, "m"
    except AssertionError as e:
        return e


def bare_assertion():
    try:
        assert False
    except AssertionError as e:
        return e


show(lambda: caught(lambda: 1 / 0).args)
show(lambda: caught(lambda: {}["b"]).args)
show(lambda: caught(lambda: [1][5]).args)
show(lambda: (ValueError().args, ValueError("a").args, ValueError("a", 2).args))
show(lambda: (ValueError(None).args, ValueError([1, "x"]).args))
show(lambda: caught(lambda: [][0]).args)
show(lambda: assertion().args)
show(lambda: bare_assertion().args)
k = KeyError("k")
show(lambda: k.args is k.args)
show(lambda: type(k.args).__name__)


class Plain(Exception):
    pass


class WithInit(Exception):
    def __init__(self, x):
        super().__init__(x, 1)


show(lambda: Plain("p", 2).args)
show(lambda: WithInit(3).args)
