// The brain's evaluation network: 768 inputs -> 5 hidden layers of 150 neurons -> 1 output.
// It learns a centipawn correction on top of the hand-crafted eval. Inputs are one-hot
// piece/square features seen from the side to move.
#pragma once
#include "chess.h"
#include <string>
#include <vector>

struct Net {
    static const int NIN = 768, HID = 150, LAYERS = 5;
    static const int OFF_B1 = NIN * HID;                      // W1 [feature][hidden], then b1
    static const int OFF_DENSE = OFF_B1 + HID;                // LAYERS-1 blocks of W [in][out] + b
    static const int DENSE_SIZE = HID * HID + HID;
    static const int OFF_OUT = OFF_DENSE + (LAYERS - 1) * DENSE_SIZE;   // w_out [HID], b_out
    static const int SIZE = OFF_OUT + HID + 1;
    static constexpr float CP_SCALE = 100.f, CP_CLAMP = 250.f;

    std::vector<float> P;
    bool blank = true;   // output layer still all zero: evalCp is 0 without running the net

    Net();
    void init(uint32_t seed);
    static int features(const int8_t* sq, int side, int* idx);   // returns count (<= 32)
    // acts (optional): LAYERS * HID post-ReLU activations, for training
    float forward(const int* idx, int n, float* acts = nullptr) const;
    int evalCp(const Position& pos) const;
    bool save(const std::wstring& path, int games) const;
    bool load(const std::wstring& path, int& games);
};

struct TrainItem {
    int idx[32];
    int n;
    float hce;      // hand-crafted eval, side-to-move cp
    float target;   // expected score for side to move, 0..1
};

class Trainer {
public:
    explicit Trainer(Net& n, float lr = 1e-3f);
    float step(const TrainItem* items, int count);   // one Adam step, returns mean loss
private:
    Net& net;
    float lr;
    int t = 0;
    std::vector<float> g, m, v;
};
