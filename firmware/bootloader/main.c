// bdmtoy bootloader: USB DFU 1.1 in the first 16 KB of flash.
//
// Starts the app unless the app asked for the bootloader (TAP_DO_BOOTLOADER),
// the BOOT1 jumper is at 1, or there is no valid app. Then it enumerates as
// ffff:0108 and takes the app image, a .bin linked for 0x08004000, block by
// block (dfu-util -d ffff:0108 -D firmware.bin -R). A USB reset or a
// DFU_DETACH after the download starts the new app.
//
// The app's first page holds its vector table and is written last, at the end
// of the download: an update that breaks off leaves no valid app, so the
// device comes back up in the bootloader rather than in half an image.
#include <string.h>
#include "board.h"
#include "tusb.h"
#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_gpio.h"

void USB_HP_CAN1_TX_IRQHandler(void)  { tud_int_handler(0); }
void USB_LP_CAN1_RX0_IRQHandler(void) { tud_int_handler(0); }
void USBWakeUp_IRQHandler(void)       { tud_int_handler(0); }

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Boot decision

static int app_valid(void)
{
    const uint32_t *vec = (const uint32_t *) BOARD_APP_BASE;
    return vec[0] > BOARD_RAM_BASE && vec[0] <= BOARD_RAM_END &&
           vec[1] >= BOARD_APP_BASE && vec[1] < BOARD_APP_END && (vec[1] & 1);
}

// Straight from reset, nothing set up: the app starts as it would on its own
static void __attribute__((noreturn)) start_app(void)
{
    const uint32_t *vec = (const uint32_t *) BOARD_APP_BASE;
    SCB->VTOR = BOARD_APP_BASE;
    __asm volatile ("msr msp, %0\n\tbx %1" :: "r" (vec[0]), "r" (vec[1]));
    while (1)   ;
}

// The BOOT1 jumper (PB2) at 1: stay, whatever the app
static int boot1_set(void)
{
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    for (volatile int i = 0; i < 100; i++)   ; // PB2 is a floating input out of reset
    const int set = LL_GPIO_IsInputPinSet(GPIOB, LL_GPIO_PIN_2);
    LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_GPIOB);
    return set;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Flash

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK)
    {
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;
    }
}

static int flash_done(void)
{
    while (FLASH->SR & FLASH_SR_BSY)   ;
    const int ok = !(FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR));
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return ok;
}

static int flash_erase(uint32_t addr)
{
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR  = addr;
    FLASH->CR |= FLASH_CR_STRT;
    const int ok = flash_done();
    FLASH->CR &= ~FLASH_CR_PER;

    for (uint32_t i = 0; ok && i < BOARD_FLASH_PAGE; i += 4)
        if (*(const volatile uint32_t *) (addr + i) != 0xFFFFFFFF)
            return 0;
    return ok;
}

// Half-words, each read back
static int flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    int ok = 1;

    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0; ok && i < len; i += 2)
    {
        const uint16_t hw = (uint16_t) (data[i] | ((i + 1 < len ? data[i + 1] : 0xFF) << 8));
        *(volatile uint16_t *) (addr + i) = hw;
        ok = flash_done() && *(const volatile uint16_t *) (addr + i) == hw;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    return ok;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// DFU

static uint8_t  page0[BOARD_FLASH_PAGE]; // the app's vector page, written last
static int      have_page0;
static volatile int start_on_reset;   // a complete image is in: run it after the next USB reset

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
    (void) alt;
    (void) state;
    return 50; // a page erase and write take about 40 ms
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, const uint8_t *data, uint16_t length)
{
    (void) alt;
    const uint32_t addr = BOARD_APP_BASE + (uint32_t) block_num * CFG_TUD_DFU_XFER_BUFSIZE;

    if (addr + length > BOARD_APP_END)
    {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_ADDRESS);
        return;
    }

    flash_unlock();

    // The first page: keep it for the end, and erase the old one now, so the
    // old app is gone and an interrupted update leaves no valid one
    if (addr < BOARD_APP_BASE + BOARD_FLASH_PAGE)
    {
        if (block_num == 0)
        {
            memset(page0, 0xFF, sizeof page0);
            have_page0 = 1;
            start_on_reset = 0;
            if (!flash_erase(BOARD_APP_BASE))
            {
                tud_dfu_finish_flashing(DFU_STATUS_ERR_ERASE);
                return;
            }
        }
        memcpy(&page0[addr - BOARD_APP_BASE], data, length);
        tud_dfu_finish_flashing(DFU_STATUS_OK);
        return;
    }

    if (addr % BOARD_FLASH_PAGE == 0 && !flash_erase(addr))
    {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_ERASE);
        return;
    }
    tud_dfu_finish_flashing(flash_write(addr, data, length) ? DFU_STATUS_OK : DFU_STATUS_ERR_VERIFY);
}

void tud_dfu_manifest_cb(uint8_t alt)
{
    (void) alt;

    flash_unlock();
    if (!have_page0 || !flash_write(BOARD_APP_BASE, page0, sizeof page0))
    {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_VERIFY);
        return;
    }
    have_page0 = 0;

    if (!app_valid())
    {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_FIRMWARE);
        return;
    }
    start_on_reset = 1;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_abort_cb(uint8_t alt)
{
    (void) alt;
    have_page0 = 0;
}

// A USB reset after a complete download starts the new app (dfu-util -R).
// TinyUSB reports no bus reset as such; the host configuring the device
// again right after it does (the reset itself is set up in main()).
static volatile int start_app_now;

void tud_mount_cb(void)
{
    if (start_on_reset)
        start_app_now = 1;
}

// DFU_DETACH after a complete download does the same: not every host can
// reset the port (libusb on Windows/WinUSB only resets the pipes)
void tud_dfu_detach_cb(void)
{
    if (start_on_reset)
        start_app_now = 1;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

int main(void)
{
    // Decided before anything is set up
    const int requested = BOARD_BOOT_MAGIC == BOARD_BOOT_REQUEST;
    BOARD_BOOT_MAGIC = 0;

    if (!requested && !boot1_set() && app_valid())
        start_app();

    board_clock_init();
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USB);

    // The Blue Pill LED (PC13, active low) says "bootloader"
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOC);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_13, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_ResetOutputPin(GPIOC, LL_GPIO_PIN_13);

    board_usb_reconnect();
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    tusb_init(0, &dev_init);

    while (1)
    {
        tud_task();

        if (start_app_now)
        {
            // Let the SET_CONFIGURATION that got us here finish first
            for (volatile uint32_t i = 0; i < 48000000 / 50 / 4; i++)
                tud_task();
            NVIC_SystemReset();
        }
    }
}
