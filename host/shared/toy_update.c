// Firmware update over the adapter's USB DFU bootloader. See toy_update.h and
// firmware/README.md.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <libusb.h>

#include "toy_update.h"
#include "../../shared/enums.h"

#define TOY_VID        0xFFFF
#define TOY_PID_APP    0x0107
#define TOY_PID_BOOT   0x0108
#define TOY_BOOT_FW    0x0200 // first firmware with the bootloader

#define EP_OUT         0x03
#define EP_IN          0x81

#define DFU_BLOCK      1024   // the bootloader's wTransferSize: one flash page
#define DFU_DETACH     0
#define DFU_DNLOAD     1
#define DFU_GETSTATUS  3
#define DFU_CLRSTATUS  4
#define DFU_ABORT      6
#define DFU_IDLE       2
#define DFU_DNLOAD_IDLE 5
#define DFU_ERROR      10

static void (*msg_cb)(const char *);

static void say(const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (msg_cb)
        msg_cb(text);
}

static void pause_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static libusb_device_handle *wait_open(libusb_context *ctx, uint16_t pid, unsigned ms)
{
    for (unsigned waited = 0; waited <= ms; waited += 200)
    {
        libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, TOY_VID, pid);
        if (h)
            return h;
        pause_ms(200);
    }
    return NULL;
}

// Interface 0; the app's interface 1 is only there on the CDC layout of
// firmware before 2.2, which still has to be told to update
static int claim(libusb_device_handle *h, int interfaces)
{
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0) != 0)
        return -1;
    for (int i = 1; i < interfaces; i++)
        libusb_claim_interface(h, i);
    return 0;
}

// One TAP command without arguments on the app's link: [4][1][cmd][2] out,
// [total][cmd][status][3 + n][data] back. Returns the status, the first data
// word in *data.
static int app_command(libusb_device_handle *h, uint16_t cmd, uint16_t *data)
{
    uint8_t out[8] = { 4, 0, 1, 0, (uint8_t) cmd, (uint8_t) (cmd >> 8), 2, 0 };
    uint8_t in[64];
    int n;

    if (libusb_bulk_transfer(h, EP_OUT, out, sizeof out, &n, 2000) != 0)
        return -1;
    if (libusb_bulk_transfer(h, EP_IN, in, sizeof in, &n, 2000) != 0 || n < 8)
        return -1;

    const int status = in[4] | in[5] << 8;
    if (data && n >= 10)
        *data = (uint16_t) (in[8] | in[9] << 8);
    return status;
}

// Ask the running app for its version, then for the bootloader
static int enter_bootloader(libusb_context *ctx)
{
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, TOY_VID, TOY_PID_APP);
    if (!h)
    {
        say("No adapter found (neither ffff:0107 nor the bootloader, ffff:0108)");
        return -1;
    }

    int ret = -1;
    uint16_t version = 0;

    // A USB reset puts the firmware's frame parser on a frame boundary
    libusb_reset_device(h);
    if (claim(h, 2) != 0)
        say("Cannot claim the adapter (is another program using it?)");
    else if (app_command(h, TAP_DO_VERSION, &version) != RET_OK || version < TOY_BOOT_FW)
        say("The adapter firmware has no USB bootloader (it needs 2.0 or later): flash bdmtoy-full.hex over SWD once, see firmware/README.md");
    else
    {
        say("Adapter firmware v%u.%u, rebooting it into the bootloader", version >> 8, version & 0xFF);
        ret = app_command(h, TAP_DO_BOOTLOADER, NULL) == RET_OK ? 0 : -1;
        if (ret)
            say("The adapter did not take the bootloader command");
    }

    libusb_release_interface(h, 0);
    libusb_release_interface(h, 1);
    libusb_close(h);
    return ret;
}

static int dfu_status(libusb_device_handle *h, uint8_t *status, unsigned *poll, uint8_t *state)
{
    uint8_t b[6];
    if (libusb_control_transfer(h, 0xA1, DFU_GETSTATUS, 0, 0, b, sizeof b, 1000) != 6)
        return -1;
    *status = b[0];
    *poll   = b[1] | b[2] << 8 | b[3] << 16;
    *state  = b[4];
    return 0;
}

// Get the bootloader to dfuIDLE: clear an error or abort a broken-off download
static int dfu_idle(libusb_device_handle *h)
{
    for (int i = 0; i < 3; i++)
    {
        uint8_t status, state;
        unsigned poll;
        if (dfu_status(h, &status, &poll, &state))
            return -1;
        if (state == DFU_IDLE)
            return 0;
        libusb_control_transfer(h, 0x21, state == DFU_ERROR ? DFU_CLRSTATUS : DFU_ABORT, 0, 0, NULL, 0, 1000);
    }
    return -1;
}

// One block (len 0: the end of the download), then poll until it is dealt with
static int dfu_download(libusb_device_handle *h, uint16_t block, const uint8_t *data, uint16_t len)
{
    if (libusb_control_transfer(h, 0x21, DFU_DNLOAD, block, 0, (uint8_t *) data, len, 2000) != len)
        return -1;

    const uint8_t want = len ? DFU_DNLOAD_IDLE : DFU_IDLE;
    for (int tries = 0; tries < 500; tries++)
    {
        uint8_t status, state;
        unsigned poll;
        if (dfu_status(h, &status, &poll, &state))
            return -1;
        if (status != 0 || state == DFU_ERROR)
        {
            say("The bootloader reports DFU status %u", status);
            return -1;
        }
        if (state == want)
            return 0;
        pause_ms(poll > 5 ? poll : 5);
    }
    return -1;
}

int toy_update(const uint8_t *image, size_t len,
               void (*msg)(const char *), void (*prog)(int))
{
    libusb_context *ctx = NULL;
    libusb_device_handle *h;
    int ret = -1;

    msg_cb = msg;

    if (len < 8 || len > 0xC000)
    {
        say("That is not an app image (%u bytes): use firmware/bin/firmware.bin", (unsigned) len);
        return -1;
    }
    if (libusb_init(&ctx) != 0)
    {
        say("Could not initialise libusb");
        return -1;
    }

    h = libusb_open_device_with_vid_pid(ctx, TOY_VID, TOY_PID_BOOT);
    if (!h)
    {
        if (enter_bootloader(ctx))
            goto out;
        h = wait_open(ctx, TOY_PID_BOOT, 5000);
        if (!h)
        {
            say("The bootloader (ffff:0108) did not show up (on Linux it needs its own udev rule, see firmware/README.md)");
            goto out;
        }
    }

    if (claim(h, 1) != 0 || dfu_idle(h) != 0)
    {
        say("The bootloader does not respond");
        goto close;
    }

    say("Bootloader ready, writing %u bytes", (unsigned) len);
    for (size_t off = 0; off < len; off += DFU_BLOCK)
    {
        const uint16_t n = (uint16_t) (len - off < DFU_BLOCK ? len - off : DFU_BLOCK);
        if (dfu_download(h, (uint16_t) (off / DFU_BLOCK), &image[off], n))
        {
            say("Writing the block at 0x%05X failed", (unsigned) off);
            goto close;
        }
        if (prog)
            prog((int) ((off + n) * 100 / len));
    }

    // The empty block ends the download: the bootloader writes the vector
    // page (manifest), and a DETACH or a USB reset then starts the new app
    if (dfu_download(h, (uint16_t) ((len + DFU_BLOCK - 1) / DFU_BLOCK), NULL, 0))
    {
        say("Finishing the download failed");
        goto close;
    }
    // Both, since WinUSB cannot reset the port; the device leaves either way
    libusb_control_transfer(h, 0x21, DFU_DETACH, 1000, 0, NULL, 0, 1000);
    libusb_release_interface(h, 0);
    libusb_reset_device(h);
    libusb_close(h);

    h = wait_open(ctx, TOY_PID_APP, 8000);
    if (!h)
    {
        say("Firmware written, but the adapter did not come back");
        goto out;
    }

    uint16_t version = 0;
    libusb_reset_device(h);
    if (claim(h, 2) == 0 && app_command(h, TAP_DO_VERSION, &version) == RET_OK)
    {
        say("Adapter firmware v%u.%u running", version >> 8, version & 0xFF);
        ret = 0;
    }
    else
        say("Firmware written, but the adapter does not answer");
    libusb_release_interface(h, 0);
    libusb_release_interface(h, 1);

close:
    libusb_close(h);
out:
    libusb_exit(ctx);
    return ret;
}
