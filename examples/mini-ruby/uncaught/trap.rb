# A trap the program does not catch ends it with the exception ruby would
# raise: "String can't be coerced into Integer (TypeError)" (after the
# position and a backtrace).
x = 1 + "a"
