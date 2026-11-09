# In an f-string too, \N{BULLET} is the named escape (not a replacement field), and python3 prints a bullet.
print(f"x\N{BULLET}")
