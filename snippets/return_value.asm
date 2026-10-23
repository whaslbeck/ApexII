; snippet: Return a fixed value
; type: asm
; about: Makes a routine return at once with A set to VALUE (and the flags of
; about: the load). Put it at the routine's first instruction.
; param: VALUE the value in A = 0x00
    LDA #{VALUE}
    RTS
