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

# String escapes. Only length is shown for the control characters (a NUL or
# an escape byte would not read in a transcript).
puts "\x41\x4a\x4A\x4a1"
puts "\x4g".length
puts "\101\60\7".length
puts "\1012"
puts "\400".length == "\0".length
puts "\a\b\e\f\v\s\0\r\n\t".length
puts "\e" == "\x1b"
puts "\s" == " "
puts "\u0041\u00e9\u{20AC}\u{1F600}"
puts "\u{41 42	43}"
puts "\u{44}\u{ 45 }"
puts "\q\8\9\ \#\z"
puts "a\
b"
puts 'a\nb\\c\'d\qe'
puts 'a\\'
puts "#{1 + 1}\x41#{"\u0042\n".length}"
puts "\"\\\'"
