# case/when, the ternary operator, statement modifiers, symbols and
# default parameters -- the rest of Ruby's everyday control flow and
# calling convention.

def describe(n)
  case n
  when 0
    "zero"
  when 1, 2, 3
    "small"
  when 4..10
    "medium"
  else
    "large"
  end
end
puts describe(0), describe(2), describe(7), describe(100)

x = 5
label = x > 3 ? "big" : "small"
puts label
puts x % 2 == 0 ? "even" : "odd"
puts(x > 0 ? "pos" : x < 0 ? "neg" : "zero")

puts "yes" if true
puts "no" if false
puts "unless-yes" unless false
i = 0
i += 1 while i < 5
puts i
j = 10
j -= 1 until j == 5
puts j

# A symbol is a string here (same trade examples/mini-scheme makes for
# its own), so `sym.class` would say "String" where real Ruby says
# "Symbol" -- the one place this sample does not ask.
sym = :hello
puts sym
h = {:a => 1, :b => 2}
puts h[:a], h[:b]
puts (:x == :x), (:x == :y)

def greet(name, greeting = "Hello", punct = "!")
  greeting + ", " + name + punct
end
puts greet("Ada")
puts greet("Ada", "Hi")
puts greet("Ada", "Hi", "?")

def with_dep(a, b = a + 1)
  [a, b]
end
puts with_dep(5).inspect
puts with_dep(5, 9).inspect

class Shape
  def initialize(kind)
    @kind = kind
  end

  def describe
    case @kind
    when :circle
      "round"
    when :square, :rectangle
      "boxy"
    else
      "unknown"
    end
  end
end

puts Shape.new(:circle).describe
puts Shape.new(:square).describe
puts Shape.new(:triangle).describe

r = (1..3)
case 2
when r
  puts "in range"
else
  puts "out of range"
end
