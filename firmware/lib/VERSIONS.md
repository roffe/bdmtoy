# Vendored libraries

Only the files the build uses are copied, unmodified. To update one, copy the
same files from the new release and rebuild both images (`make`).

| Directory | Upstream | Release | Commit |
|---|---|---|---|
| `CMSIS/Core/Include` | [ARM-software/CMSIS_6](https://github.com/ARM-software/CMSIS_6), `CMSIS/Core/Include` (Cortex-M3 and common headers) | v6.3.0 | 45dab712ad84f8cbbf2b7bfc089c19088507df6f |
| `cmsis_device_f1` | [STMicroelectronics/cmsis-device-f1](https://github.com/STMicroelectronics/cmsis-device-f1): STM32F103xB header, `system_stm32f1xx.c`, GCC startup | v4.3.5 | 8a76309ed1250d817e9c888c4417171d2ba3ba63 |
| `stm32f1xx_ll` | [STMicroelectronics/stm32f1xx-hal-driver](https://github.com/STMicroelectronics/stm32f1xx-hal-driver): the LL drivers only | v1.1.10 | 77fbb30b7a1d02533980400083e48c559aae5a4f |
| `tinyusb` | [hathach/tinyusb](https://github.com/hathach/tinyusb) `src/`: device stack, vendor and DFU classes, STM32 FSDEV port | 0.21.0 | dae3f9a366bfcddbf9dcf1b48d7500286a849539 |

Toolchain used: arm-none-eabi-gcc 16.2, newlib-nano.

These replace what `Syssup/` held until firmware 2.0: the STM32F10x Standard
Peripheral Library, the legacy STM32 USB-FS Device library (4.1) and CMSIS 4.
