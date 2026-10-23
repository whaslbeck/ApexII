.ROM_SIZE 262144

; Typed register arguments (@args in [docs]) traced to their immediate loads.
; Site 1 resolves to an enum, site 2 has no enum value, site 3 is clobbered by
; INCA, site 4 is cut by a branch target, site 5 is an LDD feeding A and B,
; site 6 reaches a paged routine through a far-call helper, site 7 is a typed
; inline field.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
PagedSwitch:
    RTS
.BANK SYSTEM
.ORG 0x8000
Caller:
    LDA #0x2c
    JSR SwitchTest
    LDA #0x10
    STA 0x50
    JSR SwitchTest
    LDA #0x2c
    INCA
    JSR SwitchTest
    LDA #0x2c
Join:
    JSR SwitchTest
    LDD #0x2c05
    JSR SwitchLamp
    LDA #0x2c
    JSR FarCall
    .DW PagedSwitch
    .DB 0x30
    JSR LampOn
    .DB 0x05
    BNE Join
    RTS
.ORG 0x8100
SwitchTest:
    RTS
.ORG 0x8110
SwitchLamp:
    RTS
.ORG 0x8120
FarCall:
    RTS
.ORG 0x8130
LampOn:
    RTS
.ORG 0xfff2
    .DW Caller, Caller, Caller, Caller, Caller, Caller, Caller
