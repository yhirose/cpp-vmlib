; Variadic procedures. A rest parameter -- `(lambda args ...)`,
; `(define (f a . rest) ...)` -- collects the arguments past the fixed ones
; into a fresh list, and `apply` spreads a list back into arguments: the
; IR's Func::rest_param and Tag::CallSpread, one at each end of a call.

(define (show . xs) (for-each display xs) (newline))

; The three shapes of formals.
(define (count . xs) (length xs))
(show (count) " " (count 1 2 3))
(define (f a b . rest) (list a b rest))
(show (f 1 2) " " (f 1 2 3 4))
(show ((lambda args args) 1 2 3) " " ((lambda args args)))
(show ((lambda (a . r) (cons r a)) 1 2 3))

; A rest list is new on every call, even when apply hands one in.
(define (id . xs) xs)
(define l (list 1 2))
(show (eq? (apply id l) l) " " (equal? (apply id l) l))

; It is an ordinary variable: captured by a closure, assigned with set!.
(define (adder . ns) (lambda (x) (apply + x ns)))
(show ((adder 1 2 3) 10))
(define (push-front . xs) (set! xs (cons 0 xs)) xs)
(show (push-front 1 2))

; apply, with and without arguments before the list.
(show (apply + '(1 2 3)) " " (apply + 1 2 '(3 4)) " " (apply + '()))
(show (apply list 1 '(2 3)) " " (apply f '(1 2 3)))
(show (apply apply (list + (list 1 2))))

; A builtin that takes any number of arguments takes them as a value too,
; folding them the way a direct call to it does.
(define (with-1-2-3 g) (g 1 2 3))
(show (with-1-2-3 +) " " (with-1-2-3 *) " " (with-1-2-3 -) " "
      (with-1-2-3 list))
(show (with-1-2-3 max) " " (with-1-2-3 min) " " (with-1-2-3 <) " "
      (with-1-2-3 >))
(show (apply - '(10)) " " (apply * '()) " " (apply max '(3 9 2)))
(show (apply string-append '("a" "b" "c")) " "
      (apply append '((1) (2 3) ())))
(show (apply = '(2 2 2)) " " (apply <= '(1 2 2)) " " (apply >= '(3 1 2)))
(show (map (lambda (xs) (apply + xs)) '((1 2) (3 4 5) ())))

; apply in tail position is a tail call -- R7RS requires it -- so a loop
; through it runs in one frame, twice as deep as the executor allows calls
; to nest: written out, and through apply as a value. Then a variadic
; procedure that recurses through apply, with an argument before the list.
(define (down n) (if (= n 0) 'done (apply down (list (- n 1)))))
(show (down 20000))
(define ap apply)
(define (down2 n) (if (= n 0) 'done (ap down2 (list (- n 1)))))
(show (down2 20000))
(define (sum acc . xs)
  (if (null? xs) acc (apply sum (+ acc (car xs)) (cdr xs))))
(show (sum 0 1 2 3 4 5))
