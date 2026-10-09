#include "TAP/TAP_shared.h"
#include "board.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_dma.h"
#include "stm32f1xx_ll_gpio.h"
#include "stm32f1xx_ll_rcc.h"
#include "stm32f1xx_ll_spi.h"
#include "stm32f1xx_ll_tim.h"
#include "stm32f1xx_ll_usart.h"

#define EP_DATA_IN 0x81

volatile uint32_t usbrec = 0;
// One spare packet, as the frame parser below always reads into it whole
uint16_t receiveBuffer[(ADAPTER_BUFzIN + 64)/2];
uint16_t sendBuffer[(ADAPTER_BUFzOUT/2)+2];

static volatile uint32_t rebootRequest;

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// USB link (TinyUSB vendor class)

void USB_HP_CAN1_TX_IRQHandler(void)  { tud_int_handler(0); }
void USB_LP_CAN1_RX0_IRQHandler(void) { tud_int_handler(0); }
void USBWakeUp_IRQHandler(void)       { tud_int_handler(0); }

// The reply frame before has fully left, its zero-length packet included.
static void usb_txWait()
{
    while (tud_mounted() &&
           (tud_vendor_write_available() != CFG_TUD_VENDOR_TX_BUFSIZE || usbd_edpt_busy(0, EP_DATA_IN)))
        tud_task();
}

// Send one frame, [total length, words].., on EP 0x81. The hosts read frames
// from the start of a USB packet, so it waits for the frame before to be
// gone and goes out as one transfer, which TinyUSB ends with a zero-length
// packet when it fills its last packet. It returns once the frame is queued:
// the interrupt handler sends it while the caller prepares the next.
uint16_t usb_sendData(const void *buffer)
{
    const uint8_t *packet = (const uint8_t *) buffer;
    uint32_t       length = ((const uint16_t *) buffer)[0] * 2;

    usb_txWait();
    while (length && tud_mounted())
    {
        const uint32_t n = tud_vendor_write(packet, length);
        packet += n;
        length -= n;
        tud_vendor_write_flush();
        if (length)
            tud_task();
    }

    return RET_OK;
}

// Frame reception: the first two bytes give the length in words; the rest is
// read from the receive FIFO as it comes. A frame is handed over through usbrec.
static uint32_t rxGot; // bytes of the frame so far
static uint32_t rxLen; // its length, bytes

static void usb_rxReset()
{
    rxGot = rxLen = 0;
}

// TinyUSB empties its FIFOs on a bus reset; start on a frame boundary with them
void tud_mount_cb(void)  { usb_rxReset(); }
void tud_umount_cb(void) { usb_rxReset(); }

// Run USB and assemble a frame. Returns usbrec: the length of a complete frame
// in receiveBuffer, 0 while there is none.
uint32_t usb_poll()
{
    tud_task();

    if (usbrec || !tud_vendor_available())
        return usbrec;

    uint8_t *rx = (uint8_t *) receiveBuffer;

    if (rxGot < 2)
        rxGot += tud_vendor_read(&rx[rxGot], 2 - rxGot);
    if (rxGot < 2)
        return 0;

    if (!rxLen)
    {
        rxLen = receiveBuffer[0] * 2;
        // Not a frame (garbage, or a confused host): drop
        // what is there and wait for the host to start over.
        if (rxLen < 8 || rxLen > ADAPTER_BUFzIN)
        {
            tud_vendor_read_flush();
            usb_rxReset();
            return 0;
        }
    }

    rxGot += tud_vendor_read(&rx[rxGot], rxLen - rxGot);
    if (rxGot < rxLen)
        return 0;

    usbrec = rxLen;
    usb_rxReset();
    return usbrec;
}

// Done with the frame in receiveBuffer
void usb_rxRelease()
{
    usbrec = 0;
}

// TAP_DO_BOOTLOADER: reset into the bootloader once the reply has gone out
void usb_requestBootloader()
{
    rebootRequest = 1;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// SPI2 (BDM, BDM new) and its DMA channels

static void SPI_PreinitDMA()
{
    LL_SPI_DisableDMAReq_RX(SPI2);
    LL_SPI_DisableDMAReq_TX(SPI2);

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

    LL_DMA_InitTypeDef dma;
    LL_DMA_StructInit(&dma);
    dma.PeriphOrM2MSrcAddress  = (uint32_t)&SPI2->DR;
    dma.NbData                 = 0;
    dma.PeriphOrM2MSrcIncMode  = LL_DMA_PERIPH_NOINCREMENT;
    dma.MemoryOrM2MDstIncMode  = LL_DMA_MEMORY_INCREMENT;
    dma.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_HALFWORD;
    dma.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_HALFWORD;
    dma.Mode                   = LL_DMA_MODE_NORMAL;

    // Rx
    dma.Direction              = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    dma.Priority               = LL_DMA_PRIORITY_HIGH;
    LL_DMA_DeInit(DMA1, LL_DMA_CHANNEL_4);
    LL_DMA_Init(DMA1, LL_DMA_CHANNEL_4, &dma);

    // Tx
    dma.Direction              = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    dma.Priority               = LL_DMA_PRIORITY_VERYHIGH;
    LL_DMA_DeInit(DMA1, LL_DMA_CHANNEL_5);
    LL_DMA_Init(DMA1, LL_DMA_CHANNEL_5, &dma);

    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_4);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_5);
    LL_DMA_ClearFlag_TC4(DMA1);
    LL_DMA_ClearFlag_TC5(DMA1);

    LL_SPI_EnableDMAReq_RX(SPI2);
    LL_SPI_EnableDMAReq_TX(SPI2);
}

// SPI2 is on APB1, 24 MHz: the clock is the fastest prescaler at or below
// the frequency asked for.
void InitSPI(const spi_cfg_t *cfg)
{
    static const uint32_t prescalers[] = {
        LL_SPI_BAUDRATEPRESCALER_DIV2,  LL_SPI_BAUDRATEPRESCALER_DIV4,
        LL_SPI_BAUDRATEPRESCALER_DIV8,  LL_SPI_BAUDRATEPRESCALER_DIV16,
        LL_SPI_BAUDRATEPRESCALER_DIV32, LL_SPI_BAUDRATEPRESCALER_DIV64,
        LL_SPI_BAUDRATEPRESCALER_DIV128, LL_SPI_BAUDRATEPRESCALER_DIV256,
    };
    uint32_t div = 0;
    while (div < 7 && cfg->frequency < (24000000u >> (div + 1)))
        div++;

    LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_SPI2);
    LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_SPI2);

    LL_SPI_InitTypeDef spi;
    LL_SPI_StructInit(&spi);
    spi.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi.Mode              = LL_SPI_MODE_MASTER;
    spi.NSS               = LL_SPI_NSS_SOFT;
    spi.BitOrder          = cfg->order    ? LL_SPI_MSB_FIRST        : LL_SPI_LSB_FIRST;
    spi.DataWidth         = cfg->size     ? LL_SPI_DATAWIDTH_16BIT  : LL_SPI_DATAWIDTH_8BIT;
    spi.ClockPolarity     = cfg->polarity ? LL_SPI_POLARITY_HIGH    : LL_SPI_POLARITY_LOW;
    spi.ClockPhase        = cfg->phase    ? LL_SPI_PHASE_2EDGE      : LL_SPI_PHASE_1EDGE;
    spi.BaudRate          = prescalers[div];
    spi.CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE;
    LL_SPI_Init(SPI2, &spi);
    LL_SPI_Enable(SPI2);

#ifdef DEBUGPRINT
    printf("Requested freq: %lu Hz, SPI clock 24 MHz / %u\n\r", (unsigned long) cfg->frequency, 2u << div);
#endif

    SPI_PreinitDMA();
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// System

// TIM2 counts the 1 ms ticks of set_Timeout() (SupportFuncs.c)
static void init_Timeout()
{
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
    LL_TIM_DisableCounter(TIM2);

    // Only a real counter under/overflow may raise the update IRQ, not the UG
    // prescaler-reload that set_Timeout() issues each call. Without this, that
    // UG spuriously fires the timeout and sleeps/waits can return instantly.
    LL_TIM_SetUpdateSource(TIM2, LL_TIM_UPDATESOURCE_COUNTER);
    LL_TIM_ClearFlag_UPDATE(TIM2);
    LL_TIM_EnableIT_UPDATE(TIM2);

    NVIC_SetPriority(TIM2_IRQn, 2);
    NVIC_EnableIRQ(TIM2_IRQn);
}

// USART2 on PA2/PA3, 115200 8N1: debug output (DEBUGPRINT)
static void init_debugUart()
{
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_3, LL_GPIO_MODE_FLOATING);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_2, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinOutputType(GPIOA, LL_GPIO_PIN_2, LL_GPIO_OUTPUT_PUSHPULL);

    LL_USART_InitTypeDef usart;
    LL_USART_StructInit(&usart);
    usart.BaudRate = 115200;
    LL_USART_Init(USART2, &usart);
    LL_USART_Enable(USART2);
}

static void RCC_Configuration()
{
    // As the StdPeriph version had it, plus USB
    LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_AFIO | LL_APB2_GRP1_PERIPH_GPIOA |
                             LL_APB2_GRP1_PERIPH_GPIOB | LL_APB2_GRP1_PERIPH_GPIOC |
                             LL_APB2_GRP1_PERIPH_SPI1  | LL_APB2_GRP1_PERIPH_USART1);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2 | LL_APB1_GRP1_PERIPH_TIM4 |
                             LL_APB1_GRP1_PERIPH_SPI2 | LL_APB1_GRP1_PERIPH_USART2 |
                             LL_APB1_GRP1_PERIPH_USB);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
}

static void InitSys()
{
    // Linked after the bootloader: the vectors are ours from here on
    SCB->VTOR = BOARD_APP_BASE;

    board_clock_init();
    RCC_Configuration();
    init_debugUart();
    init_Timeout();

    // Enable DWT timer
    if (!(CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk))
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

int main()
{
    InitSys();
    TAP_InitPins(); // first: BKPT must not float while a target comes out of reset

    board_usb_reconnect();
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    tusb_init(0, &dev_init);

    TAP_ResetState();

    uint8_t *byteptr = (uint8_t *) &receiveBuffer[0];

#ifdef DEBUGPRINT
    printf("adapter online\n\r");
#endif

    while (1)
    {
        if (usb_poll())
        {
            // We expect data in little-endian format.
            // Host makes sure not to mix commands. First command in queue determines what rest is allowed
            // Word[2] Contains command. Commands are split in categories of:
            // 0x00xx: TAP
            // 0x01xx: ???

            switch (byteptr[5]) {
                case 0x00:
                    TAP_Commands(receiveBuffer);
                    break;
                default:
                    break;
            }

            usb_rxRelease();
        }

        if (rebootRequest)
        {
            usb_txWait();
            sleep(20); // the last packet off the wire
            board_reboot_to_bootloader();
        }
    }

    return 0;
}

void assert_failed(uint8_t* file, uint32_t line)
{   while (1) {} }
