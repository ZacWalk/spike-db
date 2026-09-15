#include <limits.h>

int main(void)
{
    volatile int maximum = INT_MAX;
    volatile int overflow = maximum + 1;
    (void)overflow;
    return 0;
}
