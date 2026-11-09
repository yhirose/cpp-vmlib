# A trap caught and raised again is reported where it first failed (line
# 5), not at the `raise` (python3's traceback leads with the first frame).
def f():
    try:
        1 + "a"
    except TypeError as e:
        raise e


f()
