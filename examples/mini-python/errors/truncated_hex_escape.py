# python3 refuses a truncated \x: "SyntaxError: (unicode error) 'unicodeescape' codec can't decode bytes in position 0-2: truncated \xXX escape".
print("\x4")
