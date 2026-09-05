# f-strings and the string, list and dict methods a program reaches for.
#
# Every one of these is a scan, so every one is written in this front end's
# own IR. `upper` and `lower` are the only two that are not, because case
# is a Unicode table and not a scan. See README.md.

name = "world"
n = 42
xs = [1, 2, 3]

print(f"hello {name}")
print(f"{n} + 1 = {n + 1}")
print(f"{name.upper()} has {len(name)} letters")
print(f"{{literal braces}} and {n}")
print(f"nested: {xs[1]} {'-'.join(['a', 'b'])}")
print(f'single quotes work too: {name}')
print(f"{name!r} {n!r}")
print(f"empty: {''} done")

# Width and alignment, which are exact.
print(f"[{name:10}]")
print(f"[{name:>10}]")
print(f"[{name:^11}]")
print(f"[{n:6}]")
print(f"[{n:<6}]")
print(f"[{name:*^11}]")
for w in ["a", "bb", "ccc"]:
    print(f"{w:>5} {len(w):3}")

# A precision spec is the C library's exact expansion of the double,
# which is what CPython's is too.
print(f"{3.14159:.2f}", f"{2:.3f}", f"{1/3:.5f}")
print(f"[{3.14159:>10.2f}]", f"[{3.14159:<10.2f}]")


# String methods.
s = "  Hello, World  "
print(repr(s.strip()), repr(s.lstrip()), repr(s.rstrip()))
print("a,b,,c".split(","))
print("a b  c".split())
print("one".split(","))
print("-".join(["a", "b", "c"]))
print("banana".replace("a", "o"), "banana".replace("na", "-"))
print("banana".find("na"), "banana".find("z"))
print("banana".count("na"), "banana".count("a"), "".count("a"))
print("hello".startswith("he"), "hello".startswith("lo"))
print("hello".endswith("lo"), "hello".endswith("he"))
print("MiXeD".upper(), "MiXeD".lower())
print("hello"[1:4], "hello"[-2:], len("hello"))


# List methods.
ys = [3, 1, 2]
ys.append(4)
ys.extend([5, 6])
print(ys)
print(ys.pop(), ys.pop(0), ys)
ys.insert(1, 99)
print(ys)
ys.remove(99)
print(ys, ys.index(2), ys.count(2))
ys.reverse()
print(ys)
ys.sort()
print(ys)
ys.sort(reverse=True)
print(ys)

words = ["pear", "fig", "banana"]
words.sort(key=len)
print(words)

try:
    [].pop()
except IndexError as e:
    print("IndexError:", e)
try:
    [1].index(9)
except ValueError as e:
    print("ValueError:", e)


# Dict methods.
d = {"a": 1, "b": 2}
print(d.get("a"), d.get("z"), d.get("z", 0))
print(sorted(d.keys()), sorted(d.values()), sorted(d.items()))
d.update({"c": 3})
print(sorted(d.items()))
print(d.pop("a"), sorted(d.items()))
print(d.pop("zz", "missing"))
try:
    d.pop("zz")
except KeyError as e:
    print("KeyError:", e)


# A method of the program's own wins over a builtin name.
class Bag:
    def __init__(self):
        self.n = 0

    def get(self, k):
        return "bag:" + str(k)

    def count(self):
        return self.n

    def pop(self):
        return "popped"

bag = Bag()
print(bag.get("x"), bag.count(), bag.pop())
print({"x": 1}.get("x"), [1, 1].count(1), [1, 2].pop())
