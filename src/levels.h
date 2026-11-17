// The 150 opponents: names, ratings and how each one plays.
#pragma once
#include "search.h"
#include <random>
#include <string>
#include <vector>

struct Level {
    int number;        // 1..150
    int rating;        // 250..3500
    int tier;          // 0..14, ten bots per tier
    std::string name, title, blurb;
    int depth, timeMs, noise;
    float blunder;     // chance of a completely random move
    bool quiesce;
    float posScale;
    int aggression = 0;   // custom engines only: 0 calm .. 10 all-out attack
};

const std::vector<Level>& allLevels();
// Replaces all 150 names from a text file (one per line). Returns how many names the file had;
// fewer than 150 leaves the built-in list untouched.
int loadLevelNames(const std::wstring& path);

struct BotMove { Move move; int score; bool known; };
// moveTimeMs > 0 replaces the level's own search time (used when the bot is on a clock).
BotMove botChoose(Engine& eng, const Position& pos, const Level& L, std::atomic<bool>* stop, const Net* net,
                  std::mt19937& rng, ThinkInfo* think = nullptr, int moveTimeMs = 0);

// Opponent modelling: re-weighs the bot's top candidates by how *you* are likely to answer each one
// (from your habit model), preferring lines where your usual reply goes wrong, but only when the move
// is objectively almost as good. Returns the chosen move; *expected gets your predicted reply.
struct PatternModel;
Move anticipate(Engine& eng, const Position& root, const Level& L, Move best, const Move* cands, int ncand,
                const PatternModel& habits, const Net* net, std::atomic<bool>* stop, Move* expected);

// How long a player of this strength would sit over this move, in ms.
double botThinkMs(const Level& L, const Position& pos, int legalMoves, bool timed, long long clockMs, long long incMs,
                  std::mt19937& rng);
