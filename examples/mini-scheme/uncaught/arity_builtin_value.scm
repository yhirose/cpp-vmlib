; A library procedure used as a value is refused with its Scheme name:
; (map cons ...) calls cons with one argument. Guile's own text is
; "In procedure cons: Wrong number of arguments to #<procedure cons (_ _)>";
; the expected text is this front end's own words.
(display (map cons (list 1)))
