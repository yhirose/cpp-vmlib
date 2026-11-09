# Integer literals. A minus written against its digits belongs to the
# literal, which is how -9223372036854775808 is written at all.
puts 9223372036854775807
puts -9223372036854775808
puts -9223372036854775807 - 1
# (-2).to_s, not -(2.to_s): the literal binds tighter than the method call.
puts -2.to_s
puts -2.5.to_s
a = 5
puts a -1
puts a - 1
puts a - -1
puts 2 -1
puts 2-1
puts 3 - -2
puts [1, -2, 3].length
# ** binds tighter than a unary minus, even a literal's, and groups to the
# right.
puts -2 ** 2
puts -2.5 ** 2
puts 2 ** 3 ** 2
puts 2**10
b = 3
puts -b ** 2
puts (-b) ** 2
puts 2 * 3 ** 2
puts 10 - 2 ** 2 ** 2
