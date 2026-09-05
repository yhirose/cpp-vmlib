; Closures, and the three binding forms Scheme distinguishes between.

(define (make-counter)
  (define n 0)
  (lambda () (set! n (+ n 1)) n))
(define c1 (make-counter))
(define c2 (make-counter))
(display (c1)) (display (c1)) (display (c2)) (display (c1)) (newline)

; let evaluates its initializers in the enclosing scope; let* in the one it
; is building; letrec makes every name visible to every initializer, which
; is what two mutually recursive procedures need.
(define x 10)
(display (let ((x 1) (y x)) (list x y))) (newline)
(display (let* ((x 1) (y x)) (list x y))) (newline)
(display (letrec ((even2? (lambda (n) (if (= n 0) #t (odd2? (- n 1)))))
                  (odd2? (lambda (n) (if (= n 0) #f (even2? (- n 1))))))
           (list (even2? 10) (odd2? 10))))
(newline)

; Internal defines are letrec too: two procedures defined in one body see
; each other, whichever order they were written in.
(define (parity n)
  (define (ev? k) (if (= k 0) #t (od? (- k 1))))
  (define (od? k) (if (= k 0) #f (ev? (- k 1))))
  (if (ev? n) 'even 'odd))
(display (parity 10)) (display " ") (display (parity 7)) (newline)

; Two levels of nesting: the middle procedure carries x for the inner one.
(define (outer a)
  (lambda (b) (lambda (c) (+ a b c))))
(display (((outer 1) 2) 3)) (newline)

; A let inside a loop gives each iteration its own binding, so the
; procedures made there do not share one.
(define (collect n)
  (let build ((i 0) (acc '()))
    (if (= i n)
        (reverse acc)
        (let ((captured i))
          (build (+ i 1) (cons (lambda () captured) acc))))))
(define fs (collect 3))
(display (list ((car fs)) ((car (cdr fs))) ((car (cdr (cdr fs)))))) (newline)

; Shared state between two closures: one cell, not two copies.
(define (make-pair)
  (define v 0)
  (list (lambda () (set! v (+ v 1))) (lambda () v)))
(define p (make-pair))
((car p)) ((car p))
(display ((car (cdr p)))) (newline)

; A procedure is a value, so it can be stored, passed and returned.
(define (compose f g) (lambda (x) (f (g x))))
(define add1 (lambda (x) (+ x 1)))
(define double (lambda (x) (* x 2)))
(display ((compose add1 double) 5)) (display " ")
(display ((compose double add1) 5)) (newline)
(display (map (compose add1 double) '(1 2 3))) (newline)

; The library procedures are values too, which needed nothing special --
; every one of them is an ordinary function in the module.
(display (map car '((1 a) (2 b)))) (newline)
(display (procedure? car)) (display " ") (display (procedure? 1)) (newline)
