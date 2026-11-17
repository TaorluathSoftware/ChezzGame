#include "levels.h"
#include "patterns.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static const char* TIER_TITLE[15] = {
    "Pawn Pal", "Village Player", "Club Rookie", "Club Regular", "Squire",
    "Tactician", "Strategist", "Expert", "Candidate Master", "Master",
    "International Master", "Grandmaster", "Super Grandmaster", "Machine", "Legend"};

static const char* TIER_BLURB[15] = {
    "Just learned how the horsey moves. Leaves pieces lying around and rarely notices threats.",
    "Plays on the village green on Sundays. Knows the rules, forgets to look at your last move.",
    "Fresh club member. Grabs material eagerly and occasionally walks into a fork.",
    "A regular at club night. Develops pieces sensibly but cracks under pressure.",
    "Sound fundamentals and a sharp eye for loose pieces. Still misjudges long plans.",
    "Loves combinations. Will punish a hanging piece and set little traps of its own.",
    "Thinks in plans: pawn breaks, open files, weak squares. Hard to bluff.",
    "Rarely blunders. Converts small advantages and defends stubbornly.",
    "Tournament strength. Calculates deep and squeezes endgames for every drop.",
    "Precise and patient. Mistakes against it are punished quickly.",
    "Deep calculation, strong positional sense. Expects you to bring your best.",
    "Grandmaster-class search. Finds resources in positions that look lost.",
    "Plays near its full strength with only the faintest hint of variety.",
    "Cold, exhaustive calculation. Every move searched to the limit of its clock.",
    "The engine at full power: longest think time, no handicaps, no mercy."};

// From names.txt, top = AI 1, bottom = AI 150. That file has 140 names; the ten marked FILL were
// added to reach 150 (AI 130-139). A names.txt with 150 lines next to Chezz.exe or in data/ replaces
// the whole list at startup (see loadLevelNames).
static const char* NAMES[150] = {
    "Billy", "Bobby", "Timmy", "Jeff", "Kevin",
    "Barry", "Greg", "Derek", "Steve", "Gary",
    "Terry", "Ronnie", "Eddie", "Freddy", "Johnny",
    "Mikey", "Benny", "Danny", "Ricky", "Jimmy",
    "Little Bobby", "Uncle Jeff", "Big Barry", "Crazy Kevin", "Sleepy Steve",
    "Blindfold Bill", "Lucky Larry", "Rusty Ron", "Clumsy Carl", "Nervous Ned",
    "Quiet Quinn", "Sneaky Pete", "Cheeky Charlie", "Average Andy", "Clever Trevor",
    "Rapid Ricky", "Tactical Terry", "Bishop Brian", "Knight Nigel", "Rook Richard",
    "Pawn Patrick", "Gambit George", "Casual Colin", "Positional Paul", "Opening Oliver",
    "Endgame Edward", "Calculating Carl", "Defensive Dave", "Attacking Arthur", "Strategic Steven",
    "Benjamin", "Nathan", "Oliver", "Alexander", "Theodore",
    "Sebastian", "Nicholas", "Jonathan", "Christopher", "Frederick",
    "Maxwell", "Vincent", "Leonard", "Lawrence", "Edmund",
    "Harrison", "Winston", "Rupert", "Percival", "Reginald",
    "Montgomery", "Bartholomew", "Archibald", "Cornelius", "Alistair",
    "Fitzgerald", "Wilfred", "Ambrose", "Augustus", "Maximilian",
    "Julius Caesar", "Theodore James", "Sir Reginald", "Lord Percival", "Professor Edmund",
    "Dr. Maxwell", "Mr. Harrison", "Sir Archibald", "Professor Kingsley", "Lord Montgomery",
    "Alexander the Wise", "Theodore the Calculated", "Sebastian Sharp", "Winston White", "Edmund Black",
    "Victor Kingsley", "Arthur Sterling", "Nathaniel Graves", "Maxwell Hart", "Leonard Bishop",
    "Frederick Knight", "Oliver Rook", "Benjamin Gambit", "Charles Hastings", "Edward Wellington",
    "William Ashford", "Henry Blackwood", "James Whitmore", "Richard Sterling", "Thomas Kingsley",
    "Alfred Hawthorne", "George Pembroke", "Constantine the Great", "Sebastian Montgomery", "Augustus Wellington",
    "Theodore Blackwell", "Alexander Whitmore", "Lord Ashford", "Sir Wellington", "Professor Blackwood",
    "Dr. Sterling", "Grandmaster Gregory", "Master Maxwell", "Victor the Magnificent", "Theodore the Unbeaten",
    "Alexander the Great", "Sir Kingsley", "Lord Blackwell", "Professor Whitmore", "Grandmaster Hawthorne",
    "Master Pembroke", "Sir Hastings", "Lord Sterling", "Professor Ashford", "Dr. Blackwood",
    "Edmund the Relentless", "Winston the Unshaken", "Maximilian the Precise", "Augustus the Undefeated", "The Chessmaster",
    "The Grandmaster", "The Last Gentleman", "The Old Master", "The Professor", "The Mathematician",
    "The Strategist", "The Unchallenged", "The Eternal Champion", "The Final Master", "The Perfect Player"};

static std::vector<Level> build() {
    std::vector<Level> v;
    for (int i = 0; i < 150; i++) {
        Level L;
        L.number = i + 1;
        L.rating = (int)std::lround((250.0 + i * 3250.0 / 149.0) / 10.0) * 10;   // 250, 270, 290 ... 3500
        L.tier = i / 10;
        L.name = NAMES[i];
        L.title = TIER_TITLE[L.tier];
        L.blurb = TIER_BLURB[L.tier];
        float f = (L.rating - 250) / 3250.f;   // 0 at the bottom, 1 at the top
        // Depth caps how far ahead it can see; the clock decides how far it actually gets.
        L.depth = f < 0.85f ? std::max(1, (int)(1 + f * 14)) : 64;
        L.timeMs = 100 + (int)(2900 * f * f);
        L.noise = (int)(350 * std::pow(1 - f, 3)) + 4;
        L.blunder = 0.20f * (float)std::pow(1 - f, 4);
        L.quiesce = L.rating >= 700;
        L.posScale = std::min(1.f, 0.2f + f * 1.6f);
        v.push_back(L);
    }
    return v;
}

static std::vector<Level>& levelStore() {
    static std::vector<Level> v = build();
    return v;
}

const std::vector<Level>& allLevels() { return levelStore(); }

int loadLevelNames(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    std::vector<std::string> names;
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ')) cur.pop_back();
            if (!cur.empty()) names.push_back(cur);
            cur.clear();
        } else cur += (char)c;
    }
    while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ')) cur.pop_back();
    if (!cur.empty()) names.push_back(cur);
    fclose(f);
    if (names.size() < 150) return (int)names.size();   // incomplete list: keep the built-in names
    for (int i = 0; i < 150; i++) levelStore()[i].name = names[i].substr(0, 40);
    return 150;
}

double botThinkMs(const Level& L, const Position& pos, int legal, bool timed, long long clk, long long inc,
                  std::mt19937& rng) {
    float f = (L.rating - 250) / 3250.f;
    double complexity = std::max(0.6, std::min(1.6, legal / 28.0));
    std::uniform_real_distribution<double> U(0.6, 1.5);
    int ply = (int)pos.hist.size();
    double want;
    if (timed) {
        // spread the clock over the moves still to come, plus most of the increment
        double movesLeft = std::max(18.0, 42.0 - (ply / 2) * 0.6);
        want = clk / movesLeft + inc * 0.8;
        want *= complexity * U(rng) * (0.55 + 0.45 * f);   // weaker players move more impulsively
        if (ply < 8) want *= 0.35;                        // known openings come quickly
        want = std::min(want, clk * 0.12);                // never sink too much into one move
        if (clk < 15000) want = std::min(want, clk / 15.0 + inc * 0.5);   // time scramble
    } else {
        want = (900.0 + 2600.0 * f) * complexity * U(rng);
        if (ply < 8) want *= 0.45;
    }
    if (legal <= 1) want = std::min(want, 400.0);
    double floor = timed ? std::min(350.0, clk / 20.0) : 350.0;
    return std::max(want, floor);
}

BotMove botChoose(Engine& eng, const Position& pos, const Level& L, std::atomic<bool>* stop, const Net* net,
                  std::mt19937& rng, ThinkInfo* think, int moveTimeMs) {
    Position p = pos;
    MoveList ml;
    p.genLegal(ml);
    if (ml.n == 0) return {0, 0, false};
    if (ml.n == 1) return {ml.m[0], 0, false};
    std::uniform_real_distribution<float> U(0.f, 1.f);
    if (U(rng) < L.blunder) return {ml.m[rng() % ml.n], 0, false};

    SearchLimits lim;
    lim.maxDepth = L.depth;
    lim.moveTimeMs = L.timeMs;
    lim.quiesce = L.quiesce;
    lim.posScale = L.posScale;
    lim.nnWeight = 1.f;
    lim.noiseCp = L.noise;
    lim.noiseSeed = rng();
    lim.aggression = L.aggression;
    if (moveTimeMs > 0) lim.moveTimeMs = std::max(30, moveTimeMs);
    SearchResult r = eng.search(pos, lim, stop, net, think);
    return {r.best, r.score, r.depth > 0};
}

Move anticipate(Engine& eng, const Position& root, const Level& L, Move best, const Move* cands, int ncand,
                const PatternModel& habits, const Net* net, std::atomic<bool>* stop, Move* expected) {
    if (expected) *expected = 0;
    if (!best || !habits.ready()) return best;
    std::vector<Move> list = {best};
    for (int i = 0; i < ncand; i++)
        if (cands[i] && std::find(list.begin(), list.end(), cands[i]) == list.end()) list.push_back(cands[i]);

    float f = (L.rating - 250) / 3250.f;
    int depth = std::max(1, std::min(4, L.depth - 1));
    SearchLimits lim;
    lim.maxDepth = depth;
    lim.moveTimeMs = 4000;
    lim.quiesce = L.quiesce;
    lim.posScale = L.posScale;
    // how much to trust the habit model: grows with data and with how well it has read you
    float trust = std::min(1.f, habits.trained / 600.f) * std::min(1.f, 0.3f + habits.acc3);
    float lambda = 0.6f * trust;
    // strong bots only gamble on tiny objective losses; weak ones are happier to set traps
    int margin = (int)(25 + 90 * (1 - f));
    auto clampScore = [](int s) { return std::max(-3000, std::min(3000, s)); };

    struct Option { Move m, reply; int objective; float value; };
    std::vector<Option> opts;
    for (Move c : list) {
        if (stop && stop->load()) break;
        Position p = root;
        if (!p.make(c)) continue;
        int objective = clampScore(-eng.search(p, lim, stop, net).score);
        MoveList replies;
        p.genLegal(replies);
        Option o{c, 0, objective, (float)objective};
        if (replies.n) {
            std::vector<float> prob;
            habits.predict(p, replies, prob);
            std::vector<int> idx(replies.n);
            for (int i = 0; i < replies.n; i++) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b) { return prob[a] > prob[b]; });
            o.reply = replies.m[idx[0]];
            float covered = 0, expectVal = 0;
            SearchLimits sub = lim;
            sub.maxDepth = std::max(1, depth - 1);
            for (int k = 0; k < std::min(4, replies.n) && covered < 0.85f; k++) {
                Move r = replies.m[idx[k]];
                Position q = p;
                if (!q.make(r)) continue;
                int v = q.outcome() != 2 ? (q.inCheck() ? -3000 : 0) : clampScore(eng.search(q, sub, stop, net).score);
                expectVal += prob[idx[k]] * v;
                covered += prob[idx[k]];
            }
            // replies it can't foresee are assumed to be your best
            float expectedScore = expectVal + (1 - covered) * objective;
            o.value = objective + lambda * (expectedScore - objective);
        }
        opts.push_back(o);
    }
    if (opts.empty()) return best;
    int bestObjective = opts[0].objective;
    for (auto& o : opts) bestObjective = std::max(bestObjective, o.objective);
    const Option* pick = &opts[0];
    for (auto& o : opts)
        if (bestObjective - o.objective <= margin && o.value > pick->value) pick = &o;
    if (expected) *expected = pick->reply;
    return pick->m;
}
