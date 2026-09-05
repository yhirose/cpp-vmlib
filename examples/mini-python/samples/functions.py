# Defaults, keyword arguments, *args and **kwargs.
#
# The IR's calling convention fixes an argument count at the call site and
# Python's does not, so this front end writes its own on top: every
# function takes a positional array and a keyword object. See README.md.

def greet(name, greeting="Hello", punct="!"):
    return greeting + ", " + name + punct

print(greet("Ada"))
print(greet("Ada", "Hi"))
print(greet("Ada", punct="?"))
print(greet(greeting="Yo", name="Ada"))
print(greet("Ada", punct="...", greeting="Well"))


def total(*nums):
    s = 0
    for n in nums:
        s = s + n
    return s

print(total())
print(total(1))
print(total(1, 2, 3, 4))

xs = [10, 20, 30]
print(total(*xs))
print(total(1, *xs, 2))


def describe(label, *rest, **opts):
    parts = [label]
    for r in rest:
        parts.append(str(r))
    ks = list(opts.keys())
    for k in ks:
        parts.append(k + "=" + str(opts[k]))
    return " ".join(parts)

print(describe("a"))
print(describe("a", 1, 2))
print(describe("a", 1, loud=True, n=3))
print(describe("a", **{"z": 1}))

d = {"n": 9}
print(describe("a", 1, **d))


# A default is evaluated where the `def` stands, once, when the `def` runs.
# The famous consequence is a mutable default that accumulates.
def collect(x, into=[]):
    into.append(x)
    return into

print(collect(1))
print(collect(2))
print(collect(3, []))
print(collect(4))


# Which also means a default sees the value the name had at def time.
n = 1
def snapshot(v=n):
    return v
n = 2
print(snapshot(), n)


# Two closures made in a loop each get their own default.
fns = []
for i in range(3):
    def make(k=i):
        return k
    fns.append(make)
print(fns[0](), fns[1](), fns[2]())


# Lambdas take the same convention.
add = lambda a, b=10: a + b
print(add(1), add(1, 2))


# Methods too: self is simply the argument that goes in front.
class Counter:
    def __init__(self, start=0, step=1):
        self.n = start
        self.step = step

    def bump(self, times=1, *extra):
        self.n = self.n + self.step * times
        for e in extra:
            self.n = self.n + e
        return self.n

c = Counter()
print(c.bump(), c.bump(3), c.bump(1, 100, 200))
print(Counter(step=5).bump(2))
print(Counter(10, 2).bump())


# The errors the convention has to raise itself, because the IR's arity
# check knows neither the parameter's name nor that it had a default.
def strict(a, b):
    return a + b

try:
    strict(1)
except TypeError as e:
    print("TypeError:", e)

try:
    strict(1, 2, 3)
except TypeError as e:
    print("TypeError:", e)

try:
    strict(1, 2, c=3)
except TypeError as e:
    print("TypeError:", e)
