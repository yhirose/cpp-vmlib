; Numbers, pairs and the two places Scheme disagrees with the VM's
; defaults: what is true, and what a whole inexact number prints as.

(display (+ 1 2 3)) (display " ") (display (* 2 3 4)) (newline)
(display (- 10 1 2)) (display " ") (display (- 5)) (newline)
(display (quotient 7 2)) (display " ") (display (remainder 7 2)) (newline)
(display (modulo -7 3)) (display " ") (display (remainder -7 3)) (newline)
(display (/ 1.0 4)) (display " ") (display (expt 2 10)) (newline)
(display (abs -3)) (display " ") (display (min 2 9)) (display " ")
(display (max 2 9)) (newline)

; Only #f is false. Value::truthy() calls nil and 0 false, and its comment
; says why it will not decide: '() and 0 are both true here.
(define (t v) (if v "T" "F"))
(display (t '())) (display (t 0)) (display (t "")) (display (t #f))
(display (t #t)) (display (t 1)) (newline)
(display (not #f)) (display " ") (display (not 0)) (newline)

; and/or answer one of their operands.
(display (and 1 2)) (display " ") (display (and 1 #f 2)) (newline)
(display (or #f 3)) (display " ") (display (or #f #f)) (newline)

; Inexact numbers print with a point forced onto a whole one, which is
; to_display's output plus the one rule its comment leaves to a front end.
(display 1.0) (display " ") (display 100.0) (display " ") (display 0.5)
(newline)
(display (+ 0.1 0.2)) (newline)
(display (* 1.0 -0.0)) (newline)

(display (= 1 1)) (display " ") (display (< 1 2 3)) (display " ")
(display (< 1 3 2)) (newline)
; `(eq? '(1) '(1))` is left out: whether two structurally-equal quoted
; literals are the same object is unspecified in Scheme, and this
; front end (which does not intern literals) and Guile's compiler
; (which sometimes does) can honestly disagree about it.
(define one-list '(1))
(display (eq? 'a 'a)) (display " ") (display (eq? one-list one-list)) (display " ")
(display (equal? '(1 (2)) '(1 (2)))) (newline)

(display (number? 1)) (display (string? "s")) (display (procedure? car))
(display (boolean? #f)) (display (pair? '(1))) (display (null? '()))
(newline)

(display (string-append "a" "b" "c")) (display " ")
(display (string-length "hello")) (display " ")
(display (number->string 42)) (newline)

(display (cons 1 2)) (display " ") (display (cons 1 '(2))) (newline)
(display '(1 (2 3) "s")) (newline)
(display (list 1 2 3)) (display " ") (display (list)) (newline)

(cond ((= 1 2) (display "no"))
      ((= 1 1) (display "yes"))
      (else (display "else")))
(newline)
(when #t (display "when") (newline))
(unless #f (display "unless") (newline))
