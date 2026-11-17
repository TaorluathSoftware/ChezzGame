#include "eval.h"
#include <cstdlib>

static const int MAT_MG[7] = {0, 100, 320, 330, 500, 950, 0};
static const int MAT_EG[7] = {0, 120, 300, 320, 530, 950, 0};
static const int PHASE_W[7] = {0, 0, 1, 1, 2, 4, 0};

// Tables are written as seen from White: first row = rank 8.
static const int PAWN_MG[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0};
static const int PAWN_EG[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    80, 80, 80, 80, 80, 80, 80, 80,
    50, 50, 50, 50, 50, 50, 50, 50,
    30, 30, 30, 30, 30, 30, 30, 30,
    20, 20, 20, 20, 20, 20, 20, 20,
    10, 10, 10, 10, 10, 10, 10, 10,
     5,  5,  5,  5,  5,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0};
static const int KNIGHT_T[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50};
static const int BISHOP_T[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20};
static const int ROOK_T[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
      5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      0,  0,  0,  5,  5,  0,  0,  0};
static const int QUEEN_T[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20};
static const int KING_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20};
static const int KING_EG[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50};

static const int PASS_MG[8] = {0, 5, 10, 15, 25, 40, 60, 0};
static const int PASS_EG[8] = {0, 10, 20, 35, 60, 100, 150, 0};

int pieceValue(int type) { return MAT_MG[type]; }

static inline int pstIndex(int s, int c) {
    return c == WHITE ? (7 - sqrank(s)) * 8 + sqfile(s) : sqrank(s) * 8 + sqfile(s);
}

static int slideCount(const Position& pos, int s, int d0, int step, int us) {
    int n = 0;
    for (int d = d0; d < 8; d += step)
        for (int k = 0; k < RAY_N[s][d]; k++) {
            int q = pos.sq[RAY[s][d][k]];
            if (!q) { n++; continue; }
            if (pcolor(q) != us) n++;
            break;
        }
    return n;
}

int evalHCE(const Position& pos, float posScale) {
    int mat[2][2] = {{0, 0}, {0, 0}};   // [color][mg/eg]
    int ps[2][2] = {{0, 0}, {0, 0}};    // positional
    int phase = 0, bishops[2] = {0, 0}, pawnCount[2] = {0, 0}, nonPawn[2] = {0, 0};
    int pawnsOnFile[2][8] = {}, minRank[2][8], maxRank[2][8];
    for (int f = 0; f < 8; f++) minRank[0][f] = minRank[1][f] = 8, maxRank[0][f] = maxRank[1][f] = -1;

    for (int s = 0; s < 64; s++) {
        int p = pos.sq[s];
        if (ptype(p) != PAWN) continue;
        int c = pcolor(p), f = sqfile(s), r = sqrank(s);
        pawnsOnFile[c][f]++;
        pawnCount[c]++;
        if (r < minRank[c][f]) minRank[c][f] = r;
        if (r > maxRank[c][f]) maxRank[c][f] = r;
    }

    for (int s = 0; s < 64; s++) {
        int p = pos.sq[s];
        if (!p) continue;
        int c = pcolor(p), t = ptype(p), idx = pstIndex(s, c), f = sqfile(s), r = sqrank(s);
        mat[c][0] += MAT_MG[t];
        mat[c][1] += MAT_EG[t];
        phase += PHASE_W[t];
        if (t != PAWN && t != KING) nonPawn[c] += MAT_MG[t];
        switch (t) {
            case PAWN: {
                ps[c][0] += PAWN_MG[idx];
                ps[c][1] += PAWN_EG[idx];
                if (pawnsOnFile[c][f] > 1) { ps[c][0] -= 8; ps[c][1] -= 16; }
                bool isolated = (f == 0 || !pawnsOnFile[c][f - 1]) && (f == 7 || !pawnsOnFile[c][f + 1]);
                if (isolated) { ps[c][0] -= 10; ps[c][1] -= 14; }
                bool passed = true;
                for (int ff = f - 1; ff <= f + 1 && passed; ff++) {
                    if (ff < 0 || ff > 7) continue;
                    if (c == WHITE ? maxRank[BLACK][ff] > r : minRank[WHITE][ff] < r) passed = false;
                }
                if (passed) {
                    int rel = c == WHITE ? r : 7 - r;
                    ps[c][0] += PASS_MG[rel];
                    ps[c][1] += PASS_EG[rel];
                }
                break;
            }
            case KNIGHT: {
                ps[c][0] += KNIGHT_T[idx];
                ps[c][1] += KNIGHT_T[idx] / 2;
                int mob = 0;
                for (int i = 0; i < KN_N[s]; i++) {
                    int q = pos.sq[KN_T[s][i]];
                    if (!q || pcolor(q) != c) mob++;
                }
                ps[c][0] += (mob - 4) * 4;
                ps[c][1] += (mob - 4) * 4;
                break;
            }
            case BISHOP: {
                bishops[c]++;
                ps[c][0] += BISHOP_T[idx];
                ps[c][1] += BISHOP_T[idx] / 2;
                int mob = slideCount(pos, s, 1, 2, c);
                ps[c][0] += (mob - 6) * 5;
                ps[c][1] += (mob - 6) * 5;
                break;
            }
            case ROOK: {
                ps[c][0] += ROOK_T[idx];
                ps[c][1] += ROOK_T[idx];
                int mob = slideCount(pos, s, 0, 2, c);
                ps[c][0] += (mob - 7) * 2;
                ps[c][1] += (mob - 7) * 4;
                if (!pawnsOnFile[c][f]) {
                    int bonus = pawnsOnFile[c ^ 1][f] ? 10 : 20;
                    ps[c][0] += bonus;
                    ps[c][1] += bonus / 2;
                }
                break;
            }
            case QUEEN: {
                ps[c][0] += QUEEN_T[idx];
                ps[c][1] += QUEEN_T[idx];
                int mob = slideCount(pos, s, 0, 1, c);
                ps[c][0] += (mob - 13) * 1;
                ps[c][1] += (mob - 13) * 2;
                break;
            }
            case KING: {
                ps[c][0] += KING_MG[idx];
                ps[c][1] += KING_EG[idx];
                int rel = c == WHITE ? r : 7 - r, dir = c == WHITE ? 1 : -1;
                if (rel <= 1) {
                    for (int ff = f - 1; ff <= f + 1; ff++) {
                        if (ff < 0 || ff > 7) continue;
                        int r1 = r + dir, r2 = r + 2 * dir;
                        if (r1 >= 0 && r1 < 8 && pos.sq[r1 * 8 + ff] == mkpiece(c, PAWN)) ps[c][0] += 10;
                        else if (r2 >= 0 && r2 < 8 && pos.sq[r2 * 8 + ff] == mkpiece(c, PAWN)) ps[c][0] += 5;
                        else if (!pawnsOnFile[c][ff]) ps[c][0] -= 12;
                    }
                }
                break;
            }
        }
    }
    for (int c = 0; c < 2; c++)
        if (bishops[c] >= 2) { ps[c][0] += 30; ps[c][1] += 50; }

    // Mop-up: drive a bare king to the edge so won endings actually get converted.
    for (int c = 0; c < 2; c++) {
        int o = c ^ 1;
        if (pawnCount[o] == 0 && nonPawn[o] == 0 && mat[c][1] - mat[o][1] >= 400) {
            int ok = pos.king[o], mk = pos.king[c];
            int cd = std::abs(2 * sqfile(ok) - 7) + std::abs(2 * sqrank(ok) - 7);
            int kd = std::abs(sqfile(ok) - sqfile(mk)) + std::abs(sqrank(ok) - sqrank(mk));
            ps[c][1] += cd * 10 + (14 - kd) * 6;
        }
    }

    int ph = phase > 24 ? 24 : phase;
    int mgS = (mat[0][0] - mat[1][0]) + (int)((ps[0][0] - ps[1][0]) * posScale);
    int egS = (mat[0][1] - mat[1][1]) + (int)((ps[0][1] - ps[1][1]) * posScale);
    int score = (mgS * ph + egS * (24 - ph)) / 24;

    // A pawnless side that is only a minor piece up rarely wins.
    int strong = score > 0 ? WHITE : BLACK;
    if (pawnCount[strong] == 0 && nonPawn[strong] - nonPawn[strong ^ 1] <= 330) score /= 4;

    score = pos.side == WHITE ? score : -score;
    return score + (int)(10 * posScale);
}
