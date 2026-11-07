/* kernel.c - a small mixed integer workload for CPI / predictor measurements:
 * insertion sort, bitwise CRC-32, matrix multiply, function-pointer dispatch,
 * division-heavy fixed-point loop, byte copy. Returns a checksum. */
#include <stdint.h>

#define N_SORT 160
#define N_MAT  10
#define N_CRC  768

static uint32_t seed = 0x12345678u;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed; }

static int32_t  arr[N_SORT];
static int32_t  ma[N_MAT][N_MAT], mb[N_MAT][N_MAT], mc[N_MAT][N_MAT];
static uint8_t  buf[N_CRC], dst[N_CRC];

static void isort(int32_t *a, int n) {
    for (int i = 1; i < n; i++) {
        int32_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static uint32_t crc32(const uint8_t *p, int n) {
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static void matmul(void) {
    for (int i = 0; i < N_MAT; i++)
        for (int j = 0; j < N_MAT; j++) {
            int32_t s = 0;
            for (int k = 0; k < N_MAT; k++) s += ma[i][k] * mb[k][j];
            mc[i][j] = s;
        }
}

static int32_t op_add(int32_t a, int32_t b) { return a + b; }
static int32_t op_sub(int32_t a, int32_t b) { return a - b; }
static int32_t op_xor(int32_t a, int32_t b) { return a ^ b; }
static int32_t op_mul(int32_t a, int32_t b) { return a * b; }
static int32_t (*const ops[4])(int32_t, int32_t) = {op_add, op_sub, op_xor, op_mul};

static int32_t fixdiv(int32_t a, int32_t b) { return b ? (int32_t)(((int64_t)a << 8) / b) : 0; }

int main(void) {
    uint32_t sum = 0;
    for (int i = 0; i < N_SORT; i++) arr[i] = (int32_t)(rnd() >> 8) - (1 << 23);
    isort(arr, N_SORT);
    for (int i = 0; i < N_SORT; i += 7) sum += (uint32_t)arr[i];

    for (int i = 0; i < N_CRC; i++) buf[i] = (uint8_t)rnd();
    sum ^= crc32(buf, N_CRC);

    for (int i = 0; i < N_MAT; i++)
        for (int j = 0; j < N_MAT; j++) { ma[i][j] = (int32_t)(rnd() & 0xff) - 128; mb[i][j] = (int32_t)(rnd() & 0xff); }
    matmul();
    for (int i = 0; i < N_MAT; i++) sum += (uint32_t)mc[i][i];

    int32_t acc = 1;
    for (int i = 0; i < 400; i++) acc = ops[rnd() & 3](acc, (int32_t)(rnd() & 0xffff));
    sum += (uint32_t)acc;

    int32_t q = 0;
    for (int i = 1; i < 100; i++) q += fixdiv((int32_t)(rnd() & 0xfffff), (int32_t)(rnd() & 0xfff) + 1) / i;
    sum += (uint32_t)q;

    for (int i = 0; i < N_CRC; i++) dst[i] = buf[N_CRC - 1 - i];
    for (int i = 0; i < N_CRC; i += 13) sum += dst[i];
    return (int)sum;
}
