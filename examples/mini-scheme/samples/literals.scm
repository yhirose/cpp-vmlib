; Integer literals: the sign is part of the token, so the smallest int64 is
; written as itself. (A regression guard; past int64 the literal is a
; bignum in Guile, which this front end refuses: see errors/.)
(display 9223372036854775807) (newline)
(display -9223372036854775808) (newline)
(display (- -9223372036854775807 1)) (newline)
(display (+ 1 -1)) (newline)
(display (- 5 -3)) (newline)

; String escapes (ASCII only: Guile writes a non-ASCII character in the
; locale's encoding). Only lengths are shown for the control characters.
(display "tab\there \"q\" back\\slash \|bar\|") (newline)
(display (string-length "\a\b\f\n\r\t\v\0")) (newline)
(display "\u0041\u0062\U000043") (newline)
(display "line\
continued") (newline)
(display (string-length "\u0000")) (newline)
