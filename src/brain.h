// The shared "brain": the evaluation net every bot uses, the model of your playing habits, and the
// memory of positions and moves from your games they train on. Learning runs on a background thread
// after each finished game and keeps going until the models stop improving.
#pragma once
#include "nn.h"
#include "patterns.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma pack(push, 1)
struct Sample {
    int8_t sq[64];
    uint8_t side;
    int8_t result;    // from the side to move: +1 win, 0 draw, -1 loss
    int16_t search;   // engine score for the side to move, or NO_SCORE
};
#pragma pack(pop)
const int16_t NO_SCORE = -32768;

class Brain {
public:
    void open(const std::wstring& dataDir);
    std::shared_ptr<const Net> net();
    // Replace the network with one from the club server (only while no training is running).
    void adopt(const Net& n);
    std::shared_ptr<const PatternModel> patterns();
    // positions from the game, plus the moves *you* made in it
    void learnAsync(std::vector<Sample> game, std::vector<PatternSample> yourMoves);
    float learnNow(const std::vector<Sample>& game);
    bool busy() const { return busyF; }
    int games() const { return gamesN; }
    size_t positions() const { return memN; }
    size_t yourMoves() const { return patN; }
    float lastLoss() const { return lossF; }
    int lastReadHits() const { return readHits; }    // last game: moves of yours it predicted beforehand
    int lastReadTotal() const { return readTotal; }
    void shutdown();

private:
    float learnImpl(const std::vector<Sample>& game, const std::vector<PatternSample>& yours);
    std::wstring dir;
    std::mutex mx;
    std::shared_ptr<const Net> cur;
    std::shared_ptr<const PatternModel> pat;
    std::vector<Sample> memory;            // touched only by open() and the learning thread
    std::vector<PatternSample> patMemory;  // likewise
    std::thread worker;
    std::atomic<bool> busyF{false};
    std::atomic<int> gamesN{0}, readHits{0}, readTotal{0};
    std::atomic<size_t> memN{0}, patN{0};
    std::atomic<float> lossF{0.f};
};
