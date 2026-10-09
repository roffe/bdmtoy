// TinyUSB configuration for the app: one vendor-specific interface, the
// adapter's link.
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#include <stdint.h>
#include "../../shared/cmddesc.h"

#define CFG_TUSB_MCU             OPT_MCU_STM32F1
#define CFG_TUSB_OS              OPT_OS_NONE
#define CFG_TUSB_DEBUG           0
#define CFG_TUD_ENABLED          1
#define CFG_TUD_MAX_SPEED        OPT_MODE_FULL_SPEED
#define CFG_TUSB_MEM_ALIGN       __attribute__ ((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE   64

#define CFG_TUD_VENDOR           1

// A frame is always answered before the next is sent, so the receive FIFO
// holds one; when it is full the OUT endpoint is not re-armed and the host
// is held off (NAK).
#define CFG_TUD_VENDOR_RX_BUFSIZE (ADAPTER_BUFzIN + 64)
#define CFG_TUD_VENDOR_RX_EPSIZE  64

// A reply frame goes out as one transfer, ended by a zero-length packet when
// it fills its last packet: the hosts read frames from the start of a USB
// packet, and the interrupt handler packetises a transfer on its own while
// the main loop gets on with the next dump block.
#define CFG_TUD_VENDOR_TX_BUFSIZE (ADAPTER_BUFzOUT + 64)
#define CFG_TUD_VENDOR_TX_EPSIZE  (ADAPTER_BUFzOUT + 64)

#endif
