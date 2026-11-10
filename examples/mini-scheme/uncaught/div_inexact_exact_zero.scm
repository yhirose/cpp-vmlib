; (/ 7.0 0): the divisor is an exact zero, so this is an error too -- unlike
; (/ 7.0 0.0), which is +inf.0 in guile and here. Expected text: this front
; end's own words, not guile's.
(display (/ 7.0 0))
