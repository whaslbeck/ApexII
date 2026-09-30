; apexini detect-inline: inline-argument routines in the system bank.
; W1/W2 are marker-less wrappers (found by the return-address walk), M3/M2 carry
; the OS marker 0xCF, 0x73+N; the 0xCF 0x74 inside DataTable is a coincidence
; (no routine, no caller) and must not be reported.
.ROM_SIZE 262144
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR W1
    .DB 0x11
    JSR W2
    .DW 0x1234
    JSR M3
    .DB 0xaa, 0xbb, 0xcc
    JSR M2
    .DW 0x5678
    JSR W1
    .DB 0x22
    JSR W2
    .DW 0x4321
    JSR M3
    .DB 0x01, 0x02, 0x03
    JSR M2
    .DW 0x0009
Loop:
    BRA Loop
W1:
    PSHS A,X
    LDX 3,S
    LDA ,X+
    STX 3,S
    PULS A,X,PC
W2:
    PSHS A,B,X
    LDX 4,S
    LDD ,X++
    STX 4,S
    PULS A,B,X,PC
    .DB 0xcf, 0x76
M3:
    PSHS A,X
    LDX 3,S
    LDA ,X+
    LDA ,X+
    LDA ,X+
    STX 3,S
    PULS A,X,PC
    .DB 0xcf, 0x75
M2:
    PSHS A,B,X
    LDX 4,S
    LDD ,X
    LEAX 2,X
    STX 4,S
    PULS A,B,X,PC
DataTable:
    .DB 0x55, 0xcf, 0x74, 0x10, 0x20, 0x30
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
