.ROM_SIZE 262144

; Base ROM for the apexpatch tests: a little system-bank program with an
; inline-argument call, free space at 0x9000 and a paged routine.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
PagedRoutine:
    LDA #0x05
    RTS
.BANK SYSTEM
.ORG 0x8000
Start:
    LDA #0x0a
    STA 0x50
    JSR Check
    BNE Start
    JSR Helper
    .DB 0x42
    LDB #0x01
    RTS
.ORG 0x8100
Check:
    LDA 0x50
    CMPA #0x0a
    RTS
.ORG 0x8120
Helper:
    RTS
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
