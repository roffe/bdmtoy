# If you see read_TDO in weird places it's there to pad timings
.global BDMold_shift
.global BDMold_turbodump
.global BDMold_turbofill
.type BDMold_shift, %function
.type BDMold_turbodump, %function
.type BDMold_turbofill, %function
.thumb

# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# Load value, jump here and return: 12 / 6 (r4: 1) Branch prediction will take 6 cycles from this with larger delays..
# 6 DWT cycles+ for every loop
r1Delay:
    add r2, #-1
    bne r1Delay
bx lr

# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
.macro set_BOTH_low
    ldr    r2, [r0, #12] /* load ptr                    */
    mov    r3, #0xA0     /* It's actually 0xA000...     */
    strb   r3, [r2]      /* Set pin low                 */
.endm

.macro set_CLK_high
    ldr    r2, [r0, #8]  /* load ptr                    */
    mov    r3, #0x20     /* It's actually 0x2000...     */
    strb   r3, [r2]      /* Set pin high                */
.endm

.macro set_CLK_low
    ldr    r2, [r0, #12] /* load ptr                    */
    mov    r3, #0x20     /* It's actually 0x2000...     */
    strb   r3, [r2]      /* Set pin low                 */
.endm

.macro set_TDI_high
    ldr    r2, [r0, #8]  /* load ptr                    */
    mov    r3, #0x80     /* It's actually 0x8000...     */
    strb   r3, [r2]      /* Set pin high                */
.endm

.macro set_TDI_low
    ldr    r2, [r0, #12] /* load ptr                    */
    mov    r3, #0x80     /* It's actually 0x8000...     */
    strb   r3, [r2]      /* Set pin low                 */
.endm

.macro read_TDO
    ldr    r2, [r0, #16] /* load ptr                    */
#   mov    r3,  #1       /* Preload value to be AND'ed  */
    ldrb   r2, [r2,#1]   /* Load pin state              */
    lsr    r2,  #6       /* Shift down pin 14 to bit 0  */
#   and    r2,  r3       /* AND result with 1           */
    # Normally you'd need to AND the result but since data is only read when shifting out 0's, we know data out will be 0 (data out is pin 15)
    # Needless to say this will bite your ass if the pinout is changed
.endm

# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# DWT timer: Not used now that the code is profiled.
# Extremely useful for tweaking delays since it's run at the same speed as the processor.
.macro dwt_Capture
    ldr    r3, [r0, #20] /* Fetch pointer to DWT clock  */
    ldr    r3, [r3]      /* Capture current tick        */
    str    r3, [r0, #24] /* Store result     */
    nop                  /* Inserting nops makes sure we have a known good base value */
    nop                  /* (One cycle would magically come and go with other instructions) */
    nop
.endm

.macro dwt_Recapture
    nop
    nop
    nop
    ldr    r3, [r0, #20] /* Fetch pointer to DWT clock */
    ldr    r2, [r3]      /* Capture new time */
    ldr    r3, [r0, #24] /* Fetch old result */
    sub    r2,  r2, r3   /* Calculate delta  */
    sub    r2, #11       /* Subtract the time it took to capture these values */
    str    r2, [r0, #24] /* Store result     */
.endm

# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# This thing can go from 0 to 1 MHz ~ish (Due to configurable speeds we can't go particularly fast..)
#00: payload
#04: delayKeepState
#08: *pinSetPtr
#12: *pinClrPtr
#16: *pinRdPtr
#20: *dwtptr
#24: benchTime
#28: delayStatus
.thumb_func
BDMold_shift:

    push  {r1 - r7, lr}  /* Usual stack stuff           */

    mov    r7, #16       /* Number of bits to send      */
    ldr    r6, [r0, #0 ] /* Load data                   */
    lsl    r6, #15       /* Left-align package         */

    # dwt_Capture

/* * * * * * * * * * * * * * * * * * * * * * */

    # Take care of attention bit. DSCLK low starts the frame; the target needs
    # a moment before DSO carries this frame's status: delayStatus, not the
    # half bit time, which at 1 MHz was too short
    set_BOTH_low
    nop
    ldr    r2, [r0, #28]
    bl r1Delay
    read_TDO
    read_TDO
    mov    r5,  r2

    # Latch data out 36(30)
    set_CLK_high
    nop
    ldr    r2, [r0, #4]
    bl r1Delay
    read_TDO
    read_TDO
    nop

    # Shift out/in data 36(30)
shiftMore:
    set_CLK_low

    lsl    r6, #1
    bpl  setZero

    set_TDI_high
    b   oneSet

setZero:
    set_TDI_low
    nop
    nop

oneSet:

    ldr    r2, [r0, #4]
    bl r1Delay

    read_TDO
    orr    r6,  r2

    # Latch data out 36(30)
    set_CLK_high

    read_TDO
    ldr    r2, [r0, #4]
    bl r1Delay
    read_TDO

    sub    r7,  #1       /* Subtract number of bits left */
    bne shiftMore

/* * * * * * * * * * * * * * * * * * * * * * */

    # dwt_Recapture

    str    r6, [r0, #0 ] /* Save data          */
    mov    r0,  r5

    # Stack stuff..
    pop   {r1 - r7}
    pop   {r1}

bx r1

# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # # #
# Turbo dump and fill. SPI shifts the 16 data bits of a frame; the status bit
# in front of them is clocked by hand: DSCLK low, wait delayStatus for DSO,
# sample it, then hand the pins to SPI, whose idle-high clock is the rising
# edge. Status is checked wherever a response is due; "not ready" (status 1,
# data 0) repeats the frame up to `retries` times, anything else with status
# 1 ends the loop.
#
# Return: 0 when done, else 0x10000 | data of the response that ended it
# (0 still not ready, 1 bus error, 0xFFFF illegal command).
//
// 00: *spiSrPntr
// 04: *spiDataPntr
// 08: *pinCrhPtr
// 12: *pinClrPtr
// 16: *pinRdPtr
// 20: *dwt_pntr
// 24: dwt_time
// 28: dataPtr;
// 32: noDwords
// 36: delayStatus
// 40: dumpMore  (dump: the last long word sends DUMP, not NOP; more follow)
// 44: retries
// 48: *pinSetPtr (GPIOB BSRR)
// 52: frameGap

# r6 = CRH with DSCLK (pin 13) and DSI (pin 15) on SPI. Leaves r2 = 0x80.
.macro crh_SPI
    mov    r6, #0x80
    lsl    r6, #8
    mov    r2, #0x80
    orr    r6,  r2
    lsl    r6, #16
    orr    r6,  r5
.endm

# Start a frame whose response is checked: DSCLK low, wait for DSO, status
# bit into r1, pins to SPI.
.macro status_frame
    set_BOTH_low
    crh_SPI
    ldr    r2, [r0, #36]
    bl     r1Delay
    read_TDO
    str    r6, [r4, #0]
    mov    r1,  r2
.endm

# Start a frame whose response is not looked at.
.macro plain_frame
    set_BOTH_low
    crh_SPI
    str    r6, [r4, #0]
.endm

# r3 out on SPI; wait for the reply into r6, pins back to GPIO. Leaves r3 = 1.
# DSCLK is set high in the output latch first: back on GPIO it then idles
# high. (It used to come back low, which started the next frame right as the
# target was taking in the last one: the race behind the bad dumps.)
.macro spi_word
    ldr    r2, [r0, #4]
    strh   r3, [r2, #0]
    mov    r3, #1
1:
    ldr    r6, [r0, #0]
    ldrh   r6, [r6, #0]
    ror    r6, r3
    bpl    1b
    ldr    r2, [r0, #48]  /* pinSetPtr (BSRR) */
    mov    r3, #0x20      /* DSCLK high: byte lanes replicate, as in set_CLK_high */
    strb   r3, [r2, #0]
    mov    r3, #1
    ldr    r2, [r0, #4]
    ldrh   r6, [r2, #0]
    str    r5, [r4, #0]
.endm

# Pause frameGap between a frame that hands the target a command and the next
# frame, so the target has taken it in before DSCLK falls again.
.macro frame_gap
    ldr    r2, [r0, #52]
    bl     r1Delay
.endm

.thumb_func
BDMold_turbodump:

    push  {r4 - r7, lr}  /* Usual stack stuff           */

    ldr    r4, [r0, #8 ]
    ldr    r5, [r4, #0 ] /* Stock */
    ldr    r7, [r0, #28] /* Pointer */

DumpHigh:
    # High word: a NOP, answered with the result of the DUMP sent before
    status_frame
    mov    r3, #0
    spi_word
    ror    r1, r3
    bmi    DumpStatus
    strh   r6, [r7, #2]

    # Low word: send the next DUMP with it, or a NOP after the very last long
    # word so no read is left running
    set_BOTH_low
    crh_SPI
    mov    r3, #0x1D
    lsl    r3, #8
    orr    r3,  r2       /* DUMP32 0x1D80 */
    ldr    r2, [r0, #32]
    cmp    r2, #1
    bne    DumpSend
    ldr    r2, [r0, #40]
    cmp    r2, #0
    bne    DumpSend
    mov    r3, #0
DumpSend:
    str    r6, [r4, #0]
    spi_word
    strh   r6, [r7, #0]
    add    r7, #4

    # Let it register the DUMP before the next frame starts
    frame_gap

    ldr    r2, [r0, #32]
    sub    r2, #1
    str    r2, [r0, #32]
    bne    DumpHigh

    mov    r0, #0
    b      DumpDone

DumpStatus:
    cmp    r6, #0
    bne    DumpFault
    ldr    r2, [r0, #44]
    sub    r2, #1
    str    r2, [r0, #44]
    beq    DumpFault
    b      DumpHigh
DumpFault:
    mov    r0, #1
    lsl    r0, #16
    orr    r0,  r6
DumpDone:
    pop   {r4 - r7}
    pop   {r1}
    bx r1

# Fill: FILL, data high, data low per long word. The FILL frame is answered
# with the result of the write before it; a final NOP collects the last one.
# "Not ready" on a FILL means the CPU did not take it, so it is sent again.
.thumb_func
BDMold_turbofill:

    push  {r4 - r7, lr}  /* Usual stack stuff           */

    ldr    r4, [r0, #8 ]
    ldr    r5, [r4, #0 ] /* Stock */
    ldr    r7, [r0, #28] /* Pointer */

FillCmd:
    status_frame
    mov    r3, #0x1C
    lsl    r3, #8
    mov    r2, #0x80
    orr    r3,  r2       /* FILL32 0x1C80 */
    spi_word
    ror    r1, r3
    bmi    FillCmdStatus

    plain_frame
    ldrh   r3, [r7, #2]
    spi_word

    plain_frame
    ldrh   r3, [r7, #0]
    spi_word
    add    r7, #4

    # The write starts now; let it register before the next frame
    frame_gap

    ldr    r2, [r0, #32]
    sub    r2, #1
    str    r2, [r0, #32]
    bne    FillCmd

FillLast:
    status_frame
    mov    r3, #0
    spi_word
    ror    r1, r3
    bmi    FillLastStatus

    mov    r0, #0
    b      FillDone

FillCmdStatus:
    cmp    r6, #0
    bne    FillFault
    ldr    r2, [r0, #44]
    sub    r2, #1
    str    r2, [r0, #44]
    beq    FillFault
    b      FillCmd
FillLastStatus:
    cmp    r6, #0
    bne    FillFault
    ldr    r2, [r0, #44]
    sub    r2, #1
    str    r2, [r0, #44]
    beq    FillFault
    b      FillLast
FillFault:
    mov    r0, #1
    lsl    r0, #16
    orr    r0,  r6
FillDone:
    pop   {r4 - r7}
    pop   {r1}
    bx r1









/*

*/
