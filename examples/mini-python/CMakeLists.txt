# mini-python is the only front end that proves Arbitrary-precision
# integers -- Python's `int` is unbounded, so python3 is an oracle for a
# bignum this library deliberately has no Value tag for -- and the only one
# whose lexical structure a PEG cannot express, so it normalizes the
# indentation before parsing.
#
# The goldens are `python3`'s output (samples/gen_golden.sh) -- whose
# integers are unbounded by definition, which samples/bignums.py checks.
# Its block structure is its indentation, which a PEG has no state to
# count columns with, so layout.h normalizes it before the parser runs.
add_front_end(mini-python EXT py ORACLE python3)
