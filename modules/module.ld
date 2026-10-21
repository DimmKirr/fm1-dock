/* SPDX-License-Identifier: GPL-3.0-only */
/* A harbor module (firmware/harbor/hb_module.h). tools/hbmod.py links it twice, with BASE set to
 * two addresses, and takes the words that differ as its relocations.
 *   XIP   the image at BASE: the descriptor first, code and constants, then the load images
 *         of .data and .ram_text
 *   MRAM  .data + .bss: the one RAM region every module shares (only one runs)
 *   MRT   .ram_text: inner loops away from the flash cache (called through HB_FAR) */
MEMORY
{
    XIP  (rx)  : ORIGIN = @BASE@, LENGTH = 0x80000
    MRT  (rwx) : ORIGIN = 0x01C04000, LENGTH = 0x4000
    MRAM (rw)  : ORIGIN = 0x01C30000, LENGTH = 0x44000
}
SECTIONS
{
    .text : { KEEP(*(.hb_desc)) *(.text*) *(.rodata*) . = ALIGN(4); _hb_text_end = .; } > XIP
    .data : { . = ALIGN(4); _hb_data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _hb_data_end = .; } > MRAM AT > XIP
    _hb_data_load = LOADADDR(.data);
    .ram_text : { . = ALIGN(4); _hb_rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); _hb_rt_end = .; } > MRT AT > XIP
    _hb_rt_load = LOADADDR(.ram_text);
    .bss (NOLOAD) : { . = ALIGN(4); *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _hb_bss_end = .; } > MRAM
    /DISCARD/ : { *(.comment) *(.note*) }
}
