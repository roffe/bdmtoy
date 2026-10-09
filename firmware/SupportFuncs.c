#include "common.h"
#include "stm32f1xx_ll_tim.h"
#include "stm32f1xx_ll_usart.h"

static volatile uint32_t m_timeout;

#define DEBUGUART  USART2

void uart_putchar(char ch, FILE *f) {
    while (!LL_USART_IsActiveFlag_TXE(DEBUGUART))  ;
    LL_USART_TransmitData8(DEBUGUART, (uint8_t) ch);
}

char uart_getchar() {
    while (!LL_USART_IsActiveFlag_RXNE(DEBUGUART))  ;
    return (char) LL_USART_ReceiveData8(DEBUGUART);
}

// newlib's stdout (printf under DEBUGPRINT) goes to the debug UART
int _write(int fd, const char *ptr, int len) {
    for (int i = 0; i < len; i++) {
        if (ptr[i] == '\n')
            uart_putchar('\r', 0);
        uart_putchar(ptr[i], 0);
    }
    return len;
}

// dir: 0 input floating, 1 output push-pull, 2 input pull-down, 3 input
// pull-up, 4 output open drain; outputs at 50 MHz. Written straight into
// CRL/CRH: the turbo loops (BDMo_low.s) rewrite CRH the same way.
void SetPinDir(const uint32_t port, const uint16_t pin, const uint8_t dir)
{
    static const uint8_t cnfmode[] = { 0x4, 0x3, 0x8, 0x8, 0x7 };
    GPIO_TypeDef *gpio = (GPIO_TypeDef *) (GPIOA_BASE + (port*0x400));
    volatile uint32_t *cr = (pin < 8) ? &gpio->CRL : &gpio->CRH;
    const uint32_t shift = (pin & 7) * 4;

    *cr = (*cr & ~(0xFu << shift)) | ((uint32_t) cnfmode[dir <= 4 ? dir : 0] << shift);

    // Pull-down / pull-up are the output latch
    if      (dir == 2) gpio->BRR  = 1u << pin;
    else if (dir == 3) gpio->BSRR = 1u << pin;
}

void sleep(const uint16_t ms)
{
    set_Timeout(ms);
    while (!m_timeout) ;
}

// TIM2 counts down from ms at 1 kHz (48 MHz / 48001) and interrupts at zero
void set_Timeout(const uint16_t ms)
{
    LL_TIM_DisableCounter(TIM2);

    LL_TIM_SetCounterMode(TIM2, LL_TIM_COUNTERMODE_DOWN);
    LL_TIM_SetClockDivision(TIM2, LL_TIM_CLOCKDIVISION_DIV1);
    LL_TIM_SetPrescaler(TIM2, 48000);
    LL_TIM_SetAutoReload(TIM2, ms - 1);
    LL_TIM_GenerateEvent_UPDATE(TIM2); // load prescaler and count now

    // That UG must not count as the timeout: the update IRQ is set to fire on
    // counter underflow only (init_Timeout), and anything pending is dropped
    LL_TIM_ClearFlag_UPDATE(TIM2);
    NVIC_ClearPendingIRQ(TIM2_IRQn);

    m_timeout = 0;
    LL_TIM_EnableCounter(TIM2);
}

uint32_t get_Timeout()
{
    return m_timeout;
}

/*
void disable_Timeout()
{
    TIM_Cmd(TIM2,DISABLE);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}*/

void TIM2_IRQHandler(void)
{
    LL_TIM_DisableCounter(TIM2);
    LL_TIM_ClearFlag_UPDATE(TIM2);
    m_timeout = 1;
}

/*
void PrintIDdata(char *text, uint32_t ID) {
    printf(text);
    printf("%04X%04X\n\r", (uint16_t)( ID >> 16), (uint16_t)ID);
    printf("Rev:  %02X  ", (uint16_t)( ID >> 28)       );
    printf("DC :  %02X\n", (uint16_t)((ID >> 22)&0x03F));
    printf("PID: %03X  " , (uint16_t)((ID >> 12)&0x3FF));
    printf("MID: %03X\n\n",(uint16_t)((ID >>  1)&0x7FF));
}

// Byteswap
uint32_t SWAP(uint32_t in)
{   return ((in & 0xFF) << 24 | ((in >> 8) & 0xFF) << 16 | ((in >> 16) & 0xFF) << 8 | ((in >> 24) & 0xFF)); }

// Reverse bit-order
uint32_t REVERSE(uint32_t in) {
    uint32_t tmp = 0;
    uint8_t i;
    for (i = 0; i < 32; i++)
        tmp |= ((in >> i)&1) << (31 - i);
    return tmp;
}

void print16bit(uint16_t data) {
    printf("Data:");
    uint8_t i;
    for (i = 0; i < 16; i++) {
        if (!(i&0x7)) printf(" %u", (data >> (15 - i))&1);
        else          printf("%u",  (data >> (15 - i))&1);
    }
    printf("  (%04X)\n\r", data);
}

void printf32(char *text, uint32_t data) {
    printf(text);
    printf(" 0x%04X%04X\n\r", (uint16_t)(data>>16), (uint16_t)data);
}
*/
