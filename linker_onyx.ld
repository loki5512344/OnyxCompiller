/*
 * linker_onyx.ld — linker script for OnyxCC running on OnyxOS.
 *
 * Matches what OnyxKernel's onx.rs::load() expects:
 *   - text starts at 0x10000
 *   - all segments in [USER_BASE, USER_TOP) = [0x10000, 0x40000000)
 *   - 16-byte stack alignment
 *   - RISC-V LP64
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

KERNEL_BASE = 0x00010000;

PHDRS {
    text PT_LOAD FLAGS(5);      /* R+X */
    rodata PT_LOAD FLAGS(4);    /* R   */
    data PT_LOAD FLAGS(6);      /* R+W */
}

SECTIONS {
    . = KERNEL_BASE;

    .text : ALIGN(4) {
        *(.text.start)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(8) {
        *(.rodata .srodata .rodata.* .srodata.*)
        *(.eh_frame .eh_frame.*)
    } :rodata

    .data : ALIGN(8) {
        *(.data .sdata .data.* .sdata.*)
    } :data

    . = ALIGN(8);
    __bss_start = .;
    .bss : ALIGN(8) {
        *(.bss .sbss .bss.* .sbss.*)
        *(COMMON)
    } :data
    . = ALIGN(8);
    __bss_end = .;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.riscv.attributes)
        *(.got .got.*)
    }
}
