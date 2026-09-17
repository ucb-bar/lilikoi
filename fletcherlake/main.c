#include <stdio.h>
#include "pico/stdlib.h"

int main() {
    stdio_init_all();

    // Give the computer time to enumerate the USB serial device
    sleep_ms(2000);

    while (true) {
        printf("Hello, RP2350!\n");
        sleep_ms(1000);
    }
}
