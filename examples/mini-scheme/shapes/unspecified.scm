; A value-less expression is the VM's void immediate: a fallthrough loads a
; constant and allocates nothing. Counted: the one-armed `if` here and the
; three runtime helpers that answer it (`$display`, `$newline`, `$foreach1`). The one newobject is the runtime's `$escape`
; continuation, not this program's.
(define (f) (if #f 1))
(f)
