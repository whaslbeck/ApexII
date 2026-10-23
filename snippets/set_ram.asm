; snippet: Store a constant in RAM
; type: hook
; about: Writes VALUE to a RAM byte whenever the hooked code runs, e.g. to keep
; about: a setting or a counter fixed. A and the flags are preserved.
; param: VAR RAM byte (a label or 0xNNNN)
; param: VALUE the byte to store = 0x00
    PSHS CC,A
    LDA #{VALUE}
    STA {VAR}
    PULS CC,A
