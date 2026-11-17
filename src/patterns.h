// Your playing habits. A conditional-logit model over hand-picked move features (which piece goes
// where, captures, trades, checks, castling, retreats, pawn pushes, hanging pieces...) plus an exact
// memory of what you played in positions you have reached before. Trained only on your own moves.
#pragma once
#include "chess.h"
#include <string>
#include <unordered_map>
#include <vector>

#pragma pack(push, 1)
struct PatternSample {
    int8_t sq[64];
    uint8_t side, castling;
    int8_t ep, prevTo;     // prevTo: where your previous move landed (-1 if none)
    uint16_t move, fullmove;
};
#pragma pack(pop)

uint16_t packMove(Move m);
PatternSample makePatternSample(const Position& p, Move m);

class PatternModel {
public:
    static const int NF = 814;
    std::vector<float> w;
    int trained = 0;                 // how many of your moves it learned from
    float acc1 = 0, acc3 = 0;        // held-out top-1 / top-3 prediction accuracy
    std::unordered_map<uint64_t, std::vector<std::pair<uint16_t, uint16_t>>> book;   // position -> (move, count)

    PatternModel() : w(NF, 0.f) {}
    bool ready() const { return trained >= 40; }

    // probabilities for each move in `legal` (same order)
    void predict(Position& p, const MoveList& legal, std::vector<float>& prob) const;
    Move best(Position& p, float* prob = nullptr) const;
    std::vector<std::string> traits() const;   // plain-language habits, strongest first

    bool save(const std::wstring& path) const;
    bool load(const std::wstring& path);
    void rebuildBook(const std::vector<PatternSample>& all);
};

// Trains from scratch on `all` until held-out loss stops improving. Returns held-out log-loss.
float trainPatterns(PatternModel& m, const std::vector<PatternSample>& all);
// How many of `moves` the model gets right (top-1), measured before learning from them.
int countHits(const PatternModel& m, const std::vector<PatternSample>& moves);
