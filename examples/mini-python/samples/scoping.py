# The statements that say where a name lives, and the ones that are about
# a name rather than a value: global, nonlocal, del, assert, is -- plus
# decorators and `yield from`, which are both one rewrite each.

count = 0


def bump():
    global count
    count += 1
    return count


print(bump(), bump(), count)


def shadow():
    count = 100  # a local, because there is no `global` here
    return count


print(shadow(), count)


# `nonlocal` is what a counter closure needs, and the reason
# samples/closures.py had to use a list instead.
def counter():
    n = 0

    def step():
        nonlocal n
        n += 1
        return n

    return step


c = counter()
print(c(), c(), c())
c2 = counter()
print(c2(), c())


def outer():
    x = "outer"

    def middle():
        x = "middle"

        def inner():
            nonlocal x
            x = "inner"

        inner()
        return x

    return middle(), x


print(outer())


# `global` reaches past an enclosing function, not just one level.
def wrapper():
    count = "shadowed"

    def deep():
        global count
        count += 100
        return count

    return deep(), count


print(wrapper(), count)


# `is` is identity, and `is None` is the shape a program writes.
a = None
print(a is None, a is not None)
xs = [1, 2]
ys = xs
zs = [1, 2]
print(xs is ys, xs is zs, xs == zs)
print(xs is not zs, ys is xs)


# del, on a container.
d = {"a": 1, "b": 2, "c": 3}
del d["b"]
print(sorted(d.items()))
lst = [10, 20, 30, 40]
del lst[1]
print(lst)
del lst[-1]
print(lst)
try:
    del d["zz"]
except KeyError as e:
    print("KeyError:", e)


# assert.
assert 1 + 1 == 2
assert [1], "a non-empty list is true"
try:
    assert 1 == 2, "one is not two"
except AssertionError as e:
    print("AssertionError:", e)
try:
    assert False
except AssertionError as e:
    print("AssertionError:", repr(str(e)))


# Decorators: `@d def f` is `f = d(f)` and nothing more.
def loud(f):
    def wrapped(*args, **kw):
        return "<" + str(f(*args, **kw)) + ">"
    return wrapped


def twice(f):
    def wrapped(*args, **kw):
        return f(*args, **kw) + f(*args, **kw)
    return wrapped


@loud
def greet(who):
    return "hi " + who


print(greet("ada"))


@loud
@twice
def shout(s):
    return s.upper()


print(shout("ab"))


def tagged(tag):
    def deco(f):
        def wrapped(*args, **kw):
            return tag + ":" + str(f(*args, **kw))
        return wrapped
    return deco


@tagged("N")
def add(a, b):
    return a + b


print(add(2, 3), add(b=3, a=2))


# `yield from` is the loop it stands for.
def inner_gen():
    yield 1
    yield 2


def outer_gen():
    yield 0
    yield from inner_gen()
    yield from [3, 4]
    yield from (x for x in [5])
    yield 6


print(list(outer_gen()))


def flatten(rows):
    for row in rows:
        yield from row


print(list(flatten([[1, 2], [3], [], [4, 5]])))
