/* The app, after the 16 KB bootloader (board.h: BOARD_APP_BASE) */
MEMORY {
    FLASH (rx)  : ORIGIN = 0x08004000, LENGTH = 48K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K - 4
}
INCLUDE link_common.ld
