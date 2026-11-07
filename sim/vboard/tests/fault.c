// Jumps to an unmapped address: the engine reports a fault (exit status 125).
#include "t.h"
int main(void) { puts_("about to fault\n"); ((void (*)(void))0x1000)(); return 0; }
