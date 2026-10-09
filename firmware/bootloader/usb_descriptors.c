// USB descriptors of the bootloader: a DFU 1.1 device in DFU mode, ffff:0108,
// one interface, alt 0 = the app's flash. Microsoft OS 2.0 descriptors have
// Windows load WinUSB for it on its own, so dfu-util and the update tools
// work there without Zadig.
#include <string.h>
#include "tusb.h"
#include "board.h"

#define BOOT_VERSION        0x0100
#define VENDOR_REQUEST_MSFT 0x01

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0201, // 2.01: the host asks for the BOS descriptor
    .bDeviceClass       = 0,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = BOARD_USB_VID,
    .idProduct          = BOARD_USB_PID_BOOT,
    .bcdDevice          = BOOT_VERSION,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *) &desc_device;
}

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1))

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LEN, 0xC0, 100),
    // Interface 0, one alt setting, its name string 4. Download only; it
    // stays in DFU mode after the download (manifestation tolerant) and starts
    // the new app on the USB reset that follows.
    TUD_DFU_DESCRIPTOR(0, 1, 4, DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT,
                       1000, CFG_TUD_DFU_XFER_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return desc_config;
}

// BOS with the Microsoft OS 2.0 platform capability
#define MS_OS_20_DESC_LEN 0xA2

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MSFT),
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

// Device-level descriptor set (not composite): WinUSB, and the interface
// GUID applications find it by.
static const uint8_t desc_ms_os_20[] = {
    // Set header
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Compatible ID: WINUSB
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,

    // Registry property: DeviceInterfaceGUIDs, REG_MULTI_SZ
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0,
    'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '0', 0, 'B', 0, '2', 0, '7', 0, 'A', 0, 'F', 0, '8', 0, 'B', 0, '-', 0,
    '1', 0, 'C', 0, '9', 0, '9', 0, '-', 0, '4', 0, '5', 0, '6', 0, 'A', 0, '-', 0,
    'B', 0, '5', 0, 'C', 0, 'A', 0, '-', 0, '2', 0, '5', 0, 'D', 0, '7', 0, 'E', 0,
    '8', 0, '0', 0, '7', 0, 'B', 0, 'D', 0, 'A', 0, '5', 0, '}', 0, 0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MSFT && request->wIndex == 7)
        return tud_control_xfer(rhport, request, (void *) (uintptr_t) desc_ms_os_20, sizeof(desc_ms_os_20));

    return false;
}

// Strings: 0 language, 1 manufacturer, 2 product, 3 serial (the chip's
// unique ID, as the app reports it), 4 the DFU alt setting
static uint16_t desc_str[1 + 24];

static uint8_t str_ascii(const char *s)
{
    uint8_t n = 0;
    for (; s[n] && n < 24; n++)
        desc_str[1 + n] = (uint8_t) s[n];
    return n;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;
    static const char hex[] = "0123456789ABCDEF";
    uint8_t n;

    switch (index)
    {
        case 0:
            desc_str[1] = 0x0409;
            n = 1;
            break;
        case 1:
            n = str_ascii("TXSUITE.ORG");
            break;
        case 2:
            n = str_ascii("bdmtoy bootloader");
            break;
        case 3:
        {
            const uint8_t *uid = (const uint8_t *) UID_BASE;
            for (n = 0; n < 24; n++)
                desc_str[1 + n] = hex[(uid[n / 2] >> ((n & 1) ? 0 : 4)) & 15];
            break;
        }
        case 4:
            n = str_ascii("bdmtoy firmware");
            break;
        default:
            return NULL;
    }

    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
