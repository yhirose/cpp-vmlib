-- A string cannot hold a raw line break (it needs a backslash before it):
-- lua says "unfinished string".
print("a
b")
