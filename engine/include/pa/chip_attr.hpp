#pragma once

// Platform-conditional chip-RAM placement attribute. The png2amiga-assets
// generator emits `PA_ASSET_CHIP` in front of its big const-data arrays; on
// a m68k-amiga-elf build this expands to the ELF section the vscode-amiga-debug
// template linker script maps into CHIP memory, and on host builds it's
// silently empty so the generated .cpp compiles as ordinary .rodata.
//
// Users bypassing the engine (e.g. a plain Makefile build) can define
// PA_ASSET_CHIP themselves before including any generated .cpp.

#if !defined(PA_ASSET_CHIP)
  #if defined(__mc68000__) || defined(__m68k__)
    #define PA_ASSET_CHIP __attribute__((section(".INCBIN.MEMF_CHIP")))
  #else
    #define PA_ASSET_CHIP
  #endif
#endif
