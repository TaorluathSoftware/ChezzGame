#include "patterns.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

// Feature layout (all seen from the mover's side of the board):
//   0..5     piece type moved            6..389   piece x destination square
//   390..773 piece x origin square       774..778 capture, by victim type
//   779..781 capture is winning/even/losing   782 gives check
//   783/784  castles short/long          785 double pawn push   786 promotion
//   787..789 forward/backward/sideways   790 lands on an attacked square
//   791      ...and nothing defends it   792 the piece was under attack (escaping)
//   793      king walk with queens on    794..811 game phase x piece type
//   812      same piece as last move     813 closes in on the enemy king
enum {
    F_PIECE = 0, F_TO = 6, F_FROM = 390, F_VICTIM = 774, F_TRADE = 779, F_CHECK = 782, F_CASTLE = 783,
    F_DOUBLE = 785, F_PROMO = 786, F_DIR = 787, F_ATTACKED = 790, F_HANGS = 791, F_ESCAPE = 792, F_KINGWALK = 793,
    F_PHASE = 794, F_SAMEPIECE = 812, F_TOWARD = 813
};

static const int VAL[7] = {0, 1, 3, 3, 5, 9, 100};

uint16_t packMove(Move m) { return (uint16_t)(mfrom(m) | (mto(m) << 6) | (mpromo(m) << 12)); }

static int prevOwnTo(const Position& p) {
    size_t n = p.hist.size();
    return n >= 2 && p.hist[n - 2].m ? mto(p.hist[n - 2].m) : -1;
}

PatternSample makePatternSample(const Position& p, Move m) {
    PatternSample s;
    memcpy(s.sq, p.sq, 64);
    s.side = (uint8_t)p.side;
    s.castling = (uint8_t)p.castling;
    s.ep = (int8_t)p.ep;
    s.prevTo = (int8_t)prevOwnTo(p);
    s.move = packMove(m);
    s.fullmove = (uint16_t)std::min(p.fullmove, 65535);
    return s;
}

static void restore(const PatternSample& s, Position& p) {
    p.setBoard(s.sq, s.side);
    p.castling = s.castling;
    p.ep = s.ep;
    p.fullmove = s.fullmove;
    p.hash = p.computeHash();
}

struct Ctx { int phase; bool queens; int prevTo; };

static Ctx context(const Position& p, int prevTo) {
    int pieces = 0;
    bool queens = false;
    for (int s = 0; s < 64; s++) {
        if (!p.sq[s]) continue;
        pieces++;
        if (ptype(p.sq[s]) == QUEEN) queens = true;
    }
    return {p.fullmove <= 10 ? 0 : pieces <= 12 ? 2 : 1, queens, prevTo};
}

static int features(Position& p, Move m, const Ctx& c, int* f) {
    int n = 0, us = p.side, them = us ^ 1, from = mfrom(m), to = mto(m);
    int t = ptype(p.sq[from]) - 1;
    if (t < 0) return 0;
    int rf = us == WHITE ? from : from ^ 56, rt = us == WHITE ? to : to ^ 56;
    f[n++] = F_PIECE + t;
    f[n++] = F_TO + t * 64 + rt;
    f[n++] = F_FROM + t * 64 + rf;
    if (m & MF_CAP) {
        int victim = (m & MF_EP) ? PAWN : ptype(p.sq[to]);
        f[n++] = F_VICTIM + victim - 1;
        int va = VAL[t + 1], vv = VAL[victim];
        f[n++] = F_TRADE + (vv > va ? 0 : vv == va ? 1 : 2);
    }
    if (m & MF_DOUBLE) f[n++] = F_DOUBLE;
    if (mpromo(m)) f[n++] = F_PROMO;
    if (m & MF_CASTLE) f[n++] = to > from ? F_CASTLE : F_CASTLE + 1;
    int dr = sqrank(rt) - sqrank(rf);
    f[n++] = F_DIR + (dr > 0 ? 0 : dr < 0 ? 1 : 2);
    if (p.attacked(from, them)) f[n++] = F_ESCAPE;
    if (t + 1 == KING && !(m & MF_CASTLE) && c.queens) f[n++] = F_KINGWALK;
    f[n++] = F_PHASE + c.phase * 6 + t;
    if (from == c.prevTo) f[n++] = F_SAMEPIECE;
    int ek = p.king[them];
    auto dist = [&](int a) { return std::max(std::abs(sqfile(a) - sqfile(ek)), std::abs(sqrank(a) - sqrank(ek))); };
    if (t + 1 != KING && dist(to) < dist(from)) f[n++] = F_TOWARD;
    if (p.make(m)) {
        if (p.inCheck()) f[n++] = F_CHECK;
        if (p.attacked(to, them)) {
            f[n++] = F_ATTACKED;
            if (!p.attacked(to, us)) f[n++] = F_HANGS;
        }
        p.unmake();
    }
    return n;
}

static void softmax(std::vector<float>& s) {
    float mx = -1e30f;
    for (float v : s) mx = std::max(mx, v);
    float sum = 0;
    for (float& v : s) { v = std::exp(v - mx); sum += v; }
    for (float& v : s) v /= sum;
}

void PatternModel::predict(Position& p, const MoveList& legal, std::vector<float>& prob) const {
    prob.assign(legal.n, 0.f);
    if (!legal.n) return;
    Ctx c = context(p, prevOwnTo(p));
    int f[24];
    for (int i = 0; i < legal.n; i++) {
        int n = features(p, legal.m[i], c, f);
        float s = 0;
        for (int k = 0; k < n; k++) s += w[f[k]];
        prob[i] = s;
    }
    softmax(prob);
    // Positions you have been in before: lean on what you actually played there.
    auto it = book.find(p.hash);
    if (it != book.end()) {
        float total = 0;
        for (auto& mc : it->second) total += mc.second;
        float a = total / (total + 2.f);
        for (int i = 0; i < legal.n; i++) {
            float cnt = 0;
            for (auto& mc : it->second)
                if (mc.first == packMove(legal.m[i])) cnt = mc.second;
            prob[i] = (1 - a) * prob[i] + a * cnt / total;
        }
    }
}

Move PatternModel::best(Position& p, float* prob) const {
    MoveList ml;
    p.genLegal(ml);
    if (!ml.n) return 0;
    std::vector<float> pr;
    predict(p, ml, pr);
    int b = (int)(std::max_element(pr.begin(), pr.end()) - pr.begin());
    if (prob) *prob = pr[b];
    return ml.m[b];
}

void PatternModel::rebuildBook(const std::vector<PatternSample>& all) {
    book.clear();
    Position p;
    for (auto& s : all) {
        restore(s, p);
        auto& v = book[p.hash];
        bool found = false;
        for (auto& mc : v)
            if (mc.first == s.move) { mc.second++; found = true; }
        if (!found) v.push_back({s.move, 1});
    }
}

// ---------------------------------------------------------------- training

namespace {
struct Prepared {
    std::vector<int> feats;          // concatenated feature lists
    std::vector<int> start;          // per legal move: offset into feats (size moves+1)
    int target;
};
}

static bool prepare(const PatternSample& s, Prepared& out) {
    Position p;
    restore(s, p);
    MoveList ml;
    p.genLegal(ml);
    out.target = -1;
    out.feats.clear();
    out.start.clear();
    Ctx c = context(p, s.prevTo);
    int f[24];
    for (int i = 0; i < ml.n; i++) {
        out.start.push_back((int)out.feats.size());
        int n = features(p, ml.m[i], c, f);
        out.feats.insert(out.feats.end(), f, f + n);
        if (packMove(ml.m[i]) == s.move) out.target = i;
    }
    out.start.push_back((int)out.feats.size());
    return out.target >= 0 && ml.n > 1;
}

static float sampleLoss(const std::vector<float>& w, const Prepared& d, int* rank) {
    int moves = (int)d.start.size() - 1;
    std::vector<float> s(moves);
    for (int i = 0; i < moves; i++) {
        float v = 0;
        for (int k = d.start[i]; k < d.start[i + 1]; k++) v += w[d.feats[k]];
        s[i] = v;
    }
    if (rank) {
        int r = 0;
        for (int i = 0; i < moves; i++)
            if (s[i] > s[d.target]) r++;
        *rank = r;
    }
    softmax(s);
    return -std::log(std::max(1e-7f, s[d.target]));
}

float trainPatterns(PatternModel& m, const std::vector<PatternSample>& all) {
    // Most recent moves only, so it follows how you play now.
    size_t first = all.size() > 15000 ? all.size() - 15000 : 0;
    std::vector<Prepared> data;
    data.reserve(all.size() - first);
    for (size_t i = first; i < all.size(); i++) {
        Prepared d;
        if (prepare(all[i], d)) data.push_back(std::move(d));
    }
    std::fill(m.w.begin(), m.w.end(), 0.f);
    m.trained = (int)data.size();
    if (data.empty()) return 0.f;

    // every 8th move is held out to judge when to stop (and how well it predicts you)
    std::vector<int> train, hold;
    for (int i = 0; i < (int)data.size(); i++) (data.size() >= 40 && i % 8 == 7 ? hold : train).push_back(i);
    if (hold.empty()) hold = train;

    std::mt19937 rng(12345);
    std::vector<float> best = m.w;
    float bestLoss = 1e30f;
    int stale = 0;
    for (int epoch = 0; epoch < 60 && stale < 4; epoch++) {
        std::shuffle(train.begin(), train.end(), rng);
        float lr = 0.12f / (1.f + epoch * 0.15f);
        std::vector<float> s;
        for (int idx : train) {
            const Prepared& d = data[idx];
            int moves = (int)d.start.size() - 1;
            s.assign(moves, 0.f);
            for (int i = 0; i < moves; i++)
                for (int k = d.start[i]; k < d.start[i + 1]; k++) s[i] += m.w[d.feats[k]];
            softmax(s);
            for (int i = 0; i < moves; i++) {
                float g = s[i] - (i == d.target ? 1.f : 0.f);
                if (std::fabs(g) < 1e-4f) continue;
                for (int k = d.start[i]; k < d.start[i + 1]; k++) m.w[d.feats[k]] -= lr * g;
            }
        }
        for (float& x : m.w) x *= 0.9995f;   // light L2 so rare squares don't run away
        float loss = 0;
        int top1 = 0, top3 = 0;
        for (int idx : hold) {
            int r;
            loss += sampleLoss(m.w, data[idx], &r);
            top1 += r == 0;
            top3 += r < 3;
        }
        loss /= hold.size();
        if (loss < bestLoss - 1e-4f) {
            bestLoss = loss;
            best = m.w;
            m.acc1 = (float)top1 / hold.size();
            m.acc3 = (float)top3 / hold.size();
            stale = 0;
        } else {
            stale++;
        }
    }
    m.w = best;
    m.rebuildBook(std::vector<PatternSample>(all.begin() + first, all.end()));
    return bestLoss;
}

int countHits(const PatternModel& m, const std::vector<PatternSample>& moves) {
    int hits = 0;
    Position p;
    for (auto& s : moves) {
        restore(s, p);
        // the book only knows the position by hash; history-dependent features use prevTo
        MoveList ml;
        p.genLegal(ml);
        if (!ml.n) continue;
        Ctx c = context(p, s.prevTo);
        int f[24], bi = 0;
        float bs = -1e30f;
        for (int i = 0; i < ml.n; i++) {
            int n = features(p, ml.m[i], c, f);
            float v = 0;
            for (int k = 0; k < n; k++) v += m.w[f[k]];
            auto it = m.book.find(p.hash);
            if (it != m.book.end())
                for (auto& mc : it->second)
                    if (mc.first == packMove(ml.m[i])) v += 2.f * mc.second;
            if (v > bs) { bs = v; bi = i; }
        }
        if (packMove(ml.m[bi]) == s.move) hits++;
    }
    return hits;
}

// ---------------------------------------------------------------- habits in words

std::vector<std::string> PatternModel::traits() const {
    std::vector<std::pair<float, std::string>> t;
    if (trained < 150) return {};
    auto add = [&](float strength, const char* text) {
        if (strength > 0) t.push_back({strength, text});
    };
    add(w[F_CASTLE] - 0.4f, "castles kingside");
    add(w[F_CASTLE + 1] - 0.4f, "castles queenside");
    add(w[F_PHASE + 0 * 6 + 4] - 0.3f, "brings the queen out early");
    add(w[F_CHECK] - 0.4f, "loves giving check");
    add((w[F_TRADE] + w[F_TRADE + 1]) * 0.5f - 0.4f, "takes material when it's offered");
    add(w[F_TRADE + 1] - 0.5f, "happy to trade pieces off");
    add(-w[F_TRADE + 1] - 0.5f, "avoids trades");
    add(w[F_HANGS], "sometimes leaves pieces unprotected");
    add(-w[F_HANGS] - 1.2f, "careful never to hang a piece");
    add(w[F_ESCAPE] - 0.8f, "moves attacked pieces to safety");
    add(-w[F_ESCAPE] - 0.3f, "ignores threats to play its own ideas");
    add(w[F_TOWARD] - 0.35f, "plays for a direct attack on the king");
    add(w[F_DOUBLE] - 0.5f, "grabs space with pawns");
    add(w[F_DIR + 1] - 0.3f, "not afraid to retreat");
    add(w[F_KINGWALK] - 0.3f, "walks the king early");
    add(w[F_SAMEPIECE] - 0.4f, "moves the same piece again and again");
    add(-w[F_SAMEPIECE] - 0.6f, "develops a new piece every move");
    add(w[F_PHASE + 2 * 6 + 5] - 0.4f, "activates the king in endgames");
    std::sort(t.begin(), t.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (size_t i = 0; i < t.size() && i < 4; i++) out.push_back(t[i].second);
    return out;
}

// ---------------------------------------------------------------- persistence

struct PatHeader { char magic[4]; int nf, trained; float acc1, acc3; };

bool PatternModel::save(const std::wstring& path) const {
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    PatHeader h = {{'C', 'H', 'Z', 'P'}, NF, trained, acc1, acc3};
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(w.data(), sizeof(float), w.size(), f) == w.size();
    fclose(f);
    if (!ok) return false;
    _wremove(path.c_str());
    return _wrename(tmp.c_str(), path.c_str()) == 0;
}

bool PatternModel::load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    PatHeader h;
    std::vector<float> tmp(NF);
    bool ok = fread(&h, sizeof h, 1, f) == 1 && memcmp(h.magic, "CHZP", 4) == 0 && h.nf == NF &&
              fread(tmp.data(), sizeof(float), NF, f) == (size_t)NF;
    fclose(f);
    if (!ok) return false;
    w = tmp;
    trained = h.trained;
    acc1 = h.acc1;
    acc3 = h.acc3;
    return true;
}
