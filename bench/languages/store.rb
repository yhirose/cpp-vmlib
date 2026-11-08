def run(n)
  xs = []
  1000.times { xs.push(0) }
  d = {"a" => 0, "b" => 0}
  i = 0
  while i < n
    xs[i % 1000] = i
    d["a"] = i
    d["b"] = i * 2
    i = i + 1
  end
  xs[7] + xs[999] + d["a"] + d["b"]
end

puts run(200000)
