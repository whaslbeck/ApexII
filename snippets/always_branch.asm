; snippet: Always branch
; type: asm
; about: Replaces a conditional short branch (BEQ, BNE, BCS, ...) with BRA, so
; about: the jump is always taken. Put it at the branch instruction.
; param: TARGET where the branch goes (the original target's label)
    BRA {TARGET}
