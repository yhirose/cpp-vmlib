# Numbers, strings and the three rules Ruby does not share with the VM's
# defaults.

puts 1 + 2 * 3
puts 10 - 4, 3 * 4, 2 ** 10
# `/` and `%` on two Integers floor, where BinOp::Div truncates and
# BinOp::Mod is C's.
puts 7 / 2, -7 / 2, 7 % 3, -7 % 3, 7 % -3
puts 7 / 2.0, 1 + 2.0, 1.0 * 3

# A whole Float prints with a point, which is to_display's output plus the
# one rule its comment leaves to a front end.
puts 4.0, 100.0, 1.5
puts 0.1 + 0.2
puts 1.0 / 3

# Only nil and false are false: 0 and "" are true, where Value::truthy()
# calls 0 false and says in its own comment that this is a language's
# decision.
def t(v)
  if v
    "T"
  else
    "F"
  end
end
puts t(0) + t("") + t([]) + t(nil) + t(false) + t(1)
puts !nil, !0, !false

puts "a" + "b", "ab" * 3, "hello".length
puts "hello".upcase, "HELLO".downcase
name = "world"
puts "hi #{name}, #{1 + 1} of #{[1, 2].length}"
puts "nested #{"in" + "ner"}"

puts 1 == 1.0, 1 == "1", "a" == "a", nil == false
puts [1, 2] == [1, 2], [1] == [1, 2]
puts 1 < 2, 2 <= 2, "a" < "b"
puts (1 < 2) && (2 < 3), false || "x", nil || "y"

i = 0
n = 0
while i < 5
  i += 1
  n += i
end
puts n, i

j = 0
until j == 3
  j += 1
end
puts j

if 1 > 2
  puts "no"
elsif 2 > 3
  puts "also no"
else
  puts "else"
end

unless false
  puts "unless"
end

puts [1, 2, 3].inspect, ({"a" => 1}).inspect, nil.inspect, "s".inspect
p 1, "s", nil, true, [1, "a"]
print "no", "newline"
puts
