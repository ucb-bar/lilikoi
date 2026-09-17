#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define I2C_PORT i2c0
#define I2C_PORT_INDEX 0
#define I2C_SDA_PIN 0
#define I2C_SCL_PIN 1
#define I2C_BAUDRATE_HZ 100000
#define I2C_PROBE_TIMEOUT_US 10000

typedef struct {
    const char *name;
    uint8_t address;
} i2c_device_t;

static const i2c_device_t expected_devices[] = {
    {"ISENSE0",      0x10},
    {"ISENSE1",      0x1f},
    {"VCORE0",       0x45},
    {"VCORE1",       0x47},
    {"VCORE2",       0x40},
    {"VSENSE_PMIC",  0x48},
    {"VSENSE_VCORE", 0x49},
};

static bool i2c_device_responds(uint8_t address) {
    // A one-byte read is non-destructive and confirms that the slave ACKs its
    // address. The returned value is deliberately ignored for this check.
    uint8_t ignored;
    return i2c_read_timeout_us(I2C_PORT, address, &ignored, 1, false,
                               I2C_PROBE_TIMEOUT_US) == 1;
}

static void probe_expected_i2c_devices(void) {
    size_t found = 0;
    const size_t expected = sizeof(expected_devices) / sizeof(expected_devices[0]);

    printf("I2C device check (i2c%d, SDA=GP%d, SCL=GP%d, %d Hz)\n",
           I2C_PORT_INDEX, I2C_SDA_PIN, I2C_SCL_PIN, I2C_BAUDRATE_HZ);

    for (size_t i = 0; i < expected; ++i) {
        const i2c_device_t *device = &expected_devices[i];
        const bool present = i2c_device_responds(device->address);

        printf("  [%-7s] %-13s at 0x%02x\n",
               present ? "OK" : "MISSING", device->name, device->address);
        found += present ? 1 : 0;
    }

    printf("I2C result: %u/%u expected devices responded%s\n",
           (unsigned int)found, (unsigned int)expected,
           found == expected ? "" : " -- CHECK FAILED");
}

int main(void) {
    stdio_init_all();

    // Give the computer time to enumerate the USB serial device.
    sleep_ms(2000);

    i2c_init(I2C_PORT, I2C_BAUDRATE_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

    while (true) {
        probe_expected_i2c_devices();
        printf("\n");
        sleep_ms(5000);
    }
}
