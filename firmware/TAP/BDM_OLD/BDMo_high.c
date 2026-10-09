#include "../TAP_shared.h"
#include "BDMo_private.h"

// The CPU32 fixes a frame's response when the frame starts, and only then
// drives its status bit on DSO. Sampling DSO right after DSCLK falls can read
// it before that, which paired a stale "not ready" with real data whenever a
// slow bus cycle finished near a frame start (every long-word read of a T7's
// 13-wait-state flash failed at 1 MHz, and the turbo dump slipped a word now
// and then). Every frame now waits this long before sampling the status bit.
#define BDMOLD_STATUS_SETTLE_NS 1500

// Pause after a frame that hands the target a command (DUMP, a write's last
// operand) before DSCLK falls for the next one.
#define BDMOLD_FRAME_GAP_NS     1000

// How long a command may keep answering "not ready" before it is given up on
// (RET_MAXRETRY). A bus cycle that never ends, such as an unmapped address
// with the bus monitor off, used to hang the adapter until power-cycled.
#define BDMOLD_NOTREADY_MS      50

// The same limit for the turbo loops, counted in frames.
#define BDMOLD_TURBO_RETRIES    100000

// Memory reads and writes carry an address; register commands do not. This
// used to be decided by the address being nonzero, which broke address 0.
#define BDMOLD_HASADDR(cmd)     (((cmd) & 0xFE00) == 0x1800) // READ 0x19xx, WRITE 0x18xx

// I know it's messy but it was easier to pass along delays and such this way!
typedef struct __attribute((packed)){
    uint32_t payload;       // Data in and out. Remember to shift up data to be sent since we're MSB first!

    // Delay parameters.
    uint32_t delayKeepState;

    uint32_t *pinSetPtr;
    uint32_t *pinClrPtr;
    uint32_t *pinRdPtr;

    // You want to time EXACTLY how many host cycles something took? -Look no further..
    uint32_t *dwtptr;
    uint32_t benchTime;

    uint32_t delayStatus;   // 28: DSCLK low to status sample, r1Delay loops
} oldbdm_td;

// I know it's messy but it was easier to pass along delays and such this way!
typedef struct __attribute((packed)){
    uint32_t *spiSrPntr;

    uint32_t *spiDataPntr;
    uint32_t *pinCrhPtr;

    uint32_t *pinClrPtr;
    uint32_t *pinRdPtr;

    // You want to time EXACTLY how many host cycles something took? -Look no further..
    uint32_t *dwtptr;
    uint32_t benchTime;

    uint16_t *destPtr;
    uint32_t noDwords;

    uint32_t delayStatus;   // 36: DSCLK low to status sample, r1Delay loops
    uint32_t dumpMore;      // 40: dump: the last long word sends DUMP (more follow), else NOP
    uint32_t retries;       // 44: "not ready" frames allowed
    uint32_t *pinSetPtr;    // 48: GPIOB BSRR, to leave DSCLK high after a frame
    uint32_t frameGap;      // 52: after a command frame, before the next, r1Delay loops
} oldbdmturbo_td;

// The turbo loops return 0, or 0x10000 | the data of the response that ended
// them: 0 still not ready, 1 bus error, 0xFFFF illegal command.
extern uint32_t BDMold_shift(oldbdm_td *params);
extern uint32_t BDMold_turbodump(oldbdmturbo_td *params);
extern uint32_t BDMold_turbofill(oldbdmturbo_td *params);
oldbdm_td testdata;
oldbdmturbo_td turbodata;

static inline uint32_t BDMOLD_msCycles(const uint32_t ms)
{   return (SystemCoreClock / 1000) * ms;   }

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Port manipulation; internal
static uint16_t BDMOLD_InitPort_int()
{
    SetPinDir(P_Trst, 3);
    SetPinDir(P_CLK , 3); // pulled up, see TAP_InitPins
    SetPinDir(P_RDY , 2); ///< Except this fella; Input, pull down
    SetPinDir(P_TDI , 0);
    SetPinDir(P_TDO , 0);
    return RET_OK;
}

// Reset and run. BKPT (CLK) is driven high across the reset and left high:
// some targets (the T7) do not pull it up, and floating it as RESET rose
// enabled BDM at reset, so the target came up halted or halted at random.
static uint16_t BDMOLD_TargetReset_int()
{
    BDMOLD_InitPort_int();

    CLK_HI;
    SetPinDir(P_CLK , 1);
    SetPinDir(P_Trst, 1);
    RST_LO;
    sleep(25);
    SetPinDir(P_Trst, 3);

    set_Timeout(500);
    while(!RST_RD && !get_Timeout())   ;
    // disable_Timeout();

    return RST_RD ? RET_OK : RET_NOSTART;
}

// Reset into BDM: RESET pulsed with BKPT (CLK) held low across its rising
// edge, so background mode is enabled at reset and the CPU stops before its
// first instruction. Always resets, so every SIM register, the write-once ones
// included, starts from its reset value. (It used to skip the reset whenever
// BKPT alone halted the target, keeping whatever setup the ECU code had made.)
static uint16_t BDMOLD_TargetReady_int()
{
    SetPinDir(P_CLK, 1);
    SetPinDir(P_TDI, 1);

    CLK_LO;
    sleep(2);

    // Two attempts: pulse RESET with BKPT held low, then wait for FREEZE (RDY).
    for (uint_fast8_t attempt = 0; attempt < 2; attempt++)
    {
        SetPinDir(P_Trst, 1);
        RST_LO;
        sleep(25);
        SetPinDir(P_Trst, 3);   // release RESET; CLK/BKPT stays driven low across the edge

        ///< Give it up to 500~ ms to hit the brakes..
        set_Timeout(500);
        while(!RDY_RD && !get_Timeout())   ;

        if (RDY_RD)
            return RET_OK;
    }

#ifdef DEBUGPRINT
    printf("RDY not set\n\r");
#endif
    return RET_NOTREADY;
}

// Halt a running target through BKPT, without a reset. On a CPU32 whose last
// reset left BDM off the CPU takes a breakpoint exception instead (a T7 halts
// regardless); then this reports RET_NOTREADY and the host decides whether to
// reset into BDM (TARGETREADY).
static uint16_t BDMOLD_TargetStop_int()
{
    if (RDY_RD)
        return RET_OK;

    SetPinDir(P_CLK, 1);
    SetPinDir(P_TDI, 1);
    CLK_LO;

    set_Timeout(100);
    while(!RDY_RD && !get_Timeout())   ;

    if (RDY_RD)
        return RET_OK;

    CLK_HI;
    return RET_NOTREADY;
}

static uint16_t BDMOLD_TranslateFault(const uint16_t retdata)
{
    switch (retdata) {
        case      0: return RET_MAXRETRY;
        case      1: return RET_BUSTERMERR;
        case 0xFFFF: return RET_ILLCOMMAND;
        default:     return RET_UNKERROR;
    }
}

// Clock NOPs while the target answers "not ready" (status set, data 0), for
// at most BDMOLD_NOTREADY_MS. Returns the status bit of the response that
// ended it; its data is left in testdata.payload.
static uint32_t BDMOLD_awaitResponse()
{
    const uint32_t start = DWT->CYCCNT;
    uint32_t atn;

    do {
        testdata.payload = 0;
        atn = BDMold_shift(&testdata);
        testdata.payload &= 0xFFFF;
    } while (atn && !testdata.payload && DWT->CYCCNT - start < BDMOLD_msCycles(BDMOLD_NOTREADY_MS));

    return atn;
}

static uint16_t BDMOLD_ExecRead(const uint16_t cmd, const uint32_t Address, uint16_t *out)
{
    testdata.payload = cmd;
    BDMold_shift(&testdata);

    if (BDMOLD_HASADDR(cmd)) {
        testdata.payload = Address>>16;
        BDMold_shift(&testdata);

        testdata.payload = Address;
        BDMold_shift(&testdata);
    }

    // Long: high word first
    if (cmd&0x80) {
        if (BDMOLD_awaitResponse())
            return BDMOLD_TranslateFault(testdata.payload);
        out[1] = testdata.payload;
    }

    if (BDMOLD_awaitResponse())
        return BDMOLD_TranslateFault(testdata.payload);
    out[0] = testdata.payload;

    return RET_OK;
}

static uint16_t BDMOLD_ExecWrite(const uint16_t cmd, const uint32_t Address, const uint16_t *in)
{
    testdata.payload = cmd;
    BDMold_shift(&testdata);

    if (BDMOLD_HASADDR(cmd)) {
        testdata.payload = Address>>16;
        BDMold_shift(&testdata);

        testdata.payload = Address;
        BDMold_shift(&testdata);
    }

    if (cmd&0x80) {
        testdata.payload = in[1];
        BDMold_shift(&testdata);
    }

    testdata.payload = in[0];
    BDMold_shift(&testdata);

    // "Command complete" once the write is done
    return BDMOLD_awaitResponse() ? BDMOLD_TranslateFault(testdata.payload) : RET_OK;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// BDM abstraction; Private

#define BDMOLD_ExecR8(a,b)  BDMOLD_ExecRead(READ8_BDM , (a), (b))
#define BDMOLD_ExecR16(a,b) BDMOLD_ExecRead(READ16_BDM, (a), (b))
#define BDMOLD_ExecR32(a,b) BDMOLD_ExecRead(READ32_BDM, (a), (b))

#define BDMOLD_ExecW8(a,b)  BDMOLD_ExecWrite(WRITE8_BDM , (a), (b))
#define BDMOLD_ExecW16(a,b) BDMOLD_ExecWrite(WRITE16_BDM, (a), (b))
#define BDMOLD_ExecW32(a,b) BDMOLD_ExecWrite(WRITE32_BDM, (a), (b))

static uint16_t BDMOLD_TargetStart_int()
{
    testdata.payload = 0;
    BDMold_shift(&testdata);

    testdata.payload = BDM_GO;
    BDMold_shift(&testdata);

    // FREEZE drops as the CPU leaves BDM
    const uint32_t start = DWT->CYCCNT;
    while ( RDY_RD )
        if (DWT->CYCCNT - start > BDMOLD_msCycles(100))
            return RET_NOSTART;

    return RET_OK;
}

static uint16_t BDMOLD_TargetStatus_in()
{
    uint32_t initVal = 0, stable = 0;

    while (!stable) {
        initVal = RDY_RD;
        stable  = 1;

        for (uint_fast8_t i = 0; i < 5; i++)
        {
            if (RDY_RD != initVal)
            {
                stable = 0;
                i = 5;
            }
        }
    }

    return initVal ? RET_TARGETSTOPPED : RET_TARGETRUNNING;
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Fill and dump functions; Private
// Whole long words only. The first goes as a checked write, which also sets the
// address; FILL writes the rest. (Padding a short tail used to write past the
// received data, into the target too.)
static uint16_t BDMOLD_stuffTarget(const uint32_t Start, uint32_t Len, const uint32_t *buffer)
{
    uint16_t retval;

    if (!Len || (Len & 3))
        return RET_MALFORMED;

    retval = BDMOLD_ExecWrite(WRITE32_BDM, Start, (const uint16_t *) buffer++);
    Len   -= 4;

    if (Len && retval == RET_OK)
    {
        turbodata.destPtr  = (uint16_t *)buffer;
        turbodata.noDwords = (Len/4);
        turbodata.retries  = BDMOLD_TURBO_RETRIES;
        const uint32_t r   = BDMold_turbofill(&turbodata);
        if (r)
            retval = BDMOLD_TranslateFault(r);
    }

    return retval;
}

// [5][TAP_DO_DUMPMEM][status][addr][addr]: the dump ended at Address.
static void BDMOLD_dumpFault(uint16_t *buffer, const uint16_t status, const uint32_t Address)
{
    buffer[0] = 5;
    buffer[1] = TAP_DO_DUMPMEM;
    buffer[2] = status;
    *(uint32_t *) &buffer[3] = Address;
    usb_sendData(buffer);
}

// Streams [tot len][cmd][sts][addr][addr][data..] frames of up to 1 KB. One
// checked long-word read starts it; after that every DUMP goes out in the
// low-word frame of the long word before, and the very last sends a NOP, so
// nothing is left running when the dump ends (which used to take an extra
// read to "thrash"). Any error ends the dump with a fault frame.
static void BDMOLD_blastReceiver(uint32_t Start, const uint32_t End, uint16_t *buffer)
{
    uint32_t BytesToCopy = 1024;
    uint16_t retval;

    retval = BDMOLD_ExecR32(Start, &buffer[5]);
    if (retval != RET_OK)
    {
        BDMOLD_dumpFault(buffer, retval, Start);
        return;
    }

    if (End - Start > 4)
    {
        testdata.payload = DUMP32_BDM;
        BDMold_shift(&testdata);
    }

    for (uint32_t first = 1; Start < End; first = 0)
    {
        if (End - Start < BytesToCopy)
            BytesToCopy = End - Start;

        const uint32_t n = (BytesToCopy / 4) - first;

        if (n)
        {
            turbodata.destPtr  = &buffer[5 + 2*first];
            turbodata.noDwords = n;
            turbodata.dumpMore = (Start + BytesToCopy) < End;
            turbodata.retries  = BDMOLD_TURBO_RETRIES;
            const uint32_t r   = BDMold_turbodump(&turbodata);
            if (r)
            {
                BDMOLD_dumpFault(buffer, BDMOLD_TranslateFault(r), Start + BytesToCopy - turbodata.noDwords * 4);
                return;
            }
        }

        // [tot len] [cmd][sts] [addr][addr] [Data..]
        buffer[0] = (BytesToCopy / 2) + 5;
        buffer[1] = TAP_DO_DUMPMEM;
        buffer[2] = RET_OK;
        *(uint32_t *) &buffer[3] = Start;

        usb_sendData(buffer);
        Start += BytesToCopy;
    }
}

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Port manipulation; Public
void BDMOLD_InitPort(const uint16_t *in, uint16_t *out)
{   out[0] = BDMOLD_InitPort_int();                      }
void BDMOLD_TargetReady(const uint16_t *in, uint16_t *out)
{   out[0] = BDMOLD_TargetReady_int();                   }
void BDMOLD_TargetStop(const uint16_t *in, uint16_t *out)
{   out[0] = BDMOLD_TargetStop_int();                    }
void BDMOLD_TargetReset(const uint16_t *in, uint16_t *out)
{   out[0] = BDMOLD_TargetReset_int();                   }
void BDMOLD_TargetStart(const uint16_t *in, uint16_t *out)
{   out[0] = BDMOLD_TargetStart_int();                   }
void BDMOLD_TargetStatus(const uint16_t *in, uint16_t *out)
{   out[0] = RET_OK, out[1] = 3, out[2] = BDMOLD_TargetStatus_in(); }

/////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
// Internal configuration; Public
void BDMOLD_setup(const float TargetFreq)
{
    // Hardware accelerated by SPI
    spi_cfg_t cfg;
    cfg.frequency = (uint32_t)TargetFreq;
    cfg.polarity  = SPI_HIGH;
    cfg.phase     = SPI_SECOND;
    cfg.order     = SPI_MSB;
    cfg.size      = SPI_WORD;
    InitSPI(&cfg);

    turbodata.pinClrPtr   = (uint32_t *) &GPIOB->BRR;
    turbodata.pinRdPtr    = (uint32_t *) &GPIOB->IDR;
    turbodata.dwtptr      = (uint32_t *) &DWT->CYCCNT;
    turbodata.pinCrhPtr   = (uint32_t *) &GPIOB->CRH;
    turbodata.spiDataPntr = (uint32_t *) &SPI2->DR;
    turbodata.spiSrPntr   = (uint32_t *) &SPI2->SR;

    testdata.dwtptr    = (uint32_t *) &DWT->CYCCNT;
    testdata.pinSetPtr = (uint32_t *) &GPIOB->BSRR;
    testdata.pinClrPtr = (uint32_t *) &GPIOB->BRR;
    testdata.pinRdPtr  = (uint32_t *) &GPIOB->IDR;

    testdata.delayKeepState = TAP_calcDelay(24, 0.5, TargetFreq);

    turbodata.pinSetPtr   = (uint32_t *) &GPIOB->BSRR;
    BDMOLD_SetTiming(BDMOLD_STATUS_SETTLE_NS, BDMOLD_FRAME_GAP_NS);
}

// r1Delay loops for ns: ~6 cycles a loop
static uint32_t BDMOLD_nsLoops(const uint32_t ns)
{   return (SystemCoreClock / 1000000) * ns / 1000 / 6 + 1;   }

// Frame timing: settleNs from DSCLK falling to sampling the status bit,
// gapNs after a command frame. TAP_DO_BDMTIMING sets them for odd targets or
// cables; SETINTERFACE puts the defaults back.
void BDMOLD_SetTiming(const uint32_t settleNs, const uint32_t gapNs)
{
    const uint32_t settle = BDMOLD_nsLoops(settleNs);
    testdata.delayStatus  = (testdata.delayKeepState > settle) ? testdata.delayKeepState : settle;
    turbodata.delayStatus = settle;
    turbodata.frameGap    = BDMOLD_nsLoops(gapNs);
}

// [addr][addr],[len][len], [data]++
void BDMOLD_WriteMemory(const uint16_t *in, uint16_t *out)
{
    uint32_t Address  = *(uint32_t *) &in[0];
    uint32_t Len      = *(uint32_t *) &in[2];
    uint16_t *dataptr =  (uint16_t *) &in[4];
    uint16_t retval   = RET_OK;

    while (Len && retval == RET_OK)
    {
        // dword
        if (Len > 3) {
            retval = BDMOLD_ExecW32(Address, dataptr);
            Len     -= 4;
            Address += 4;
            dataptr += 2;
        }
        // word
        else if (Len > 1) {
            retval = BDMOLD_ExecW16(Address, dataptr);
            Len     -= 2;
            Address += 2;
            dataptr += 1;
        }
        // byte
        else
        {
            retval = BDMOLD_ExecW8(Address, dataptr);
            Len     -= 1;
            Address += 1;
            dataptr += 1;
        }
    }

    out[0] = retval;
}

// In: [addr][addr],[len][len]
void BDMOLD_ReadMemory(const uint16_t *in, uint16_t *out)
{
    uint16_t *dataptr =  (uint16_t *) &out[2];
    uint32_t Address  = *(uint32_t *) &in[0];
    uint32_t Len      = *(uint32_t *) &in[2];
    uint16_t retval   = RET_OK;
    uint32_t noRead   = 0;

    // The reply must fit the send buffer next to its headers
    if (Len > ADAPTER_BUFzOUT - 16)
    {
        out[0] = RET_MALFORMED;
        return;
    }

    // What is left decides the size of each read (it used to be the total,
    // so a 6-byte read returned 8)
    while (noRead < Len && retval == RET_OK)
    {
        // dword
        if (Len - noRead > 3)
        {
            retval = BDMOLD_ExecR32(Address, dataptr);
            noRead  += 4;
            Address += 4;
            dataptr += 2;
        }
        // word
        else if (Len - noRead > 1)
        {
            retval = BDMOLD_ExecR16(Address, dataptr);
            noRead  += 2;
            Address += 2;
            dataptr += 1;
        }
        // byte
        else
        {
            retval = BDMOLD_ExecR8(Address, dataptr);
            *dataptr &= 0xFF;
            noRead  += 1;
            Address += 1;
            dataptr += 1;
        }
    }

    out[0] = retval;
    out[1] = 2 + (noRead>>1) + (noRead&1);
}

// [addr][addr],[len][len], [data..]
void BDMOLD_FillMemory(const uint16_t *in, uint16_t *out)
{
    TAP_ReadCMD_t *cmd = (TAP_ReadCMD_t *) in;
    out[0] = BDMOLD_stuffTarget(cmd->Address, cmd->Length, (const uint32_t *) &in[4]);
}

// [addr][addr],[len][len]
void BDMOLD_DumpMemory(const uint16_t *in, uint16_t *out)
{
    TAP_ReadCMD_t *cmd = (TAP_ReadCMD_t *) in;

    // Whole long words from an even address
    if (!cmd->Length || (cmd->Length & 3) || (cmd->Address & 1))
    {
        BDMOLD_dumpFault(out, RET_MALFORMED, cmd->Address);
        return;
    }
    BDMOLD_blastReceiver(cmd->Address, cmd->Address + cmd->Length, out);
}

// [register][size][data]++
void BDMOLD_WriteRegister(const uint16_t *in, uint16_t *out)
{
#ifdef DEBUGPRINT
    printf("BDM_WriteReg: %04x, %04x%04x\n\r", in[0], in[3], in[2]);
#endif

    switch (*in & 0xFFF0)
    {
        case BDMOLD_W_DREG: // D/AREG
        case BDMOLD_W_SREG: // SREG
            // Can only write dwords!
            if (in[1] != 4) {
                out[0] = RET_UNALIGNED;
                return;
            }
            out[0] = BDMOLD_ExecWrite(*in, 0, (uint16_t *)&in[2]);
            out[1] = 2;
            return;
        default:
            out[0] = RET_NOTSUP;
            break;
    }
}

// [register][size]
void BDMOLD_ReadRegister(const uint16_t *in, uint16_t *out)
{
    switch (*in & 0xFFF0)
    {
        case BDMOLD_R_DREG: // D/AREG
        case BDMOLD_R_SREG: // SREG
            if (in[1] != 4) {
                out[0] = RET_UNALIGNED;
                return;
            }
            out[0] = BDMOLD_ExecRead(*in, 0, (uint16_t *)&out[2]);
            out[1] = 4;
            // The target only returns dwords and that is what the host gets
            return;
        default:
            out[0] = RET_NOTSUP;
            break;
    }
}

// [addr][addr],[len][len]
void BDMOLD_AssistFlash(const uint16_t *in, uint16_t *out)
{
    TAP_AssistCMD_t *cmd = (TAP_AssistCMD_t *) in;
    uint32_t Last   = cmd->Address + cmd->Length;
    uint16_t retval;
    uint32_t regRead;

    // Since we intend to thrash a bunch of buffers we better store the data of these pointers.
    uint32_t driverStart = cmd->DriverStart;
    uint32_t bufferStart = cmd->BufferStart;
    uint32_t bufferLen   = cmd->BufferLen;
    uint32_t flashStart  = cmd->Address;
    // uint32_t flashLen    = cmd->Length;

    while (flashStart < Last)
    {
        uint16_t *ptr = TAP_RequestData(flashStart, bufferLen);

        // Got no data!
        if (!ptr)
        {
            TAP_UpdateStatus(RET_NOPTR, 0);
            return;
        }

        // while running..
        set_Timeout(60000);
        retval = BDMOLD_TargetStatus_in();
        while (retval == RET_TARGETRUNNING && !get_Timeout())
            retval = BDMOLD_TargetStatus_in();

        // Timeout
        if (get_Timeout())
        {
            TAP_UpdateStatus(RET_TIMEOUT, RET_TARGETRUNNING);
            return;
        }
        // What's wrong??
        /*else if (retval != RET_TARGETSTOPPED)
        {
            // printf("Target stop fail\n\r");
            return;
        }*/
        // disable_Timeout();

        retval = BDMOLD_ExecRead(R_DREG_BDM, 0, (uint16_t *)&regRead);
        // Could not fetch status
        if (retval != RET_OK)
        {
            TAP_UpdateStatus(RET_RWERR, retval);
            return;
        }

        // Driver says something went wrong..
        else if (regRead != 1)
        {
            TAP_UpdateStatus(RET_DRIVERFAIL, retval);
            return;
        }

        retval = BDMOLD_ExecWrite(W_SREG_BDM, 0, (uint16_t *)&driverStart);
        // Could not set RPC
        if (retval != RET_OK)
        {
            TAP_UpdateStatus(RET_RWERR, retval);
            return;
        }

        retval = BDMOLD_ExecWrite(WRITE32_BDM, bufferStart, ptr);
        if (retval != RET_OK)
        {
            TAP_UpdateStatus(RET_RWERR, retval);
            return;
        }
        ptr += 2;

        turbodata.destPtr  = (uint16_t *) ptr;
        turbodata.noDwords = (bufferLen - 4) / 4;
        turbodata.retries  = BDMOLD_TURBO_RETRIES;
        if (turbodata.noDwords && BDMold_turbofill(&turbodata))
        {
            TAP_UpdateStatus(RET_RWERR, 0);
            return;
        }

        // Didn't start
        retval = BDMOLD_TargetStart_int();
        if (retval != RET_OK)
        {
            TAP_UpdateStatus(RET_NOSTART, retval);
            return;
        }

        flashStart += bufferLen;
    }

    // while running..
    set_Timeout(60000);
    retval = BDMOLD_TargetStatus_in();
    while (retval == RET_TARGETRUNNING && !get_Timeout())
        retval = BDMOLD_TargetStatus_in();

    // Timeout
    if (get_Timeout())
    {
        TAP_UpdateStatus(RET_TIMEOUT, RET_TARGETRUNNING);
        return;
    }

    // disable_Timeout();

    retval = BDMOLD_ExecRead(R_DREG_BDM, 0, (uint16_t *)&regRead);
    // Could not fetch status
    if (retval != RET_OK)
    {
        TAP_UpdateStatus(RET_RWERR, retval);
        return;
    }

    // Driver says something went wrong..
    else if (regRead != 1)
    {
        TAP_UpdateStatus(RET_DRIVERFAIL, retval);
        return;
    }

    TAP_UpdateStatus(RET_OK, 0);
}
