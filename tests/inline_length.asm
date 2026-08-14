.ROM_SIZE 524288

; EFFECT consumes a 5-byte inline payload: it reads the return address off the
; stack (LDU ,S), advances it past the payload (LEAU 5,U) and writes it back
; (STU ,S).  check_inline_length reads that 5 out of the routine and flags any
; [inline] length that disagrees — the silent "wrong inline length hides code"
; failure mode.

.BANK SYSTEM
.ORG 0x8000
START:
    JSR EFFECT
    .DB 0x11, 0x22, 0x33, 0x44, 0x55
    RTS
EFFECT:
    LDU ,S
    LEAU 5,U
    STU ,S
    RTS

.ORG 0xfff0
    .DW START, START, START, START, START, START, START, START
