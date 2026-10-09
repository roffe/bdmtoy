// TinyUSB configuration for the bootloader: USB DFU 1.1 (DFU mode only).
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU             OPT_MCU_STM32F1
#define CFG_TUSB_OS              OPT_OS_NONE
#define CFG_TUSB_DEBUG           0
#define CFG_TUD_ENABLED          1
#define CFG_TUD_MAX_SPEED        OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_ALIGN       __attribute__ ((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE   64

#define CFG_TUD_DFU              1
// One block is one flash page
#define CFG_TUD_DFU_XFER_BUFSIZE 1024

#endif
