# A bare `except:` before another clause: python3 refuses the program,
# "SyntaxError: default 'except:' must be last", at the bare clause.
try:
    print(1)
except:
    print(2)
except ValueError:
    print(3)
