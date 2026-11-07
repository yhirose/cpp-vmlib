; R7RS boilerplate: define-record-type lives in (scheme base), and
; guile (unlike this front end) needs telling so.
(import (scheme base) (scheme write))

; case, and define-record-type -- R7RS's answer to "a class", built the
; same way examples/mini-python's and examples/mini-ruby's classes are: an
; object whose hidden state names its type, and each accessor a func with
; no source body of its own. Neither needed anything from the IR that was
; not already here.

(define (weekday n)
  (case n
    ((0) "Sunday")
    ((1) "Monday")
    ((2 3 4 5) "midweek")
    ((6) "Saturday")
    (else "unknown")))

(display (weekday 0)) (display " ") (display (weekday 3))
(display " ") (display (weekday 6)) (display " ") (display (weekday 9))
(newline)

; case falls through to else when nothing matches, and a clause may list
; more than one datum -- symbols compare with eqv? the same as numbers do.
(define (classify c)
  (case c
    ((a e i o u) "vowel")
    (else "other")))
(display (classify 'a)) (display " ") (display (classify 'z)) (newline)

(define-record-type point
  (make-point x y)
  point?
  (x point-x set-point-x!)
  (y point-y set-point-y!))

(define p (make-point 3 4))
(display (point? p)) (display " ") (display (point? 5)) (newline)
(display (point-x p)) (display " ") (display (point-y p)) (newline)
(set-point-x! p 10)
(display (point-x p)) (newline)

(define (norm2 pt) (+ (* (point-x pt) (point-x pt))
                      (* (point-y pt) (point-y pt))))
(display (norm2 (make-point 3 4))) (newline)

; A field with no mutator is read-only -- set-point-x! exists because the
; record type said so, and only because it did.
(define-record-type circle
  (make-circle radius)
  circle?
  (radius circle-radius))

(define c (make-circle 5))
(display (circle-radius c)) (display " ") (display (circle? c)) (newline)
(display (circle? p)) (display " ") (display (point? c)) (newline)

; Records in a list, each answering for itself -- the shape a program
; actually uses a "class" for.
(define pts (list (make-point 1 1) (make-point 2 2) (make-point 3 3)))
(for-each (lambda (pt) (display (norm2 pt)) (display " ")) pts)
(newline)

; case dispatching on a record's own field.
(define (describe pt)
  (case (point-x pt)
    ((0) "on the y-axis")
    ((1 2 3) "small x")
    (else "large x")))
(display (describe (make-point 0 0))) (newline)
(display (describe (make-point 2 0))) (newline)
(display (describe (make-point 99 0))) (newline)

; A record displays the way guile prints one, fields in the order they
; were declared, and a field the constructor does not take starts as #f.
(define-record-type node (make-node val) node? (val node-val)
  (next node-next set-node-next!))
(define nd (make-node 1))
(display (node-next nd)) (newline)
(set-node-next! nd (make-node 2))
(display nd) (newline)

; A field is the program's whatever its name. The one below starts with
; byte 1, written raw: the type a record was made by is kept where no
; field can reach it, so the predicate still knows its own.
(define-record-type tagged (make-tagged rec) tagged? (rec tagged-rec))
(define tg (make-tagged 5))
(display (tagged? tg)) (display " ") (display (tagged-rec tg)) (newline)
