; How an inexact number prints: the shortest digits that round trip, in
; Guile's layout. Plain digits from 1.0e-3 up to 1.0e7 (and further while the
; digits plus three zeros reach), an exponent outside that with no sign
; and no padding ("1.0e7", "1.0e-4"), a ".0" on a whole number or a
; one-digit mantissa. display, number->string and a list agree.

(define big (* 1.0e300 1.0e300))
(define values
  (list 0.0 -0.0 0.1 0.5 1.0 100000.0 1000000.0 10000000.0 123456789.0
        12345678.5 12345678000.0 123456780000.0 0.001 0.0001 0.00012345
        1.0e-5 1.5e-10 1.0e15 1.0e16 1.0e20 12345678901234567.0 18446744073709551616.0
        (+ 0.1 0.2) (/ 1.0 3.0) 2.5e-5 5.0e-324 1.7976931348623157e308
        big (- big) (- big big) -1.5))
(for-each (lambda (v) (display v) (newline)) values)
(display (list 0.0001 1.0e7 100000.0 12345678.5)) (newline)
(display (number->string 1.0e-4)) (newline)
(display (string-append "x" (number->string 1.0e21) "y")) (newline)
