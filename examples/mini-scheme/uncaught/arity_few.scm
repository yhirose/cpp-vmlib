; Too few arguments: guile refuses the call, where this padded them with
; nil. The expected text is this front end's own words: guile's is
; "Wrong number of arguments to #<procedure f (a b)>", with the formals,
; which a trap does not carry.
(define (f a b) a)
(display (f 1))
