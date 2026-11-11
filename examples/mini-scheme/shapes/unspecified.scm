; A value-less expression is the VM's void immediate: a fallthrough loads a
; constant and allocates nothing. The one newobject the bytecode holds is the
; runtime's escape continuation (`$escape`), not this program's.
(define (loop i)
  (if (< i 10) (begin (if (= i -1) 1) (loop (+ i 1))) (display "")))
(loop 0)
(display (list (when #f 1) (unless #t 1)))
