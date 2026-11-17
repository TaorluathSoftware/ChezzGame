// Chess rules: board, move generation, make/unmake, SAN, FEN.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum { EMPTY = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { WHITE = 0, BLACK = 1 };

// piece = type | (color << 3); square 0 = a1, 63 = h8
inline int ptype(int p) { return p & 7; }
inline int pcolor(int p) { return p >> 3; }
inline int mkpiece(int c, int t) { return t | (c << 3); }
inline int sqfile(int s) { return s & 7; }
inline int sqrank(int s) { return s >> 3; }

typedef uint32_t Move;
enum : uint32_t { MF_CAP = 1u << 15, MF_EP = 1u << 16, MF_CASTLE = 1u << 17, MF_DOUBLE = 1u << 18 };
inline int mfrom(Move m) { return m & 63; }
inline int mto(Move m) { return (m >> 6) & 63; }
inline int mpromo(Move m) { return (m >> 12) & 7; }
inline Move mkmove(int f, int t, int promo = 0, uint32_t flags = 0) {
    return (Move)(f | (t << 6) | (promo << 12)) | flags;
}

extern int KN_T[64][8], KN_N[64], KG_T[64][8], KG_N[64];
extern int RAY[64][8][7], RAY_N[64][8];   // dir: 0 E,1 NE,2 N,3 NW,4 W,5 SW,6 S,7 SE (odd = diagonal)
void chessInit();

struct Undo { Move m; int captured, castling, ep, halfmove; uint64_t hash; };

struct MoveList {
    Move m[256];
    int n = 0;
    void add(Move x) { m[n++] = x; }
};

struct Position {
    int8_t sq[64];
    int side = WHITE, castling = 0, ep = -1, halfmove = 0, fullmove = 1;
    int king[2] = {4, 60};
    uint64_t hash = 0;
    std::vector<Undo> hist;

    void setStart();
    bool setFEN(const std::string& f);
    void setBoard(const int8_t* board, int stm);   // bare board, no castling/ep (used by the learner)
    std::string fen() const;
    uint64_t computeHash() const;

    bool attacked(int s, int by) const;
    bool inCheck() const { return attacked(king[side], side ^ 1); }
    void genPseudo(MoveList& ml, bool capturesOnly = false) const;
    void genLegal(MoveList& ml);
    bool make(Move m);   // false (and nothing changed) if the move leaves the king in check
    void unmake();
    void makeNull();
    void unmakeNull();

    bool isRepetition() const;
    int repetitionCount() const;
    bool insufficientMaterial() const;
    // 2 = game continues, otherwise result from White's view (+1/0/-1); reason filled if decided
    int outcome(std::string* reason = nullptr);

    std::string san(Move m);
    std::string uci(Move m) const;
};

std::string squareName(int s);
uint64_t perft(Position& p, int depth);
