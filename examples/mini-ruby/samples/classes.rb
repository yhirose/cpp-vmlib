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
