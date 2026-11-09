-- lua refuses a backslash before anything it has no escape for:
-- "invalid escape sequence near '"a\q'".
print("a\qb")
