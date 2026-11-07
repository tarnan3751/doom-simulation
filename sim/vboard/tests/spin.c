// Runs forever; prints one dot first so the run can be recognised.
#include "t.h"
int main(void) {
    putc_('.');
    for (;;) __asm__ volatile("");
}
