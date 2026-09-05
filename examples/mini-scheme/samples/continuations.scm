; call/cc, the half of it this library can express.
;
; The top-level README lists multi-shot continuations under "What stays out
; of reach": a coroutine is one-shot, its parked frames move rather than
; being copied. But the *escape* half -- a continuation invoked while the
; call/cc that made it is still on the stack -- is an unwind to a known
; point, which is a TryCatch and a Throw. That is the half almost every
; real program uses, and it is what this sample is.

; The shape everyone meets first: leave early with a value.
(display (call/cc (lambda (k) (+ 1 (k 42))))) (newline)

; A continuation that is never invoked is just an ignored argument.
(display (call/cc (lambda (k) (+ 1 2)))) (newline)

; Escaping out of a fold, several frames down, without unwinding by hand.
(define (product-or-zero xs)
  (call/cc
    (lambda (return)
      (define (go l acc)
        (cond ((null? l) acc)
              ((= (car l) 0) (return 0))
              (else (go (cdr l) (* acc (car l))))))
      (go xs 1))))
(display (product-or-zero '(1 2 3 4))) (newline)
(display (product-or-zero '(1 2 0 4))) (newline)

; A search that stops at the first hit, from inside for-each -- the
; classic reason to want an escape at all, since for-each has no break.
(define (find-first pred xs)
  (call/cc
    (lambda (found)
      (for-each (lambda (x) (if (pred x) (found x) #f)) xs)
      #f)))
(display (find-first (lambda (x) (> x 2)) '(1 2 3 4))) (newline)
(display (find-first (lambda (x) (> x 9)) '(1 2 3 4))) (newline)

; Through several frames at once: the escape unwinds all of them.
(define (deep n k) (if (= n 0) (k 'bottom) (deep (- n 1) k)))
(display (call/cc (lambda (k) (deep 100 k)))) (newline)

; Nested call/cc: each escape names its own activation, so the inner one
; leaves only the inner.
(display (call/cc
           (lambda (outer)
             (+ 1 (call/cc (lambda (inner) (inner 10)))))))
(newline)
(display (call/cc
           (lambda (outer)
             (+ 1 (call/cc (lambda (inner) (outer 10)))))))
(newline)

; The escape is a procedure like any other, so it can be passed along.
(define (use-escape k) (k "escaped"))
(display (call/cc (lambda (k) (use-escape k) "not reached"))) (newline)

; And it composes with the ordinary value: the call/cc's own answer is
; whichever arrived first.
(define (either-way flag)
  (call/cc (lambda (k) (if flag (k "early") "late"))))
(display (either-way #t)) (display " ") (display (either-way #f)) (newline)
