# Closures, and the one place Python differs from every other front end
# here: its bindings are function-scoped and its closures are *late*.

def counter():
    n = 0
    def bump():
        # No `nonlocal` in this subset, so a counter keeps its state in a
        # container rather than in a rebound name -- see README.md.
        n[0] += 1
        return n[0]
    n = [0]
    return bump

c1 = counter()
c2 = counter()
print(c1(), c1(), c2(), c1())

# Late binding: three lambdas made in a loop all see the loop variable's
# final value, because they share one binding rather than getting one per
# iteration. That is why CellFresh runs once here, at the function's
# entry, where examples/mini-js runs it per iteration.
fs = []
for i in range(3):
    fs.append(lambda: i)
print(fs[0](), fs[1](), fs[2]())

# Binding by default argument is the usual workaround -- absent here, so
# the explicit one is a factory.
def make(k):
    return lambda: k

gs = []
for i in range(3):
    gs.append(make(i))
print(gs[0](), gs[1](), gs[2]())

# Two levels of nesting.
def outer(x):
    def middle(y):
        def inner(z):
            return x + y + z
        return inner
    return middle

print(outer(1)(2)(3))

double = lambda v: v * 2
add = lambda a, b: a + b
print(double(21), add(1, 2))

def apply_twice(f, v):
    return f(f(v))

print(apply_twice(double, 3), apply_twice(lambda s: s + "!", "hi"))

def fact(n):
    if n < 2:
        return 1
    return n * fact(n - 1)

print(fact(10), fact(25))

# A binding is function-scoped, not block-scoped: `found` outlives the
# loop and the `if` it was assigned in.
def search(xs, want):
    found = -1
    for idx in range(len(xs)):
        if xs[idx] == want:
            found = idx
    return found

print(search([3, 1, 2], 1), search([3, 1, 2], 9))
