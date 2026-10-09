// Firmware update over the adapter's USB DFU bootloader (firmware 2.0+).
// Used by the CLI and the Linux GUI; needs libusb-1.0.
#ifndef TOY_UPDATE_H
#define TOY_UPDATE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Write image (an app .bin, linked for 0x08004000) to the adapter. A running
// app is rebooted into the bootloader first; one already in the bootloader
// (ffff:0108) is used as it is. Afterwards the new app is started and asked
// for its version. msg gets progress text, prog the percentage done; either
// may be NULL. Returns 0 on success.
int toy_update(const uint8_t *image, size_t len,
               void (*msg)(const char *text), void (*prog)(int percent));

#ifdef __cplusplus
}
#endif
#endif
