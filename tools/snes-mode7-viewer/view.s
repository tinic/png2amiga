; Minimal LoROM Mode 7 viewer for png2amiga output (see build.sh).
; frame.bin (16 KB tilemap + 16 KB tiles) goes in bank 1, frame.pal
; (512 bytes CGRAM) in bank 0. DIRECT=1 sets direct colour.
;
; Mode 7 VRAM interleaves the two: tilemap in the low bytes, tile pixels
; in the high bytes. M7VOFS is -1 because the first visible line is line
; 1; with 0 the frame's top row is hidden.
.p816
.smart

.segment "CODE"
reset:
    sei
    clc
    xce
    rep #$38
    .a16
    .i16
    ldx #$1FFF
    txs
    lda #$0000
    tcd
    sep #$20
    .a8
    lda #$80
    sta $2100           ; force blank
    stz $4200           ; no NMI/IRQ
    lda #$07
    sta $2105           ; BGMODE 7
    stz $211A           ; M7SEL
    stz $211B           ; M7A = $0100
    lda #$01
    sta $211B
    stz $211C           ; M7B = 0
    stz $211C
    stz $211D           ; M7C = 0
    stz $211D
    stz $211E           ; M7D = $0100
    lda #$01
    sta $211E
    stz $211F           ; centre X
    stz $211F
    stz $2120           ; centre Y
    stz $2120
    stz $210D           ; M7HOFS
    stz $210D
    lda #$FF            ; M7VOFS = -1: the first visible line is line 1
    sta $210E
    lda #$1F
    sta $210E
    lda #$01
    sta $212C           ; TM = BG1
    stz $212D
    stz $212E
    stz $212F
    lda #DIRECT
    sta $2130           ; CGWSEL bit 0: direct colour
    stz $2131
    stz $2132
    lda #$E0
    sta $2132           ; fixed colour black
    stz $2133           ; SETINI

    ; Tilemap -> VRAM low bytes.
    stz $2115           ; increment after $2118
    stz $2116
    stz $2117
    stz $4300           ; DMA 0: A->B, one register
    lda #$18
    sta $4301
    ldx #$8000
    stx $4302
    lda #$01
    sta $4304
    ldx #$4000
    stx $4305
    lda #$01
    sta $420B

    ; Tiles -> VRAM high bytes.
    lda #$80
    sta $2115           ; increment after $2119
    stz $2116
    stz $2117
    lda #$19
    sta $4301
    ldx #$C000
    stx $4302
    lda #$01
    sta $4304
    ldx #$4000
    stx $4305
    lda #$01
    sta $420B

    ; Palette -> CGRAM.
    stz $2121
    lda #$22
    sta $4301
    ldx #.loword(palette)
    stx $4302
    lda #^palette
    sta $4304
    ldx #$0200
    stx $4305
    lda #$01
    sta $420B

    lda #$0F
    sta $2100           ; screen on
forever:
    bra forever

nothing:
    rti

.segment "RODATA"
palette:
    .incbin "frame.pal"

.segment "HEADER"
    .byte "PNG2AMIGA MODE7 TEST "   ; 21 bytes
    .byte $20                      ; LoROM, slow
    .byte $00                      ; ROM only
    .byte $06                      ; 64 KB
    .byte $00                      ; no SRAM
    .byte $01                      ; North America
    .byte $00                      ; developer
    .byte $00                      ; version
    .word $0000, $FFFF             ; checksum complement, checksum

.segment "VECTORS"
    .word 0, 0
    .word nothing, nothing, nothing, nothing, 0, nothing   ; native COP BRK ABORT NMI - IRQ
    .word 0, 0
    .word nothing, 0, nothing, nothing, reset, nothing     ; emulation COP - ABORT NMI RESET IRQ

.segment "BANK1"
    .incbin "frame.bin"
