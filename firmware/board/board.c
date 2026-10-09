#include "board.h"
#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_gpio.h"
#include "stm32f1xx_ll_rcc.h"
#include "stm32f1xx_ll_system.h"
#include "stm32f1xx_ll_utils.h"

void board_clock_init(void)
{
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
    LL_FLASH_EnablePrefetch();

    LL_RCC_HSE_Enable();
    while (!LL_RCC_HSE_IsReady())   ;

    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSE_DIV_1, LL_RCC_PLL_MUL_6);
    LL_RCC_PLL_Enable();
    while (!LL_RCC_PLL_IsReady())   ;

    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_2);
    LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL)   ;

    // USB at PLL / 1 = 48 MHz
    LL_RCC_SetUSBClockSource(LL_RCC_USB_CLKSOURCE_PLL);

    LL_SetSystemCoreClock(48000000);
}

void board_usb_reconnect(void)
{
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode      (GPIOA, LL_GPIO_PIN_12, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinOutputType(GPIOA, LL_GPIO_PIN_12, LL_GPIO_OUTPUT_PUSHPULL);
    LL_GPIO_ResetOutputPin  (GPIOA, LL_GPIO_PIN_12);

    // ~10 ms at 48 MHz; the host needs 2.5 us to see a disconnect, give it plenty
    for (volatile uint32_t i = 0; i < 48000000 / 100 / 4; i++)   ;

    LL_GPIO_SetPinMode      (GPIOA, LL_GPIO_PIN_12, LL_GPIO_MODE_FLOATING);
}

void board_reboot_to_bootloader(void)
{
    BOARD_BOOT_MAGIC = BOARD_BOOT_REQUEST;
    NVIC_SystemReset();
}

// newlib's __libc_init_array calls it; the startup files that would bring it
// are not linked (-nostartfiles)
void _init(void) {}
