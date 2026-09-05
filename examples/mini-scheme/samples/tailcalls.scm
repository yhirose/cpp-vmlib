; In Scheme, iteration *is* tail recursion. Func::tail_calls is not an
; optimization here -- it is the difference between a loop working and not,
; and every number below is far past RunOptions::max_call_depth (10000).

(define (sum n acc) (if (= n 0) acc (sum (- n 1) (+ acc n))))
(display (sum 1000000 0)) (newline)

; Named let is Scheme's loop, and it is nothing but a procedure that
; tail-calls itself.
(display (let loop ((i 0) (acc 0))
           (if (= i 500000) acc (loop (+ i 1) (+ acc 1)))))
(newline)

; Mutual tail recursion: the frame is reused across two procedures.
(define (even2? n) (if (= n 0) #t (odd2? (- n 1))))
(define (odd2? n) (if (= n 0) #f (even2? (- n 1))))
(display (even2? 300000)) (display " ") (display (odd2? 300001)) (newline)

; A tail call in every branch of a `cond`, which is where the IR has to
; walk through the If chain to find the call still in tail position.
(define (classify n acc)
  (cond ((= n 0) acc)
        ((= (modulo n 3) 0) (classify (- n 1) (+ acc 1)))
        (else (classify (- n 1) acc))))
(display (classify 200000 0)) (newline)

; Through `begin` and `let`, which the IR also has to see past.
(define (through n acc)
  (if (= n 0)
      acc
      (begin
        (let ((next (- n 1)))
          (through next (+ acc 2))))))
(display (through 150000 0)) (newline)

; A tail call through a value rather than a name.
(define (drive f n) (if (= n 0) 'done (f f (- n 1))))
(display (drive drive 120000)) (newline)

; Continuation-passing style, which is all tail calls by construction --
; the shape a Scheme compiler is expected to make free.
(define (cps-sum n acc k)
  (if (= n 0) (k acc) (cps-sum (- n 1) (+ acc n) k)))
(display (cps-sum 200000 0 (lambda (v) v))) (newline)

; Not every call is a tail call, and that is the point of the distinction:
; this one has a multiplication left to do afterwards, so it stacks -- and
; stays inside the limit on purpose.
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(display (fact 20)) (newline)
