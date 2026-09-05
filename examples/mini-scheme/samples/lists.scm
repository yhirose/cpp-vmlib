; Lists. A pair is a two-element array here, with car at 0 and cdr at 1 --
; nothing else in this subset produces one, so TypeOf tells a pair from
; everything else without a tag of its own.

(define l '(1 2 3 4 5))
(display l) (newline)
(display (car l)) (display " ") (display (cdr l)) (newline)
(display (length l)) (display " ") (display (list-ref l 2)) (newline)
(display (reverse l)) (newline)
(display (append '(1 2) '(3 4))) (newline)
(display (append '(1) '(2) '(3))) (newline)

; An improper list prints with the dot, which is why rendering a pair is
; its own function rather than a join.
(display (cons 1 2)) (display " ") (display (cons 1 (cons 2 3))) (newline)
(display (cons '(1 2) '(3))) (newline)

(display (map (lambda (x) (* x x)) l)) (newline)
(for-each (lambda (x) (display x) (display ",")) l) (newline)

(define (fold f init xs)
  (if (null? xs) init (fold f (f init (car xs)) (cdr xs))))
(display (fold + 0 l)) (display " ") (display (fold * 1 l)) (newline)

(define (filter pred xs)
  (cond ((null? xs) '())
        ((pred (car xs)) (cons (car xs) (filter pred (cdr xs))))
        (else (filter pred (cdr xs)))))
(display (filter (lambda (x) (> x 2)) l)) (newline)

(define (range a b)
  (if (>= a b) '() (cons a (range (+ a 1) b))))
(display (range 0 6)) (newline)
(display (fold + 0 (map (lambda (x) (* x x)) (range 1 11)))) (newline)

(display (memq 3 l)) (display " ") (display (memq 9 l)) (newline)
(define table '((a 1) (b 2) (c 3)))
(display (assoc 'b table)) (display " ") (display (assoc 'z table)) (newline)

; Quoted data is built at bind time: a list becomes a chain of pairs and a
; symbol becomes a string, which is why display shows it bare.
(display '(a b (c d) 1 "s")) (newline)
(display (equal? '(1 (2 3)) '(1 (2 3)))) (display " ")
(display (equal? '(1 (2 3)) '(1 (2 4)))) (newline)

(define (flatten xs)
  (cond ((null? xs) '())
        ((pair? (car xs)) (append (flatten (car xs)) (flatten (cdr xs))))
        (else (cons (car xs) (flatten (cdr xs))))))
(display (flatten '(1 (2 (3 4)) 5))) (newline)

(define (insert x sorted)
  (cond ((null? sorted) (list x))
        ((< x (car sorted)) (cons x sorted))
        (else (cons (car sorted) (insert x (cdr sorted))))))
(define (sort xs)
  (if (null? xs) '() (insert (car xs) (sort (cdr xs)))))
(display (sort '(5 2 8 1 9 3))) (newline)
