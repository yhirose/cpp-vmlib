# Tuples, unpacking, and the sequence builtins that hand them back.
#
# The IR has one sequence type and Python has two, which would be a detail
# if they printed and compared the same. They do not, so a tuple here is
# its array wrapped in an object under a key no source can write, and the
# sequence funcs unwrap before they look. See README.md.

t = (1, 2, 3)
print(t)
print(t[0], t[-1], len(t))
print(t[1:], t[:2])
print((), (1,), (1, 2))
print(type(t).__name__, type([1]).__name__)

# A tuple is not a list, and says so.
print(t == (1, 2, 3), t == (1, 2), t == [1, 2, 3])
print((1, 2) + (3,))
print(2 in t, 9 in t)

# Parentheses alone are grouping, not a tuple.
print((1 + 2) * 3, type((1)).__name__)

# The comma is what makes one, so a bare comma list is a tuple too.
pair = 1, 2
print(pair, type(pair).__name__)


def divmod2(a, b):
    return a // b, a % b

print(divmod2(17, 5))
q, r = divmod2(17, 5)
print(q, r)

a, b = 1, 2
a, b = b, a
print(a, b)

x, y, z = [10, 20, 30]
print(x, y, z)

first, second = "ab"
print(first, second)

nested = [(1, "one"), (2, "two")]
for n, name in nested:
    print(n, name)

try:
    p, q = (1, 2, 3)
except ValueError as e:
    print("ValueError:", e)

try:
    p, q, s = (1, 2)
except ValueError as e:
    print("ValueError:", e)


# enumerate and zip build tuples; a for loop takes them apart.
xs = ["a", "b", "c"]
for i, v in enumerate(xs):
    print(i, v)
for i, v in enumerate(xs, 1):
    print(i, v)

ys = [10, 20, 30, 40]
for name, n in zip(xs, ys):
    print(name, n)
print(list(zip(xs, ys)))


# A dict, taken apart the two ways a program actually does it.
d = {"b": 2, "a": 1, "c": 3}
for k in d.keys():
    print(k, d[k])
for k, v in d.items():
    print(k, "=", v)
# `.items()` and `.values()` answer a list here, where Python answers a
# view -- so the sample asks what is in one, never what it prints.
print(list(d.values()), sum(d.values()))
print(list(d.items()))


# sorted, sum, min, max.
nums = [5, 1, 4, 2, 3]
print(sorted(nums), nums)
print(sorted(nums, reverse=True))
print(sorted(xs, key=lambda s: s))
print(sum(nums), min(nums), max(nums))
print(min(3, 7), max(3, 7))
print(sorted(d.items()))
print(sorted([(2, "b"), (1, "c"), (1, "a")]))

words = ["pear", "fig", "banana"]
print(sorted(words, key=len))
print(sorted(words, key=len, reverse=True))


def by_second(pair):
    return pair[1]

print(sorted([(1, 9), (2, 3), (3, 6)], key=by_second))
print(max([(1, 9), (2, 3)]), min([(1, 9), (2, 3)]))
print(tuple([1, 2]), tuple("ab"))
