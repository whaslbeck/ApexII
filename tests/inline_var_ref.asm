.ROM_SIZE 524288

; Regression: a variable-length inline payload whose bytes look like an in-ROM
; address (0x80 0x30 = 0x8030).  The analysis pass must consume the whole
; bytes_until payload — NOT decode those two bytes as a pointer and plant a
; spurious label/reference at 0x8030 (the Dr. Who B31_A64f4 failure). No
; Bff_A8030 label may appear.

.BANK SYSTEM
.ORG 0x8000
START:
    JSR EFF
    .DB 0x80, 0x30, 0x80, 0x30, 0x00
    RTS
EFF:
    LDU ,S
    RTS
.ORG 0x8030
    RTS
.ORG 0xfff0
    .DW START, START, START, START, START, START, START, START
