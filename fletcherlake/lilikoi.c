/*
 * Lilikoi: single RP2350 firmware image behind fl-agent's LilikoiBoardBackend.
 *
 * Machine protocol over USB CDC, one request per line, no echo, no prompt:
 *
 *   request   <token> <command> [args...]\n
 *   response  <token> OK [key=value ...]\n
 *             <token> ERR <CODE> <message>\n
 *   async     * <text>\n                      (banner only)
 *
 * The host picks the token; the firmware echoes it so stale replies are
 * detectable.  Chip-specific values (voltage limits) arrive with each request;
 * the firmware persists nothing but enforces PCB bounds.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pmic.h"
#include "serialtl_phy.h"

#define LILIKOI_FW_VERSION "0.1.0"
#define LILIKOI_PROTO_VERSION 1
#define LINE_MAX 160
#define MAX_ARGS 8
#define MAX_PULSE_MS 10000

enum {
    FPGA_DONE_GPIO = 7,
    FPGA_RESET_GPIO = 8,          /* drive high to assert; high-Z releases */
    DSP24_RESET_REQUEST_GPIO = 9, /* drive low to press the reset button; high-Z releases */
};

/* STANDIN(power-enable): no rail-enable GPIOs are known.  When the schematic
 * identifies them, drive them in cmd_power() and report them in cmd_info(). */

static bool fpga_reset_asserted;
static bool chip_reset_asserted;
static bool sertl_ready;
static char board_uid[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

static void release_gpio(uint gpio) {
    gpio_set_dir(gpio, GPIO_IN);
    gpio_disable_pulls(gpio);
}

static void fpga_reset_set(bool asserted) {
    if (asserted) {
        gpio_put(FPGA_RESET_GPIO, 1);
        gpio_set_dir(FPGA_RESET_GPIO, GPIO_OUT);
    } else {
        release_gpio(FPGA_RESET_GPIO);
    }
    fpga_reset_asserted = asserted;
}

/* Polarity for the installed chip is selected in hardware by SW7 RESET_DIR;
 * the MCU only simulates the button press, so no polarity parameter here. */
static void chip_reset_set(bool asserted) {
    if (asserted) {
        gpio_put(DSP24_RESET_REQUEST_GPIO, 0);
        gpio_set_dir(DSP24_RESET_REQUEST_GPIO, GPIO_OUT);
    } else {
        release_gpio(DSP24_RESET_REQUEST_GPIO);
    }
    chip_reset_asserted = asserted;
}

/* ---- response helpers ---------------------------------------------------- */

static void reply_ok(const char *token, const char *kv) {
    if (kv && *kv) printf("%s OK %s\n", token, kv);
    else printf("%s OK\n", token);
    stdio_flush();
}

static void reply_err(const char *token, const char *code, const char *message) {
    printf("%s ERR %s %s\n", token, code, message);
    stdio_flush();
}

static bool parse_uint(const char *text, unsigned long max, unsigned long *out) {
    if (!text || !*text) return false;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value > max) return false;
    *out = value;
    return true;
}

/* ---- commands ------------------------------------------------------------ */

static void cmd_info(const char *token) {
    char kv[160];
    snprintf(kv, sizeof kv,
             "fw=%s proto=%d board=fletcherlake_rp2350b uid=%s sertl=%s rails=%d "
             "pcb_min_mv=%d pcb_max_mv=%d",
             LILIKOI_FW_VERSION, LILIKOI_PROTO_VERSION, board_uid,
             sertl_ready ? "ready" : "failed", PMIC_RAIL_COUNT, LILIKOI_PCB_RAIL_MIN_MV,
             LILIKOI_PCB_RAIL_MAX_MV);
    reply_ok(token, kv);
}

static void cmd_scan(const char *token) {
    const pmic_device_t *devices;
    unsigned count;
    unsigned found = pmic_scan(&devices, &count);
    char kv[200];
    size_t used = 0;
    for (unsigned i = 0; i < count && used < sizeof kv; ++i) {
        used += (size_t)snprintf(kv + used, sizeof kv - used, "%s=%d ", devices[i].name,
                                 devices[i].present ? 1 : 0);
    }
    if (used < sizeof kv) snprintf(kv + used, sizeof kv - used, "found=%u expected=%u", found, count);
    reply_ok(token, kv);
}

static void format_rail(unsigned rail, const pmic_rail_state_t *state, char *kv, size_t size) {
    snprintf(kv, size, "rail=%u name=%s mv=%u code=0x%02x step_uv=%u status=0x%02x snapshot_mv=%u",
             rail, pmic_rail_name(rail), state->mv, state->code, state->step_uv, state->status,
             state->snapshot_valid ? state->snapshot_mv : 0);
}

static void cmd_rail(const char *token, int argc, char **argv) {
    if (argc < 1) {
        reply_err(token, "BAD_REQUEST", "usage: rail count|snapshot|get <n>|set <n> <mv> <min_mv> <max_mv>|restore <n|all>");
        return;
    }
    char kv[160];
    if (strcmp(argv[0], "count") == 0) {
        snprintf(kv, sizeof kv, "count=%d", PMIC_RAIL_COUNT);
        reply_ok(token, kv);
        return;
    }
    if (strcmp(argv[0], "snapshot") == 0) {
        snprintf(kv, sizeof kv, "snapshotted=%u expected=%d", pmic_snapshot_all(), PMIC_RAIL_COUNT);
        reply_ok(token, kv);
        return;
    }
    if (strcmp(argv[0], "restore") == 0 && argc >= 2 && strcmp(argv[1], "all") == 0) {
        unsigned restored = 0;
        for (unsigned i = 0; i < PMIC_RAIL_COUNT; ++i) restored += pmic_restore(i, NULL) == PMIC_OK;
        snprintf(kv, sizeof kv, "restored=%u expected=%d", restored, PMIC_RAIL_COUNT);
        if (restored == PMIC_RAIL_COUNT) reply_ok(token, kv);
        else reply_err(token, "I2C", kv);
        return;
    }

    unsigned long rail;
    if (argc < 2 || !parse_uint(argv[1], PMIC_RAIL_COUNT - 1, &rail)) {
        reply_err(token, "BAD_RAIL", "rail must be 0..2");
        return;
    }
    pmic_rail_state_t state = {0};
    pmic_result_t result;
    if (strcmp(argv[0], "get") == 0) {
        result = pmic_get((unsigned)rail, &state);
    } else if (strcmp(argv[0], "restore") == 0) {
        result = pmic_restore((unsigned)rail, &state);
    } else if (strcmp(argv[0], "set") == 0) {
        unsigned long mv, min_mv, max_mv;
        if (argc < 5 || !parse_uint(argv[2], 65535, &mv) || !parse_uint(argv[3], 65535, &min_mv) ||
            !parse_uint(argv[4], 65535, &max_mv)) {
            reply_err(token, "BAD_REQUEST", "usage: rail set <n> <mv> <min_mv> <max_mv>");
            return;
        }
        result = pmic_set((unsigned)rail, (uint16_t)mv, (uint16_t)min_mv, (uint16_t)max_mv, &state);
    } else {
        reply_err(token, "BAD_REQUEST", "unknown rail action");
        return;
    }
    format_rail((unsigned)rail, &state, kv, sizeof kv);
    if (result == PMIC_OK) reply_ok(token, kv);
    else reply_err(token, pmic_result_name(result), kv);
}

static void cmd_fpga(const char *token, int argc, char **argv) {
    if (argc >= 1 && strcmp(argv[0], "done") == 0) {
        char kv[32];
        snprintf(kv, sizeof kv, "done=%u", gpio_get(FPGA_DONE_GPIO));
        reply_ok(token, kv);
        return;
    }
    reply_err(token, "BAD_REQUEST", "usage: fpga done");
}

static void cmd_reset(const char *token, int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[0], "fpga") != 0 && strcmp(argv[0], "chip") != 0)) {
        reply_err(token, "BAD_REQUEST", "usage: reset fpga|chip assert|release|pulse [ms]");
        return;
    }
    const bool fpga = strcmp(argv[0], "fpga") == 0;
    void (*set_reset)(bool) = fpga ? fpga_reset_set : chip_reset_set;
    if (strcmp(argv[1], "assert") == 0) {
        set_reset(true);
    } else if (strcmp(argv[1], "release") == 0) {
        set_reset(false);
    } else if (strcmp(argv[1], "pulse") == 0) {
        unsigned long ms = 10;
        if (argc >= 3 && (!parse_uint(argv[2], MAX_PULSE_MS, &ms) || ms == 0)) {
            reply_err(token, "BAD_REQUEST", "pulse must be 1..10000 ms");
            return;
        }
        set_reset(true);
        sleep_ms((uint32_t)ms);
        set_reset(false);
    } else {
        reply_err(token, "BAD_REQUEST", "usage: reset fpga|chip assert|release|pulse [ms]");
        return;
    }
    char kv[48];
    snprintf(kv, sizeof kv, "fpga_reset=%d chip_reset=%d done=%u", fpga_reset_asserted,
             chip_reset_asserted, gpio_get(FPGA_DONE_GPIO));
    reply_ok(token, kv);
}

static void cmd_sertl(const char *token, int argc, char **argv) {
    serialtl_phy_stats_t stats;
    serialtl_phy_get_stats(&stats);
    char kv[200];
    if (argc < 1 || strcmp(argv[0], "status") == 0) {
        snprintf(kv, sizeof kv,
                 "ready=%d clk=%u out_valid=%u out_ready=%u in_valid=%u in_ready=%u rx_phits=%lu "
                 "tx_phits=%lu",
                 sertl_ready, gpio_get(SERIALTL_CLOCK_GPIO), gpio_get(SERIALTL_FPGA_TO_MCU_VALID_GPIO),
                 gpio_get(SERIALTL_MCU_READY_GPIO), gpio_get(SERIALTL_MCU_TO_FPGA_VALID_GPIO),
                 gpio_get(SERIALTL_FPGA_READY_GPIO), (unsigned long)stats.rx_phits,
                 (unsigned long)stats.tx_phits);
        reply_ok(token, kv);
    } else if (strcmp(argv[0], "clear") == 0) {
        serialtl_phy_clear_stats();
        reply_ok(token, "cleared=1");
    } else if (strcmp(argv[0], "rx") == 0) {
        unsigned long count = 16;
        if (argc >= 2 && (!parse_uint(argv[1], 32, &count) || count == 0)) {
            reply_err(token, "BAD_REQUEST", "count must be 1..32");
            return;
        }
        size_t used = (size_t)snprintf(kv, sizeof kv, "phits=");
        uint8_t phit;
        unsigned received = 0;
        while (received < count && used + 2 < sizeof kv && serialtl_phy_try_receive(&phit)) {
            kv[used++] = "0123456789abcdef"[phit & 0x0f];
            ++received;
        }
        used += (size_t)snprintf(kv + used, sizeof kv - used, " count=%u", received);
        reply_ok(token, kv);
    } else {
        reply_err(token, "BAD_REQUEST", "usage: sertl status|clear|rx [count]");
    }
}

static void cmd_sense(const char *token, int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* STANDIN(sense-parts): see pmic.c; register maps for the ADC/current monitors
     * are unknown, so the host receives an explicit failure instead of a guess. */
    reply_err(token, "UNSUPPORTED", "sense readers not implemented: part numbers unknown");
}

static void cmd_power(const char *token, int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* STANDIN(power-enable): rail enables unknown; the host composes power_on/off
     * from resets and rail restores until this is implemented. */
    reply_err(token, "UNSUPPORTED", "power sequencing not implemented: enable GPIOs unknown");
}

static void handle_line(char *line) {
    char *save;
    char *token = strtok_r(line, " \t", &save);
    if (!token) return;
    for (const char *p = token; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            reply_err("?", "BAD_REQUEST", "token must be [A-Za-z0-9_-]");
            return;
        }
    }
    char *command = strtok_r(NULL, " \t", &save);
    if (!command) {
        reply_err(token, "BAD_REQUEST", "missing command");
        return;
    }
    char *argv[MAX_ARGS];
    int argc = 0;
    char *arg;
    while (argc < MAX_ARGS && (arg = strtok_r(NULL, " \t", &save)) != NULL) argv[argc++] = arg;

    if (strcmp(command, "ping") == 0) reply_ok(token, NULL);
    else if (strcmp(command, "info") == 0) cmd_info(token);
    else if (strcmp(command, "scan") == 0) cmd_scan(token);
    else if (strcmp(command, "rail") == 0) cmd_rail(token, argc, argv);
    else if (strcmp(command, "fpga") == 0) cmd_fpga(token, argc, argv);
    else if (strcmp(command, "reset") == 0) cmd_reset(token, argc, argv);
    else if (strcmp(command, "sertl") == 0) cmd_sertl(token, argc, argv);
    else if (strcmp(command, "sense") == 0) cmd_sense(token, argc, argv);
    else if (strcmp(command, "power") == 0) cmd_power(token, argc, argv);
    else reply_err(token, "UNKNOWN", "unknown command");
}

int main(void) {
    stdio_init_all();
    pico_get_unique_board_id_string(board_uid, sizeof board_uid);

    gpio_init(FPGA_DONE_GPIO);
    release_gpio(FPGA_DONE_GPIO);
    gpio_init(FPGA_RESET_GPIO);
    gpio_put(FPGA_RESET_GPIO, 0);
    fpga_reset_set(false);
    gpio_init(DSP24_RESET_REQUEST_GPIO);
    gpio_put(DSP24_RESET_REQUEST_GPIO, 0);
    chip_reset_set(false); /* Safe state: both resets released; never pulse the chip on boot. */

    pmic_init();
    pmic_snapshot_all();
    sertl_ready = serialtl_phy_init();

    printf("* lilikoi %s proto=%d uid=%s ready\n", LILIKOI_FW_VERSION, LILIKOI_PROTO_VERSION, board_uid);
    stdio_flush();

    char line[LINE_MAX];
    size_t used = 0;
    bool overflow = false;
    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            if (overflow) {
                reply_err("?", "BAD_REQUEST", "line too long");
            } else if (used) {
                line[used] = '\0';
                handle_line(line);
            }
            used = 0;
            overflow = false;
        } else if (used + 1 < sizeof line) {
            line[used++] = (char)c;
        } else {
            overflow = true;
        }
    }
}
