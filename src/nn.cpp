#include "nn.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

// y += a * x. Written as an axpy (no reduction) so the compiler vectorises it without fast-math;
// the AVX2 build is picked at startup when the CPU has it.
static void axpyBase(float* y, const float* x, float a, int n) {
    for (int i = 0; i < n; i++) y[i] += a * x[i];
}
__attribute__((target("avx2"))) static void axpyAvx2(float* y, const float* x, float a, int n) {
    for (int i = 0; i < n; i++) y[i] += a * x[i];
}
static void (*axpy)(float*, const float*, float, int) = axpyBase;
static struct PickAxpy {
    PickAxpy() {
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) axpy = axpyAvx2;
    }
} pickAxpy;

Net::Net() : P(SIZE, 0.f) { init(12345); }

void Net::init(uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    for (int i = 0; i < OFF_B1; i++) P[i] = U(rng) * 0.2f;        // ~32 active inputs per position
    for (int j = 0; j < HID; j++) P[OFF_B1 + j] = 0.1f;
    float lim = std::sqrt(6.f / HID);                             // He-uniform for the ReLU layers
    for (int l = 0; l < LAYERS - 1; l++) {
        float* W = &P[OFF_DENSE + l * DENSE_SIZE];
        for (int i = 0; i < HID * HID; i++) W[i] = U(rng) * lim;
        for (int j = 0; j < HID; j++) W[HID * HID + j] = 0.f;
    }
    for (int j = 0; j <= HID; j++) P[OFF_OUT + j] = 0.f;
    blank = true;   // output starts at zero: an untrained net changes nothing
}

int Net::features(const int8_t* sq, int side, int* idx) {
    int n = 0;
    for (int s = 0; s < 64 && n < 32; s++) {
        int p = sq[s];
        if (!p) continue;
        int rel = pcolor(p) == side ? 0 : 1;
        int rs = side == WHITE ? s : (s ^ 56);
        idx[n++] = (rel * 6 + ptype(p) - 1) * 64 + rs;
    }
    return n;
}

float Net::forward(const int* idx, int n, float* acts) const {
    float a[HID], b[HID];
    memcpy(a, &P[OFF_B1], sizeof(a));
    for (int i = 0; i < n; i++) axpy(a, &P[idx[i] * HID], 1.f, HID);
    for (int j = 0; j < HID; j++) a[j] = a[j] > 0.f ? a[j] : 0.f;
    if (acts) memcpy(acts, a, sizeof(a));
    float *cur = a, *nxt = b;
    for (int l = 0; l < LAYERS - 1; l++) {
        const float* W = &P[OFF_DENSE + l * DENSE_SIZE];
        memcpy(nxt, W + HID * HID, sizeof(a));
        for (int i = 0; i < HID; i++)
            if (cur[i] != 0.f) axpy(nxt, W + i * HID, cur[i], HID);   // ReLU leaves about half at zero
        for (int j = 0; j < HID; j++) nxt[j] = nxt[j] > 0.f ? nxt[j] : 0.f;
        if (acts) memcpy(acts + (l + 1) * HID, nxt, sizeof(a));
        float* t = cur; cur = nxt; nxt = t;
    }
    float out = P[OFF_OUT + HID];
    for (int j = 0; j < HID; j++) out += P[OFF_OUT + j] * cur[j];
    return out;
}

int Net::evalCp(const Position& pos) const {
    if (blank) return 0;
    int idx[32];
    int n = features(pos.sq, pos.side, idx);
    float cp = forward(idx, n) * CP_SCALE;
    if (cp > CP_CLAMP) cp = CP_CLAMP;
    if (cp < -CP_CLAMP) cp = -CP_CLAMP;
    return (int)cp;
}

struct NetHeader { char magic[4]; int in, h, layers, games; };

bool Net::save(const std::wstring& path, int games) const {
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    NetHeader hd = {{'C', 'H', 'Z', '2'}, NIN, HID, LAYERS, games};
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && fwrite(P.data(), sizeof(float), P.size(), f) == P.size();
    fclose(f);
    if (!ok) return false;
    _wremove(path.c_str());
    return _wrename(tmp.c_str(), path.c_str()) == 0;
}

bool Net::load(const std::wstring& path, int& games) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    NetHeader hd;
    std::vector<float> tmp(SIZE);
    bool ok = fread(&hd, sizeof(hd), 1, f) == 1 && memcmp(hd.magic, "CHZ2", 4) == 0 && hd.in == NIN && hd.h == HID &&
              hd.layers == LAYERS && fread(tmp.data(), sizeof(float), tmp.size(), f) == tmp.size();
    fclose(f);
    if (!ok) return false;   // an older brain shape: start fresh
    for (float x : tmp)
        if (!std::isfinite(x)) return false;
    P = tmp;
    blank = true;
    for (int j = 0; j <= HID; j++)
        if (P[OFF_OUT + j] != 0.f) blank = false;
    games = hd.games;
    return true;
}

Trainer::Trainer(Net& n, float lr_) : net(n), lr(lr_), g(Net::SIZE), m(Net::SIZE, 0.f), v(Net::SIZE, 0.f) {}

float Trainer::step(const TrainItem* items, int count) {
    const int H = Net::HID, L = Net::LAYERS;
    std::fill(g.begin(), g.end(), 0.f);
    float loss = 0.f;
    std::vector<float> acts(L * H);
    float delta[Net::HID], prev[Net::HID];
    for (int k = 0; k < count; k++) {
        const TrainItem& it = items[k];
        float raw = net.forward(it.idx, it.n, acts.data());
        float cp = raw * Net::CP_SCALE;
        bool clamped = cp > Net::CP_CLAMP || cp < -Net::CP_CLAMP;
        if (cp > Net::CP_CLAMP) cp = Net::CP_CLAMP;
        if (cp < -Net::CP_CLAMP) cp = -Net::CP_CLAMP;
        float p = 1.f / (1.f + std::exp(-(it.hce + cp) / 400.f));
        float err = p - it.target;
        loss += err * err;
        if (clamped) continue;
        float gr = 2.f * err * p * (1.f - p) / 400.f * Net::CP_SCALE;   // d loss / d raw

        // output layer
        const float* top = &acts[(L - 1) * H];
        g[Net::OFF_OUT + H] += gr;
        for (int j = 0; j < H; j++) {
            g[Net::OFF_OUT + j] += gr * top[j];
            delta[j] = top[j] > 0.f ? gr * net.P[Net::OFF_OUT + j] : 0.f;
        }
        // dense layers, last to first
        for (int l = L - 2; l >= 0; l--) {
            const float* in = &acts[l * H];
            const float* W = &net.P[Net::OFF_DENSE + l * Net::DENSE_SIZE];
            float* gW = &g[Net::OFF_DENSE + l * Net::DENSE_SIZE];
            for (int j = 0; j < H; j++) gW[H * H + j] += delta[j];
            for (int i = 0; i < H; i++) {
                if (in[i] == 0.f) { prev[i] = 0.f; continue; }   // dead input: no weight gradient, no error flows back
                axpy(gW + i * H, delta, in[i], H);
                const float* row = W + i * H;
                float s = 0.f;
                for (int j = 0; j < H; j++) s += row[j] * delta[j];
                prev[i] = s;
            }
            memcpy(delta, prev, sizeof(delta));
        }
        // first layer: only the active input rows
        for (int j = 0; j < H; j++) g[Net::OFF_B1 + j] += delta[j];
        for (int i = 0; i < it.n; i++) axpy(&g[it.idx[i] * H], delta, 1.f, H);
    }
    if (count == 0) return 0.f;
    net.blank = false;
    t++;
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, decay = 1e-5f;
    float c1 = 1.f - std::pow(b1, (float)t), c2 = 1.f - std::pow(b2, (float)t);
    float inv = 1.f / count;
    for (int i = 0; i < Net::SIZE; i++) {
        float gi = g[i] * inv + decay * net.P[i];
        m[i] = b1 * m[i] + (1.f - b1) * gi;
        v[i] = b2 * v[i] + (1.f - b2) * gi * gi;
        net.P[i] -= lr * (m[i] / c1) / (std::sqrt(v[i] / c2) + eps);
    }
    return loss * inv;
}
