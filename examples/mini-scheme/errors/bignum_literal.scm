; An exact integer past int64 is a bignum in Guile, which displays
; 9223372036854775808. This front end has none, so the literal is refused
; where it is read rather than clamped to 9223372036854775807.
(display 9223372036854775808)
