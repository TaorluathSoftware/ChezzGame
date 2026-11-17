// Alpha-beta search (PVS, transposition table, null move, LMR, quiescence).
#pragma once
#include "chess.h"
#include "nn.h"
#include <atomic>
#include <chrono>
#include <vector>

const int INF_SCORE = 32000, MATE_SCORE = 30000;

struct SearchLimits {
    int maxDepth = 64;
    int moveTimeMs = 1000;
    bool quiesce = true;
    float posScale = 1.f;
    float nnWeight = 1.f;
    int noiseCp = 0;          // random offset per root move: how "human" the bot's mistakes are
    uint32_t noiseSeed = 0;
    int aggression = 0;       // bonus for pieces near the enemy king (custom engines)
};

struct SearchResult {
    Move best = 0;
    int score = 0, depth = 0;
    uint64_t nodes = 0;
};

// Live view into a running search, read by the UI to draw the bot's "thoughts".
struct ThinkInfo {
    std::atomic<uint32_t> best{0}, current{0};
    std::atomic<int> depth{0}, score{0}, ncand{0};
    std::atomic<uint32_t> cand[4];
    void reset() {
        best = 0; current = 0; depth = 0; score = 0; ncand = 0;
        for (auto& c : cand) c = 0;
    }
};

class Engine {
public:
    Engine();
    void clear();
    SearchResult search(const Position& root, const SearchLimits& lim, std::atomic<bool>* stop, const Net* net,
                        ThinkInfo* think = nullptr);

private:
    ThinkInfo* think = nullptr;
    struct TTEntry { uint64_t key; uint32_t move; int16_t score; int8_t depth; uint8_t flag; };
    enum { TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3 };
    static const int MAXPLY = 100;

    std::vector<TTEntry> tt;
    uint64_t ttMask = 0;
    // the 5x150 net costs ~90k multiply-adds per call, so remember its answers per position
    struct NNEntry { uint64_t key; int32_t cp; };
    std::vector<NNEntry> nnCache;
    const Net* nnCacheNet = nullptr;
    Position pos;
    SearchLimits lim;
    std::atomic<bool>* ext = nullptr;
    const Net* net = nullptr;
    bool stopped = false;
    int completed = 0;
    uint64_t nodes = 0;
    std::chrono::steady_clock::time_point t0;
    Move killers[MAXPLY + 1][2];
    int history[2][64][64];
    std::vector<Move> rootMoves;
    std::vector<int> rootScores;

    int evaluate();
    int noiseOf(Move m) const;
    int searchRoot(int depth, Move& best);
    int negamax(int depth, int alpha, int beta, int ply, bool allowNull);
    int quiesce(int alpha, int beta, int ply);
    void checkTime();
    void scoreMoves(const MoveList& ml, int* sc, Move ttMove, int ply);
    int elapsedMs() const;
};
