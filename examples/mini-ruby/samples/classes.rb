# Classes. There is no class in the IR: a class is an object holding its
# base's table and its methods, keyed with a prefix no Ruby identifier can
# start with, and an instance is a plain object pointing back at it. Every
# question a class system answers -- dispatch, inheritance, `is_a?` -- is a
# walk up that chain. See README.md.

class Point
  attr_accessor :x, :y

  def initialize(x, y)
    @x = x
    @y = y
  end

  def to_s
    "(" + @x.to_s + ", " + @y.to_s + ")"
  end

  def ==(other)
    @x == other.x && @y == other.y
  end
end

p1 = Point.new(1, 2)
p2 = Point.new(3, 4)
puts p1
puts p1.x, p1.y
p1.x = 10
puts p1
puts(Point.new(1, 2) == Point.new(1, 2))
puts(p1 == p2)

class Animal
  def initialize(name)
    @name = name
  end

  def speak
    @name + " makes a sound"
  end

  def describe
    @name + " is a " + self.class.to_s
  end
end

class Dog < Animal
  def initialize(name, breed)
    super(name)
    @breed = breed
  end

  def speak
    super() + " (specifically, a bark)"
  end
end

a = Animal.new("Generic")
d = Dog.new("Rex", "Labrador")
puts a.speak
puts d.speak
puts d.describe

animals = [a, d]
animals.each { |x| puts x.speak }

puts(d.is_a?(Dog))
puts(d.is_a?(Animal))
puts(a.is_a?(Dog))
puts(d.instance_of?(Dog))
puts(d.instance_of?(Animal))

class Counter
  def initialize
    @n = 0
  end

  def bump(by = 1)
    @n += by
    self
  end

  def total
    @n
  end
end

c = Counter.new
c.bump.bump(3).bump
puts c.total

class Empty
end

e = Empty.new
puts e.class

# With no initialize anywhere up the chain, new is Object's, which takes
# no arguments: given some, they run and an ArgumentError follows.
def made_arg
  puts "made_arg runs"
  1
end
[lambda { Empty.new(made_arg) }, lambda { Empty.new(*[]) }].each do |f|
  begin
    puts f.call.class
  rescue => e
    puts e.message
  end
end

# `obj.attr = v` is worth v, whatever the setter answers, and v runs even
# when nothing defines the setter.
class Box
  attr_accessor :item
end
def store(box)
  box.item = made_arg
end
p store(Box.new)
begin
  3.item = made_arg
rescue => e
  puts e.class
end

# A bare `super` passes the method's own parameters on as they are now --
# a *rest spread back out, and the block -- from inside a block too. With
# no method above to reach it is a NoMethodError, raised once super's
# arguments have run.
class Shape
  def describe(name, sides = 0, *extra)
    puts "#{name} #{sides} #{extra.inspect}"
    yield if block_given?
  end
end
class Square < Shape
  def describe(name, sides = 4, *extra)
    name = name + "!"
    super
  end
end
class Inside < Shape
  def describe(name)
    [1].each { |i| super }
  end
end
Square.new.describe("sq")
Square.new.describe("sq", 5, 6, 7) { puts "with block" }
Inside.new.describe("in")
class Alone
  def go
    super(made_arg)
  end
end
begin
  Alone.new.go
rescue => e
  puts e.class
end

# `a == b` is a call of a's ==, whatever b is -- and a number's == hands
# a value that is not a number to that value's own.
class Loose
  def ==(other)
    true
  end
end
loose = Loose.new
p loose == 1, loose == "x", 1 == loose, 2.5 == loose, "x" == loose
