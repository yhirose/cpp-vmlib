# Classes. There is no class in the IR: a class value is a constructor
# closure over a method table, and an instance is an object pointing back
# at it. Python declares `self` itself, so nothing is implicit here.

class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm2(self):
        return self.x * self.x + self.y * self.y

    def label(self, tag):
        return tag + "(" + str(self.x) + ", " + str(self.y) + ")"

    def scaled(self, k):
        return Point(self.x * k, self.y * k)

p = Point(3, 4)
print(p.x, p.y)
print(p.norm2())
print(p.label("P"))
print(p.scaled(2).norm2())
print(type(p).__name__)

q = Point(1, 2)
q.x = 9
q.extra = "added later"
print(q.x, q.norm2(), q.extra)

ps = []
for i in range(1, 4):
    ps.append(Point(i, i))
norms = []
for one in ps:
    norms.append(one.norm2())
print(norms)

# A class with state that grows, and a method that returns another
# instance -- the two shapes an object idiom is built from.
class Counter:
    def __init__(self):
        self.n = 0
        self.log = []

    def bump(self, by):
        self.n += by
        self.log.append(self.n)
        return self

    def total(self):
        return self.n

c = Counter()
c.bump(1).bump(2).bump(3)
print(c.total(), c.log)

# A class closing over what was around it when it was declared.
def make_class(offset):
    class Shifted:
        def __init__(self, v):
            self.v = v + offset

        def get(self):
            return self.v
    return Shifted

S = make_class(100)
print(S(5).get(), S(1).get())

# Instances hold bignums like anything else.
class Accumulator:
    def __init__(self):
        self.total = 1

    def times(self, k):
        self.total = self.total * k
        return self

a = Accumulator()
for i in range(1, 26):
    a.times(i)
print(a.total)
