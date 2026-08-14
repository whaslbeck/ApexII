.ROM_SIZE 524288

; An inline far-code routine whose payload here is a null (0x0000) pointer — the
; regular "no body" case.  Default config warns (inline_far_code_invalid); the
; far_code_allow_null option emits it plainly with no warning.  Both round-trip.

.BANK SYSTEM
.ORG 0x8000
START:
    JSR EFFECT
    .DB 0x00, 0x00, 0x00
    RTS
EFFECT:
    RTS

.ORG 0xfff0
    .DW START, START, START, START, START, START, START
