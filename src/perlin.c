// Pure fixed-point Perlin noise — no floating point anywhere.
//
// The noise lattice works in 16.16 fixed-point space (N = 1<<16).
// Pixel coordinates arrive in 0.1 mm units and are scaled to lattice space
// by dividing by scale_t (also in 0.1 mm), so scale_t controls feature size.

#include "perlin.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Lookup tables
// ---------------------------------------------------------------------------

static const uint8_t s_perm[256] = {
    151,160,137, 91, 90, 15,131, 13,201, 95, 96, 53,194,233,  7,225,
    140, 36,103, 30, 69,142,  8, 99, 37,240, 21, 10, 23,190,  6,148,
    247,120,234, 75,  0, 26,197, 62, 94,252,219,203,117, 35, 11, 32,
     57,177, 33, 88,237,149, 56, 87,174, 20,125,136,171,168, 68,175,
     74,165, 71,134,139, 48, 27,166, 77,146,158,231, 83,111,229,122,
     60,211,133,230,220,105, 92, 41, 55, 46,245, 40,244,102,143, 54,
     65, 25, 63,161,  1,216, 80, 73,209, 76,132,187,208, 89, 18,169,
    200,196,135,130,116,188,159, 86,164,100,109,198,173,186,  3, 64,
     52,217,226,250,124,123,  5,202, 38,147,118,126,255, 82, 85,212,
    207,206, 59,227, 47, 16, 58, 17,182,189, 28, 42,223,183,170,213,
    119,248,152,  2, 44,154,163, 70,221,153,101,155,167, 43,172,  9,
    129, 22, 39,253, 19, 98,108,110, 79,113,224,232,178,185,112,104,
    218,246, 97,228,251, 34,242,193,238,210,144, 12,191,179,162,241,
     81, 51,145,235,249, 14,239,107, 49,192,214, 31,181,199,106,157,
    184, 84,204,176,115,121, 50, 45,127,  4,150,254,138,236,205, 93,
    222,114, 67, 29, 24, 72,243,141,128,195, 78, 66,215, 61,156,180,
};

// Quintic smoothstep fade table: fade[i] = round((1<<12) * f(i/256))
// f(t) = 6t^5 - 15t^4 + 10t^3
static const uint16_t s_fade[256] = {
       0,    0,    0,    0,    0,    0,    0,    0,    1,    1,    2,    3,
       3,    4,    6,    7,    9,   10,   12,   14,   17,   19,   22,   25,
      29,   32,   36,   40,   45,   49,   54,   60,   65,   71,   77,   84,
      91,   98,  105,  113,  121,  130,  139,  148,  158,  167,  178,  188,
     199,  211,  222,  234,  247,  259,  273,  286,  300,  314,  329,  344,
     359,  374,  390,  407,  424,  441,  458,  476,  494,  512,  531,  550,
     570,  589,  609,  630,  651,  672,  693,  715,  737,  759,  782,  805,
     828,  851,  875,  899,  923,  948,  973,  998, 1023, 1049, 1074, 1100,
    1127, 1153, 1180, 1207, 1234, 1261, 1289, 1316, 1344, 1372, 1400, 1429,
    1457, 1486, 1515, 1543, 1572, 1602, 1631, 1660, 1690, 1719, 1749, 1778,
    1808, 1838, 1868, 1898, 1928, 1958, 1988, 2018, 2048, 2077, 2107, 2137,
    2167, 2197, 2227, 2257, 2287, 2317, 2346, 2376, 2405, 2435, 2464, 2493,
    2523, 2552, 2580, 2609, 2638, 2666, 2695, 2723, 2751, 2779, 2806, 2834,
    2861, 2888, 2915, 2942, 2968, 2995, 3021, 3046, 3072, 3097, 3122, 3147,
    3172, 3196, 3220, 3244, 3267, 3290, 3313, 3336, 3358, 3380, 3402, 3423,
    3444, 3465, 3486, 3506, 3525, 3545, 3564, 3583, 3601, 3619, 3637, 3654,
    3672, 3688, 3705, 3721, 3736, 3751, 3766, 3781, 3795, 3809, 3822, 3836,
    3848, 3861, 3873, 3884, 3896, 3907, 3917, 3928, 3937, 3947, 3956, 3965,
    3974, 3982, 3990, 3997, 4004, 4011, 4018, 4024, 4030, 4035, 4041, 4046,
    4050, 4055, 4059, 4063, 4066, 4070, 4073, 4076, 4078, 4081, 4083, 4085,
    4086, 4088, 4089, 4091, 4092, 4092, 4093, 4094, 4094, 4095, 4095, 4095,
    4095, 4095, 4095, 4095,
};

// ---------------------------------------------------------------------------
// Fixed-point helpers (16.16 format, N = 1<<16)
// ---------------------------------------------------------------------------

#define N  (1L << 16)
#define P(x) s_perm[(x) & 255]

static long fp_lerp(long t, long a, long b) { return a + (t * (b - a) >> 12); }

static long fp_fade(long t) {
    int idx  = (int)(t >> 8);
    int idx1 = idx < 255 ? idx + 1 : 255;
    long t0 = s_fade[idx];
    long t1 = s_fade[idx1];
    return t0 + ((t & 255) * (t1 - t0) >> 8);
}

static long fp_grad(long hash, long x, long y, long z) {
    long h = hash & 15;
    long u = h < 8 ? x : y;
    long v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

// Raw 3D Perlin noise in 16.16 fixed-point space.
// Returns a signed value roughly in the range ±40000.
static long inoise(uint32_t x, uint32_t y, uint32_t z) {
    long X = (x >> 16) & 255, Y = (y >> 16) & 255, Z = (z >> 16) & 255;
    long fx = (long)(x & (N - 1));
    long fy = (long)(y & (N - 1));
    long fz = (long)(z & (N - 1));

    long u = fp_fade(fx), v = fp_fade(fy), w = fp_fade(fz);
    long A  = P(X)   + Y, AA = P(A)   + Z, AB = P(A+1) + Z;
    long B  = P(X+1) + Y, BA = P(B)   + Z, BB = P(B+1) + Z;

    return fp_lerp(w,
        fp_lerp(v,
            fp_lerp(u, fp_grad(P(AA),   fx,     fy,     fz    ),
                       fp_grad(P(BA),   fx - N, fy,     fz    )),
            fp_lerp(u, fp_grad(P(AB),   fx,     fy - N, fz    ),
                       fp_grad(P(BB),   fx - N, fy - N, fz    ))),
        fp_lerp(v,
            fp_lerp(u, fp_grad(P(AA+1), fx,     fy,     fz - N),
                       fp_grad(P(BA+1), fx - N, fy,     fz - N)),
            fp_lerp(u, fp_grad(P(AB+1), fx,     fy - N, fz - N),
                       fp_grad(P(BB+1), fx - N, fy - N, fz - N))));
}

// ---------------------------------------------------------------------------
// Public API — fractal Brownian motion (fBm), fully integer
// ---------------------------------------------------------------------------
//
// fBm amplitude sum for n octaves (Q8, 256 = 1.0):
//   n=1: 256   n=2: 384   n=3: 448   n=4: 480
//   n=5: 496   n=6: 504   n=7: 508   n=8: 510
//
// inoise() raw output: ±~40000.  After Q8 accumulation:
//   value_max ≈ 40000 * amp_sum_q8 / 256
// Normalise to [0,255]:
//   result = (value * 128 / value_max) + 128

uint8_t perlin_sample(int16_t x_t, int16_t y_t, uint32_t time_ms,
                      int16_t scale_t, uint16_t speed_c100, int octaves) {
    if (octaves < 1) octaves = 1;
    if (octaves > 8) octaves = 8;
    if (scale_t <= 0) scale_t = 1;

    int32_t value     = 0;
    int32_t amp_q8    = 256;  // 1.0 in Q8
    int32_t amp_sum_q8 = 0;

    for (int oct = 0; oct < octaves; oct++) {
        int freq = 1 << oct;  // 1, 2, 4, 8, ...

        // nx = x_t * freq * N / scale_t  (all in 16.16 lattice space)
        // Negative x_t casts correctly to uint32_t for lattice wrapping.
        uint32_t nx = (uint32_t)((int64_t)x_t * freq * N / scale_t);
        uint32_t ny = (uint32_t)((int64_t)y_t * freq * N / scale_t);

        // nz = time_ms * speed_c100 * freq / 400000  (noise-units/s mapping)
        // Original float formula: nz = time_s * (speed_c100/400) * freq * N
        // Equivalent: time_ms * speed_c100 * freq * N / 400000
        // Split integer/fractional to keep result in uint32_t without overflow:
        //   tc = time_ms * speed_c100 * freq  (fits uint64_t for sane values)
        //   nz = (tc / 400000) << 16  |  (tc % 400000) * 65536 / 400000
        uint64_t tc  = (uint64_t)(uint32_t)time_ms * speed_c100 * (uint32_t)freq;
        uint32_t nz  = ((uint32_t)(tc / 400000) << 16)
                     | (uint32_t)((tc % 400000) * 65536 / 400000);

        int32_t raw = (int32_t)inoise(nx, ny, nz);
        value      += (raw * amp_q8) >> 8;
        amp_sum_q8 += amp_q8;
        amp_q8    >>= 1;
    }

    // value range: ±(40000 * amp_sum_q8 / 256)
    int32_t max_val = (int32_t)((int64_t)40000 * amp_sum_q8 / 256);
    int32_t result  = (int32_t)((int64_t)value * 128 / max_val) + 128;
    if (result <   0) result =   0;
    if (result > 255) result = 255;
    return (uint8_t)result;
}
