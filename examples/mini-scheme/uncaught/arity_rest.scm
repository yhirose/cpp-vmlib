; A procedure with a rest parameter needs the arguments before it.
; Expected text: this front end's own words, not guile's.
(define (f a . rest) a)
(display (f))
