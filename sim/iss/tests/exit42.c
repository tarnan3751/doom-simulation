#include "t.h"
int main(void)
{
    const char *s = "ok\n";
    while (*s) {
        while (!(R32(UART_STATUS) & 1)) ;
        R32(UART_TX) = (uint8_t)*s++;
    }
    return 0x12A;   /* low byte 42 */
}
