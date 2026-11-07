#include "t.h"
volatile uint32_t kres[24];
volatile uint32_t kst[4];
int main(void)
{
    kst[0] = R32(KBD_STATUS);
    kst[1] = R32(KBD_STATUS);          /* status has no side effect */
    for (int i = 0; i < 20; i++) kres[i] = R32(KBD_DATA);
    kst[2] = R32(KBD_STATUS);
    kst[3] = R32(KBD_DATA);
    return 0;
}
