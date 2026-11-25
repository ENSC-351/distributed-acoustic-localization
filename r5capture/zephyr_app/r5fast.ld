/* r5capture: sampling code, per-frame variables and the capture thread's stack in the
 * R5's fast internal memory (TCM, 32 KB at address 0), clear of the boot springboard
 * and the exception vectors Zephyr copies to 0x0 at boot (both below 0x100).
 * .r5fast_text is loaded from the ELF by remoteproc; .r5fast isn't, and
 * zephyr_app/src/fastmem.c zeroes it at boot.
 * The board file calls this memory ATCM, but Linux's device tree for this core has
 * atcm-enable = 0, btcm-enable = 1, loczrama = 0: it is physically the BTCM, and the
 * memory at 0x41010000 is disabled (a synchronous external abort). */
.r5fast_text 0x200 : AT(0x200)
{
    __r5fast_text_start = .;
    KEEP(*(.r5fast_text*))
    __r5fast_text_end = .;
}
ASSERT(__r5fast_text_end <= 0x1000, "fast code runs into the fast data at 0x1000")

.r5fast 0x1000 (NOLOAD) :
{
    __r5fast_start = .;
    KEEP(*(.r5fast))
    __r5fast_end = .;
}
ASSERT(__r5fast_end <= 0x8000, "fast data does not fit in the 32 KB TCM")
