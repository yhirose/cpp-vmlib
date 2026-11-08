def run(n):
    xs = []
    for _ in range(1000):
        xs.append(0)
    d = {"a": 0, "b": 0}
    i = 0
    while i < n:
        xs[i % 1000] = i
        d["a"] = i
        d["b"] = i * 2
        i = i + 1
    return xs[7] + xs[999] + d["a"] + d["b"]

print(run(200000))
