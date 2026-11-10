; (/ 1 2 0) is (/ (/ 1 2) 0): the exact zero divisor is refused after the
; first division, as guile does. Expected text: this front end's own words.
(display (/ 1 2 0))
