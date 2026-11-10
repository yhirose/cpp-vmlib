; Division. Two exact integers that divide evenly give an exact integer,
; as in Guile; an inexact operand gives an inexact number. (A quotient that
; is no integer is a rational in Guile, which this subset does not have, so
; none appears here -- see README.md.)
(display (/ 6 3)) (newline)
(display (/ -6 3)) (newline)
(display (/ 0 5)) (newline)
(display (/ 8 2 2)) (newline)
(display (/ 1)) (newline)
(display (/ 6.0 3)) (newline)
(display (/ 6 3.0)) (newline)
(display (/ 1.0 4)) (newline)
; An inexact divisor of zero is inf or nan, not an error.
(display (/ 7.0 0.0)) (newline)
(display (/ 7 0.0)) (newline)
(display (/ 0 0.0)) (newline)
; The result is a number like any other.
(display (+ 1 (/ 6 3))) (newline)
(display (number->string (/ 6 3))) (newline)
(display (= (/ 6 3) 2)) (newline)
(display (eqv? (/ 6 3) 2)) (newline)
