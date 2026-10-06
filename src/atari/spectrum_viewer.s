/* Standalone PAL 8 MHz 68000 TOS Spectrum viewer, GNU assembler syntax.
 * Position-independent code; 51104-byte SPU data follows picture.
 * TOS header reserves 33024 bytes of BSS for a 256-byte-aligned screen.
 * Timer B fires after the blank first line. CPU is in STOP when it fires.
 * Each image line: 24 postincrement longword moves = 480 cycles;
 * three MOVEA (12) + five NOP (20) = 32, total 512 PAL cycles.
 * Three independent palette pointers avoid adding gaps between banks.
 * SPU slot changes: x = 10*c + (c odd ? -5 : 1), then x+160.
 * Space/Escape returns to TOS and restores display and interrupt state.
 */
    .text
    .globl _start
_start:
    clr.l -(%sp)
    move.w #32,-(%sp)
    trap #1
    addq.l #6,%sp
    lea state(%pc),%a6
    move.l %d0,(%a6)
    move.w %sr,4(%a6)
    move.w #0x2700,%sr
    clr.b 26(%a6)
    movea.l 0x5a0.w,%a0
    move.l %a0,%d0
    beq.s cookies_done
cookie_loop:
    move.l (%a0)+,%d0
    beq.s cookies_done
    move.l (%a0)+,%d1
    cmpi.l #0x5f435055,%d0
    bne.s video_cookie
    tst.l %d1
    bne unsupported
video_cookie:
    cmpi.l #0x5f56444f,%d0
    bne.s cookie_loop
    swap %d1
    cmpi.w #1,%d1
    bhi unsupported
    move.b %d1,26(%a6)
    bra.s cookie_loop
cookies_done:
    move.w required_ste(%pc),%d0
    beq.s supported
    tst.b 26(%a6)
    beq unsupported
supported:
    cmpi.b #2,0xffff8260.w
    beq unsupported
    tst.b 26(%a6)
    beq.s save_st
    move.b 0xffff820d.w,68(%a6)
    move.b 0xffff820f.w,69(%a6)
    move.b 0xffff8265.w,70(%a6)
    clr.b 0xffff820d.w
    clr.b 0xffff820f.w
    clr.b 0xffff8265.w
save_st:
    move.l 0x70.w,8(%a6)
    move.l 0x120.w,12(%a6)
    move.b 0xfffffa07.w,16(%a6)
    move.b 0xfffffa09.w,17(%a6)
    move.b 0xfffffa13.w,18(%a6)
    move.b 0xfffffa15.w,19(%a6)
    move.b 0xfffffa1b.w,20(%a6)
    move.b 0xfffffa21.w,21(%a6)
    move.b 0xffff8201.w,22(%a6)
    move.b 0xffff8203.w,23(%a6)
    move.b 0xffff8260.w,24(%a6)
    move.b 0xffff820a.w,25(%a6)
    movem.l 0xffff8240.w,%d0-%d7
    movem.l %d0-%d7,32(%a6)
    clr.b 0xfffffa07.w
    clr.b 0xfffffa09.w
    clr.b 0xfffffa13.w
    clr.b 0xfffffa15.w
    clr.b 0xfffffa1b.w
    lea vbl(%pc),%a0
    move.l %a0,0x70.w
    lea raster(%pc),%a0
    move.l %a0,0x120.w
    lea picture(%pc),%a0
    move.l %a0,%a1
    adda.l #51104+255,%a1
    move.l %a1,%d0
    andi.l #0xffffff00,%d0
    move.l %d0,%a1
    move.l %d0,28(%a6)
    move.w #7999,%d1
copy:
    move.l (%a0)+,(%a1)+
    dbra %d1,copy
    move.l %a0,64(%a6)
    lsr.l #8,%d0
    move.b %d0,0xffff8203.w
    lsr.w #8,%d0
    move.b %d0,0xffff8201.w
    clr.b 0xffff8260.w
    move.b #2,0xffff820a.w
    /* Stop mouse packets: their coordinate bytes overlap keyboard scancodes. */
mouse_disable:
    btst #1,0xfffffc00.w
    beq.s mouse_disable
    move.b #0x12,0xfffffc02.w
keyboard_drain:
    btst #0,0xfffffc00.w
    beq.s loop
    move.b 0xfffffc02.w,%d0
    bra.s keyboard_drain
loop:
    stop #0x2300
    btst #0,0xfffffc00.w
    beq.s loop
    move.b 0xfffffc02.w,%d0
    cmpi.b #0x39,%d0
    beq.s exit
    cmpi.b #1,%d0
    bne.s loop
exit:
    move.w #0x2700,%sr
mouse_enable:
    btst #1,0xfffffc00.w
    beq.s mouse_enable
    move.b #8,0xfffffc02.w
    clr.b 0xfffffa1b.w
    move.l 8(%a6),0x70.w
    move.l 12(%a6),0x120.w
    move.b 16(%a6),0xfffffa07.w
    move.b 17(%a6),0xfffffa09.w
    move.b 18(%a6),0xfffffa13.w
    move.b 19(%a6),0xfffffa15.w
    move.b 21(%a6),0xfffffa21.w
    move.b 20(%a6),0xfffffa1b.w
    move.b 22(%a6),0xffff8201.w
    move.b 23(%a6),0xffff8203.w
    move.b 24(%a6),0xffff8260.w
    move.b 25(%a6),0xffff820a.w
    tst.b 26(%a6)
    beq.s restore_palette
    move.b 68(%a6),0xffff820d.w
    move.b 69(%a6),0xffff820f.w
    move.b 70(%a6),0xffff8265.w
restore_palette:
    movem.l 32(%a6),%d0-%d7
    movem.l %d0-%d7,0xffff8240.w
leave:
    move.w 4(%a6),%sr
    move.l (%a6),-(%sp)
    move.w #32,-(%sp)
    trap #1
    addq.l #6,%sp
    clr.w -(%sp)
    trap #1
unsupported:
    lea error_message(%pc),%a0
    move.l %a0,-(%sp)
    move.w #9,-(%sp)
    trap #1
    addq.l #6,%sp
    bra leave
error_message:
    .asciz "Spectrum viewer requires an 8 MHz 68000 ST/STE, color monitor; 4096 requires STE.\r\n"
    .even
vbl:
    move.l %d0,-(%sp)
    move.l 64(%a6),%a0
    movem.l (%a0)+,%d0-%d7
    movem.l %d0-%d7,0xffff8240.w
    clr.b 0xfffffa1b.w
    move.b #1,0xfffffa21.w
    bset #0,0xfffffa07.w
    bset #0,0xfffffa13.w
    move.b #8,0xfffffa1b.w
    move.l (%sp)+,%d0
    rte
raster:
    move.w #0x2700,%sr
    clr.b 0xfffffa1b.w
    moveq #-1,%d7
    move.w #0x8240,%d7
    movea.l %d7,%a2
    movea.l %d7,%a3
    .rept 16
    nop
    .endr
    .rept 8
    move.l (%a0)+,(%a2)+
    .endr
    .rept 8
    move.l (%a0)+,(%a3)+
    .endr
    .rept 198
    movea.l %d7,%a1
    movea.l %d7,%a2
    movea.l %d7,%a3
    .rept 5
    nop
    .endr
    .rept 8
    move.l (%a0)+,(%a1)+
    .endr
    .rept 8
    move.l (%a0)+,(%a2)+
    .endr
    .rept 8
    move.l (%a0)+,(%a3)+
    .endr
    .endr
    clr.w 0xffff8240.w
    bclr #0,0xfffffa0f.w
    rte
state:
    .space 72
required_ste:
    .word 0
picture:

