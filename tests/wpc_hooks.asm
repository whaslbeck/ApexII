; apexini hooks: the WPC game-hook table at 0x8001 (far pointers until the first
; invalid one), the far-call helper (JSR followed by an inline slot address at
; >= 3 sites), its call sites plus an LDX #slot, and RTS stubs (hidden unless
; --all).  The lone JSR
; Other / .DW slot is below the helper threshold and must not count.
.ROM_SIZE 262144
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
GameStart:
    CLRA
    RTS
GameDrain:
    LDA #1
    RTS
.BANK SYSTEM
.ORG 0x8000
    .DB 0x3e
HookTable:
    .DW GameStart
    .DB 0x30
    .DW Stub
    .DB 0xff
    .DW GameDrain
    .DB 0x30
    .DW Stub
    .DB 0xff
    .DW Stub
    .DB 0xff
    .DW Stub
    .DB 0xff
    .DW Stub
    .DB 0xff
    .DW Stub
    .DB 0xff
    .DW Stub
    .DB 0xff
    .DB 0x00, 0x00, 0x00
Reset:
    JSR FarCall
    .DW 0x8001
    JSR FarCall
    .DW 0x8007
    JSR FarCall
    .DW 0x8007
    LDX #0x8001
    JSR Other
    .DW 0x800a
Loop:
    BRA Loop
FarCall:
    RTS
Other:
    RTS
Stub:
    RTS
