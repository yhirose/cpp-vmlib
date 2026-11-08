# c[k] and c[k] = v: an index in range, a key a dict has or lacks, a stored
# None (a hit, not a miss), a negative index, an index out of range.
xs = [10, 20, 30]
xs[0] = 11
xs[-1] = 33
print(xs, xs[1], xs[-3])

d = {"a": 1, 2: "two"}
d["a"] = 5
d["b"] = 6
d[3] = "three"
print(d["a"], d["b"], d[2], d[3], len(d))

# a key holding None is found
d["n"] = None
print(d["n"], len(d))
d["n"] = 0
print(d["n"])

# the misses are errors
for f in (lambda: d["zzz"], lambda: d[99], lambda: xs[3], lambda: xs[-4]):
    try:
        f()
    except KeyError as e:
        print("KeyError", e)
    except IndexError as e:
        print("IndexError", e)

def put(c, k, v):
    c[k] = v

for c, k in ((xs, 3), (xs, -4)):
    try:
        put(c, k, 0)
    except IndexError as e:
        print("IndexError", e)
try:
    put((1, 2), 0, 0)
except TypeError as e:
    print("TypeError")
try:
    put("abc", 0, "x")
except TypeError as e:
    print("TypeError")

# keys of other types are dict keys all the same
t = {}
t[1.5] = "float"
t[None] = "none"
print(t[1.5], t[None], len(t))

# attributes: a new one and an old one
class P:
    def __init__(self):
        self.x = 1
p = P()
p.x = 2
p.y = 3
print(p.x, p.y)
try:
    p.zzz
except AttributeError:
    print("AttributeError")

# a compound store reads then writes, each of the receiver and key once
calls = []
def which():
    calls.append(1)
    return d
which()["a"] += 10
print(d["a"], len(calls))
xs[1] *= 2
print(xs)
