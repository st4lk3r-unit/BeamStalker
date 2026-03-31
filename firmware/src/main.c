#include "beamstalker.h"

int main(void) {
    bs_init();
    for (;;) bs_run();
    return 0;
}
