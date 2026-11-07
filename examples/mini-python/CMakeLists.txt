# The goldens are `python3`'s output (samples/gen_golden.sh) -- whose
# integers are unbounded by definition, which samples/bignums.py checks.
# Its block structure is its indentation, which a PEG has no state to
# count columns with, so layout.h normalizes it before the parser runs.
add_front_end(mini-python EXT py ORACLE python3)
