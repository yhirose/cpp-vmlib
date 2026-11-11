; The value in a "Wrong type" report is written as Scheme writes it:
; #<unspecified>, not the VM's `void`.
(display (+ 1 (if #f #f)))
