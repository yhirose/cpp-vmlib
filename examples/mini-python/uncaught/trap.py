# A trap the program does not catch ends it with the exception python3
# would raise: "TypeError: unsupported operand type(s) for +: 'int' and
# 'str'" (after the traceback python3 prints).
print(1 + "a")
