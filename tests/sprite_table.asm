.ROM_SIZE 524288
.BANK SYSTEM
.ORG 0x8000
TablePtr:
    .DW Img1
    .DW Img2
.ORG 0x8010
Img1:
    .DB 0x05
    .DB 0x80, 0x40, 0x20
.ORG 0x8020
Img2:
    .DB 0x00, 0x00, 0x00, 0x02, 0x05
    .DB 0x80, 0x40
.ORG 0x8030
Reset:
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
