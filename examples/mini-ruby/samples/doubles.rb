# How a Float prints: the shortest digits that round trip, in Ruby's
# layout. Fixed notation from 1e-4 up to (not including) 1e15, an exponent
# outside that ("1.0e+15", "1.0e-05"), and ".0" on a whole number. The same
# text from puts, to_s, interpolation and the inspect of an array.
big = 1.0e300 * 1.0e300
nan = big - big
values = [
  -0.0, 0.1, 100000.0, 0.0001, 0.00001, 99999999999999.0, 1.0e15, 1.0e16,
  12345678901234567.0, 5.0e-324, big, -big, nan
]
values.each { |v| puts v }
p [0.0001, 0.00001, 1.0e-7]
puts 1.0e15.to_s + " " + 0.0001.to_s + " " + 1.0e-05.inspect
puts "#{1.0e16} #{0.00001} #{100000.0} #{-0.0}"
