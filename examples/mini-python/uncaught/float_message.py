# An exception whose message is a float words it as str() does: python3
# ends with "ValueError: 0.0001", where the shortest-digits text of the
# VM would say 1e-04.
raise ValueError(0.0001)
