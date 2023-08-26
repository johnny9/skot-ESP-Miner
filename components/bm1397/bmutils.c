#include "bmutils.h"

int largest_power_of_two(int num) {
    int power = 0;

    while (num > 1) {
        num = num >> 1;
        power++;
    }

    return 1 << power;
}