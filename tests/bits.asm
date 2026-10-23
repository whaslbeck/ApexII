.ROM_SIZE 262144

; Bit map test: two bit routines (on, test) behind inline wrappers (bit
; number after the call) and an A-register wrapper, called with flags and
; lamps (matrix 0x48 / 0x00) from the game code.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
.BANK SYSTEM
.ORG 0x8000
Start:
    JSR FlagOn
    .DB 0x05
    JSR FlagTest
    .DB 0x05
    JSR LampOn
    .DB 0x11
    LDA #0x07
    JSR FlagOnA
    JSR FlagOn
    .DB 0x07
    LDB #0x48
    LDA #0x09
    JSR BitOn
    RTS
BitOn:
    RTS
BitTest:
    RTS
FlagOn:
    PSHS A,B,X
    LDX 5,S
    LDA ,X+
    STX 5,S
    LDB #0x48
    JSR BitOn
    PULS A,B,X,PC
LampOn:
    PSHS A,B,X
    LDX 5,S
    LDA ,X+
    STX 5,S
    LDB #0x00
    JSR BitOn
    PULS A,B,X,PC
FlagTest:
    PSHS A,B,X
    LDX 5,S
    LDA ,X+
    STX 5,S
    LDB #0x48
    JSR BitTest
    PULS A,B,X,PC
LampTest:
    PSHS A,B,X
    LDX 5,S
    LDA ,X+
    STX 5,S
    LDB #0x00
    JSR BitTest
    PULS A,B,X,PC
FlagOnA:
    PSHS B
    LDB #0x48
    JSR BitOn
    PULS B,PC
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
