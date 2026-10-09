# bdmtoy firmware: the app (bin/firmware.*) and the USB DFU bootloader
# (bin/bootloader.*). bin/bdmtoy-full.hex holds both, for a first flash over
# SWD; after that the app is updated over USB (README.md).
#
# Libraries are vendored in lib/ (lib/VERSIONS.md).

BASENAME = arm-none-eabi
CC       = $(BASENAME)-gcc
OBJCOPY  = $(BASENAME)-objcopy
SIZE     = $(BASENAME)-size

# Keep in mind that -O2 is quite aggressive and has caused trouble with the
# timing-sensitive TAP code; -O1 it is.
CPU      = -mcpu=cortex-m3 -mthumb
DEFS     = -DSTM32F103xB -DUSE_FULL_LL_DRIVER -DHSE_VALUE=8000000 $(DEVFLAGS)
INCS     = -I. -Iboard \
           -Ilib/CMSIS/Core/Include \
           -Ilib/cmsis_device_f1/Include \
           -Ilib/stm32f1xx_ll/Inc \
           -Ilib/tinyusb/src
CFLAGS   = $(CPU) -std=gnu11 -O1 -g3 -Wall -ffunction-sections -fdata-sections $(DEFS) $(INCS)
ASFLAGS  = $(CPU) -g3 $(DEFS)
LDFLAGS  = $(CPU) -Wl,--gc-sections -nostartfiles --specs=nano.specs --specs=nosys.specs

TUSB = lib/tinyusb/src
LIBSRC = \
	lib/cmsis_device_f1/Source/system_stm32f1xx.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_gpio.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_rcc.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_spi.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_tim.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_usart.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_utils.c \
	lib/stm32f1xx_ll/Src/stm32f1xx_ll_dma.c \
	$(TUSB)/tusb.c \
	$(TUSB)/common/tusb_fifo.c \
	$(TUSB)/device/usbd.c \
	$(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c \
	$(TUSB)/portable/st/stm32_fsdev/fsdev_common.c \
	board/board.c
LIBASM = lib/cmsis_device_f1/Source/startup_stm32f103xb.s

APPSRC = \
	main.c \
	SupportFuncs.c \
	app/usb_descriptors.c \
	$(TUSB)/class/vendor/vendor_device.c \
	TAP/TAP_shared.c \
	TAP/BDM_HC12/BDMs_high.c \
	TAP/BDM_NEW/BDMn_high.c \
	TAP/BDM_OLD/BDMo_high.c \
	TAP/JTAG/JTAG_high.c \
	TAP/NEXUS/NEXUS_high.c \
	TAP/UART_MON/UARTMON_high.c
APPASM = \
	TAP/TAP_lowlev.s \
	TAP/BDM_HC12/BDMs_low.s \
	TAP/BDM_NEW/BDMn_low.s \
	TAP/BDM_OLD/BDMo_low.s

BOOTSRC = \
	bootloader/main.c \
	bootloader/usb_descriptors.c \
	$(TUSB)/class/dfu/dfu_device.c

# Each image gets its own objects: they see different tusb_config.h files.
APPOBJ  = $(addprefix obj/app/,  $(APPSRC:.c=.o)  $(APPASM:.s=.o)  $(LIBSRC:.c=.o) $(LIBASM:.s=.o))
BOOTOBJ = $(addprefix obj/boot/, $(BOOTSRC:.c=.o) $(LIBSRC:.c=.o) $(LIBASM:.s=.o))

.PHONY: all clean flash flash-app

all: bin/firmware.bin bin/firmware.hex bin/bootloader.bin bin/bootloader.hex bin/bdmtoy-full.hex
	@$(SIZE) bin/bootloader.elf bin/firmware.elf

obj/app/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -Iapp -c $< -o $@
obj/app/%.o: %.s
	@mkdir -p $(dir $@)
	@$(CC) $(ASFLAGS) -c $< -o $@
obj/boot/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -Ibootloader -c $< -o $@
obj/boot/%.o: %.s
	@mkdir -p $(dir $@)
	@$(CC) $(ASFLAGS) -c $< -o $@

bin/firmware.elf: $(APPOBJ) link_app.ld link_common.ld
	@mkdir -p bin
	@$(CC) $(LDFLAGS) -Wl,-Map=bin/firmware.map -Tlink_app.ld -o $@ $(APPOBJ)

bin/bootloader.elf: $(BOOTOBJ) link_boot.ld link_common.ld
	@mkdir -p bin
	@$(CC) $(LDFLAGS) -Wl,-Map=bin/bootloader.map -Tlink_boot.ld -o $@ $(BOOTOBJ)

bin/%.bin: bin/%.elf
	@$(OBJCOPY) -O binary $< $@
bin/%.hex: bin/%.elf
	@$(OBJCOPY) -O ihex $< $@

# Bootloader and app in one, for the first flash over SWD
bin/bdmtoy-full.hex: bin/bootloader.elf bin/firmware.elf
	@$(OBJCOPY) -O ihex bin/bootloader.elf obj/boot.hex
	@$(OBJCOPY) -O ihex bin/firmware.elf obj/app.hex
	@grep -v -e ':00000001FF' -e '^:04000005' obj/boot.hex > $@
	@grep -v -e '^:04000005' obj/app.hex >> $@

# Over SWD with an ST-LINK. flash: bootloader and app; flash-app: the app only.
# CPUTAPID 0: accept the clone STM32s (CKS32, APM32) many Blue Pills carry
OPENOCD = openocd -f interface/stlink.cfg -c "transport select hla_swd" -c "set CPUTAPID 0" -f target/stm32f1x.cfg
flash: bin/bdmtoy-full.hex
	$(OPENOCD) -c init -c halt -c "program bin/bdmtoy-full.hex verify" -c "reset run" -c shutdown
flash-app: bin/firmware.elf
	$(OPENOCD) -c init -c halt -c "program bin/firmware.elf verify" -c "reset run" -c shutdown

clean:
	rm -rf obj/app obj/boot obj/*.hex bin/*.elf bin/*.map bin/*.bin bin/*.hex

# Header changes rebuild everything: the tree is small
$(APPOBJ) $(BOOTOBJ): $(wildcard *.h */*.h */*/*.h board/*.h app/*.h bootloader/*.h) makefile
