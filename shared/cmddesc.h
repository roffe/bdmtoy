/// command descriptors
#ifndef __CMDDESC_H__
#define __CMDDESC_H__

#define ADAPTER_BUFzIN     2048
#define ADAPTER_BUFzOUT    2048

/// Firmware version, major << 8 | minor, answered to TAP_DO_VERSION.
/// 1.0: version command, fixed old-BDM read/dump/fill handshake, address 0,
/// always-resetting TARGETREADY, BKPT-only TARGETSTOP, USB flow control.
/// 2.0: USB DFU bootloader (firmware updates over USB, TAP_DO_BOOTLOADER),
/// TinyUSB, ST LL drivers and CMSIS 6 in place of StdPeriph and the legacy
/// USB library. The app lives at 0x08004000.
/// 2.1: DSCLK/BKPT pulled up while idle, from power-up on (a T7 powered up
/// with the adapter on it came up halted).
/// 2.2: vendor-specific USB interface (EP 0x03/0x81 as before) with Microsoft
/// OS 2.0 descriptors in place of CDC-ACM: WinUSB loads on its own on Windows,
/// and Linux binds no tty driver. Hosts claim interface 0 only.
#define ADAPTER_FW_VERSION 0x0202

#define GLOBALTIMEOUT         4

/// Number of 16-bit words in the header of each command
#define cmd_hdr_sz                         2 // All commands contains at least two words, size and command itself.

/// Size descriptors, TAP commands
#define TAP_ReadCMD_sz      (cmd_hdr_sz +  4) // Read/Dump data request
#define TAP_Config_sz       (cmd_hdr_sz +  4)
#define TAP_ReadReg_sz      (cmd_hdr_sz +  2)
#define TAP_WriteReg_sz     (cmd_hdr_sz +  3)
#define TAP_AssistCMD_sz    (cmd_hdr_sz + 10)
/// Auxiliary defines for TAP commands
#define MAX_RWLEN                        32 // Maximum number of bytes "Read data"/"Write data" request lets you read or write. If you need more use assisted functions.

/// TAP commands, structs

// Read/dump data request
typedef struct {
    uint32_t Address;
    uint32_t Length;
} TAP_ReadCMD_t;

// Byte - Dword write request
typedef struct {
    uint32_t Address;
    uint32_t Length;
    uint32_t Data;
} TAP_WriteCMD_t;

typedef struct {
    uint32_t Address;
    uint32_t Length;
    uint32_t DriverStart;
    uint32_t BufferStart;
    uint32_t BufferLen;
} TAP_AssistCMD_t;

/////////////////////////////////////////////////////////////////////
/// < dwords might get padded. Do _NOT_ use the pointer trick with these!

typedef struct {
    uint16_t   PAD:15;
    uint8_t Endian: 1; // Useless atm... 1: big, 0: small.
} cfgmask_host_t;

// TAP configuration
typedef struct {
    uint16_t       Type;
    uint32_t       Frequency;
    cfgmask_host_t cfgmask;
} TAP_Config_host_t;

typedef struct {
    uint32_t frequency;
    uint16_t polarity;
    uint16_t phase;
    uint16_t order;
    uint16_t size;
} spi_cfg_t;

#endif
