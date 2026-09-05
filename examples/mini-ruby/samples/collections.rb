# Arrays, Hashes and Ranges, and the internal iterators Ruby reaches for
# instead of a loop.

xs = [3, 1, 2]
puts xs.inspect, xs.length, xs[0], xs[-1], xs[9].inspect
xs.push(4)
xs[1] = 11
puts xs.inspect
puts xs.first, xs.last
puts xs.sort.inspect, xs.reverse.inspect
puts xs.include?(4), xs.include?(99)
puts xs.join("-"), [].join("-")
puts ([1] + [2, 3]).inspect

h = {"a" => 1, "b" => 2}
puts h.inspect, h.length, h["a"], h["zz"].inspect
h["c"] = 3
h["a"] = 10
puts h.inspect, h.keys.inspect

# A Hash's keys are values, not names -- the same distinction the header
# draws between Object (string keys) and Map.
k = {1 => "int", "1" => "str", 2.5 => "float"}
puts k[1], k["1"], k[2.5], k.length

r = (1..5)
puts r.to_a.inspect, r.to_a.length
sum = 0
r.each { |i| sum += i }
puts sum

# The iterator family, each taking its block the way every call in this
# front end does.
puts [1, 2, 3].map { |x| x * x }.inspect
puts [1, 2, 3, 4].select { |x| x % 2 == 0 }.inspect
puts [1, 2, 3, 4].inject(0) { |a, b| a + b }
puts [1, 2, 3, 4].inject { |a, b| a * b }
puts [].inject { |a, b| a + b }.inspect

out = []
{"x" => 1, "y" => 2}.each { |key, value| out.push("#{key}=#{value}") }
puts out.inspect

idx = []
["a", "b", "c"].each_with_index { |v, i| idx.push("#{i}#{v}") }
puts idx.inspect

# Chained, which is the shape Ruby is written in.
puts (1..6).to_a.select { |x| x % 2 == 0 }.map { |x| x * 10 }.inject(0) { |a, b| a + b }

# Nested containers.
m = {"row" => [1, 2], "deep" => {"a" => [3]}}
puts m["row"][1], m["deep"]["a"][0]
m["row"].push(3)
puts m.inspect

# Arrays are references; a method that pushes changes what the caller has.
def grow(list)
  list.push("added")
end
target = [1]
grow(target)
puts target.inspect
