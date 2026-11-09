; (+ 1 "a") ends the program with guile's error:
; "In procedure +: Wrong type: \"a\"" (after a backtrace).
(display (+ 1 "a"))
