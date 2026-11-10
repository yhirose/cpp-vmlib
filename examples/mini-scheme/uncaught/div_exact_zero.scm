; (/ 7 0): an exact zero divisor is an error in guile ("Numerical overflow"),
; where this printed +inf.0. The expected text is this front end's own words
; (guile adds a backtrace and "Throw to key"); the position is the call's,
; which a trap raised inside the library helper reports as well.
(display "before ")
(display (/ 7 0))
