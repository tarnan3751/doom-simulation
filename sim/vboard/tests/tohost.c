// --isa-test fixtures: store VAL to tohost (1 = pass).
#include "t.h"
volatile uint32_t tohost;
volatile uint32_t fromhost;
int main(void) { tohost = VAL; for (;;) ; }
