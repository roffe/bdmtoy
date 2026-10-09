/* The bootloader, in the first 16 KB of flash (board.h: BOARD_BOOT_BASE) */
MEMORY {
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K - 4
}
INCLUDE link_common.ld
