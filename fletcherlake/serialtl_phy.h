#ifndef FLETCHERLAKE_SERIALTL_PHY_H
#define FLETCHERLAKE_SERIALTL_PHY_H

#include <stdbool.h>
#include <stdint.h>

enum {
    SERIALTL_CLOCK_GPIO = 16,
    SERIALTL_FPGA_TO_MCU_DATA_GPIO = 12,
    SERIALTL_FPGA_TO_MCU_VALID_GPIO = 17,
    SERIALTL_MCU_READY_GPIO = 18,
    SERIALTL_MCU_TO_FPGA_DATA_GPIO = 32,
    SERIALTL_MCU_TO_FPGA_VALID_GPIO = 36,
    SERIALTL_FPGA_READY_GPIO = 37,
};

typedef struct {
    uint32_t rx_phits;
    uint32_t tx_phits;
    uint32_t rx_overflow;
} serialtl_phy_stats_t;

bool serialtl_phy_init(void);
bool serialtl_phy_try_receive(uint8_t *phit);
bool serialtl_phy_try_send(uint8_t phit);
void serialtl_phy_get_stats(serialtl_phy_stats_t *stats);
void serialtl_phy_clear_stats(void);

#endif
