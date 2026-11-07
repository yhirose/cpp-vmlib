# The same through a method call: ruby refuses the program, "both block
# arg and actual block given", at the call's line.
class C
  def m
    yield
  end
end
b = proc { 1 }
puts C.new.m(&b) { 2 }
