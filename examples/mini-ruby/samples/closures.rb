# Closures, and the one scoping rule Ruby has that nothing else here does:
# a block shares its method's locals, so what it assigns to a name that
# already exists outside stays outside.

def make_counter
  n = 0
  lambda { n += 1 }
end
c1 = make_counter
c2 = make_counter
puts c1.call, c1.call, c2.call, c1.call

# The block's assignment lands on the method's local, not on a new one --
# which is why this counts rather than staying at zero.
total = 0
[1, 2, 3, 4].each { |x| total += x }
puts total

# A block *parameter*, on the other hand, is the block's own, and shadows
# an outer name of the same spelling.
x = "outer"
[1].each { |x| x = "inner" }
puts x

# Two closures over one binding: one cell, not two copies.
def make_pair
  v = 0
  [lambda { v += 1 }, lambda { v }]
end
pair = make_pair
pair[0].call
pair[0].call
puts pair[1].call

# Two levels of nesting.
def outer(a)
  lambda { |b| lambda { |c| a + b + c } }
end
puts outer(1).call(2).call(3)

# A method calling another method is a *capture* here: with no classes,
# a `def` binds a name like an assignment does, so the callee travels the
# same way any other captured value would.
def helper(v)
  v * 2
end
def uses_helper(v)
  helper(v) + 1
end
puts uses_helper(5)

# Mutual recursion between two methods.
def even2?(n)
  if n == 0
    true
  else
    odd2?(n - 1)
  end
end
def odd2?(n)
  if n == 0
    false
  else
    even2?(n - 1)
  end
end
puts even2?(10), odd2?(10)

# A closure stored in a collection, called later.
handlers = {}
handlers["greet"] = lambda { |who| "hi #{who}" }
handlers["shout"] = lambda { |who| "HI #{who.upcase}" }
puts handlers["greet"].call("there")
puts handlers["shout"].call("there")

# Recursion through a lambda held in a local.
fact = nil
fact = lambda { |n| if n < 2 then 1 else n * fact.call(n - 1) end }
puts fact.call(10)
