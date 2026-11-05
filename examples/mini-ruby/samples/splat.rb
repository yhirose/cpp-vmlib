# Splats. A `*rest` parameter is the IR's Func::rest_param, and a call
# with `*x` among its arguments is a Tag::CallSpread: one at each end of a
# call whose count is decided when it runs.

# A last `*rest` parameter, after required and default ones.
def count(*xs)
  xs.length
end
def tail(first, *rest)
  [first, rest]
end
def opt(a, b = 10, *r)
  [a, b, r]
end
p count, count(1, 2, 3)
p tail(1), tail(1, 2, 3)
p opt(1), opt(1, 2), opt(1, 2, 3, 4)

# `*x` anywhere among the arguments, and in an array literal: an array's
# elements, nothing for nil, a range's members, any other value by itself.
xs = [1, 2, 3]
p count(*xs), count(0, *xs), count(*xs, *xs), count(*nil)
p count(*(1..4)), count(*5)
p [*xs], [0, *xs, 4], [*nil], [*(1..3)], [*7]
copy = [*xs]
copy.push(99)
p xs, copy

# Blocks and lambdas take a `*rest` too, and Proc#arity reads it from
# FnArity: negative for "and any number more", Ruby's own convention.
blk = proc { |a, *r| [a, r] }
p blk.call(1, 2, 3), blk.call(*xs), blk.arity
lam = lambda { |a, *r| r.length }
p lam.arity, lam.call(1), lam.call(*xs)
begin
  lam.call
rescue => e
  puts "raised"
end
p proc { |*r| r }.arity, lambda { |*r| r }.arity, proc { |a, b| a }.arity

# yield, super, new and a forwarded block all spread the same way.
def gives
  yield *[4, 5]
end
gives { |a, b| p [a, b] }
def pass(*args, &blk)
  blk.call(*args)
end
p pass(1, 2) { |x, y| x + y }
class Base
  def initialize(*parts)
    @parts = parts
  end
  def parts
    @parts
  end
  def sum(*more)
    total = 0
    @parts.each { |x| total += x }
    more.each { |x| total += x }
    total
  end
end
class Child < Base
  def sum(*more)
    super(*more) * 10
  end
end
b = Base.new(*xs)
p b.parts, b.sum, b.sum(*xs)
p Child.new(1, 2).sum(3)

# push and puts take any number of values, a splat among them.
a = []
a.push(1, 2)
a.push(*xs)
a.push(*a)
p a
puts *xs
puts 0, *xs
