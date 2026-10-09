/* Fletcher Lake's on-board RP2350B (48 GPIO package). */
#ifndef _BOARDS_FLETCHERLAKE_RP2350B_H
#define _BOARDS_FLETCHERLAKE_RP2350B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FLETCHERLAKE_RP2350B
#define PICO_RP2350A 0

/* No default UART: GP0/GP1 are the PMIC I2C bus.  All firmware uses USB stdio. */

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 0
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 1
#endif

/* Preserve the flash assumptions used by the project's previous pico2 build. */
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
