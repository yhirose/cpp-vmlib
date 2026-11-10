; Too many arguments are refused too; they were silently dropped. Expected
; text: this front end's own words, not guile's.
(define (f a) a)
(display (f 1 2))
