# Generators. `yield` is Tag::Yield, a `def` containing one is
# Func::is_generator, and `for x in gen` is GenResume in a loop.

def squares(n):
    for i in range(n):
        yield i * i

out = []
for v in squares(5):
    out.append(v)
print(out)
print(list(squares(4)))

# Lazy: nothing runs until the loop asks, and it stops again at each
# yield.
effects = []

def noisy():
    effects.append("started")
    yield "a"
    effects.append("middle")
    yield "b"
    effects.append("finished")

g = noisy()
print(effects)
got = []
for v in g:
    got.append(v)
print(got, effects)

# A generator closes over what was around it.
def from_offset(k):
    def gen():
        yield k
        yield k + 1
    return gen

print(list(from_offset(10)()))

# Generators compose, because a generator is iterable like anything else.
def doubled(src):
    for v in src:
        yield v * 2

print(list(doubled(squares(4))))

# Breaking out leaves the generator part-way through; nothing forces it
# to finish.
partial = []
for v in squares(1000):
    if v > 9:
        break
    partial.append(v)
print(partial)

# A generator over a bignum sequence, which is where the two halves of
# this front end meet.
def powers(base, n):
    acc = 1
    for i in range(n):
        yield acc
        acc = acc * base

print(list(powers(2, 8)))
print(list(powers(10, 5))[-1])
big = list(powers(2, 101))
print(big[-1])

print(type(squares(1)).__name__)
