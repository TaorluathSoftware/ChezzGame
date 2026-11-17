// Puzzles. Daily puzzles are mate-in-2/3 positions found by the engine in seeded practice games:
// the seed is the date, the search uses fixed depths and no neural net, so every copy of the app
// produces the same puzzles on the same day without a server. Members can also publish their own;
// the engine must confirm a forced mate before a puzzle is accepted. Included by main.cpp after
// engines.inl.

struct Puzzle {
    std::string id, fen, title, author;
    int mate = 0;
};

static std::vector<Puzzle> g_daily, g_community;
static std::vector<std::string> g_puzSolved, g_puzStarted;   // all-time solved ids; ids opened today
static int g_puzDay = 0, g_puzWanted = 0, g_puzCreated = 0;
static std::atomic<bool> g_puzGenBusy{false};
static bool g_puzComposing = false, g_puzCommunityLoaded = false, g_puzChecking = false;
static Engine* g_puzEngine = nullptr;   // UI-thread engine for checking answers (fixed shallow depths)

static int today() {
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    return lt ? (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday : 20260101;
}
static int dailyAllowance() { return g_plan == 0 ? 1 : g_plan == 1 ? 5 : 100000; }
static int createAllowance() { return g_plan == 0 ? 0 : g_plan == 1 ? 1 : g_plan == 2 ? 12 : 100000; }
static bool puzzleSolved(const std::string& id) { return std::find(g_puzSolved.begin(), g_puzSolved.end(), id) != g_puzSolved.end(); }

// Moves to mate for the side to move if it is forced within maxMoves, else 0.
static int forcedMate(Engine& eng, const Position& p, int maxMoves, Move* best = nullptr) {
    SearchLimits lim;
    lim.maxDepth = 2 * maxMoves;
    lim.moveTimeMs = 120000;   // depth is the limit, never the clock: results must not vary by machine
    lim.nnWeight = 0.f;
    SearchResult r = eng.search(p, lim, nullptr, nullptr);
    if (best) *best = r.best;
    if (r.score >= MATE_SCORE - 2 * maxMoves) return (MATE_SCORE - r.score + 1) / 2;
    return 0;
}

// Plays seeded, deliberately uneven practice games until a position with a forced mate turns up.
static bool generatePuzzle(unsigned seed, Puzzle& out, Engine& eng) {
    std::mt19937 rng(seed);
    for (int game = 0; game < 40; game++) {
        Position p;
        p.setStart();
        eng.clear();
        for (int ply = 0; ply < 160; ply++) {
            if (p.outcome() != 2) break;
            if (ply >= 14) {
                int n = forcedMate(eng, p, 2);
                if (n == 0 && evalHCE(p, 1.f) > 500 && rng() % 3 == 0) n = forcedMate(eng, p, 3);
                if (n >= 2) {
                    out.fen = p.fen();
                    out.mate = n;
                    return true;
                }
            }
            MoveList ml;
            p.genLegal(ml);
            Move m;
            if (rng() % 100 < 30) m = ml.m[rng() % ml.n];
            else {
                SearchLimits lim;
                lim.maxDepth = 2;
                lim.moveTimeMs = 120000;
                lim.nnWeight = 0.f;
                lim.noiseCp = 40;
                lim.noiseSeed = rng();
                m = eng.search(p, lim, nullptr, nullptr).best;
            }
            if (!m || !p.make(m)) break;
        }
    }
    return false;
}

// ---------------------------------------------------------------- local state

static void savePuzzleState() {
    FILE* f = _wfopen((g_data + L"\\puzzles_state.txt").c_str(), L"w");
    if (!f) return;
    fprintf(f, "D %d %d\n", g_puzDay, g_puzCreated);
    for (auto& s : g_puzStarted) fprintf(f, "T %s\n", s.c_str());
    for (auto& s : g_puzSolved) fprintf(f, "S %s\n", s.c_str());
    fclose(f);
    f = _wfopen((g_data + L"\\daily_puzzles.txt").c_str(), L"w");
    if (!f) return;
    for (auto& p : g_daily) fprintf(f, "%d|%d|%s\n", g_puzDay, p.mate, p.fen.c_str());
    fclose(f);
}

static void loadPuzzleState() {
    int day = today();
    g_puzSolved.clear();
    g_puzStarted.clear();
    g_daily.clear();
    g_puzCreated = 0;
    int savedDay = 0;
    if (FILE* f = _wfopen((g_data + L"\\puzzles_state.txt").c_str(), L"r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.size() < 3) continue;
            if (l[0] == 'D') sscanf(l.c_str() + 2, "%d %d", &savedDay, &g_puzCreated);
            else if (l[0] == 'T') g_puzStarted.push_back(l.substr(2));
            else if (l[0] == 'S') g_puzSolved.push_back(l.substr(2));
        }
        fclose(f);
    }
    if (savedDay != day) {   // a new day: the allowances start over
        g_puzStarted.clear();
        g_puzCreated = 0;
    }
    g_puzDay = day;
    if (FILE* f = _wfopen((g_data + L"\\daily_puzzles.txt").c_str(), L"r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            int d = 0, mate = 0;
            char fen[400];
            if (sscanf(line, "%d|%d|%399[^\n\r]", &d, &mate, fen) == 3 && d == day) {
                Puzzle p;
                p.id = "d" + std::to_string(day) + "_" + std::to_string(g_daily.size());
                p.title = "Daily puzzle " + std::to_string(g_daily.size() + 1);
                p.author = "the club";
                p.mate = mate;
                p.fen = fen;
                g_daily.push_back(p);
            }
        }
        fclose(f);
    }
}

// Makes sure `want` of today's puzzles exist, generating the missing ones on a background thread.
static void ensureDaily(int want) {
    want = min(want, 60);
    g_puzWanted = max(g_puzWanted, want);
    if (g_puzGenBusy || (int)g_daily.size() >= g_puzWanted) return;
    g_puzGenBusy = true;
    int day = g_puzDay, from = (int)g_daily.size(), to = g_puzWanted;
    std::thread([day, from, to]() {
        Engine* eng = new Engine();
        for (int i = from; i < to; i++) {
            Puzzle p;
            if (!generatePuzzle((unsigned)day * 131u + (unsigned)i * 7919u, p, *eng)) break;
            p.id = "d" + std::to_string(day) + "_" + std::to_string(i);
            p.title = "Daily puzzle " + std::to_string(i + 1);
            p.author = "the club";
            PostMessageW(g_hwnd, WM_NET, 0, (LPARAM) new std::function<void()>([p, day, i]() {
                if (day != g_puzDay || i != (int)g_daily.size()) return;
                g_daily.push_back(p);
                savePuzzleState();
                invalidate();
            }));
        }
        delete eng;
        PostMessageW(g_hwnd, WM_NET, 0, (LPARAM) new std::function<void()>([]() {
            g_puzGenBusy = false;
            invalidate();
        }));
    }).detach();
}

static void refreshCommunityPuzzles() {
    if (g_offline || g_myUid.empty()) return;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::Doc> docs;
        fb::Err e = fb::docList(g_sess, "puzzles", docs, 200);
        return [e, docs]() {
            g_puzCommunityLoaded = true;
            if (e != fb::OK) return;
            std::vector<std::pair<long long, Puzzle>> all;
            for (auto& d : docs) {
                Puzzle p;
                p.id = "c" + d.id;
                p.fen = d.str("fen");
                p.title = d.str("title");
                p.author = d.str("authorName");
                p.mate = (int)d.num("mate");
                all.push_back({d.num("createdAt"), p});
            }
            std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first > b.first; });
            g_community.clear();
            for (auto& a : all) g_community.push_back(a.second);
            invalidate();
        };
    });
}

static void openPuzzles() {
    g_socialMsg.clear();
    g_puzComposing = false;
    if (g_puzDay != today()) loadPuzzleState();
    goScreen(SCR_PUZZLES);
    ensureDaily(min(dailyAllowance(), 5));
    refreshCommunityPuzzles();
}

// ---------------------------------------------------------------- solving

static void startPuzzle(const Puzzle& pz) {
    bool known = std::find(g_puzStarted.begin(), g_puzStarted.end(), pz.id) != g_puzStarted.end();
    if (!known && !puzzleSolved(pz.id) && (int)g_puzStarted.size() >= dailyAllowance()) {
        socialSay(std::wstring(L"That's today's puzzles for the ") + PLAN_NAME[g_plan] + L" plan (" +
                  std::to_wstring(dailyAllowance()) + L" a day). The gem icon shows the plans.", true);
        return;
    }
    Position test;
    if (!test.setFEN(pz.fen)) { socialSay(L"That puzzle's position is damaged.", true); return; }
    if (!known) { g_puzStarted.push_back(pz.id); savePuzzleState(); }
    resetBoardState();
    g.online = false;
    g_customOn = false;
    g.puzzle = true;
    g.pos.setFEN(pz.fen);
    g.human = g.pos.side;
    g.flipped = g.human == BLACK;
    g.timed = false;
    g.puzMate = max(1, pz.mate);
    g.puzState = 0;
    g.puzHint = -1;
    g.puzTries = 0;
    g.puzId = pz.id;
    g.puzTitle = widen(pz.title);
    g.puzBy = widen(pz.author);
    g.puzMsg.clear();
    if (!g_puzEngine) g_puzEngine = new Engine();
    g_puzEngine->clear();
    goScreen(SCR_GAME);
    playSound(SND_START);
    invalidate();
}

static void glideMove(Move m, float delay) {
    int from = mfrom(m), to = mto(m);
    if (g_glideMul == 0.f) return;
    addGlide(g.pos.sq[to], squareRect(from), squareRect(to), to, (0.16f + delay) * g_glideMul);
    (void)delay;
}

static void puzzleSolvedNow(bool byYou) {
    g.puzState = byYou ? 1 : 2;
    g.puzHint = -1;
    if (byYou) {
        if (!puzzleSolved(g.puzId)) g_puzSolved.push_back(g.puzId);
        savePuzzleState();
        g.puzMsg = g.puzTries == 0 ? L"Solved, first try." : L"Solved.";
        playSound(SND_END);
    } else {
        g.puzMsg = L"That was the solution.";
    }
}

// Your move in a puzzle: right only if it mates, or keeps a forced mate in the moves that remain.
static void puzzleMove(Move m) {
    if (g.puzState != 0) return;
    int need = g.puzMate;
    std::string san = g.pos.san(m);
    bool capture = (m & MF_CAP) != 0;
    if (!g.pos.make(m)) return;
    bool ok = false;
    Move reply = 0;
    if (g.pos.outcome() != 2) ok = g.pos.inCheck();   // checkmate (a faster mate than asked still counts)
    else if (need > 1) {
        SearchLimits lim;
        lim.maxDepth = 2 * (need - 1);
        lim.moveTimeMs = 120000;
        lim.nnWeight = 0.f;
        SearchResult r = g_puzEngine->search(g.pos, lim, nullptr, nullptr);
        ok = r.score <= -(MATE_SCORE - 2 * (need - 1));
        reply = r.best;
    }
    g.selected = -1;
    g.targets.clear();
    g.puzHint = -1;
    if (!ok) {
        g.pos.unmake();
        g.puzTries++;
        g.puzMsg = L"Not that one. Look again.";
        invalidate();
        return;
    }
    g.sans.push_back(widen(san));
    g.last = m;
    glideMove(m, 0);
    playSound(g.pos.inCheck() ? SND_CHECK : capture ? SND_CAPTURE : SND_MOVE);
    if (g.pos.outcome() != 2) {
        puzzleSolvedNow(true);
    } else if (reply) {
        g.sans.push_back(widen(g.pos.san(reply)));
        g.pos.make(reply);
        g.last = reply;
        glideMove(reply, 0.12f);
        g.puzMate = need - 1;
        g.puzMsg = L"Right. Now mate in " + std::to_wstring(g.puzMate) + L".";
    }
    invalidate();
}

static void puzzleHint() {
    if (g.puzState != 0) return;
    Move best = 0;
    forcedMate(*g_puzEngine, g.pos, g.puzMate, &best);
    if (best) {
        g.puzHint = mfrom(best);
        g.puzTries++;   // a hint means it wasn't first try
        g.puzMsg = L"Look at the circled piece.";
    }
    invalidate();
}

static void puzzleShowSolution() {
    if (g.puzState != 0) return;
    for (int guard = 0; guard < 8 && g.pos.outcome() == 2; guard++) {
        Move best = 0;
        int n = forcedMate(*g_puzEngine, g.pos, max(1, g.puzMate), &best);
        if (!best) break;
        g.sans.push_back(widen(g.pos.san(best)));
        g.pos.make(best);
        g.last = best;
        if (g.pos.outcome() != 2) break;
        MoveList ml;
        g.pos.genLegal(ml);
        SearchLimits lim;
        lim.maxDepth = max(1, 2 * (n - 1));
        lim.moveTimeMs = 120000;
        lim.nnWeight = 0.f;
        Move reply = g_puzEngine->search(g.pos, lim, nullptr, nullptr).best;
        if (!reply) reply = ml.m[0];
        g.sans.push_back(widen(g.pos.san(reply)));
        g.pos.make(reply);
        g.last = reply;
        g.puzMate = max(1, n - 1);
    }
    puzzleSolvedNow(false);
    invalidate();
}

static const Puzzle* nextPuzzle() {
    for (int i = 0; i < (int)g_daily.size() && i < dailyAllowance(); i++)
        if (!puzzleSolved(g_daily[i].id) && g_daily[i].id != g.puzId) return &g_daily[i];
    return nullptr;
}

// ---------------------------------------------------------------- composing

static RectF puzzleEditRect(int i) {   // 0 position (FEN), 1 title
    RectF r = socialRight();
    return RectF(r.X + S(24), r.Y + S(92) + i * S(70), r.Width - S(48), S(34));
}

static void publishPuzzle() {
    if (g_puzChecking) return;
    if (!needPlan(1, L"Publishing puzzles")) return;
    if (g_puzCreated >= createAllowance()) {
        socialSay(std::wstring(L"You've published today's ") + std::to_wstring(createAllowance()) + L" for the " + PLAN_NAME[g_plan] + L" plan.", true);
        return;
    }
    std::string fen = narrow(editText(g_edGen[0])), title = narrow(editText(g_edGen[1]));
    while (!title.empty() && title.back() == ' ') title.pop_back();
    Position p;
    int kings[2] = {0, 0};
    bool ok = p.setFEN(fen);
    if (ok)
        for (int s = 0; s < 64; s++)
            if (ptype(p.sq[s]) == KING) kings[pcolor(p.sq[s])]++;
    if (!ok || kings[0] != 1 || kings[1] != 1 || p.attacked(p.king[p.side ^ 1], p.side) || p.outcome() != 2) {
        socialSay(L"That isn't a legal position. Paste a FEN, or press USE MY BOARD.", true);
        return;
    }
    if (title.size() < 3 || title.size() > 40) { socialSay(L"Give it a title of 3 to 40 characters.", true); return; }
    g_puzChecking = true;
    socialSay(L"The engine is checking for a forced mate\x2026");
    std::string normal = p.fen(), me = g_myUid, myName = g_myName;
    std::thread([p, normal, title, me, myName]() {
        Engine* eng = new Engine();
        int n = forcedMate(*eng, p, 3);
        delete eng;
        PostMessageW(g_hwnd, WM_NET, 0, (LPARAM) new std::function<void()>([n, normal, title, me, myName]() {
            g_puzChecking = false;
            if (n < 1) { socialSay(L"No forced mate in 1 to 3 moves for the side to move, so it can't be published.", true); return; }
            std::string body = fb::fvBody({{"fen", fb::fvStr(normal)}, {"title", fb::fvStr(title)}, {"mate", fb::fvInt(n)},
                                           {"author", fb::fvStr(me)}, {"authorName", fb::fvStr(myName)}, {"createdAt", fb::fvInt(fb::nowMs())}});
            netAsync([body, n]() -> std::function<void()> {
                fb::Err e = fb::docCreate(g_sess, "puzzles", "", body);
                return [e, n]() {
                    if (e == fb::OK) {
                        g_puzCreated++;
                        savePuzzleState();
                        g_puzComposing = false;
                        showLoginEdits(false);
                        socialSay(L"Published: mate in " + std::to_wstring(n) + L".");
                        refreshCommunityPuzzles();
                    } else {
                        socialSay(e == fb::AUTH ? L"The server refused: publishing needs Silver or above." : L"Couldn't publish it.", true);
                    }
                };
            });
        }));
    }).detach();
}

// ---------------------------------------------------------------- rendering

static void puzzleRow(Graphics& gr, const Puzzle& p, RectF r, int id, bool locked) {
    Pen rule(C_LEDGER, 1.f);
    gr.DrawLine(&rule, r.X, r.Y + r.Height - 1, r.X + r.Width, r.Y + r.Height - 1);
    bool solved = puzzleSolved(p.id);
    if (!locked && r.Contains(g_mouseX, g_mouseY)) {
        SolidBrush hl(C_HILITE);
        gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
    }
    drawIcon(gr, IC_PUZZLE, RectF(r.X + S(6), r.Y + S(8), r.Height - S(16), r.Height - S(16)), locked ? C_FAINT : C_PRINT);
    drawText(gr, widen(p.title), RectF(r.X + S(58), r.Y + S(4), r.Width - S(180), r.Height * 0.52f), F_SERIF, S(16),
             locked ? C_FAINT : C_PRINT, FontStyleBold, 0, 2);
    drawText(gr, L"mate in " + std::to_wstring(p.mate) + L"  \x00B7  by " + widen(p.author),
             RectF(r.X + S(58), r.Y + r.Height * 0.54f, r.Width - S(160), r.Height * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
    drawText(gr, locked ? L"locked" : solved ? L"solved" : L"to do", RectF(r.X, r.Y, r.Width - S(14), r.Height), F_HAND, S(14),
             solved ? C_RED : C_FAINT, 0, 2, 1);
    if (!locked) g_buttons.push_back({r, id});
}

static void renderPuzzles(Graphics& gr) {
    drawTopBar(gr, L"puzzles  \x00B7  fresh ones every day, and puzzles composed by members");
    RectF L = socialLeft();
    paperSheet(gr, L);
    sheetHeader(gr, L, L"TODAY'S PUZZLES");
    int allow = dailyAllowance();
    drawText(gr, allow > 1000 ? std::wstring(L"unlimited") : std::to_wstring(allow) + L" a day on " + PLAN_NAME[g_plan],
             RectF(L.X, L.Y + S(8), L.Width - S(18), S(30)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    float rh = S(56), y = L.Y + S(52);
    int maxRows = (int)((L.Y + L.Height - y - S(60)) / rh), shown = 0;
    for (int i = 0; i < (int)g_daily.size() && shown < maxRows && i < allow; i++, shown++)
        puzzleRow(gr, g_daily[i], RectF(L.X + S(8), y + shown * rh, L.Width - S(16), rh), B_PUZROW + i, false);
    if (g_puzGenBusy)
        drawText(gr, L"The engine is setting up more puzzles\x2026", RectF(L.X + S(24), y + shown * rh + S(8), L.Width - S(48), S(30)),
                 F_HAND, S(14), C_FAINT);
    else if (g_daily.empty())
        drawText(gr, L"No puzzles could be set up today.", RectF(L.X + S(24), y + S(8), L.Width - S(48), S(30)), F_HAND, S(14), C_FAINT);
    float fy = L.Y + L.Height - S(50);
    if (g_plan >= 2) inkButton(gr, B_PUZ_MORE, RectF(L.X + S(18), fy, S(210), S(36)), L"MORE PUZZLES", !g_puzGenBusy && shown < maxRows);
    else
        drawText(gr, g_plan == 0 ? L"Silver members get 5 a day; Gold and Ruby get as many as they like."
                                 : L"Gold and Ruby members get as many as they like.",
                 RectF(L.X + S(18), fy - S(6), L.Width - S(36), S(44)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 1, true);

    RectF R = socialRight();
    paperSheet(gr, R, g_puzComposing);
    float ix = R.X + S(24), iw = R.Width - S(48);
    if (g_puzComposing) {
        drawText(gr, L"COMPOSE A PUZZLE", RectF(ix, R.Y + S(18), iw, S(30)), F_MONO, S(16), C_PRINT, FontStyleBold);
        fieldBox(gr, puzzleEditRect(0), L"POSITION (FEN)");
        fieldBox(gr, puzzleEditRect(1), L"TITLE");
        float by = puzzleEditRect(1).Y + S(52);
        inkButton(gr, B_PUZ_USEPOS, RectF(ix, by, S(230), S(36)), L"USE MY BOARD");
        drawText(gr, L"copies the position from the last game or puzzle you had open", RectF(ix + S(244), by, iw - S(244), S(36)),
                 F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 1, true);
        by += S(54);
        stampButton(gr, B_PUZ_PUBLISH, RectF(ix + S(4), by, S(220), S(56)), L"PUBLISH", L"", !g_puzChecking);
        inkButton(gr, B_PUZ_CANCEL, RectF(ix + S(250), by + S(10), S(130), S(36)), L"CANCEL");
        by += S(74);
        drawText(gr, L"The side to move must have a forced checkmate in 1 to 3 moves. The engine checks before anything is "
                     L"published. Published today: " + std::to_wstring(g_puzCreated) +
                     (createAllowance() > 1000 ? L"" : L" of " + std::to_wstring(createAllowance())) + L".",
                 RectF(ix, by, iw, S(64)), F_SERIF, S(13), C_FAINT, FontStyleItalic, 0, 0, true);
        statusLine(gr, RectF(ix, by + S(70), iw, S(60)));
        return;
    }
    sheetHeader(gr, R, L"FROM THE MEMBERS");
    y = R.Y + S(52);
    maxRows = (int)((R.Y + R.Height - y - S(110)) / rh);
    if (g_community.empty())
        drawText(gr, g_offline ? L"Members' puzzles need a connection." : !g_puzCommunityLoaded ? L"Fetching\x2026"
                                                                         : L"No member has published a puzzle yet.",
                 RectF(R.X + S(24), y + S(8), R.Width - S(48), S(40)), F_HAND, S(14), C_FAINT);
    for (int i = 0; i < (int)g_community.size() && i < maxRows; i++)
        puzzleRow(gr, g_community[i], RectF(R.X + S(8), y + i * rh, R.Width - S(16), rh), B_PUZCROW + i, false);
    fy = R.Y + R.Height - S(50);
    inkButton(gr, B_PUZ_COMPOSE, RectF(R.X + S(18), fy, S(230), S(36)), L"COMPOSE A PUZZLE");
    if (g_plan < 1) {
        drawBadge(gr, 1, RectF(R.X + S(258), fy + S(6), S(22), S(22)));
        drawText(gr, L"Silver", RectF(R.X + S(286), fy, S(100), S(36)), F_HAND, S(13), C_FAINT);
    }
    statusLine(gr, RectF(R.X + S(18), fy - S(54), R.Width - S(36), S(48)));
}

// The panel beside the board while a puzzle is open.
static void renderPuzzleSide(Graphics& gr) {
    Geom b = boardGeom();
    float x0 = b.x + b.size + b.frame + S(30), w = g_W - x0 - S(24);
    RectF sh(x0, b.y - b.frame, w, b.size + 2 * b.frame);
    paperSheet(gr, sh);
    float ix = sh.X + S(18), iw = sh.Width - S(36), y = sh.Y + S(12);
    drawText(gr, L"CHEZZ CHESS CLUB", RectF(ix, y, iw, S(24)), F_SERIF, S(14), C_PRINT, FontStyleBold);
    drawText(gr, L"puzzle", RectF(ix, y, iw, S(24)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    y += S(28);
    Pen print(C_PRINT, 1.f);
    gr.DrawLine(&print, ix, y, ix + iw, y);
    gr.DrawLine(&print, ix, y + 3, ix + iw, y + 3);
    y += S(14);
    drawText(gr, g.puzTitle, RectF(ix, y, iw, S(36)), F_SERIF, S(22), C_PRINT, FontStyleBold);
    y += S(38);
    drawText(gr, L"set by " + g.puzBy, RectF(ix, y, iw, S(22)), F_SERIF, S(13), C_FAINT, FontStyleItalic);
    y += S(34);
    std::wstring task = std::wstring(g.human == WHITE ? L"White" : L"Black") + L" to move";
    if (g.puzState == 0) task += L": mate in " + std::to_wstring(g.puzMate);
    drawText(gr, task, RectF(ix, y, iw, S(34)), F_HAND, S(19), C_INK);
    y += S(44);
    std::wstring line;
    for (size_t i = 0; i < g.sans.size(); i++) line += (i ? L"  " : L"") + g.sans[i];
    if (!line.empty()) {
        drawText(gr, L"So far:", RectF(ix, y, S(70), S(26)), F_SERIF, S(12), C_FAINT, FontStyleItalic);
        drawText(gr, line, RectF(ix + S(70), y - S(2), iw - S(70), S(56)), F_HAND, S(15), C_INK, 0, 0, 0, true);
    }
    y += S(62);
    if (!g.puzMsg.empty())
        drawText(gr, g.puzMsg, RectF(ix, y, iw, S(60)), F_HAND, S(17), g.puzState == 1 ? C_RED : C_INK, 0, 0, 0, true);

    float bh = S(38), bw = (iw - S(10)) / 2, by = sh.Y + sh.Height - S(18) - 2 * bh - S(8);
    if (g.puzState == 0) {
        inkButton(gr, B_PUZ_HINT, RectF(ix, by, bw, bh), L"HINT");
        inkButton(gr, B_PUZ_SOLVE, RectF(ix + bw + S(10), by, bw, bh), L"SHOW SOLUTION");
    } else {
        inkButton(gr, B_PUZ_NEXT, RectF(ix, by, bw, bh), L"NEXT PUZZLE", nextPuzzle() != nullptr);
        inkButton(gr, B_PUZ_AGAIN, RectF(ix + bw + S(10), by, bw, bh), L"TRY IT AGAIN");
    }
    inkButton(gr, B_FLIP, RectF(ix, by + bh + S(8), bw, bh), L"TURN BOARD");
    inkButton(gr, B_PUZ_BACK, RectF(ix + bw + S(10), by + bh + S(8), bw, bh), L"ALL PUZZLES");
}

static bool onPuzzleButton(int id) {
    if (id >= B_PUZROW && id < B_PUZROW + 100) {
        if (id - B_PUZROW < (int)g_daily.size()) startPuzzle(g_daily[id - B_PUZROW]);
        return true;
    }
    if (id >= B_PUZCROW && id < B_PUZCROW + 300) {
        if (id - B_PUZCROW < (int)g_community.size()) startPuzzle(g_community[id - B_PUZCROW]);
        return true;
    }
    switch (id) {
        case B_PUZ_MORE:
            if (needPlan(2, L"Unlimited puzzles")) ensureDaily((int)g_daily.size() + 5);
            invalidate();
            return true;
        case B_PUZ_COMPOSE:
            if (!needPlan(1, L"Publishing puzzles")) return true;
            if (g_offline) { socialSay(L"Publishing needs a connection.", true); return true; }
            g_puzComposing = true;
            g_socialMsg.clear();
            showLoginEdits(false);
            invalidate();
            return true;
        case B_PUZ_CANCEL: g_puzComposing = false; showLoginEdits(false); invalidate(); return true;
        case B_PUZ_USEPOS: SetWindowTextW(g_edGen[0], widen(g.pos.fen()).c_str()); return true;
        case B_PUZ_PUBLISH: publishPuzzle(); return true;
        case B_PUZ_HINT: puzzleHint(); return true;
        case B_PUZ_SOLVE: puzzleShowSolution(); return true;
        case B_PUZ_BACK: g.puzzle = false; g_anims.clear(); openPuzzles(); return true;
        case B_PUZ_NEXT:
            if (const Puzzle* n = nextPuzzle()) { Puzzle copy = *n; startPuzzle(copy); }
            return true;
        case B_PUZ_AGAIN: {
            for (auto* list : {&g_daily, &g_community})
                for (auto& p : *list)
                    if (p.id == g.puzId) { Puzzle copy = p; startPuzzle(copy); return true; }
            return true;
        }
    }
    return false;
}
