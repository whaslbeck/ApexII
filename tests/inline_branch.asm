.ROM_SIZE 524288

; A routine (Helper) that consumes one inline byte off its return address.
; Reached by a JSR (which pushes a return address -> inline byte applies) and by
; a BCC (a branch pushes nothing -> the inline byte must NOT be consumed).
.BANK SYSTEM
.ORG 0x8000
Caller:
    JSR Helper
    .DB 0x42
    BCC Helper
    NOP
    RTS
.ORG 0x8020
Helper:
    LDX ,S
    LDB ,X+
    STX ,S
    RTS
.ORG 0xfff2
    .DW Caller, Caller, Caller, Caller, Caller, Caller, Caller
