#include "chess.h"
#include <cctype>
#include <cstring>
#include <sstream>

uint64_t Z_PIECE[16][64], Z_SIDE, Z_CASTLE[16], Z_EP[8];
int KN_T[64][8], KN_N[64], KG_T[64][8], KG_N[64];
int RAY[64][8][7], RAY_N[64][8];
static int CASTLE_MASK[64];
static const int DF[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int DR[8] = {0, 1, 1, 1, 0, -1, -1, -1};

static uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline bool onBoard(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

void chessInit() {
    static bool done = false;
    if (done) return;
    done = true;
    uint64_t seed = 0xC4E55C0FFEEULL;
    for (auto& row : Z_PIECE) for (auto& z : row) z = splitmix(seed);
    Z_SIDE = splitmix(seed);
    for (auto& z : Z_CASTLE) z = splitmix(seed);
    for (auto& z : Z_EP) z = splitmix(seed);

    static const int KNF[8] = {1, 2, 2, 1, -1, -2, -2, -1}, KNR[8] = {2, 1, -1, -2, -2, -1, 1, 2};
    for (int s = 0; s < 64; s++) {
        int f = sqfile(s), r = sqrank(s);
        KN_N[s] = KG_N[s] = 0;
        for (int i = 0; i < 8; i++) {
            if (onBoard(f + KNF[i], r + KNR[i])) KN_T[s][KN_N[s]++] = (r + KNR[i]) * 8 + f + KNF[i];
            if (onBoard(f + DF[i], r + DR[i])) KG_T[s][KG_N[s]++] = (r + DR[i]) * 8 + f + DF[i];
        }
        for (int d = 0; d < 8; d++) {
            RAY_N[s][d] = 0;
            int nf = f + DF[d], nr = r + DR[d];
            while (onBoard(nf, nr)) {
                RAY[s][d][RAY_N[s][d]++] = nr * 8 + nf;
                nf += DF[d];
                nr += DR[d];
            }
        }
        CASTLE_MASK[s] = 15;
    }
    CASTLE_MASK[0] = 15 & ~2;
    CASTLE_MASK[7] = 15 & ~1;
    CASTLE_MASK[4] = 15 & ~3;
    CASTLE_MASK[56] = 15 & ~8;
    CASTLE_MASK[63] = 15 & ~4;
    CASTLE_MASK[60] = 15 & ~12;
}

std::string squareName(int s) {
    std::string r;
    r += char('a' + sqfile(s));
    r += char('1' + sqrank(s));
    return r;
}

void Position::setStart() { setFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); }

bool Position::setFEN(const std::string& f) {
    std::istringstream ss(f);
    std::string board, stm, cast, eps;
    int hm = 0, fm = 1;
    ss >> board >> stm >> cast >> eps;
    if (!(ss >> hm)) hm = 0;
    if (!(ss >> fm)) fm = 1;
    memset(sq, 0, sizeof(sq));
    int r = 7, file = 0;
    for (char c : board) {
        if (c == '/') { r--; file = 0; continue; }
        if (isdigit((unsigned char)c)) { file += c - '0'; continue; }
        int col = isupper((unsigned char)c) ? WHITE : BLACK, t;
        switch (tolower((unsigned char)c)) {
            case 'p': t = PAWN; break;
            case 'n': t = KNIGHT; break;
            case 'b': t = BISHOP; break;
            case 'r': t = ROOK; break;
            case 'q': t = QUEEN; break;
            case 'k': t = KING; break;
            default: return false;
        }
        if (r < 0 || file > 7) return false;
        sq[r * 8 + file] = (int8_t)mkpiece(col, t);
        file++;
    }
    side = stm == "b" ? BLACK : WHITE;
    castling = 0;
    for (char c : cast) {
        if (c == 'K') castling |= 1;
        if (c == 'Q') castling |= 2;
        if (c == 'k') castling |= 4;
        if (c == 'q') castling |= 8;
    }
    ep = -1;
    if (eps.size() == 2) ep = (eps[0] - 'a') + (eps[1] - '1') * 8;
    halfmove = hm;
    fullmove = fm;
    for (int s = 0; s < 64; s++)
        if (ptype(sq[s]) == KING) king[pcolor(sq[s])] = s;
    hash = computeHash();
    hist.clear();
    return true;
}

void Position::setBoard(const int8_t* board, int stm) {
    memcpy(sq, board, 64);
    side = stm;
    castling = 0;
    ep = -1;
    halfmove = 0;
    fullmove = 1;
    for (int s = 0; s < 64; s++)
        if (ptype(sq[s]) == KING) king[pcolor(sq[s])] = s;
    hash = computeHash();
    hist.clear();
}

std::string Position::fen() const {
    static const char PC[] = " pnbrqk";
    std::string out;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int p = sq[r * 8 + f];
            if (!p) { empty++; continue; }
            if (empty) { out += char('0' + empty); empty = 0; }
            char c = PC[ptype(p)];
            out += pcolor(p) == WHITE ? (char)toupper(c) : c;
        }
        if (empty) out += char('0' + empty);
        if (r) out += '/';
    }
    out += side == WHITE ? " w " : " b ";
    std::string c;
    if (castling & 1) c += 'K';
    if (castling & 2) c += 'Q';
    if (castling & 4) c += 'k';
    if (castling & 8) c += 'q';
    out += c.empty() ? "-" : c;
    out += ' ';
    out += ep >= 0 ? squareName(ep) : "-";
    out += " " + std::to_string(halfmove) + " " + std::to_string(fullmove);
    return out;
}

uint64_t Position::computeHash() const {
    uint64_t h = 0;
    for (int s = 0; s < 64; s++)
        if (sq[s]) h ^= Z_PIECE[sq[s]][s];
    h ^= Z_CASTLE[castling];
    if (ep >= 0) h ^= Z_EP[sqfile(ep)];
    if (side == BLACK) h ^= Z_SIDE;
    return h;
}

bool Position::attacked(int s, int by) const {
    int f = sqfile(s);
    if (by == WHITE) {
        if (f > 0 && s - 9 >= 0 && sq[s - 9] == mkpiece(WHITE, PAWN)) return true;
        if (f < 7 && s - 7 >= 0 && sq[s - 7] == mkpiece(WHITE, PAWN)) return true;
    } else {
        if (f > 0 && s + 7 < 64 && sq[s + 7] == mkpiece(BLACK, PAWN)) return true;
        if (f < 7 && s + 9 < 64 && sq[s + 9] == mkpiece(BLACK, PAWN)) return true;
    }
    int kn = mkpiece(by, KNIGHT), kg = mkpiece(by, KING);
    for (int i = 0; i < KN_N[s]; i++)
        if (sq[KN_T[s][i]] == kn) return true;
    for (int i = 0; i < KG_N[s]; i++)
        if (sq[KG_T[s][i]] == kg) return true;
    for (int d = 0; d < 8; d++) {
        for (int k = 0; k < RAY_N[s][d]; k++) {
            int p = sq[RAY[s][d][k]];
            if (!p) continue;
            if (pcolor(p) == by) {
                int t = ptype(p);
                if (t == QUEEN || ((d & 1) ? t == BISHOP : t == ROOK)) return true;
            }
            break;
        }
    }
    return false;
}

static void addPromos(MoveList& ml, int f, int t, uint32_t flags, bool queenOnly) {
    ml.add(mkmove(f, t, QUEEN, flags));
    if (queenOnly) return;
    ml.add(mkmove(f, t, KNIGHT, flags));
    ml.add(mkmove(f, t, ROOK, flags));
    ml.add(mkmove(f, t, BISHOP, flags));
}

void Position::genPseudo(MoveList& ml, bool capsOnly) const {
    int us = side, them = us ^ 1;
    for (int s = 0; s < 64; s++) {
        int p = sq[s];
        if (!p || pcolor(p) != us) continue;
        int t = ptype(p);
        if (t == PAWN) {
            int fwd = us == WHITE ? 8 : -8, r = sqrank(s);
            int promoR = us == WHITE ? 6 : 1, startR = us == WHITE ? 1 : 6;
            int to = s + fwd;
            if (sq[to] == 0) {
                if (r == promoR) addPromos(ml, s, to, 0, capsOnly);
                else if (!capsOnly) {
                    ml.add(mkmove(s, to));
                    if (r == startR && sq[to + fwd] == 0) ml.add(mkmove(s, to + fwd, 0, MF_DOUBLE));
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                int f = sqfile(s) + df;
                if (f < 0 || f > 7) continue;
                int c = to + df;
                if (sq[c] && pcolor(sq[c]) == them) {
                    if (r == promoR) addPromos(ml, s, c, MF_CAP, capsOnly);
                    else ml.add(mkmove(s, c, 0, MF_CAP));
                } else if (c == ep) {
                    ml.add(mkmove(s, c, 0, MF_CAP | MF_EP));
                }
            }
        } else if (t == KNIGHT || t == KING) {
            const int* T = t == KNIGHT ? KN_T[s] : KG_T[s];
            int n = t == KNIGHT ? KN_N[s] : KG_N[s];
            for (int i = 0; i < n; i++) {
                int q = sq[T[i]];
                if (!q) { if (!capsOnly) ml.add(mkmove(s, T[i])); }
                else if (pcolor(q) == them) ml.add(mkmove(s, T[i], 0, MF_CAP));
            }
        } else {
            int d0 = t == BISHOP ? 1 : 0, step = t == QUEEN ? 1 : 2;
            for (int d = d0; d < 8; d += step) {
                for (int k = 0; k < RAY_N[s][d]; k++) {
                    int to = RAY[s][d][k], q = sq[to];
                    if (!q) { if (!capsOnly) ml.add(mkmove(s, to)); continue; }
                    if (pcolor(q) == them) ml.add(mkmove(s, to, 0, MF_CAP));
                    break;
                }
            }
        }
    }
    if (capsOnly) return;
    if (us == WHITE) {
        if ((castling & 1) && sq[7] == mkpiece(WHITE, ROOK) && !sq[5] && !sq[6] &&
            !attacked(4, BLACK) && !attacked(5, BLACK) && !attacked(6, BLACK))
            ml.add(mkmove(4, 6, 0, MF_CASTLE));
        if ((castling & 2) && sq[0] == mkpiece(WHITE, ROOK) && !sq[3] && !sq[2] && !sq[1] &&
            !attacked(4, BLACK) && !attacked(3, BLACK) && !attacked(2, BLACK))
            ml.add(mkmove(4, 2, 0, MF_CASTLE));
    } else {
        if ((castling & 4) && sq[63] == mkpiece(BLACK, ROOK) && !sq[61] && !sq[62] &&
            !attacked(60, WHITE) && !attacked(61, WHITE) && !attacked(62, WHITE))
            ml.add(mkmove(60, 62, 0, MF_CASTLE));
        if ((castling & 8) && sq[56] == mkpiece(BLACK, ROOK) && !sq[59] && !sq[58] && !sq[57] &&
            !attacked(60, WHITE) && !attacked(59, WHITE) && !attacked(58, WHITE))
            ml.add(mkmove(60, 58, 0, MF_CASTLE));
    }
}

void Position::genLegal(MoveList& out) {
    MoveList tmp;
    genPseudo(tmp);
    out.n = 0;
    for (int i = 0; i < tmp.n; i++)
        if (make(tmp.m[i])) { unmake(); out.add(tmp.m[i]); }
}

bool Position::make(Move m) {
    Undo u;
    u.m = m; u.castling = castling; u.ep = ep; u.halfmove = halfmove; u.hash = hash; u.captured = 0;
    int f = mfrom(m), t = mto(m), p = sq[f], us = side;
    hash ^= Z_CASTLE[castling];
    if (ep >= 0) hash ^= Z_EP[sqfile(ep)];
    if (m & MF_EP) {
        int cs = t + (us == WHITE ? -8 : 8);
        u.captured = sq[cs];
        sq[cs] = 0;
        hash ^= Z_PIECE[u.captured][cs];
    } else if (sq[t]) {
        u.captured = sq[t];
        hash ^= Z_PIECE[u.captured][t];
    }
    sq[t] = (int8_t)p;
    sq[f] = 0;
    hash ^= Z_PIECE[p][f] ^ Z_PIECE[p][t];
    if (mpromo(m)) {
        int np = mkpiece(us, mpromo(m));
        sq[t] = (int8_t)np;
        hash ^= Z_PIECE[p][t] ^ Z_PIECE[np][t];
    }
    if (m & MF_CASTLE) {
        int rf = t > f ? f + 3 : f - 4, rt = t > f ? f + 1 : f - 1;
        int r = sq[rf];
        sq[rt] = (int8_t)r;
        sq[rf] = 0;
        hash ^= Z_PIECE[r][rf] ^ Z_PIECE[r][rt];
    }
    if (ptype(p) == KING) king[us] = t;
    castling &= CASTLE_MASK[f] & CASTLE_MASK[t];
    hash ^= Z_CASTLE[castling];
    ep = -1;
    if (m & MF_DOUBLE) {
        ep = (f + t) / 2;
        hash ^= Z_EP[sqfile(ep)];
    }
    halfmove = (ptype(p) == PAWN || u.captured) ? 0 : halfmove + 1;
    if (us == BLACK) fullmove++;
    side ^= 1;
    hash ^= Z_SIDE;
    hist.push_back(u);
    if (attacked(king[us], side)) {
        unmake();
        return false;
    }
    return true;
}

void Position::unmake() {
    Undo u = hist.back();
    hist.pop_back();
    side ^= 1;
    int us = side;
    Move m = u.m;
    int f = mfrom(m), t = mto(m), p = sq[t];
    if (mpromo(m)) p = mkpiece(us, PAWN);
    sq[f] = (int8_t)p;
    sq[t] = 0;
    if (m & MF_EP) sq[t + (us == WHITE ? -8 : 8)] = (int8_t)u.captured;
    else sq[t] = (int8_t)u.captured;
    if (m & MF_CASTLE) {
        int rf = t > f ? f + 3 : f - 4, rt = t > f ? f + 1 : f - 1;
        sq[rf] = sq[rt];
        sq[rt] = 0;
    }
    if (ptype(p) == KING) king[us] = f;
    castling = u.castling;
    ep = u.ep;
    halfmove = u.halfmove;
    hash = u.hash;
    if (us == BLACK) fullmove--;
}

void Position::makeNull() {
    Undo u;
    u.m = 0; u.captured = 0; u.castling = castling; u.ep = ep; u.halfmove = halfmove; u.hash = hash;
    hist.push_back(u);
    if (ep >= 0) hash ^= Z_EP[sqfile(ep)];
    ep = -1;
    hash ^= Z_SIDE;
    side ^= 1;
    halfmove = 0;   // stops repetition scans from crossing the null move
}

void Position::unmakeNull() {
    Undo u = hist.back();
    hist.pop_back();
    side ^= 1;
    ep = u.ep;
    halfmove = u.halfmove;
    hash = u.hash;
}

bool Position::isRepetition() const {
    int n = (int)hist.size();
    for (int i = n - 2, k = 2; i >= 0 && k <= halfmove; i -= 2, k += 2)
        if (hist[i].hash == hash) return true;
    return false;
}

int Position::repetitionCount() const {
    int n = (int)hist.size(), c = 0;
    for (int i = n - 2, k = 2; i >= 0 && k <= halfmove; i -= 2, k += 2)
        if (hist[i].hash == hash) c++;
    return c;
}

bool Position::insufficientMaterial() const {
    int minors = 0, bishopColor[2] = {0, 0}, bishops = 0;
    for (int s = 0; s < 64; s++) {
        int t = ptype(sq[s]);
        if (t == PAWN || t == ROOK || t == QUEEN) return false;
        if (t == KNIGHT) minors++;
        if (t == BISHOP) { minors++; bishops++; bishopColor[(sqfile(s) + sqrank(s)) & 1]++; }
    }
    if (minors <= 1) return true;
    return minors == bishops && (bishopColor[0] == 0 || bishopColor[1] == 0);
}

int Position::outcome(std::string* reason) {
    MoveList ml;
    genLegal(ml);
    if (ml.n == 0) {
        if (inCheck()) {
            if (reason) *reason = "checkmate";
            return side == WHITE ? -1 : 1;
        }
        if (reason) *reason = "stalemate";
        return 0;
    }
    if (halfmove >= 100) { if (reason) *reason = "50-move rule"; return 0; }
    if (repetitionCount() >= 2) { if (reason) *reason = "threefold repetition"; return 0; }
    if (insufficientMaterial()) { if (reason) *reason = "insufficient material"; return 0; }
    return 2;
}

std::string Position::san(Move m) {
    static const char PC[] = " PNBRQK";
    int f = mfrom(m), t = mto(m), p = sq[f], ty = ptype(p);
    std::string s;
    if (m & MF_CASTLE) {
        s = t > f ? "O-O" : "O-O-O";
    } else {
        if (ty == PAWN) {
            if (m & MF_CAP) { s += char('a' + sqfile(f)); s += 'x'; }
        } else {
            s += PC[ty];
            MoveList ml;
            genLegal(ml);
            bool amb = false, sameFile = false, sameRank = false;
            for (int i = 0; i < ml.n; i++) {
                Move o = ml.m[i];
                if (mto(o) != t || mfrom(o) == f || sq[mfrom(o)] != p) continue;
                amb = true;
                if (sqfile(mfrom(o)) == sqfile(f)) sameFile = true;
                if (sqrank(mfrom(o)) == sqrank(f)) sameRank = true;
            }
            if (amb) {
                if (!sameFile) s += char('a' + sqfile(f));
                else if (!sameRank) s += char('1' + sqrank(f));
                else s += squareName(f);
            }
            if (m & MF_CAP) s += 'x';
        }
        s += squareName(t);
        if (mpromo(m)) { s += '='; s += PC[mpromo(m)]; }
    }
    if (make(m)) {
        if (inCheck()) {
            MoveList ml;
            genLegal(ml);
            s += ml.n ? '+' : '#';
        }
        unmake();
    }
    return s;
}

std::string Position::uci(Move m) const {
    static const char PC[] = "  nbrq";
    std::string s = squareName(mfrom(m)) + squareName(mto(m));
    if (mpromo(m)) s += PC[mpromo(m)];
    return s;
}

uint64_t perft(Position& p, int depth) {
    MoveList ml;
    p.genPseudo(ml);
    uint64_t n = 0;
    for (int i = 0; i < ml.n; i++) {
        if (!p.make(ml.m[i])) continue;
        n += depth <= 1 ? 1 : perft(p, depth - 1);
        p.unmake();
    }
    return n;
}
