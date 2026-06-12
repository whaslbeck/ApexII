.ROM_SIZE 524288

.BANK SYSTEM
.ORG 0x8000
Main:
    JSR R_id
    JSR R_chg
    JSR R_mv
    JSR R_rm
    RTS

.ORG 0x8040
R_id:
    LDA #0x11
    LDB #0x22
    ADDA #0x33
    STA 0x1000
    STB 0x1001
    RTS

.ORG 0x8060
R_chg:
    LDA #0x44
    LDB #0x55
    ADDA #0x66
    STA 0x1002
    STB 0x1003
    RTS

.ORG 0x8080
R_mv:
    LDA #0x77
    LDB #0x88
    ADDA #0x99
    STA 0x1004
    STB 0x1005
    RTS

.ORG 0x80a0
R_rm:
    LDA #0xaa
    LDB #0xbb
    ADDA #0xcc
    STA 0x1006
    STB 0x1007
    RTS

.ORG 0x8100
Str1:
    STRING "HELLO WORLD"

.ORG 0xfff2
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
