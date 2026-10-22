/* SPDX-License-Identifier: GPL-3.0-only */
/* A harbor probe (firmware/harbor/hb_module.h, "Probes"), linked as a module is (tools/hbmod.py:
 * twice, at two bases, for its relocations).
 *   XIP   the image at BASE: the descriptor first, code and constants, then the load images
 *         of .data and .ram_text (the host runs all of it from RAM, where it received it)
 *   MRAM  .data + .bss at HB_PROBE_RAM, above the received file
 *   MRT   .ram_text, as a module's */
MEMORY
{
    XIP  (rx)  : ORIGIN = @BASE@, LENGTH = 0x30000
    MRT  (rwx) : ORIGIN = 0x01C04000, LENGTH = 0x4000
    MRAM (rw)  : ORIGIN = 0x01C60000, LENGTH = 0x14000
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
