# c[k] and c[k] = v: an index in range, at the length, below zero and past
# the end; a hash key it has, lacks, or holds nil under -- and an
# assignment that is itself a value.
xs = [10, 20, 30]
xs[0] = 11
xs[-1] = 33
puts xs.inspect

# at the length: it grows; past it: it pads with nil
xs[3] = 40
xs[6] = 70
puts xs.inspect, xs.length

h = {"a" => 1, 2 => "two"}
h["a"] = 5
h["b"] = 6
h[3] = "three"
puts h["a"], h["b"], h[2], h[3], h.length

# a missing key is nil, and so is a key holding nil
puts h["zzz"].inspect, h[99].inspect
h["n"] = nil
puts h["n"].inspect, h.length
h["n"] = 0
puts h["n"]

h["w"] = 1
h["w"] += 1
xs[0] -= 1
puts h["w"], xs[0]

# an assignment is its value: a method answers it
def put(c, k, v)
  c[k] = v
end
puts put(xs, 1, 99), xs[1]
puts put(h, "q", 8), h["q"]

# a compound store reads then writes, each of the receiver and key once
calls = []
which = lambda { calls.push(1); h }
which.call["a"] += 10
puts h["a"], calls.length

# the key types
h[1.5] = "f"
h[nil] = "n"
puts h[1.5], h[nil]
