#include "brain.h"
#include "eval.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

static const size_t MAX_MEMORY = 300000;
static const size_t MAX_PATTERNS = 50000;

template <class T>
static void readAll(const std::wstring& path, std::vector<T>& out) {
    out.clear();
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        T s;
        while (fread(&s, sizeof(s), 1, f) == 1) out.push_back(s);
        fclose(f);
    }
}

template <class T>
static void appendOrRewrite(const std::wstring& path, const std::vector<T>& all, const std::vector<T>& added, bool rewrite) {
    if (FILE* f = _wfopen(path.c_str(), rewrite ? L"wb" : L"ab")) {
        if (rewrite) fwrite(all.data(), sizeof(T), all.size(), f);
        else fwrite(added.data(), sizeof(T), added.size(), f);
        fclose(f);
    }
}

void Brain::open(const std::wstring& dataDir) {
    dir = dataDir;
    auto n = std::make_shared<Net>();
    int g = 0;
    if (!n->load(dir + L"\\brain.bin", g)) {
        n = std::make_shared<Net>();
        g = 0;
    }
    gamesN = g;
    cur = n;
    readAll(dir + L"\\memory.bin", memory);
    if (memory.size() > MAX_MEMORY) memory.erase(memory.begin(), memory.end() - MAX_MEMORY);
    memN = memory.size();

    readAll(dir + L"\\yourmoves.bin", patMemory);
    if (patMemory.size() > MAX_PATTERNS) patMemory.erase(patMemory.begin(), patMemory.end() - MAX_PATTERNS);
    patN = patMemory.size();
    auto pm = std::make_shared<PatternModel>();
    if (!pm->load(dir + L"\\habits.bin") && !patMemory.empty()) trainPatterns(*pm, patMemory);
    pm->rebuildBook(patMemory);
    pat = pm;
}

std::shared_ptr<const Net> Brain::net() {
    std::lock_guard<std::mutex> lk(mx);
    return cur;
}

void Brain::adopt(const Net& n) {
    if (worker.joinable()) worker.join();   // never land on top of a training run
    auto fresh = std::make_shared<Net>(n);
    fresh->save(dir + L"\\brain.bin", gamesN);
    std::lock_guard<std::mutex> lk(mx);
    cur = fresh;
}

std::shared_ptr<const PatternModel> Brain::patterns() {
    std::lock_guard<std::mutex> lk(mx);
    return pat;
}

void Brain::learnAsync(std::vector<Sample> game, std::vector<PatternSample> yours) {
    if (worker.joinable()) worker.join();
    busyF = true;
    worker = std::thread([this, game, yours]() {
        learnImpl(game, yours);
        busyF = false;
    });
}

float Brain::learnNow(const std::vector<Sample>& game) {
    if (worker.joinable()) worker.join();
    return learnImpl(game, {});
}

void Brain::shutdown() {
    if (worker.joinable()) worker.join();
}

static void makeItem(const Sample& s, TrainItem& it) {
    Position p;
    p.setBoard(s.sq, s.side);
    it.n = Net::features(s.sq, s.side, it.idx);
    it.hce = (float)evalHCE(p, 1.f);
    float z = (s.result + 1) * 0.5f;
    if (s.search != NO_SCORE) {
        float q = 1.f / (1.f + std::exp(-s.search / 400.f));
        it.target = 0.5f * z + 0.5f * q;
    } else {
        it.target = z;
    }
}

float Brain::learnImpl(const std::vector<Sample>& game, const std::vector<PatternSample>& yours) {
    // ---- your habits: score how well it read you this game, then retrain on everything you've played
    if (!yours.empty()) {
        auto before = patterns();
        readHits = before->ready() ? countHits(*before, yours) : 0;
        readTotal = before->ready() ? (int)yours.size() : 0;
        bool trimmed = false;
        patMemory.insert(patMemory.end(), yours.begin(), yours.end());
        if (patMemory.size() > MAX_PATTERNS) {
            patMemory.erase(patMemory.begin(), patMemory.end() - MAX_PATTERNS);
            trimmed = true;
        }
        auto pm = std::make_shared<PatternModel>();
        trainPatterns(*pm, patMemory);
        pm->save(dir + L"\\habits.bin");
        appendOrRewrite(dir + L"\\yourmoves.bin", patMemory, yours, trimmed);
        {
            std::lock_guard<std::mutex> lk(mx);
            pat = pm;
        }
        patN = patMemory.size();
    }
    if (game.empty()) return 0.f;

    // ---- position evaluation: many passes over this game, then full sweeps of everything remembered
    Net work = *net();
    bool trimmed = false;
    memory.insert(memory.end(), game.begin(), game.end());
    if (memory.size() > MAX_MEMORY) {
        memory.erase(memory.begin(), memory.end() - MAX_MEMORY);
        trimmed = true;
    }
    size_t base = memory.size() - game.size();
    std::mt19937 rng((unsigned)(memory.size() * 2654435761u) ^ (unsigned)gamesN);

    Trainer tr(work);
    const int B = 64;
    std::vector<TrainItem> batch(B);
    double lossSum = 0;
    int steps = 0;
    auto runOrder = [&](std::vector<size_t>& order) {
        std::shuffle(order.begin(), order.end(), rng);
        double l = 0;
        int st = 0;
        for (size_t i = 0; i < order.size(); i += B) {
            int cnt = (int)std::min<size_t>(B, order.size() - i);
            for (int k = 0; k < cnt; k++) makeItem(memory[order[i + k]], batch[k]);
            l += tr.step(batch.data(), cnt);
            st++;
        }
        lossSum += l;
        steps += st;
        return st ? l / st : 0.0;
    };

    // the new game until its loss settles (at least 6 passes, at most 30)
    double prev = 1e9;
    for (int pass = 0; pass < 30; pass++) {
        std::vector<size_t> order;
        for (size_t i = 0; i < game.size(); i++) order.push_back(base + i);
        // mix in older positions so it doesn't overfit this one game
        size_t replay = std::min(memory.size(), game.size() * 3);
        for (size_t i = 0; i < replay; i++) order.push_back(rng() % memory.size());
        double l = runOrder(order);
        if (pass >= 5 && prev - l < prev * 0.01) break;
        prev = l;
    }
    // two full sweeps over memory (large memories get a 40k random slice per sweep)
    for (int sweep = 0; sweep < 2; sweep++) {
        std::vector<size_t> order;
        if (memory.size() <= 40000)
            for (size_t i = 0; i < memory.size(); i++) order.push_back(i);
        else
            for (int i = 0; i < 40000; i++) order.push_back(rng() % memory.size());
        runOrder(order);
    }

    int g = gamesN + 1;
    work.save(dir + L"\\brain.bin", g);
    appendOrRewrite(dir + L"\\memory.bin", memory, game, trimmed);
    {
        std::lock_guard<std::mutex> lk(mx);
        cur = std::make_shared<Net>(work);
    }
    gamesN = g;
    memN = memory.size();
    float loss = steps ? (float)(lossSum / steps) : 0.f;
    lossF = loss;
    return loss;
}
