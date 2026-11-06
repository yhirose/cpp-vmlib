# Lists and dicts. A list is an Array and a dict is a value-keyed Map,
# which is the distinction the header draws between Object (string keys)
# and Map -- and Python's between an attribute and a subscript.

xs = [10, 20, 30]
print(xs, len(xs), xs[0], xs[2], xs[-1], xs[-3])
xs[1] = 21
xs.append(40)
print(xs, len(xs))

# Slicing: absent ends default, negatives count from the end, and both are
# clamped -- slice_error()'s comment says a language that wants that
# normalizes in its own lowering, so this front end does.
print(xs[1:3], xs[:2], xs[2:], xs[:], xs[-2:], xs[1:100], xs[3:1])
print("hello"[1:3], "hello"[-3:], "hello"[0])

print([1, 2] + [3], [0] * 3, [] == [], [1] == [1, 2])
print(2 in [1, 2, 3], 9 in [1, 2, 3], "e" in "hello")

d = {"x": 1, "y": 2}
print(d, len(d), d["x"], d.get("zz", "default"))
d["z"] = 3
d["x"] = 10
print(d, list(d.keys()))
print("x" in d, "q" in d, "q" not in d)

# A dict's keys are values, not names: a number and the string that prints
# like it are two keys.
k = {1: "int", "1": "str", 2.5: "float"}
print(k[1], k["1"], k[2.5], len(k))

nested = {"row": [1, 2], "deep": {"a": [3]}}
print(nested["row"][1], nested["deep"]["a"][0])
nested["row"].append(3)
print(nested)

total = 0
for v in [1, 2, 3, 4]:
    total += v
print(total)

keys = []
for key in {"a": 1, "b": 2}:
    keys.append(key)
print(keys)

chars = []
for c in "abc":
    chars.append(c)
print(chars, "".join(chars), "-".join(["x", "y"]))

# Lists are references, and a slice is a copy.
alias = xs
copy = xs[:]
alias.append(99)
print(len(xs), len(copy), alias == xs)

# `xs += it` extends the list in place, from any iterable, so every name
# for it sees the change; `xs = xs + ys` makes a new one.
xs = [1]
same = xs
xs += [2]
xs += (3,)
print(same, xs is same)
xs = xs + [4]
print(same, xs is same)

# A builtin method's arguments run once, in source order, whatever order
# it reads them in, and one it does not take is a TypeError once they
# have run; a method of the program's own by the same name is handed
# them as any call is, keywords and all.
def arg(tag, v):
    print("arg", tag)
    return v

ys = [3, 1, 2]
ys.sort(reverse=arg("reverse", True), key=arg("key", None))
print(ys, "xxaxx".strip(arg("chars", "x")), "--a--".rstrip("-"))
try:
    ys.append(arg("first", 1), arg("second", 2))
except TypeError:
    print("TypeError")

class Sorter:
    def sort(self, key=None, reverse=False):
        return (key, reverse)

print(Sorter().sort(key=arg("own key", len), reverse=True)[1])
