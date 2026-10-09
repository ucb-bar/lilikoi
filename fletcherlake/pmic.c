/* Regulator driver carried over from vcore_sweep.c, minus the interactive arming. */
#include "pmic.h"

#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define I2C_PORT i2c0
#define I2C_TIMEOUT_US 10000

#define TPS6287B_REG_VSET 0x00
#define TPS6287B_REG_CONTROL2 0x02
#define TPS6287B_REG_STATUS 0x04
#define TPS6287B_STATUS_SEVERE_MASK 0x3c

#define TPS62868_REG_VOUT1 0x01
#define TPS62868_REG_STATUS 0x05
#define TPS62868_STATUS_FAULT_MASK 0x19

typedef enum {
    CONVERTER_TPS6287B,
    CONVERTER_TPS628681A,
} converter_type_t;

typedef struct {
    const char *name;
    uint8_t address;
    converter_type_t type;
    uint8_t original_vset;
    uint8_t control2;
    bool snapshot_valid;
} rail_t;

static rail_t rails[PMIC_RAIL_COUNT] = {
    {"vcore0", 0x45, CONVERTER_TPS6287B, 0, 0, false},
    {"vcore1", 0x47, CONVERTER_TPS628681A, 0, 0, false},
    {"vcore2", 0x40, CONVERTER_TPS628681A, 0, 0, false},
};

/* STANDIN(sense-parts): VSENSE/ISENSE part numbers and register maps are unknown;
 * only presence is checked.  Add register readers here once the schematic says
 * what sits at 0x10/0x1f/0x48/0x49. */
static pmic_device_t devices[] = {
    {"isense0", 0x10, false},
    {"isense1", 0x1f, false},
    {"vcore0", 0x45, false},
    {"vcore1", 0x47, false},
    {"vcore2", 0x40, false},
    {"vsense_pmic", 0x48, false},
    {"vsense_vcore", 0x49, false},
};

static bool read_register(uint8_t address, uint8_t reg, uint8_t *value) {
    if (i2c_write_timeout_us(I2C_PORT, address, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, address, value, 1, false, I2C_TIMEOUT_US) == 1;
}

static bool write_register(uint8_t address, uint8_t reg, uint8_t value) {
    const uint8_t command[] = {reg, value};
    return i2c_write_timeout_us(I2C_PORT, address, command, sizeof(command), false,
                                I2C_TIMEOUT_US) == (int)sizeof(command);
}

static uint16_t step_uv(const rail_t *rail) {
    if (rail->type != CONVERTER_TPS6287B) return 5000; /* TPS628681A: 5 mV/code */
    const uint8_t range = (rail->control2 >> 2) & 0x03;
    if (range == 0) return 1250;
    if (range == 1) return 2500;
    return 5000;
}

static uint16_t code_to_mv(const rail_t *rail, uint8_t code) {
    const uint32_t uv = 400000u + (uint32_t)code * step_uv(rail);
    return (uint16_t)((uv + 500u) / 1000u);
}

static bool mv_to_code(const rail_t *rail, uint16_t mv, uint8_t *code) {
    const uint32_t uv = (uint32_t)mv * 1000u;
    if (uv < 400000u) return false;
    const uint32_t offset = uv - 400000u;
    const uint16_t step = step_uv(rail);
    if (offset % step != 0 || offset / step > 255u) return false;
    *code = (uint8_t)(offset / step);
    return true;
}

static uint8_t vset_register(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B ? TPS6287B_REG_VSET : TPS62868_REG_VOUT1;
}

static uint8_t status_register(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B ? TPS6287B_REG_STATUS : TPS62868_REG_STATUS;
}

static uint8_t severe_mask(const rail_t *rail) {
    return rail->type == CONVERTER_TPS6287B ? TPS6287B_STATUS_SEVERE_MASK
                                             : TPS62868_STATUS_FAULT_MASK;
}

static void fill_state(const rail_t *rail, uint8_t code, uint8_t status,
                       pmic_rail_state_t *state) {
    if (!state) return;
    state->mv = code_to_mv(rail, code);
    state->code = code;
    state->step_uv = step_uv(rail);
    state->status = status;
    state->snapshot_valid = rail->snapshot_valid;
    state->snapshot_mv = rail->snapshot_valid ? code_to_mv(rail, rail->original_vset) : 0;
}

void pmic_init(void) {
    i2c_init(I2C_PORT, PMIC_I2C_BAUDRATE_HZ);
    gpio_set_function(PMIC_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PMIC_I2C_SCL_PIN, GPIO_FUNC_I2C);
}

const char *pmic_result_name(pmic_result_t result) {
    switch (result) {
    case PMIC_OK: return "OK";
    case PMIC_ERR_BAD_RAIL: return "BAD_RAIL";
    case PMIC_ERR_NO_SNAPSHOT: return "NO_SNAPSHOT";
    case PMIC_ERR_RANGE: return "RANGE";
    case PMIC_ERR_PCB_BOUND: return "PCB_BOUND";
    case PMIC_ERR_UNREPRESENTABLE: return "UNREPRESENTABLE";
    case PMIC_ERR_I2C: return "I2C";
    case PMIC_ERR_FAULT: return "FAULT";
    case PMIC_ERR_READBACK: return "READBACK";
    }
    return "UNKNOWN";
}

const char *pmic_rail_name(unsigned rail) {
    return rail < PMIC_RAIL_COUNT ? rails[rail].name : "?";
}

bool pmic_snapshot(unsigned rail_index) {
    if (rail_index >= PMIC_RAIL_COUNT) return false;
    rail_t *rail = &rails[rail_index];
    rail->snapshot_valid = false;
    if (rail->type == CONVERTER_TPS6287B &&
        !read_register(rail->address, TPS6287B_REG_CONTROL2, &rail->control2)) {
        return false;
    }
    if (!read_register(rail->address, vset_register(rail), &rail->original_vset)) return false;
    rail->snapshot_valid = true;
    return true;
}

unsigned pmic_snapshot_all(void) {
    unsigned ok = 0;
    for (unsigned i = 0; i < PMIC_RAIL_COUNT; ++i) ok += pmic_snapshot(i) ? 1 : 0;
    return ok;
}

pmic_result_t pmic_get(unsigned rail_index, pmic_rail_state_t *state) {
    if (rail_index >= PMIC_RAIL_COUNT) return PMIC_ERR_BAD_RAIL;
    const rail_t *rail = &rails[rail_index];
    uint8_t code, status;
    if (!read_register(rail->address, vset_register(rail), &code)) return PMIC_ERR_I2C;
    if (!read_register(rail->address, status_register(rail), &status)) return PMIC_ERR_I2C;
    fill_state(rail, code, status, state);
    return PMIC_OK;
}

pmic_result_t pmic_restore(unsigned rail_index, pmic_rail_state_t *state) {
    if (rail_index >= PMIC_RAIL_COUNT) return PMIC_ERR_BAD_RAIL;
    const rail_t *rail = &rails[rail_index];
    if (!rail->snapshot_valid) return PMIC_ERR_NO_SNAPSHOT;
    if (!write_register(rail->address, vset_register(rail), rail->original_vset)) {
        return PMIC_ERR_I2C;
    }
    uint8_t readback;
    if (!read_register(rail->address, vset_register(rail), &readback)) return PMIC_ERR_I2C;
    if (readback != rail->original_vset) return PMIC_ERR_READBACK;
    fill_state(rail, readback, 0, state);
    return PMIC_OK;
}

pmic_result_t pmic_set(unsigned rail_index, uint16_t mv, uint16_t min_mv, uint16_t max_mv,
                       pmic_rail_state_t *state) {
    if (rail_index >= PMIC_RAIL_COUNT) return PMIC_ERR_BAD_RAIL;
    const rail_t *rail = &rails[rail_index];
    if (!rail->snapshot_valid) return PMIC_ERR_NO_SNAPSHOT;
    /* The host's chip limits must themselves sit inside what the PCB tolerates. */
    if (min_mv > max_mv || min_mv < LILIKOI_PCB_RAIL_MIN_MV || max_mv > LILIKOI_PCB_RAIL_MAX_MV) {
        return PMIC_ERR_PCB_BOUND;
    }
    if (mv < min_mv || mv > max_mv) return PMIC_ERR_RANGE;

    uint8_t code;
    if (!mv_to_code(rail, mv, &code)) return PMIC_ERR_UNREPRESENTABLE;

    uint8_t status;
    /* Reading status clears latched flags so the post-write check is fresh. */
    if (!read_register(rail->address, status_register(rail), &status)) return PMIC_ERR_I2C;
    if (!write_register(rail->address, vset_register(rail), code)) return PMIC_ERR_I2C;

    uint8_t readback;
    if (!read_register(rail->address, vset_register(rail), &readback) || readback != code) {
        pmic_restore(rail_index, NULL);
        return PMIC_ERR_READBACK;
    }
    sleep_ms(20);
    if (!read_register(rail->address, status_register(rail), &status)) {
        pmic_restore(rail_index, NULL);
        return PMIC_ERR_I2C;
    }
    if (status & severe_mask(rail)) {
        pmic_restore(rail_index, NULL);
        fill_state(rail, code, status, state);
        return PMIC_ERR_FAULT;
    }
    fill_state(rail, code, status, state);
    return PMIC_OK;
}

unsigned pmic_scan(const pmic_device_t **out, unsigned *count) {
    unsigned found = 0;
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
        uint8_t ignored;
        /* A one-byte read only confirms the address ACK; the value is discarded. */
        devices[i].present = i2c_read_timeout_us(I2C_PORT, devices[i].address, &ignored, 1,
                                                 false, I2C_TIMEOUT_US) == 1;
        found += devices[i].present ? 1 : 0;
    }
    if (out) *out = devices;
    if (count) *count = sizeof(devices) / sizeof(devices[0]);
    return found;
}
