# Inheritance, and the fact that nothing in the IR knows about it.
#
# A class is an object holding its methods, its base's table and the
# constructor closure that is its identity; an instance is an object
# pointing back at it. Every question a class system answers -- which
# method wins, what an instance is, which `except` catches -- is then a
# walk up that chain. See README.md.

class Shape:
    def __init__(self, name):
        self.name = name

    def area(self):
        return 0

    def describe(self):
        # Calls area() on whatever the instance turned out to be, which is
        # the whole point: this method does not know its subclasses.
        return self.name + " area=" + str(self.area())


class Rect(Shape):
    def __init__(self, w, h):
        super().__init__("rect")
        self.w = w
        self.h = h

    def area(self):
        return self.w * self.h


class Square(Rect):
    def __init__(self, side):
        super().__init__(side, side)
        self.name = "square"

    def describe(self):
        return "a " + super().describe()


shapes = [Shape("nothing"), Rect(3, 4), Square(5)]
for s in shapes:
    print(s.describe())

print(Square(2).area(), Square(2).w, Square(2).h)


# A subclass that adds nothing still inherits the constructor.
class Tall(Rect):
    def area(self):
        return super().area() * 2

t = Tall(2, 3)
print(t.area(), t.name, t.describe())


# isinstance walks the same chain.
sq = Square(1)
print(isinstance(sq, Square), isinstance(sq, Rect), isinstance(sq, Shape))
print(isinstance(Rect(1, 1), Square))
print(isinstance(3, int), isinstance("x", str), isinstance([1], list))
print(isinstance(sq, int))


# __str__, __repr__ and __eq__ are found the same way any method is.
class Money:
    def __init__(self, amount):
        self.amount = amount

    def __str__(self):
        return "$" + str(self.amount)

    def __repr__(self):
        return "Money(" + str(self.amount) + ")"

    def __eq__(self, other):
        return self.amount == other.amount


m = Money(5)
print(m)
print(str(m), repr(m))
print(m == Money(5), m == Money(6))
print([Money(1), Money(2)] == [Money(1), Money(2)])


class Coins(Money):
    def __str__(self):
        return super().__str__() + " in coins"

print(Coins(3))
print(Coins(3) == Money(3))


# Exception classes of the program's own.
class AppError(Exception):
    pass


class NotFound(AppError):
    def __init__(self, what):
        super().__init__("no such " + what)
        self.what = what


def lookup(k):
    if k == "bad":
        raise NotFound("key")
    if k == "worse":
        raise AppError("something else")
    return k


for k in ["ok", "bad", "worse"]:
    try:
        print("got", lookup(k))
    except NotFound as e:
        print("NotFound:", e, e.what)
    except AppError as e:
        print("AppError:", e)

# The base catches the derived one.
try:
    lookup("bad")
except AppError as e:
    print("caught by base:", e)

# And Exception catches everything, including the builtin ones.
for f in [lambda: lookup("bad"), lambda: 1 // 0, lambda: [][0]]:
    try:
        f()
    except Exception as e:
        print("Exception:", type(e).__name__)

print(isinstance(NotFound("x"), AppError), isinstance(AppError("y"), NotFound))
