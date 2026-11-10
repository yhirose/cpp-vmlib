; (quotient 7.0 0): an error, worded and positioned as the exact case is. It
; was a thrown string reported at the helper's position, "quotient: numerical
; overflow". Expected text: this front end's own words, not guile's.
(display "before ")
(display (quotient 7.0 0))
