# What the VM itself traps on reaches `except` as the exception python3
# raises for the same fault: its class and its message. Where CPython's
# words differ between releases (a zero divisor, a name read before it is
# assigned, the recursion limit's wording) only the class is shown.
def show(f):
    try:
        f()
    except Exception as e:
        print(type(e).__name__ + ": " + str(e))


def only_class(f):
    try:
        f()
    except Exception as e:
        print(type(e).__name__)


def plus():
    return 1 + "a"


def concat():
    return "a" + 1


def minus():
    return 1 - "a"


def times():
    return [1] * [2]


def divide():
    return 1 / "a"


def power():
    return 2 ** "a"


def less():
    return 1 < "a"


def greater_equal():
    return None >= 1


def negate():
    return -"a"


def call_int():
    x = 5
    return x()


def call_none():
    f = None
    return f()


def not_subscriptable():
    x = 5
    return x[0]


def index_key():
    xs = [1]
    return xs["a"]


def assign_str():
    s = "abc"
    s[0] = "x"


def length():
    return len(5)


def attribute():
    x = None
    return x.name


def zero():
    return 1 / 0


def floor_zero():
    return 1 // 0


def modulo_zero():
    return 1 % 0


def forever():
    return forever()


def before_assignment():
    print(y)
    y = 1


for f in [plus, concat, minus, times, divide, power, less, greater_equal,
          negate, call_int, call_none, not_subscriptable, index_key,
          assign_str, length, attribute, zero]:
    show(f)

for f in [floor_zero, modulo_zero, forever, before_assignment]:
    only_class(f)


# A clause names a class, so a trap of another kind goes past it.
def selective():
    try:
        try:
            1 + "a"
        except ZeroDivisionError:
            print("wrong clause")
    except TypeError as e:
        print("outer: " + str(e))


selective()
