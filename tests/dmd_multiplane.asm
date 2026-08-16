.ROM_SIZE 524288

; Two 4-colour DMD frames, each stored as TWO bit-planes back-to-back (a short
; type-1 frame is 8 bytes: type, special, then two 256-run escapes filling the
; 512-byte plane).  dmd_fullframe[2] must consume both planes (16 bytes); with a
; single plane the report_dmd_short warning must flag the missing plane.

.BANK SYSTEM
.ORG 0x8000
FRAME_A:
    .DB 0x01, 0xff, 0xff, 0x00, 0xaa, 0xff, 0x00, 0xbb   ; plane 0
    .DB 0x01, 0xff, 0xff, 0x00, 0xcc, 0xff, 0x00, 0xdd   ; plane 1
FRAME_B:
    .DB 0x01, 0xff, 0xff, 0x00, 0x11, 0xff, 0x00, 0x22   ; plane 0
    .DB 0x01, 0xff, 0xff, 0x00, 0x33, 0xff, 0x00, 0x44   ; plane 1

.ORG 0xfff0
    .DW FRAME_A, FRAME_A, FRAME_A, FRAME_A, FRAME_A, FRAME_A, FRAME_A, FRAME_A
