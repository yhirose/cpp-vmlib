# begin / rescue / ensure. `ensure` is a Defer inside the Scope wrapping
# the begin, so it runs however the block is left -- which is the same tag
# examples/mini-culebra reaches for `defer` and examples/mini-python for
# `with`, from a third direction.

def attempt
  begin
    yield
  rescue => e
    "rescued: #{e.message}"
  end
end

puts attempt { 1 + 1 }
puts attempt { raise "boom" }

order = []
begin
  order.push("body")
  raise "thrown"
rescue => e
  order.push("rescue #{e.message}")
ensure
  order.push("ensure")
end
order.push("after")
puts order.inspect

# ensure on the way out through a return: the value is decided before the
# deferred block runs.
trace = []
def through_return(trace)
  begin
    trace.push("body")
    return "returned"
  ensure
    trace.push("ensure on return")
  end
end
puts through_return(trace), trace.inspect

# ensure on the way out through an exception nothing here catches.
unwound = []
def through_raise(unwound)
  begin
    raise "deep"
  ensure
    unwound.push("ensure on raise")
  end
end
puts attempt { through_raise(unwound) }, unwound.inspect

# ensure with no rescue at all, inside a loop that breaks out of it.
loop_log = []
i = 0
while i < 3
  begin
    if i == 1
      i = 99
    end
    loop_log.push("body #{i}")
  ensure
    loop_log.push("ensure #{i}")
  end
  i += 1
end
puts loop_log.inspect

# Nested: the inner rescue handles, so the outer one does not see it.
nested = []
begin
  begin
    raise "inner"
  rescue => e
    nested.push("inner saw #{e.message}")
    raise "rethrown"
  ensure
    nested.push("inner ensure")
  end
rescue => e
  nested.push("outer saw #{e.message}")
end
puts nested.inspect

# A raise from inside a block reaches the handler around the iteration.
puts attempt { [1, 2, 3].each { |x| raise "at #{x}" } }

# ZeroDivisionError is raised by this front end, not trapped by the VM.
puts attempt { 1 / 0 }
