# The headline. Ruby has two kinds of callable, and the difference between
# them is exactly one flag and one intrinsic in this IR.
#
#   * A proc is lenient: extra arguments are dropped and missing ones are
#     nil. That is Func::lenient_arity.
#   * A lambda is strict: a mismatch raises ArgumentError. That is an
#     ArgCount test the lambda's own prologue makes -- and ArgCount's
#     comment in vmlib.h says it is "Only interesting under
#     Func::lenient_arity, where the two can differ", which is this.
#   * And Proc#arity is FnArity, "the one fact a front end needs to check
#     'this callback takes two arguments' before calling it".

pr = Proc.new { |a, b| [a, b] }
l = lambda { |a, b| [a, b] }

puts pr.call(1, 2).inspect
puts pr.call(1).inspect
puts pr.call(1, 2, 3).inspect

puts l.call(1, 2).inspect

def strictly(f, args)
  begin
    if args.length == 1
      f.call(args[0])
    else
      f.call(args[0], args[1])
    end
  rescue => e
    "raised"
  end
end

puts strictly(l, [1])
puts strictly(l, [1, 2]).inspect
puts strictly(pr, [1]).inspect

# arity, for every shape a block can have.
puts lambda { }.arity
puts lambda { |a| }.arity
puts lambda { |a, b| }.arity
puts lambda { |a, b, c| }.arity
puts Proc.new { }.arity
puts Proc.new { |a| }.arity
puts Proc.new { |a, b| }.arity

# A callable is a value: it can be stored, passed and returned.
def apply_twice(f, v)
  f.call(f.call(v))
end
double = lambda { |x| x * 2 }
puts apply_twice(double, 3)

fs = [lambda { |x| x + 1 }, lambda { |x| x * 10 }]
fs.each { |f| puts f.call(5) }

# Dispatching on arity, which is what FnArity is for -- a caller deciding
# how to call something it was handed.
def invoke(f)
  if f.arity == 0
    f.call
  elsif f.arity == 1
    f.call(10)
  else
    f.call(10, 20)
  end
end
puts invoke(lambda { 1 })
puts invoke(lambda { |a| a })
puts invoke(lambda { |a, b| a + b })

# A closure over a counter, in both flavours.
def make_counter
  n = 0
  lambda { n += 1 }
end
c1 = make_counter
c2 = make_counter
puts c1.call
puts c1.call
puts c2.call
