#include <stdint.h>

int64_t tl_sum_bytes(const uint8_t* p, int64_t n) {
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += p[i];
    return s;
}

void tl_fill(int64_t* out, int64_t v) { *out = v; }

int32_t tl_is_even(int64_t v) { return v % 2 == 0; }
