/* Fletcher Lake PMIC bus: VCORE regulators and sense-device presence. */
#ifndef FLETCHERLAKE_PMIC_H
#define FLETCHERLAKE_PMIC_H

#include <stdbool.h>
#include <stdint.h>

/* I2C0 on GP0/GP1 is the schematic's SDA_PMIC/SCL_PMIC bus. */
#define PMIC_I2C_SDA_PIN 0
#define PMIC_I2C_SCL_PIN 1
#define PMIC_I2C_BAUDRATE_HZ 100000

/*
 * Absolute bounds the PCB itself tolerates, independent of the installed chip.
 * Chip-specific limits arrive from the host on every write and must fit
 * inside these.  Overridable with -DLILIKOI_PCB_RAIL_MIN_MV=... at build time.
 */
#ifndef LILIKOI_PCB_RAIL_MIN_MV
#define LILIKOI_PCB_RAIL_MIN_MV 400  /* STANDIN(pcb-bounds): regulator floor, not a board rating */
#endif
#ifndef LILIKOI_PCB_RAIL_MAX_MV
#define LILIKOI_PCB_RAIL_MAX_MV 1200 /* STANDIN(pcb-bounds): confirm against the VCORE trace/load rating */
#endif

#define PMIC_RAIL_COUNT 3

typedef enum {
    PMIC_OK = 0,
    PMIC_ERR_BAD_RAIL,
    PMIC_ERR_NO_SNAPSHOT,
    PMIC_ERR_RANGE,          /* outside the host-supplied chip limits */
    PMIC_ERR_PCB_BOUND,      /* host limits or request exceed PCB bounds */
    PMIC_ERR_UNREPRESENTABLE,/* not an exact regulator step */
    PMIC_ERR_I2C,
    PMIC_ERR_FAULT,          /* regulator reported a fault after the write */
    PMIC_ERR_READBACK,
} pmic_result_t;

typedef struct {
    uint16_t mv;
    uint8_t code;
    uint16_t step_uv;
    uint8_t status;
    bool snapshot_valid;
    uint16_t snapshot_mv;
} pmic_rail_state_t;

typedef struct {
    const char *name;
    uint8_t address;
    bool present;
} pmic_device_t;

void pmic_init(void);
const char *pmic_result_name(pmic_result_t result);

/* Rail names are vcore0..vcore2; index matches the scheduler's integer rail. */
const char *pmic_rail_name(unsigned rail);
bool pmic_snapshot(unsigned rail);
unsigned pmic_snapshot_all(void);
pmic_result_t pmic_get(unsigned rail, pmic_rail_state_t *state);
pmic_result_t pmic_set(unsigned rail, uint16_t mv, uint16_t min_mv, uint16_t max_mv,
                       pmic_rail_state_t *state);
pmic_result_t pmic_restore(unsigned rail, pmic_rail_state_t *state);

/* Probe every expected device; returns the number that acknowledged. */
unsigned pmic_scan(const pmic_device_t **devices, unsigned *count);

#endif
