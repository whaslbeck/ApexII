.ROM_SIZE 262144

; patch_base.asm with patch_p2.patch.ini applied by hand: the reference image
; for apexpatch build (allocation, detour, far pointer).
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
.ORG 0x8016
MyRoutine:
    LDA #0x05
    STA 0x0100
    RTS
Detour:
    INC 0x0100
    LDA 0x50
    JMP 0x8103
.ORG 0x8100
Check:
    JMP Detour
    CMPA #0x0a
    RTS
.ORG 0x8120
Helper:
    RTS
.ORG 0x9100
    .DB 0x80, 0x16, 0xff
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
