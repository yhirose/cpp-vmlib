; Integer literals: the sign is part of the token, so the smallest int64 is
; written as itself. (A regression guard; past int64 the literal is a
; bignum in Guile, which this front end refuses: see errors/.)
(display 9223372036854775807) (newline)
(display -9223372036854775808) (newline)
(display (- -9223372036854775807 1)) (newline)
(display (+ 1 -1)) (newline)
(display (- 5 -3)) (newline)
