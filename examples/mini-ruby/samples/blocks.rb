# Blocks. Ruby passes them out of band, and this front end models that by
# giving every function an implicit parameter 0 that holds the block --
# so `yield` is a call of it, `block_given?` is it not being nil, and
# `&blk` names it rather than declaring a new one.

def twice
  yield 1
  yield 2
end
twice { |v| puts "got #{v}" }

def maybe
  if block_given?
    yield 7
  else
    "no block"
  end
end
puts maybe { |v| "block saw #{v}" }
puts maybe

# `&blk` names the same parameter, which is why it can be called, passed
# on and asked its arity.
def named(&blk)
  blk.call(9)
end
puts named { |v| v + 1 }

def forward(&blk)
  relay(&blk)
end
def relay
  yield "relayed"
end
puts(forward { |v| v })

def arity_of(&blk)
  blk.arity
end
puts arity_of { }
puts arity_of { |a| }
puts arity_of { |a, b| }

# A block is a closure: it sees the method it was written in, and what it
# assigns to a name that already exists out there stays out there.
total = 0
[1, 2, 3, 4].each { |x| total += x }
puts total

seen = []
[10, 20].each_with_index { |v, i| seen.push("#{i}:#{v}") }
puts seen.inspect

# do ... end and { } are the same thing.
sum = 0
(1..4).each do |k|
  sum += k
end
puts sum

# Blocks nest, and each has its own parameters.
pairs = []
[1, 2].each do |a|
  [10, 20].each do |b|
    pairs.push(a * b)
  end
end
puts pairs.inspect

# A method taking a block and calling it more than once, with the block
# closing over something that changes between calls.
def repeat(n)
  i = 0
  while i < n
    yield i
    i += 1
  end
end
log = []
repeat(3) { |i| log.push(i * i) }
puts log.inspect

# `next` inside a block is its return, and `break` leaves the iteration.
evens = [1, 2, 3, 4, 5].select { |x| x % 2 == 0 }
puts evens.inspect
puts [1, 2, 3].map { |x| x * 3 }.inspect
puts [1, 2, 3, 4].inject(0) { |a, b| a + b }
puts [1, 2, 3, 4].inject { |a, b| a * b }
puts 3.times { |i| print i }
