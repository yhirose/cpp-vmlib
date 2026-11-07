# A call given a block both written after it and as `&b`: ruby refuses the
# program, "both block arg and actual block given", at the call's line.
def f
  yield
end
b = proc { 1 }
puts f(&b) { 2 }
