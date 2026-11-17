#include "search.h"
#include "eval.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>

Engine::Engine() {
    tt.resize(1 << 20);   // 16 MB
    ttMask = tt.size() - 1;
    nnCache.resize(1 << 18);
    clear();
}

void Engine::clear() {
    std::fill(tt.begin(), tt.end(), TTEntry{0, 0, 0, 0, 0});
    std::fill(nnCache.begin(), nnCache.end(), NNEntry{0, 0});
    memset(killers, 0, sizeof(killers));
    memset(history, 0, sizeof(history));
}

int Engine::elapsedMs() const {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

void Engine::checkTime() {
    if (ext && ext->load()) { stopped = true; return; }
    if (completed < 1) return;
    if (elapsedMs() >= lim.moveTimeMs) stopped = true;
}

int Engine::evaluate() {
    int e = evalHCE(pos, lim.posScale);
    if (lim.aggression) {   // custom engines: reward pieces closing in on the enemy king
        int t = 0, ek = pos.king[pos.side ^ 1], mk = pos.king[pos.side];
        for (int s = 0; s < 64; s++) {
            int p = pos.sq[s], ty = ptype(p);
            if (!p || ty == PAWN || ty == KING) continue;
            bool mine = pcolor(p) == pos.side;
            int k = mine ? ek : mk;
            int d = std::max(std::abs(sqfile(s) - sqfile(k)), std::abs(sqrank(s) - sqrank(k)));
            t += mine ? 7 - d : d - 7;
        }
        e += t * lim.aggression / 2;
    }
    if (net && lim.nnWeight > 0.f) {
        NNEntry& c = nnCache[pos.hash & (nnCache.size() - 1)];
        if (c.key != pos.hash) {
            c.key = pos.hash;
            c.cp = net->evalCp(pos);
        }
        e += (int)(c.cp * lim.nnWeight);
    }
    return e;
}

int Engine::noiseOf(Move m) const {
    if (lim.noiseCp <= 0) return 0;
    uint32_t h = (m & 0x7fff) * 2654435761u ^ lim.noiseSeed;
    h ^= h >> 15; h *= 0x2c1b3c6dU; h ^= h >> 12; h *= 0x297a2d39U; h ^= h >> 15;
    float u1 = (h & 0xffff) / 65535.f, u2 = (h >> 16) / 65535.f;
    return (int)((u1 + u2 - 1.f) * 2.f * lim.noiseCp);
}

static inline int toTT(int s, int ply) { return s >= MATE_SCORE - 1000 ? s + ply : s <= -MATE_SCORE + 1000 ? s - ply : s; }
static inline int fromTT(int s, int ply) { return s >= MATE_SCORE - 1000 ? s - ply : s <= -MATE_SCORE + 1000 ? s + ply : s; }

static bool hasNonPawn(const Position& p, int c) {
    for (int s = 0; s < 64; s++) {
        int q = p.sq[s];
        if (q && pcolor(q) == c && ptype(q) != PAWN && ptype(q) != KING) return true;
    }
    return false;
}

void Engine::scoreMoves(const MoveList& ml, int* sc, Move ttMove, int ply) {
    for (int i = 0; i < ml.n; i++) {
        Move m = ml.m[i];
        if (m == ttMove) sc[i] = 1 << 30;
        else if (m & MF_CAP) {
            int victim = (m & MF_EP) ? PAWN : ptype(pos.sq[mto(m)]);
            int attacker = ptype(pos.sq[mfrom(m)]);
            sc[i] = 1000000 + victim * 100 - attacker + (mpromo(m) ? 5000 : 0);
        } else if (mpromo(m)) sc[i] = 900000 + mpromo(m);
        else if (ply <= MAXPLY && killers[ply][0] == m) sc[i] = 800000;
        else if (ply <= MAXPLY && killers[ply][1] == m) sc[i] = 790000;
        else sc[i] = history[pos.side][mfrom(m)][mto(m)];
    }
}

static inline void pickNext(MoveList& ml, int* sc, int i) {
    int b = i;
    for (int j = i + 1; j < ml.n; j++)
        if (sc[j] > sc[b]) b = j;
    if (b != i) {
        std::swap(ml.m[i], ml.m[b]);
        std::swap(sc[i], sc[b]);
    }
}

int Engine::quiesce(int alpha, int beta, int ply) {
    if ((++nodes & 1023) == 0) checkTime();
    if (stopped) return 0;
    int stand = evaluate();
    if (!lim.quiesce || ply >= MAXPLY) return stand;
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    MoveList ml;
    pos.genPseudo(ml, true);
    int sc[256];
    scoreMoves(ml, sc, 0, MAXPLY + 1);
    int best = stand;
    for (int i = 0; i < ml.n; i++) {
        pickNext(ml, sc, i);
        Move m = ml.m[i];
        if (!mpromo(m)) {
            int victim = (m & MF_EP) ? PAWN : ptype(pos.sq[mto(m)]);
            if (stand + pieceValue(victim) + 200 < alpha) continue;
        }
        if (!pos.make(m)) continue;
        int s = -quiesce(-beta, -alpha, ply + 1);
        pos.unmake();
        if (stopped) return 0;
        if (s > best) {
            best = s;
            if (s > alpha) {
                alpha = s;
                if (s >= beta) break;
            }
        }
    }
    return best;
}

int Engine::negamax(int depth, int alpha, int beta, int ply, bool allowNull) {
    if (ply >= MAXPLY) return evaluate();
    if ((++nodes & 1023) == 0) checkTime();
    if (stopped) return 0;
    if (pos.halfmove >= 100 || pos.isRepetition() || pos.insufficientMaterial()) return 0;

    alpha = std::max(alpha, -MATE_SCORE + ply);
    beta = std::min(beta, MATE_SCORE - ply - 1);
    if (alpha >= beta) return alpha;

    bool inCheck = pos.inCheck();
    if (inCheck) depth++;
    if (depth <= 0) return quiesce(alpha, beta, ply);

    bool pv = beta - alpha > 1;
    TTEntry& e = tt[pos.hash & ttMask];
    Move ttMove = 0;
    if (e.key == pos.hash) {
        ttMove = e.move;
        if (!pv && e.depth >= depth) {
            int s = fromTT(e.score, ply);
            if (e.flag == TT_EXACT || (e.flag == TT_LOWER && s >= beta) || (e.flag == TT_UPPER && s <= alpha)) return s;
        }
    }

    int staticEval = inCheck ? -INF_SCORE : evaluate();
    if (!pv && !inCheck && std::abs(beta) < MATE_SCORE - 1000) {
        if (depth <= 3 && staticEval - 110 * depth >= beta) return staticEval;
        if (allowNull && depth >= 3 && staticEval >= beta && hasNonPawn(pos, pos.side)) {
            int R = 2 + depth / 6;
            pos.makeNull();
            int s = -negamax(depth - 1 - R, -beta, -beta + 1, ply + 1, false);
            pos.unmakeNull();
            if (stopped) return 0;
            if (s >= beta) return s >= MATE_SCORE - 1000 ? beta : s;
        }
    }

    MoveList ml;
    pos.genPseudo(ml);
    int sc[256];
    scoreMoves(ml, sc, ttMove, ply);
    int legal = 0, best = -INF_SCORE, origAlpha = alpha;
    Move bestM = 0;
    for (int i = 0; i < ml.n; i++) {
        pickNext(ml, sc, i);
        Move m = ml.m[i];
        bool quiet = !(m & MF_CAP) && !mpromo(m);
        if (!pv && !inCheck && quiet && legal > 0 && depth <= 2 && staticEval + 150 * depth <= alpha) continue;
        if (!pos.make(m)) continue;
        legal++;
        int s;
        if (legal == 1) {
            s = -negamax(depth - 1, -beta, -alpha, ply + 1, true);
        } else {
            int r = 0;
            if (depth >= 3 && quiet && !inCheck && legal > 3 && !pos.inCheck()) {
                r = 1 + (legal > 10) + (depth >= 8);
                if (pv) r--;
                if (r > depth - 2) r = depth - 2;
                if (r < 0) r = 0;
            }
            s = -negamax(depth - 1 - r, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && r > 0) s = -negamax(depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (s > alpha && s < beta) s = -negamax(depth - 1, -beta, -alpha, ply + 1, true);
        }
        pos.unmake();
        if (stopped) return 0;
        if (s > best) {
            best = s;
            bestM = m;
            if (s > alpha) {
                alpha = s;
                if (s >= beta) {
                    if (quiet) {
                        if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
                        int& h = history[pos.side][mfrom(m)][mto(m)];
                        h += depth * depth;
                        if (h > 600000) for (auto& a : history) for (auto& b : a) for (int& c : b) c /= 2;
                    }
                    break;
                }
            }
        }
    }
    if (legal == 0) return inCheck ? -MATE_SCORE + ply : 0;

    if (e.key != pos.hash || depth >= e.depth) {
        e.key = pos.hash;
        e.move = bestM;
        e.score = (int16_t)toTT(best, ply);
        e.depth = (int8_t)std::min(depth, 127);
        e.flag = best >= beta ? TT_LOWER : (best > origAlpha ? TT_EXACT : TT_UPPER);
    }
    return best;
}

int Engine::searchRoot(int depth, Move& bestOut) {
    int alpha = -INF_SCORE, beta = INF_SCORE, best = -INF_SCORE;
    Move bm = 0;
    std::fill(rootScores.begin(), rootScores.end(), -INF_SCORE);
    for (size_t i = 0; i < rootMoves.size(); i++) {
        Move m = rootMoves[i];
        int n = noiseOf(m);
        if (think) think->current = m;
        pos.make(m);
        int s;
        if (i == 0) {
            s = -negamax(depth - 1, -(beta - n), -(alpha - n), 1, true) + n;
        } else {
            s = -negamax(depth - 1, -(alpha - n) - 1, -(alpha - n), 1, true) + n;
            if (!stopped && s > alpha) s = -negamax(depth - 1, -(beta - n), -(alpha - n), 1, true) + n;
        }
        pos.unmake();
        if (stopped) break;
        rootScores[i] = s;
        if (s > best) {
            best = s;
            bm = m;
            if (s > alpha) alpha = s;
        }
    }
    // Best move first next iteration, the rest by their (bounded) scores.
    std::vector<size_t> order(rootMoves.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        bool ab = rootMoves[a] == bm, bb = rootMoves[b] == bm;
        if (ab != bb) return ab;
        return rootScores[a] > rootScores[b];
    });
    std::vector<Move> nm;
    std::vector<int> ns;
    for (size_t i : order) { nm.push_back(rootMoves[i]); ns.push_back(rootScores[i]); }
    rootMoves = nm;
    rootScores = ns;
    bestOut = bm;
    return best;
}

SearchResult Engine::search(const Position& root, const SearchLimits& L, std::atomic<bool>* stop, const Net* n,
                            ThinkInfo* ti) {
    think = ti;
    if (n != nnCacheNet) {   // a newly trained brain: old answers are stale
        std::fill(nnCache.begin(), nnCache.end(), NNEntry{0, 0});
        nnCacheNet = n;
    }
    pos = root;
    lim = L;
    ext = stop;
    net = n;
    stopped = false;
    completed = 0;
    nodes = 0;
    t0 = std::chrono::steady_clock::now();
    memset(killers, 0, sizeof(killers));
    for (auto& a : history) for (auto& b : a) for (int& c : b) c /= 4;

    SearchResult r;
    MoveList ml;
    pos.genLegal(ml);
    if (ml.n == 0) return r;
    int sc[256];
    Move ttMove = tt[pos.hash & ttMask].key == pos.hash ? tt[pos.hash & ttMask].move : 0;
    scoreMoves(ml, sc, ttMove, 0);
    rootMoves.clear();
    for (int i = 0; i < ml.n; i++) {
        pickNext(ml, sc, i);
        rootMoves.push_back(ml.m[i]);
    }
    rootScores.assign(rootMoves.size(), -INF_SCORE);
    r.best = rootMoves[0];

    for (int d = 1; d <= L.maxDepth && d < MAXPLY; d++) {
        Move bm = 0;
        int s = searchRoot(d, bm);
        if (bm) r.best = bm;
        if (stopped) break;
        completed = d;
        r.score = s;
        r.depth = d;
        if (think) {
            think->best = r.best;
            think->depth = d;
            think->score = s;
            int k = (int)std::min<size_t>(4, rootMoves.size());
            for (int i = 0; i < k; i++) think->cand[i] = rootMoves[i];
            think->ncand = k;
        }
        if (std::abs(s) >= MATE_SCORE - 200) break;
        if (elapsedMs() > L.moveTimeMs * 55 / 100) break;
    }
    r.nodes = nodes;
    return r;
}
