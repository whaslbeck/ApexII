.ROM_SIZE 524288

; A [labels] entry (MyBlock at 0x8020) lands inside a bytes[32] data range and is
; referenced by an operand.  The raw-bytes emitter must break the .DB rows there
; and emit the "MyBlock:" definition, or the reference can't be reassembled (the
; Dr. Who LampStringDirectory / B3d_A5916 failure).  Byte output is unchanged, so
; it round-trips.

.BANK SYSTEM
.ORG 0x8000
START:
    LDX #0x8020         ; -> LDX #MyBlock (must resolve)
    RTS
.ORG 0x8010
    .DB 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    .DB 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
    .DB 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    .DB 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10

.ORG 0xfff0
    .DW START, START, START, START, START, START, START, START
