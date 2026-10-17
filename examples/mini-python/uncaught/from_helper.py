# An exception a runtime helper raises, in a bracket that spans two lines:
# python3 ends with "IndexError: list index out of range", and points at
# the subscript on the second.
xs = [1, 2]
total = (xs[0] +
         xs[5])
