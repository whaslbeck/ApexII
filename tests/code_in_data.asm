.ROM_SIZE 524288

; A [data] range that hides a real call: the bytes 0xBD 0x80 0x10 are `JSR
; ROUTINE`, but the range is classified as data so flow analysis never sees it.
; The report_code_in_data option flags exactly this — the far-pointer-reached
; bodies the disassembler misses.

.BANK SYSTEM
.ORG 0x8000
    .DB 0x01, 0xBD, 0x80, 0x10, 0x02
ROUTINE:
    RTS

.ORG 0xfff0
    .DW ROUTINE, ROUTINE, ROUTINE, ROUTINE, ROUTINE, ROUTINE, ROUTINE, ROUTINE
