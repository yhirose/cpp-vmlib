# What the VM itself traps on reaches `rescue` as the exception ruby raises
# for the same fault: its class and its message. ruby 3.3 words a missing
# method "undefined method 'x' for an instance of Integer" and earlier
# releases otherwise, so a NoMethodError shows its class only.
def show(label)
  begin
    yield
  rescue => e
    puts "#{label}: #{e.class}: #{e.message}"
  end
end

def only_class(label)
  begin
    yield
  rescue => e
    puts "#{label}: #{e.class}"
  end
end

def two(a, b)
  a
end

x = 5
n = nil
s = "abc"

show("int + string") { x + "a" }
show("string + int") { s + 1 }
show("int - string") { x - "a" }
show("array + int") { [1] + 1 }
show("string * string") { s * "b" }
show("int ** string") { x ** "a" }
show("int < string") { x < "a" }
show("array index") { [1]["a"] }
show("int / 0") { x / 0 }
show("int % 0") { x % 0 }
show("arity") { two(1) }

only_class("nil + int") { n + 1 }
only_class("nil index") { n[0] }
only_class("call") { x.call }
only_class("unary") { -n }
