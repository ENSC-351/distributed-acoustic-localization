/* r5capture: per-frame variables in the R5's ATCM (fast internal memory), at a
 * fixed address clear of the boot springboard (0x0-0x4c) and the exception
 * vectors Zephyr copies to 0x0 at boot (0x0-0x3c). Not loaded from the ELF:
 * zephyr_app/src/fastmem.c zeroes it at boot. (BTCM at 0x41010000 is not
 * accessible from this core: reads gave a synchronous external abort.) */
.r5fast 0x1000 (NOLOAD) :
{
    __r5fast_start = .;
    KEEP(*(.r5fast))
    __r5fast_end = .;
}
