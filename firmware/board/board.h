// Shared by the app and the bootloader: flash layout, clocks, USB start-up.
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "stm32f1xx.h"

// 64 KB of flash, 1 KB pages: the bootloader in the first 16 KB, the app
// after it. The images are linked for these addresses (link_boot.ld,
// link_app.ld).
#define BOARD_FLASH_PAGE   1024u
#define BOARD_BOOT_BASE    0x08000000u
#define BOARD_APP_BASE     0x08004000u
#define BOARD_APP_END      0x08010000u

// The last word of RAM is kept out of both images. The app writes the magic
// there and resets to make the bootloader stay; the bootloader clears it.
#define BOARD_RAM_BASE     0x20000000u
#define BOARD_RAM_END      0x20005000u
#define BOARD_BOOT_MAGIC   (*(volatile uint32_t *) (BOARD_RAM_END - 4))
#define BOARD_BOOT_REQUEST 0xB0071040u

// USB IDs. The bootloader has its own, so a tool never takes one for the other.
#define BOARD_USB_VID      0xFFFF
#define BOARD_USB_PID_APP  0x0107
#define BOARD_USB_PID_BOOT 0x0108

// 8 MHz crystal, PLL x6: 48 MHz core and USB, APB1 24 MHz (SPI2, TIM2 x2 =
// 48 MHz), APB2 48 MHz.
void board_clock_init(void);

// Hold D+ low for a moment before the USB peripheral takes the pins. The
// pull-up on D+ is fixed, so this is the only way the host notices that the
// device restarted (after a reset into or out of the bootloader).
void board_usb_reconnect(void);

// Reset into the bootloader and stay there.
void board_reboot_to_bootloader(void);

#endif
