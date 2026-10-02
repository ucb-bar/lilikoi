#include "serialtl_phy.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "serialtl_phy.pio.h"

static const PIO rx_pio = pio0;
static const PIO tx_pio = pio1;
static uint rx_sm;
static uint tx_sm;
static serialtl_phy_stats_t phy_stats;
static bool initialized;

bool serialtl_phy_init(void) {
#if !PICO_RP2350 || PICO_RP2350A
#error "Fletcher Lake SerialTL requires the RP2350B 48-GPIO package"
#endif
    if (initialized) return true;

    if (pio_set_gpio_base(rx_pio, 0) != PICO_OK ||
        pio_set_gpio_base(tx_pio, 16) != PICO_OK) {
        return false;
    }

    int rx_claim = pio_claim_unused_sm(rx_pio, false);
    int tx_claim = pio_claim_unused_sm(tx_pio, false);
    if (rx_claim < 0 || tx_claim < 0) return false;
    rx_sm = (uint)rx_claim;
    tx_sm = (uint)tx_claim;

    uint rx_offset = pio_add_program(rx_pio, &serialtl_rx_program);
    uint tx_offset = pio_add_program(tx_pio, &serialtl_tx_program);
    serialtl_rx_program_init(rx_pio, rx_sm, rx_offset);
    serialtl_tx_program_init(tx_pio, tx_sm, tx_offset);
    initialized = true;
    return true;
}

bool serialtl_phy_try_receive(uint8_t *phit) {
    if (!initialized || pio_sm_is_rx_fifo_empty(rx_pio, rx_sm)) return false;
    *phit = (uint8_t)((pio_sm_get(rx_pio, rx_sm) >> 28) & 0x0fu);
    ++phy_stats.rx_phits;
    return true;
}

bool serialtl_phy_try_send(uint8_t phit) {
    if (!initialized || pio_sm_is_tx_fifo_full(tx_pio, tx_sm)) return false;
    pio_sm_put(tx_pio, tx_sm, 0x10u | (phit & 0x0fu));
    ++phy_stats.tx_phits;
    return true;
}

void serialtl_phy_get_stats(serialtl_phy_stats_t *stats) {
    *stats = phy_stats;
}

void serialtl_phy_clear_stats(void) {
    phy_stats = (serialtl_phy_stats_t){0};
}
