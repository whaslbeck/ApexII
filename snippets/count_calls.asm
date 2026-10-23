; snippet: Count executions
; type: hook
; about: Increments a RAM byte each time the hooked code runs (wraps at 256).
; about: Pair it with a ram op that names the byte.
; param: VAR RAM byte to count in (a label or 0xNNNN)
    INC {VAR}
