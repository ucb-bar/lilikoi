#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "serialtl_phy.h"

enum {
    FPGA_DONE_GPIO = 7,
    FPGA_RESET_GPIO = 8,
    DSP24_RESET_REQUEST_GPIO = 9,
};

static bool fpga_reset_asserted;
static bool chip_reset_asserted;

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

static void chip_reset_set(bool asserted) {
    if (asserted) {
        gpio_put(DSP24_RESET_REQUEST_GPIO, 0);
        gpio_set_dir(DSP24_RESET_REQUEST_GPIO, GPIO_OUT);
    } else {
        release_gpio(DSP24_RESET_REQUEST_GPIO);
    }
    chip_reset_asserted = asserted;
}

static void print_help(void) {
    puts("commands:");
    puts("  status");
    puts("  sertl status | clear | rx [count]");
    puts("  reset fpga assert | release | pulse <ms>");
    puts("  reset chip assert | release | pulse <ms>");
    puts("  help");
}

static void print_status(void) {
    serialtl_phy_stats_t stats;
    serialtl_phy_get_stats(&stats);
    printf("FPGA_DONE GP7=%u, FPGA reset=%s, DSP24 reset request=%s\n",
           gpio_get(FPGA_DONE_GPIO),
           fpga_reset_asserted ? "ASSERTED (GP8 driven high)" : "released (GP8 high-Z)",
           chip_reset_asserted ? "ASSERTED (GP9 driven low)" : "released (GP9 high-Z)");
    printf("SerialTL clk=%u out_valid=%u out_ready=%u in_valid=%u in_ready=%u"
           " rx_phits=%lu tx_phits=%lu\n",
           gpio_get(SERIALTL_CLOCK_GPIO),
           gpio_get(SERIALTL_FPGA_TO_MCU_VALID_GPIO),
           gpio_get(SERIALTL_MCU_READY_GPIO),
           gpio_get(SERIALTL_MCU_TO_FPGA_VALID_GPIO),
           gpio_get(SERIALTL_FPGA_READY_GPIO),
           (unsigned long)stats.rx_phits, (unsigned long)stats.tx_phits);
}

static unsigned parse_ms(const char *text, unsigned default_ms) {
    if (!text || !*text) return default_ms;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    return (*end == '\0' && value > 0 && value <= 10000) ? (unsigned)value : 0;
}

static void handle_reset(char *target, char *action, char *duration) {
    if (!target || !action ||
        (strcmp(target, "fpga") != 0 && strcmp(target, "chip") != 0)) {
        puts("usage: reset fpga|chip assert|release|pulse [ms]");
        return;
    }
    void (*set_reset)(bool) = strcmp(target, "fpga") == 0 ? fpga_reset_set : chip_reset_set;
    if (strcmp(action, "assert") == 0) {
        set_reset(true);
    } else if (strcmp(action, "release") == 0) {
        set_reset(false);
    } else if (strcmp(action, "pulse") == 0) {
        unsigned ms = parse_ms(duration, 10);
        if (!ms) {
            puts("pulse must be 1..10000 ms");
            return;
        }
        if (strcmp(target, "chip") == 0)
            puts("WARNING: confirm SW7 RESET_DIR for the installed DSP24 before testing reset.");
        set_reset(true);
        sleep_ms(ms);
        set_reset(false);
    } else {
        puts("usage: reset fpga|chip assert|release|pulse [ms]");
        return;
    }
    print_status();
}

static void handle_sertl(char *action, char *argument) {
    if (!action || strcmp(action, "status") == 0) {
        print_status();
    } else if (strcmp(action, "clear") == 0) {
        serialtl_phy_clear_stats();
        puts("SerialTL counters cleared");
    } else if (strcmp(action, "rx") == 0) {
        unsigned count = parse_ms(argument, 16);
        if (!count || count > 256) {
            puts("count must be 1..256");
            return;
        }
        unsigned received = 0;
        uint8_t phit;
        while (received < count && serialtl_phy_try_receive(&phit)) {
            printf("%x%c", phit, received + 1 == count ? '\n' : ' ');
            ++received;
        }
        if (!received) puts("no received phits queued");
        else if (received < count) printf("(%u phits)\n", received);
    } else {
        puts("usage: sertl status|clear|rx [count]");
    }
}

static void handle_command(char *line) {
    char *save;
    char *command = strtok_r(line, " \t", &save);
    if (!command) return;
    if (strcmp(command, "help") == 0) {
        print_help();
    } else if (strcmp(command, "status") == 0) {
        print_status();
    } else if (strcmp(command, "reset") == 0) {
        handle_reset(strtok_r(NULL, " \t", &save), strtok_r(NULL, " \t", &save),
                     strtok_r(NULL, " \t", &save));
    } else if (strcmp(command, "sertl") == 0) {
        handle_sertl(strtok_r(NULL, " \t", &save), strtok_r(NULL, " \t", &save));
    } else {
        printf("unknown command '%s'; type help\n", command);
    }
}

int main(void) {
    stdio_init_all();

    gpio_init(FPGA_DONE_GPIO);
    release_gpio(FPGA_DONE_GPIO);
    gpio_init(FPGA_RESET_GPIO);
    gpio_put(FPGA_RESET_GPIO, 0);
    fpga_reset_set(false);
    gpio_init(DSP24_RESET_REQUEST_GPIO);
    gpio_put(DSP24_RESET_REQUEST_GPIO, 0);
    chip_reset_set(false); /* Never pulse the physical DSP24 automatically. */

    sleep_ms(1500);
    puts("Fletcher Lake RP2350 diagnostics");
    puts("DSP24 reset is RELEASED. Confirm SW7 RESET_DIR before hardware reset testing.");
    printf("SerialTL PIO: %s (4-bit, FPGA-clocked at 5 MHz)\n",
           serialtl_phy_init() ? "ready" : "FAILED");
    print_help();
    print_status();
    printf("> ");
    fflush(stdout);

    char line[128];
    size_t used = 0;
    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r' || c == '\n') {
            if (used) {
                putchar('\n');
                line[used] = '\0';
                handle_command(line);
                used = 0;
                printf("> ");
                fflush(stdout);
            }
        } else if ((c == '\b' || c == 127) && used) {
            --used;
            printf("\b \b");
            fflush(stdout);
        } else if (isprint(c) && used + 1 < sizeof(line)) {
            char echoed = (char)tolower((unsigned char)c);
            line[used++] = echoed;
            putchar(echoed);
            fflush(stdout);
        }
    }
}
