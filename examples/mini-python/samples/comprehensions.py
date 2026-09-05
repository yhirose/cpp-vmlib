# Comprehensions. Each one is a function of its own, called immediately --
# which is CPython's model too, and the reason the loop variable does not
# leak and the generator form can be lazy without anything else changing.

xs = [1, 2, 3, 4, 5]

print([x * x for x in xs])
print([x for x in xs if x % 2 == 0])
print([x * 2 for x in xs if x > 1 if x < 5])
print([x if x % 2 else -x for x in xs])
print([c for c in "hello" if c != "l"])
print([len(w) for w in ["a", "bb", "ccc"]])

# The target does not leak.
x = "untouched"
print([x for x in xs], x)

# Nested clauses, in the order Python writes them.
print([(i, j) for i in range(3) for j in range(2)])
print([i * j for i in range(1, 4) for j in range(1, 4) if i != j])

pairs = [(1, "a"), (2, "b"), (3, "c")]
print([n for n, s in pairs])
print([s * n for n, s in pairs])

grid = [[1, 2], [3, 4], [5, 6]]
print([v for row in grid for v in row])
print([[v * 2 for v in row] for row in grid])

# A comprehension closes over what surrounds it.
k = 10
print([x + k for x in xs])


def scaled(factor):
    return [x * factor for x in xs]

print(scaled(3))


# Dict comprehensions.
print({w: len(w) for w in ["a", "bb", "ccc"]})
print({n: s for n, s in pairs})
print({x: x * x for x in xs if x % 2})

d = {"a": 1, "b": 2, "c": 3}
print({k2: v * 10 for k2, v in d.items()})
print({v: k2 for k2, v in d.items()})


# Generator expressions are lazy: the trace shows when the body runs.
trace = []


def note(v):
    trace.append(v)
    return v


g = (note(x) for x in xs)
print("nothing yet:", trace)
print(next(g), trace)
print(next(g), trace)
print(list(g), trace)

print(sum(x * x for x in xs))
print(max(len(w) for w in ["a", "bb", "ccc"]))
print(list(x for x in xs if x > 3))

# A generator expression runs at most as far as it is asked to.
trace2 = []


def note2(v):
    trace2.append(v)
    return v


for v in (note2(x) for x in xs):
    if v > 2:
        break
print(trace2)
