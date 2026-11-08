local function run(n)
  local xs = {}
  for k = 1, 1000 do xs[k] = 0 end
  local d = {a = 0, b = 0}
  local i = 0
  while i < n do
    xs[i % 1000 + 1] = i
    d["a"] = i
    d["b"] = i * 2
    i = i + 1
  end
  return xs[8] + xs[1000] + d["a"] + d["b"]
end

print(run(200000))
