.ROM_SIZE 524288

.BANK SYSTEM
.ORG 0x8000
Start:
    BRA Target
Target:
    RTS

.ORG 0xfffe
    .DW Start
