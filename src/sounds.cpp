#include "sounds.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

static const int RATE = 44100;
static const float TAU = 6.2831853f;
static std::vector<char> g_wav[SND_COUNT];

typedef std::vector<float> Buf;

static void ensure(Buf& b, float seconds) {
    size_t n = (size_t)(seconds * RATE);
    if (b.size() < n) b.resize(n, 0.f);
}

// A piece set down on a wooden board: a short filtered click plus two damped body resonances
// whose pitch drops slightly as the knock dies away.
static void tock(Buf& b, float at, float pitch, float gain, float hardness, unsigned seed) {
    ensure(b, at + 0.25f);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    size_t s0 = (size_t)(at * RATE);
    float lp = 0.f, ph1 = 0.f, ph2 = 0.f;
    for (size_t i = 0; i < (size_t)(0.22f * RATE) && s0 + i < b.size(); i++) {
        float t = (float)i / RATE;
        float f = pitch * (1.f + 0.6f * expf(-t / 0.008f));
        ph1 += TAU * f / RATE;
        ph2 += TAU * f * 2.76f / RATE;
        float body = sinf(ph1) * expf(-t / 0.040f) + 0.45f * sinf(ph2) * expf(-t / 0.014f);
        lp += (U(rng) - lp) * (0.25f + 0.5f * hardness);   // one-pole low-pass on noise
        float click = lp * expf(-t / (0.0025f + 0.002f * hardness)) * (1.2f + hardness);
        b[s0 + i] += gain * (0.75f * body + click);
    }
}

// A small bell: inharmonic partials with long decays.
static void bell(Buf& b, float at, float f, float gain, float decay) {
    ensure(b, at + decay * 5);
    size_t s0 = (size_t)(at * RATE);
    static const float PART[4] = {1.f, 2.01f, 2.76f, 5.4f}, AMP[4] = {1.f, 0.45f, 0.3f, 0.12f};
    for (size_t i = 0; s0 + i < b.size(); i++) {
        float t = (float)i / RATE, v = 0.f;
        for (int k = 0; k < 4; k++) v += AMP[k] * sinf(TAU * f * PART[k] * t) * expf(-t * (1.f + k) / decay);
        float attack = t < 0.004f ? t / 0.004f : 1.f;
        b[s0 + i] += gain * v * attack;
    }
}

static std::vector<char> toWav(Buf b) {
    float peak = 1e-6f;
    for (float v : b) peak = std::fmax(peak, std::fabs(v));
    float k = 0.8f / peak;
    // short fade at the end so nothing clicks
    size_t fade = std::min<size_t>(b.size(), 400);
    for (size_t i = 0; i < fade; i++) b[b.size() - 1 - i] *= (float)i / fade;
    uint32_t dataBytes = (uint32_t)(b.size() * 2);
    std::vector<char> w(44 + dataBytes);
    auto put32 = [&](size_t o, uint32_t v) { memcpy(&w[o], &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { memcpy(&w[o], &v, 2); };
    memcpy(&w[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, RATE);
    put32(28, RATE * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&w[36], "data", 4);
    put32(40, dataBytes);
    for (size_t i = 0; i < b.size(); i++) {
        float v = b[i] * k;
        v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
        int16_t s = (int16_t)(v * 32767);
        memcpy(&w[44 + i * 2], &s, 2);
    }
    return w;
}

void synthInit() {
    static bool done = false;
    if (done) return;
    done = true;
    Buf b;

    tock(b, 0.f, 230, 1.f, 0.3f, 1);
    g_wav[SND_MOVE] = toWav(b);

    b.clear();   // a heavier knock with a second piece rattling
    tock(b, 0.f, 175, 1.f, 0.9f, 2);
    tock(b, 0.035f, 260, 0.55f, 0.7f, 3);
    g_wav[SND_CAPTURE] = toWav(b);

    b.clear();
    tock(b, 0.f, 230, 0.8f, 0.3f, 4);
    bell(b, 0.05f, 1318.5f, 0.32f, 0.22f);
    g_wav[SND_CHECK] = toWav(b);

    b.clear();   // king, then rook
    tock(b, 0.f, 215, 0.9f, 0.3f, 5);
    tock(b, 0.09f, 245, 0.8f, 0.35f, 6);
    g_wav[SND_CASTLE] = toWav(b);

    b.clear();
    tock(b, 0.f, 230, 0.7f, 0.3f, 7);
    bell(b, 0.04f, 784.f, 0.25f, 0.18f);
    bell(b, 0.13f, 1046.5f, 0.25f, 0.25f);
    g_wav[SND_PROMOTE] = toWav(b);

    b.clear();   // the clock is pressed: two short plastic clicks
    tock(b, 0.f, 900, 0.6f, 1.f, 8);
    tock(b, 0.07f, 1100, 0.45f, 1.f, 9);
    g_wav[SND_START] = toWav(b);

    b.clear();   // a new message: two soft high blips
    bell(b, 0.f, 987.77f, 0.22f, 0.10f);
    bell(b, 0.09f, 1318.5f, 0.2f, 0.14f);
    g_wav[SND_NOTIFY] = toWav(b);

    b.clear();   // a soft arpeggiated chime
    bell(b, 0.f, 523.25f, 0.35f, 0.5f);
    bell(b, 0.14f, 659.25f, 0.32f, 0.5f);
    bell(b, 0.28f, 783.99f, 0.32f, 0.7f);
    g_wav[SND_END] = toWav(b);
}

const std::vector<char>& synthWav(int id) { return g_wav[id]; }
