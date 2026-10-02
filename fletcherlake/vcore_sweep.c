/*
To run:
cd /Users/kellytou/Desktop/pico/fletcherlake
cmake -S . -B build-sweep -DPICO_BOARD=pico2
cmake --build build-sweep --target vcore_sweep -j
picotool load build-sweep/vcore_sweep.uf2 -vx
*/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define I2C_PORT i2c0
#define I2C_SDA_PIN 0
#define I2C_SCL_PIN 1
#define I2C_BAUDRATE_HZ 100000
#define I2C_TIMEOUT_US 10000

#define TPS6287B_REG_VSET 0x00
#define TPS6287B_REG_CONTROL2 0x02
#define TPS6287B_REG_STATUS 0x04
#define TPS6287B_STATUS_SEVERE_MASK 0x3c

#define TPS62868_REG_VOUT1 0x01
#define TPS62868_REG_STATUS 0x05
#define TPS62868_STATUS_FAULT_MASK 0x19

#define COMMAND_BUFFER_SIZE 128
#define MAX_SWEEP_POINTS 256
#define MAX_DWELL_MS 60000

// ---------------------------------------------------------------------------
// USER CONFIGURATION
// All voltage limits are integer millivolts. Leave writes disabled until the
// six limits below have been replaced with values approved for the load.
// These defaults can alternatively be overridden with CMake -D options.
// ---------------------------------------------------------------------------
#ifndef VCORE_SWEEP_WRITES_ENABLED
#define VCORE_SWEEP_WRITES_ENABLED 0  // TODO: change to 1 after setting limits
#endif
#ifndef VCORE0_SAFE_MIN_MV
#define VCORE0_SAFE_MIN_MV 0  // TODO: replace 0 with the approved minimum
#endif
#ifndef VCORE0_SAFE_MAX_MV
#define VCORE0_SAFE_MAX_MV 0  // TODO: replace 0 with the approved maximum
#endif
#ifndef VCORE1_SAFE_MIN_MV
#define VCORE1_SAFE_MIN_MV 0  // TODO: replace 0 with the approved minimum
#endif
#ifndef VCORE1_SAFE_MAX_MV
#define VCORE1_SAFE_MAX_MV 0  // TODO: replace 0 with the approved maximum
#endif
#ifndef VCORE2_SAFE_MIN_MV
#define VCORE2_SAFE_MIN_MV 0  // TODO: replace 0 with the approved minimum
#endif
#ifndef VCORE2_SAFE_MAX_MV
#define VCORE2_SAFE_MAX_MV 0  // TODO: replace 0 with the approved maximum
#endif

#if VCORE_SWEEP_WRITES_ENABLED &&                                         \
    (VCORE0_SAFE_MIN_MV == 0 || VCORE0_SAFE_MAX_MV == 0 ||              \
     VCORE1_SAFE_MIN_MV == 0 || VCORE1_SAFE_MAX_MV == 0 ||              \
     VCORE2_SAFE_MIN_MV == 0 || VCORE2_SAFE_MAX_MV == 0)
#error "Replace all VCORE safe-limit placeholders before enabling writes"
#endif

#if VCORE_SWEEP_WRITES_ENABLED &&                                         \
    (VCORE0_SAFE_MIN_MV > VCORE0_SAFE_MAX_MV ||                          \
     VCORE1_SAFE_MIN_MV > VCORE1_SAFE_MAX_MV ||                          \
     VCORE2_SAFE_MIN_MV > VCORE2_SAFE_MAX_MV)
#error "Each VCORE minimum must be less than or equal to its maximum"
#endif

typedef enum {
    CONVERTER_TPS6287B,
    CONVERTER_TPS628681A,
} converter_type_t;

typedef struct {
    const char *name;
    uint8_t address;
    converter_type_t type;
    uint16_t safe_min_mv;
    uint16_t safe_max_mv;
    uint8_t original_vset;
    uint8_t control2;
    bool snapshot_valid;
} rail_t;

static rail_t rails[] = {
    {"vcore0", 0x45, CONVERTER_TPS6287B,
     VCORE0_SAFE_MIN_MV, VCORE0_SAFE_MAX_MV, 0, 0, false},
    {"vcore1", 0x47, CONVERTER_TPS628681A,
     VCORE1_SAFE_MIN_MV, VCORE1_SAFE_MAX_MV, 0, 0, false},
    {"vcore2", 0x40, CONVERTER_TPS628681A,
     VCORE2_SAFE_MIN_MV, VCORE2_SAFE_MAX_MV, 0, 0, false},
};

static bool writes_armed;

static bool read_register(uint8_t address, uint8_t reg, uint8_t *value) {
    if (i2c_write_timeout_us(I2C_PORT, address, &reg, 1, true,
                             I2C_TIMEOUT_US) != 1) {
        return false;
    }

    return i2c_read_timeout_us(I2C_PORT, address, value, 1, false,
                               I2C_TIMEOUT_US) == 1;
}

static bool write_register(uint8_t address, uint8_t reg, uint8_t value) {
    const uint8_t command[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, address, command, sizeof(command),
                                false, I2C_TIMEOUT_US) ==
           (int)sizeof(command);
}

static uint16_t tps6287b_step_uv(const rail_t *rail) {
    const uint8_t range = (rail->control2 >> 2) & 0x03;

    if (range == 0) {
        return 1250;
    }
    if (range == 1) {
        return 2500;
    }
    return 5000;
}

static uint16_t code_to_mv(const rail_t *rail, uint8_t code) {
    uint32_t voltage_uv;

    if (rail->type == CONVERTER_TPS6287B) {
        voltage_uv = 400000u + (uint32_t)code * tps6287b_step_uv(rail);
    } else {
        // The schematic specifies TPS628681A: voltage factor 1, 5 mV/code.
        voltage_uv = 400000u + (uint32_t)code * 5000u;
    }

    return (uint16_t)((voltage_uv + 500u) / 1000u);
}

static bool mv_to_code(const rail_t *rail, uint16_t voltage_mv,
                       uint8_t *code) {
    const uint32_t voltage_uv = (uint32_t)voltage_mv * 1000u;
    const uint16_t step_uv = rail->type == CONVERTER_TPS6287B
                                 ? tps6287b_step_uv(rail)
                                 : 5000u;

    if (voltage_uv < 400000u) {
        return false;
    }

    const uint32_t offset_uv = voltage_uv - 400000u;
    if ((offset_uv % step_uv) != 0 || (offset_uv / step_uv) > 255u) {
        return false;
    }

    *code = (uint8_t)(offset_uv / step_uv);
    return true;
}

static uint8_t vset_register(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B ? TPS6287B_REG_VSET
                                             : TPS62868_REG_VOUT1;
}

static uint8_t status_register(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B ? TPS6287B_REG_STATUS
                                             : TPS62868_REG_STATUS;
}

static uint8_t severe_status_mask(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B
               ? TPS6287B_STATUS_SEVERE_MASK
               : TPS62868_STATUS_FAULT_MASK;
}

static rail_t *find_rail(const char *name) {
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
        if (strcmp(name, rails[i].name) == 0) {
            return &rails[i];
        }
    }
    return NULL;
}

static bool snapshot_rail(rail_t *rail) {
    rail->snapshot_valid = false;

    if (rail->type == CONVERTER_TPS6287B &&
        !read_register(rail->address, TPS6287B_REG_CONTROL2,
                       &rail->control2)) {
        return false;
    }

    if (!read_register(rail->address, vset_register(rail),
                       &rail->original_vset)) {
        return false;
    }

    rail->snapshot_valid = true;
    return true;
}

static bool read_voltage(const rail_t *rail, uint16_t *voltage_mv,
                         uint8_t *raw_code) {
    if (!read_register(rail->address, vset_register(rail), raw_code)) {
        return false;
    }

    *voltage_mv = code_to_mv(rail, *raw_code);
    return true;
}

static bool restore_rail(rail_t *rail) {
    if (!rail->snapshot_valid) {
        printf("ERROR: %s has no valid startup snapshot\n", rail->name);
        return false;
    }

    if (!write_register(rail->address, vset_register(rail),
                        rail->original_vset)) {
        printf("ERROR: failed to restore %s\n", rail->name);
        return false;
    }

    uint8_t readback;
    if (!read_register(rail->address, vset_register(rail), &readback) ||
        readback != rail->original_vset) {
        printf("ERROR: %s restore readback mismatch\n", rail->name);
        return false;
    }

    printf("RESTORED %s to %u mV (register 0x%02x)\n", rail->name,
           code_to_mv(rail, rail->original_vset), rail->original_vset);
    return true;
}

static bool set_voltage(rail_t *rail, uint16_t voltage_mv) {
    uint8_t code;
    uint8_t ignored_status;

    if (!VCORE_SWEEP_WRITES_ENABLED) {
        printf("DENIED: this firmware was built read-only\n");
        return false;
    }
    if (!writes_armed) {
        printf("DENIED: run 'arm WRITE' first\n");
        return false;
    }
    if (!rail->snapshot_valid) {
        printf("DENIED: no valid startup snapshot for %s\n", rail->name);
        return false;
    }
    if (voltage_mv < rail->safe_min_mv || voltage_mv > rail->safe_max_mv) {
        printf("DENIED: %s safe range is %u..%u mV\n", rail->name,
               rail->safe_min_mv, rail->safe_max_mv);
        return false;
    }
    if (!mv_to_code(rail, voltage_mv, &code)) {
        printf("DENIED: %u mV is not representable in %s's current voltage "
               "range\n",
               voltage_mv, rail->name);
        return false;
    }

    // Clear previously latched flags before changing the output.
    if (!read_register(rail->address, status_register(rail),
                       &ignored_status)) {
        printf("ERROR: could not read %s status before write\n", rail->name);
        return false;
    }

    if (!write_register(rail->address, vset_register(rail), code)) {
        printf("ERROR: I2C write failed for %s\n", rail->name);
        return false;
    }

    uint8_t readback;
    if (!read_register(rail->address, vset_register(rail), &readback) ||
        readback != code) {
        printf("ERROR: %s register readback mismatch; restoring\n",
               rail->name);
        restore_rail(rail);
        return false;
    }

    sleep_ms(20);
    uint8_t status;
    if (!read_register(rail->address, status_register(rail), &status)) {
        printf("ERROR: could not read %s status after write; restoring\n",
               rail->name);
        restore_rail(rail);
        return false;
    }
    if ((status & severe_status_mask(rail)) != 0) {
        printf("FAULT: %s status=0x%02x; restoring\n", rail->name, status);
        restore_rail(rail);
        return false;
    }

    printf("SET %s %u mV (register 0x%02x, status 0x%02x)\n", rail->name,
           code_to_mv(rail, code), code, status);
    return true;
}

static void print_rail(const rail_t *rail) {
    uint16_t voltage_mv;
    uint8_t code;

    if (!rail->snapshot_valid || !read_voltage(rail, &voltage_mv, &code)) {
        printf("  %-6s address=0x%02x UNAVAILABLE\n", rail->name,
               rail->address);
        return;
    }

    printf("  %-6s address=0x%02x programmed=%u mV register=0x%02x",
           rail->name, rail->address, voltage_mv, code);
    if (rail->type == CONVERTER_TPS6287B) {
        printf(" step=%u.%03u mV", tps6287b_step_uv(rail) / 1000,
               tps6287b_step_uv(rail) % 1000);
    } else {
        printf(" step=5 mV active-register=VOUT1");
    }
#if VCORE_SWEEP_WRITES_ENABLED
    printf(" safe=%u..%u mV", rail->safe_min_mv, rail->safe_max_mv);
#endif
    printf("\n");
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  status\n");
    printf("  read <vcore0|vcore1|vcore2>\n");
    printf("  arm WRITE\n");
    printf("  disarm\n");
    printf("  set <rail> <millivolts>\n");
    printf("  sweep <rail> <start_mV> <stop_mV> <step_mV> <dwell_ms>\n");
    printf("  restore <rail|all>\n");
    printf("A sweep always restores the startup register value.\n");
}

static void print_status(void) {
    printf("VCORE writes: %s, runtime arm: %s\n",
           VCORE_SWEEP_WRITES_ENABLED ? "COMPILED IN" : "READ ONLY",
           writes_armed ? "ARMED" : "disarmed");
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
        print_rail(&rails[i]);
    }
}

static void restore_all(void) {
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
        restore_rail(&rails[i]);
    }
}

static void handle_command(char *line) {
    char command[16] = {0};
    char rail_name[16] = {0};
    char argument[16] = {0};
    unsigned int voltage_mv;
    unsigned int start_mv;
    unsigned int stop_mv;
    unsigned int step_mv;
    unsigned int dwell_ms;

    if (sscanf(line, "%15s", command) != 1) {
        return;
    }
    for (char *p = command; *p != '\0'; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }

    if (strcmp(command, "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(command, "status") == 0) {
        print_status();
        return;
    }
    if (strcmp(command, "disarm") == 0) {
        writes_armed = false;
        printf("Writes disarmed\n");
        return;
    }
    if (strcmp(command, "arm") == 0) {
        if (sscanf(line, "%*s %15s", argument) == 1 &&
            strcmp(argument, "WRITE") == 0 && VCORE_SWEEP_WRITES_ENABLED) {
            writes_armed = true;
            printf("Writes ARMED until reset or 'disarm'\n");
        } else if (!VCORE_SWEEP_WRITES_ENABLED) {
            printf("DENIED: rebuild with explicit safe limits\n");
        } else {
            printf("DENIED: exact command is 'arm WRITE'\n");
        }
        return;
    }

    if (strcmp(command, "read") == 0) {
        if (sscanf(line, "%*s %15s", rail_name) != 1) {
            printf("Usage: read <rail>\n");
            return;
        }
        rail_t *rail = find_rail(rail_name);
        if (rail == NULL) {
            printf("ERROR: unknown rail '%s'\n", rail_name);
            return;
        }
        print_rail(rail);
        return;
    }

    if (strcmp(command, "restore") == 0) {
        if (sscanf(line, "%*s %15s", rail_name) != 1) {
            printf("Usage: restore <rail|all>\n");
            return;
        }
        if (!VCORE_SWEEP_WRITES_ENABLED || !writes_armed) {
            printf("DENIED: voltage writes are unavailable or disarmed\n");
            return;
        }
        if (strcmp(rail_name, "all") == 0) {
            restore_all();
            return;
        }
        rail_t *rail = find_rail(rail_name);
        if (rail == NULL) {
            printf("ERROR: unknown rail '%s'\n", rail_name);
            return;
        }
        restore_rail(rail);
        return;
    }

    if (strcmp(command, "set") == 0) {
        if (sscanf(line, "%*s %15s %u", rail_name, &voltage_mv) != 2 ||
            voltage_mv > UINT16_MAX) {
            printf("Usage: set <rail> <millivolts>\n");
            return;
        }
        rail_t *rail = find_rail(rail_name);
        if (rail == NULL) {
            printf("ERROR: unknown rail '%s'\n", rail_name);
            return;
        }
        set_voltage(rail, (uint16_t)voltage_mv);
        return;
    }

    if (strcmp(command, "sweep") == 0) {
        if (sscanf(line, "%*s %15s %u %u %u %u", rail_name, &start_mv,
                   &stop_mv, &step_mv, &dwell_ms) != 5 ||
            start_mv > UINT16_MAX || stop_mv > UINT16_MAX ||
            step_mv > UINT16_MAX || step_mv == 0 ||
            dwell_ms > MAX_DWELL_MS) {
            printf("Usage: sweep <rail> <start_mV> <stop_mV> <step_mV> "
                   "<dwell_ms<=60000>\n");
            return;
        }
        rail_t *rail = find_rail(rail_name);
        if (rail == NULL) {
            printf("ERROR: unknown rail '%s'\n", rail_name);
            return;
        }

        const bool ascending = start_mv <= stop_mv;
        unsigned int voltage = start_mv;
        unsigned int points = 0;
        bool success = true;

        while (true) {
            if (++points > MAX_SWEEP_POINTS) {
                printf("ERROR: sweep exceeds %u points\n", MAX_SWEEP_POINTS);
                success = false;
                break;
            }
            if (!set_voltage(rail, (uint16_t)voltage)) {
                success = false;
                break;
            }
            printf("MEASURE %s now; waiting %u ms\n", rail->name, dwell_ms);
            sleep_ms(dwell_ms);

            if (voltage == stop_mv) {
                break;
            }
            if (ascending) {
                if (voltage + step_mv > stop_mv) {
                    printf("ERROR: step does not land exactly on stop voltage\n");
                    success = false;
                    break;
                }
                voltage += step_mv;
            } else {
                if (voltage < stop_mv + step_mv) {
                    printf("ERROR: step does not land exactly on stop voltage\n");
                    success = false;
                    break;
                }
                voltage -= step_mv;
            }
        }

        printf("Sweep %s; restoring startup value\n",
               success ? "complete" : "aborted");
        restore_rail(rail);
        return;
    }

    printf("ERROR: unknown command '%s'; enter 'help'\n", command);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    i2c_init(I2C_PORT, I2C_BAUDRATE_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

    printf("\nFletcher Lake VCORE voltage tool\n");
    printf("I2C0 SDA=GP0 SCL=GP1 at %u Hz\n", I2C_BAUDRATE_HZ);

    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
        if (!snapshot_rail(&rails[i])) {
            printf("WARNING: could not snapshot %s at 0x%02x\n",
                   rails[i].name, rails[i].address);
        }
    }

    print_status();
    print_help();
    printf("> ");

    char line[COMMAND_BUFFER_SIZE];
    size_t length = 0;
    while (true) {
        const int input = getchar_timeout_us(1000);
        if (input == PICO_ERROR_TIMEOUT) {
            tight_loop_contents();
            continue;
        }
        if (input == '\r' || input == '\n') {
            if (length != 0) {
                line[length] = '\0';
                printf("\n");
                handle_command(line);
                length = 0;
            }
            printf("> ");
            continue;
        }
        if ((input == '\b' || input == 0x7f) && length != 0) {
            --length;
            printf("\b \b");
            continue;
        }
        if (isprint(input) && length + 1 < sizeof(line)) {
            line[length++] = (char)input;
            putchar(input);
        }
    }
}
