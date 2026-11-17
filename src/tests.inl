// Windowless checks, run from the command line (nothing appears on screen):
//   Chezz.exe --data <dir> --selftest     engine, brain, habits, audio, parser  -> <dir>\selftest.txt
//   Chezz.exe --data <dir> --shots <out>  every screen rendered to PNG files in <out>
//   Chezz.exe --data <dir> --nettest      read-mostly pass over the live server -> <dir>\nettest.txt
// Included by main.cpp just before WinMain.

static FILE* g_testOut;
static int g_testFails;

static void check(bool ok, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!ok) g_testFails++;
    if (g_testOut) {
        fprintf(g_testOut, "%s  %s\n", ok ? "PASS" : "FAIL", buf);
        fflush(g_testOut);
    }
}

static void randomPosition(Position& p, std::mt19937& rng, int plies) {
    p.setStart();
    for (int i = 0; i < plies; i++) {
        MoveList ml;
        p.genLegal(ml);
        if (!ml.n) break;
        p.make(ml.m[rng() % ml.n]);
    }
}

static int runSelfTest() {
    g_testOut = _wfopen((g_data + L"\\selftest.txt").c_str(), L"w");
    std::mt19937 rng(2026);

    // ---- move generation
    {
        struct T { const char* fen; int depth; uint64_t expect; };
        T tests[] = {{"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
                     {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
                     {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
                     {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
                     {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379}};
        for (auto& t : tests) {
            Position p;
            p.setFEN(t.fen);
            uint64_t n = perft(p, t.depth);
            check(n == t.expect, "perft depth %d = %llu (expected %llu)", t.depth, (unsigned long long)n, (unsigned long long)t.expect);
        }
    }

    // ---- levels
    {
        const auto& lv = allLevels();
        bool tens = true, rising = true, unique = true;
        for (size_t i = 0; i < lv.size(); i++) {
            if (lv[i].rating % 10) tens = false;
            if (i && lv[i].rating <= lv[i - 1].rating) rising = false;
            for (size_t j = 0; j < i; j++)
                if (lv[j].name == lv[i].name) unique = false;
        }
        check(lv.size() == 150, "150 levels (%d)", (int)lv.size());
        check(tens, "every rating is a multiple of 10");
        check(rising, "ratings strictly increase");
        check(lv.front().rating == 250 && lv.back().rating == 3500, "ratings run %d to %d", lv.front().rating, lv.back().rating);
        check(unique, "all 150 names are unique");
    }

    // ---- the brain: 5 layers of 150
    Net net;
    {
        int expect = 768 * 150 + 150 + 4 * (150 * 150 + 150) + 150 + 1;
        check(Net::LAYERS == 5 && Net::HID == 150 && Net::SIZE == expect, "network is 768 -> 5 x 150 -> 1 (%d weights)", Net::SIZE);
        Position p;
        p.setStart();
        check(net.blank && net.evalCp(p) == 0, "an untrained brain changes nothing (eval 0)");

        // teach it material balance from random positions; the loss must fall
        std::vector<TrainItem> items(512);
        for (auto& it : items) {
            randomPosition(p, rng, 10 + rng() % 60);
            it.n = Net::features(p.sq, p.side, it.idx);
            it.hce = 0;
            int bal = 0;
            static const int V[7] = {0, 1, 3, 3, 5, 9, 0};
            for (int s = 0; s < 64; s++)
                if (p.sq[s]) bal += (pcolor(p.sq[s]) == p.side ? 1 : -1) * V[ptype(p.sq[s])];
            it.target = 1.f / (1.f + std::exp(-bal * 0.5f));
        }
        Trainer tr(net);
        float first = 0, last = 0;
        for (int step = 0; step < 240; step++) {
            float l = tr.step(&items[(step * 64) % 448], 64);
            if (step < 8) first += l / 8;
            if (step >= 232) last += l / 8;
        }
        check(last < first * 0.6f, "training lowers the loss (%.4f -> %.4f)", first, last);
        check(!net.blank, "a trained brain is no longer blank");

        std::wstring path = g_data + L"\\selftest_net.bin";
        int games = 0;
        Net back;
        bool io = net.save(path, 7) && back.load(path, games) && games == 7;
        bool same = io;
        for (int i = 0; i < 12 && same; i++) {
            randomPosition(p, rng, 20 + i * 3);
            if (net.evalCp(p) != back.evalCp(p)) same = false;
        }
        check(same, "brain saves and loads back identically");
        _wremove(path.c_str());

        auto t0 = Clock::now();
        int n = 0, sink = 0;
        randomPosition(p, rng, 24);
        while (std::chrono::duration<float>(Clock::now() - t0).count() < 0.4f) {
            for (int i = 0; i < 200; i++) sink += net.evalCp(p);
            n += 200;
        }
        float secs = std::chrono::duration<float>(Clock::now() - t0).count();
        check(n / secs > 3000, "network speed: %.0f evaluations per second (checksum %d)", n / secs, sink & 1);
    }

    // ---- search
    {
        Engine* eng = new Engine();
        Position p;
        p.setFEN("6k1/5ppp/8/8/8/8/8/R3K3 w Q - 0 1");
        SearchLimits lim;
        lim.maxDepth = 4;
        lim.moveTimeMs = 2000;
        SearchResult r = eng->search(p, lim, nullptr, nullptr);
        check(p.uci(r.best) == "a1a8", "finds mate in one (%s)", p.uci(r.best).c_str());

        p.setStart();
        eng->clear();
        SearchLimits l2;
        l2.moveTimeMs = 1500;
        r = eng->search(p, l2, nullptr, &net);
        check(r.depth >= 4 && r.best, "1.5 s search with the 5x150 brain reaches depth %d (%llu nodes)", r.depth,
              (unsigned long long)r.nodes);

        // two bots play each other; every move must be legal and the game must stay consistent
        p.setStart();
        eng->clear();
        bool legalAll = true;
        int plies = 0;
        for (; plies < 50 && p.outcome() == 2; plies++) {
            const Level& L = allLevels()[plies % 2 ? 20 : 120];
            BotMove bm = botChoose(*eng, p, L, nullptr, &net, rng, nullptr, 60);
            MoveList ml;
            p.genLegal(ml);
            bool found = false;
            for (int i = 0; i < ml.n; i++)
                if (ml.m[i] == bm.move) found = true;
            if (!found || !p.make(bm.move)) { legalAll = false; break; }
        }
        check(legalAll && p.hash == p.computeHash(), "bot versus bot: %d legal plies, position hash consistent", plies);

        // thinking time budget on a clock
        p.setStart();
        double tot = 0;
        for (int i = 0; i < 200; i++) tot += botThinkMs(allLevels()[100], p, 30, true, 300000, 0, rng);
        check(tot / 200 > 1500 && tot / 200 < 36000, "a 5+0 bot spends about %.1f s on a middlegame move", tot / 200 / 1000);
        delete eng;
    }

    // ---- habit model: a player who always pushes the most advanced piece should become predictable
    {
        std::vector<PatternSample> moves;
        Position p;
        for (int g2 = 0; g2 < 60; g2++) {
            randomPosition(p, rng, 6 + rng() % 30);
            MoveList ml;
            p.genLegal(ml);
            if (ml.n < 2) continue;
            int best = 0, bestRank = -1;
            for (int i = 0; i < ml.n; i++) {
                int to = mto(ml.m[i]), rank = p.side == WHITE ? sqrank(to) : 7 - sqrank(to);
                if (rank > bestRank) { bestRank = rank; best = i; }
            }
            moves.push_back(makePatternSample(p, ml.m[best]));
        }
        for (int rep = 0; rep < 3; rep++) moves.insert(moves.end(), moves.begin(), moves.begin() + moves.size() / (rep + 1));
        PatternModel pm;
        trainPatterns(pm, moves);
        check(pm.trained > 100 && pm.acc1 > 0.3f, "habit model learns a pattern: %d moves, %.0f%% predicted", pm.trained, pm.acc1 * 100);
    }

    // ---- brain learning cycle on disk
    {
        std::wstring dir = g_data + L"\\selftest_brain";
        CreateDirectoryW(dir.c_str(), NULL);
        for (const wchar_t* f : {L"\\brain.bin", L"\\memory.bin", L"\\yourmoves.bin", L"\\habits.bin"}) _wremove((dir + f).c_str());
        Brain b;
        b.open(dir);
        std::vector<Sample> game;
        Position p;
        p.setStart();
        for (int i = 0; i < 40; i++) {
            MoveList ml;
            p.genLegal(ml);
            if (!ml.n) break;
            Sample s;
            memcpy(s.sq, p.sq, 64);
            s.side = (uint8_t)p.side;
            s.result = (int8_t)(p.side == WHITE ? 1 : -1);
            s.search = NO_SCORE;
            game.push_back(s);
            p.make(ml.m[rng() % ml.n]);
        }
        float loss = b.learnNow(game);
        Brain again;
        again.open(dir);
        check(again.games() == 1 && again.positions() == game.size() && !again.net()->blank,
              "a game is learned, saved and reloaded (loss %.4f, %d positions)", loss, (int)again.positions());
        for (const wchar_t* f : {L"\\brain.bin", L"\\memory.bin", L"\\yourmoves.bin", L"\\habits.bin"}) _wremove((dir + f).c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // ---- audio
    {
        synthInit();
        bool all = true;
        for (int i = 0; i < SND_COUNT; i++) {
            const std::vector<char>& w = synthWav(i);
            if (audioDecodeWav((const unsigned char*)w.data(), w.size()).size() < 2000) all = false;
        }
        check(all, "all %d built-in sounds decode", (int)SND_COUNT);
        // an awkward file: 8-bit mono at 11025 Hz
        unsigned char wav[44 + 100];
        memcpy(wav, "RIFF", 4);
        uint32_t v = 36 + 100; memcpy(wav + 4, &v, 4);
        memcpy(wav + 8, "WAVEfmt ", 8);
        v = 16; memcpy(wav + 16, &v, 4);
        uint16_t h = 1; memcpy(wav + 20, &h, 2); memcpy(wav + 22, &h, 2);
        v = 11025; memcpy(wav + 24, &v, 4); memcpy(wav + 28, &v, 4);
        h = 1; memcpy(wav + 32, &h, 2);
        h = 8; memcpy(wav + 34, &h, 2);
        memcpy(wav + 36, "data", 4);
        v = 100; memcpy(wav + 40, &v, 4);
        for (int i = 0; i < 100; i++) wav[44 + i] = (unsigned char)(128 + 100 * sin(i * 0.3));
        size_t got = audioDecodeWav(wav, sizeof wav).size();
        check(got == 800, "8-bit mono 11 kHz file resamples to 44.1 kHz stereo (%d samples)", (int)got);

        audioSetVolume(0.f);   // silent: this only checks the mixer runs
        audioInit(g_assets);
        auto t0 = Clock::now();
        audioPlay(SND_MOVE);
        int ms = -1;
        for (int i = 0; i < 100 && ms < 0; i++) {
            Sleep(2);
            if (audioFramesPlayed() > 0) ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        }
        if (audioFramesPlayed() == 0 && ms < 0) check(true, "no sound device here, mixer skipped");
        else check(ms >= 0 && ms < 60, "mixer starts a sound %d ms after the call", ms);
        std::string src;
        for (int i = 0; i < SND_COUNT; i++) src += std::string(i ? ", " : "") + audioSource(i);
        check(true, "sound sources: %s", src.c_str());
        Sleep(120);
        audioShutdown();
    }

    // ---- parser and ids
    {
        Json j = Json::parse("{\"a\":[1,2,{\"b\":\"x\\u00e9y\"}],\"c\":true,\"d\":{\"integerValue\":\"42\"}}");
        check(j["a"][1].n == 2 && j["a"][2]["b"].s == "x\xC3\xA9y" && j["c"].b && j["d"]["integerValue"].s == "42", "JSON parser");
        check(fb::chatIdFor("bbb", "aaa") == "aaa_bbb" && fb::chatIdFor("aaa", "bbb") == "aaa_bbb", "chat id is order-independent");
    }

    // ---- names from names.txt
    {
        const auto& lv = allLevels();
        check(lv[0].name == "Billy" && lv[149].name == "The Perfect Player", "names: AI 1 is %s, AI 150 is %s", lv[0].name.c_str(),
              lv[149].name.c_str());
    }

    // ---- daily puzzles: a real forced mate, the same for everyone, solvable by its own line
    {
        Engine* e1 = new Engine();
        Engine* e2 = new Engine();
        Puzzle a, b;
        unsigned seed = 20261113u * 131u + 7919u;
        bool ok = generatePuzzle(seed, a, *e1) && generatePuzzle(seed, b, *e2);
        check(ok && a.fen == b.fen && a.mate == b.mate, "daily puzzle is repeatable: mate in %d from %s", a.mate, a.fen.c_str());
        Position p;
        bool mated = false;
        if (ok && p.setFEN(a.fen)) {
            e1->clear();
            int need = forcedMate(*e1, p, 3);
            for (int guard = 0; guard < 8 && need > 0 && !mated; guard++) {
                Move best = 0;
                need = forcedMate(*e1, p, need, &best);
                if (!best || !p.make(best)) break;
                if (p.outcome() != 2) { mated = p.inCheck(); break; }
                MoveList ml;
                p.genLegal(ml);
                p.make(ml.m[0]);   // any defence: the mate must hold against all of them
                need = max(1, need - 1);
                need = forcedMate(*e1, p, need) ? need : 0;
            }
        }
        check(mated, "following the engine's line from the puzzle ends in checkmate");
        delete e1;
        delete e2;
    }

    // ---- tournament pairings
    {
        Tourney t;
        t.id = "T";
        for (int i = 0; i < 7; i++) { t.players.push_back("p" + std::to_string(i)); t.names.push_back("n"); t.teams.push_back(""); }
        std::vector<TourResult> res;
        bool sound = true;
        for (int round = 1; round <= 4 && sound; round++) {
            std::vector<std::string> made = makePairings(t, res, round, 5 + round);
            std::map<std::string, int> seen;
            int byes = 0;
            for (auto& sp : made) {
                PairingView pv = parsePairing(sp);
                seen[pv.white]++;
                if (pv.black == "BYE") byes++;
                else seen[pv.black]++;
                for (auto& old : t.pairings) {   // no rematches in the first rounds
                    PairingView o = parsePairing(old);
                    if (pv.black != "BYE" && ((o.white == pv.white && o.black == pv.black) || (o.white == pv.black && o.black == pv.white))) sound = false;
                }
                res.push_back({pv.black == "BYE" ? byeId(round, pv.white) : pairingGameId("T", round, pv.white, pv.black), pv.white, pv.black, "1-0"});
            }
            if (made.size() != 4 || byes != 1 || seen.size() != 7) sound = false;
            for (auto& kv : seen)
                if (kv.second != 1) sound = false;
            t.pairings.insert(t.pairings.end(), made.begin(), made.end());
            t.round = round;
        }
        std::map<std::string, int> pts = tourPoints(t, res);
        int total = 0;
        for (auto& kv : pts) total += kv.second;
        check(sound && total == 4 * 4 * 2 && roundComplete(t, res), "Swiss pairings: 7 players, 4 rounds, one bye a round, no rematches, %d half-points", total);
    }

    // ---- member-built engines
    {
        EngineSpec weak, strong;
        weak.depth = 1; weak.noise = 400; weak.blunder = 30; weak.pos = 20;
        strong.depth = 14; strong.noise = 0; strong.blunder = 0; strong.pos = 100; strong.aggression = 8;
        strong.name = "Test";
        Level L = levelFromSpec(strong);
        Engine* eng = new Engine();
        Position p;
        p.setStart();
        BotMove bm = botChoose(*eng, p, L, nullptr, nullptr, rng, nullptr, 150);
        MoveList ml;
        p.genLegal(ml);
        bool legal = false;
        for (int i = 0; i < ml.n; i++) legal |= ml.m[i] == bm.move;
        delete eng;
        check(engineRating(weak) <= 500 && engineRating(strong) == 3500 && L.aggression == 8 && legal,
              "engine dials: weakest rates %d, strongest %d, and a built engine plays a legal move", engineRating(weak), engineRating(strong));
    }

    // ---- the club brain: splitting a network up for the server and putting it back together
    {
        Net before;
        before.init(99);
        std::vector<float> rebuilt(Net::SIZE, 0.f);
        bool ok = true;
        int covered = 0;
        for (int i = 0; i < BRAIN_CHUNKS; i++) {
            int from = i * BRAIN_CHUNK_FLOATS, count = min(BRAIN_CHUNK_FLOATS, Net::SIZE - from);
            if (count <= 0) { ok = false; break; }
            std::string text = base64Encode(&before.P[from], count * sizeof(float));
            std::vector<unsigned char> raw = base64Decode(text);
            if ((int)raw.size() != count * (int)sizeof(float)) { ok = false; break; }
            if (text.size() > 1048487) ok = false;   // one Firestore field must hold it
            memcpy(&rebuilt[from], raw.data(), raw.size());
            covered += count;
        }
        for (int i = 0; i < Net::SIZE && ok; i++)
            if (rebuilt[i] != before.P[i]) ok = false;
        check(ok && covered == Net::SIZE, "the brain splits into %d pieces for the server and comes back identical (%d weights)",
              BRAIN_CHUNKS, covered);

        // and a brain from the server replaces the one here
        std::wstring dir = g_data + L"\\selftest_cloud";
        CreateDirectoryW(dir.c_str(), NULL);
        for (const wchar_t* f : {L"\\brain.bin", L"\\memory.bin", L"\\yourmoves.bin", L"\\habits.bin"}) _wremove((dir + f).c_str());
        Brain b;
        b.open(dir);
        Net fromServer;
        fromServer.init(7);
        fromServer.P[Net::OFF_OUT] = 0.25f;   // not blank: it has been trained
        fromServer.blank = false;
        b.adopt(fromServer);
        Position p;
        randomPosition(p, rng, 18);
        int here = b.net()->evalCp(p), there = fromServer.evalCp(p);
        Brain reopened;
        reopened.open(dir);
        check(here == there && reopened.net()->evalCp(p) == there, "a brain from the server is adopted and kept (%+d cp)", there);
        for (const wchar_t* f : {L"\\brain.bin", L"\\memory.bin", L"\\yourmoves.bin", L"\\habits.bin"}) _wremove((dir + f).c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // ---- themes
    {
        Theme keep = g_theme;
        bool ok = true;
        for (int mode = 0; mode < 3; mode++)
            for (int board = 0; board < COUNT_OF(BOARDS); board++) {
                g_theme.mode = mode;
                g_theme.board = board;
                g_theme.texture = board % 3;
                g_theme.fonts = board % COUNT_OF(FONTSETS);
                applyTheme();
                if (!g_felt || !g_paper || !g_maple || !g_darkwood || !g_walnut) ok = false;
            }
        g_theme.mode = 1;
        applyTheme();
        bool dark = C_PAPER.GetR() < 80 && C_PRINT.GetR() > 200;
        g_theme.mode = 2;
        applyTheme();
        bool hc = C_PAPER.GetR() == 0 && C_PRINT.GetR() == 255;
        g_theme = keep;
        applyTheme();
        check(ok && dark && hc, "every theme, board and font set applies (dark and high-contrast palettes verified)");
    }

    if (g_testOut) {
        fprintf(g_testOut, "%s (%d failed)\n", g_testFails ? "SOME FAILED" : "ALL PASSED", g_testFails);
        fclose(g_testOut);
    }
    return g_testFails;
}

// ---------------------------------------------------------------- hidden renders

static bool savePng(HBITMAP bmp, const std::wstring& path) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, info);
    CLSID clsid = {};
    bool found = false;
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(info[i].MimeType, L"image/png")) { clsid = info[i].Clsid; found = true; }
    if (!found) return false;
    Bitmap* b = Bitmap::FromHBITMAP(bmp, NULL);
    bool ok = b && b->Save(path.c_str(), &clsid, NULL) == Ok;
    delete b;
    return ok;
}

static void mockGame() {
    g.pos.setStart();
    g.sans.clear();
    g.ucis.clear();
    static const char* line[] = {"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5c6", "d7c6", "e1g1", "f7f6", "d2d4",
                                 "e5d4", "f3d4", "c6c5", "d4b3", "d8d1", "f1d1", "c8g4", "f2f3", "g4e6", "b1c3"};
    for (const char* u : line) {
        MoveList ml;
        g.pos.genLegal(ml);
        for (int i = 0; i < ml.n; i++)
            if (g.pos.uci(ml.m[i]) == u) {
                g.sans.push_back(widen(g.pos.san(ml.m[i])));
                g.ucis.push_back(u);
                g.last = ml.m[i];
                g.pos.make(ml.m[i]);
                break;
            }
    }
    g.human = BLACK;
    g.flipped = false;
    g.level = 36;
    g.over = false;
    g.online = false;
    g.timed = true;
    g.baseMs = 300000;
    g.incMs = 3000;
    g.clockMs[0] = 243000;
    g.clockMs[1] = 198000;
    g.turnStart = Clock::now();
    g.predHits = 4;
    g.predTotal = 10;
    g.selected = -1;
    g.targets.clear();
}

static int runShots(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), NULL);
    SetWindowPos(g_hwnd, NULL, 0, 0, 1320, 880, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // stays hidden
    int saved = 0, shots = 0;
    auto shot = [&](Screen s, const wchar_t* name) {
        g_screen = s;
        g_bgKey = -1;
        g_sceneDirty = true;
        HDC dc = GetDC(g_hwnd);
        render(dc);
        ReleaseDC(g_hwnd, dc);
        // the ladder screen composites its cached list into the final frame, so save that one
        shots++;
        if (savePng(s == SCR_MENU || overlayActive() ? g_memL.bmp : g_sceneL.bmp, dir + L"\\" + name + L".png")) saved++;
    };
    Theme keep = g_theme;
    auto theme = [&](int mode, int board, int fonts, int accent, int texture) {
        g_theme = Theme();
        for (int i = 0; i < (int)g_pieceSets.size(); i++)
            if (g_pieceSets[i] == L"classic") g_theme.pieceSet = i;
        g_theme.mode = mode;
        g_theme.board = board;
        g_theme.fonts = fonts;
        g_theme.accent = accent;
        g_theme.texture = texture;
        applyTheme();
    };
    theme(0, 0, 0, 0, keep.texture);
    loadPieceSet();

    // made-up people so the social screens have something to show
    g_myUid = "me";
    g_myName = "Ewan";
    g_profile = fb::Profile();
    g_profile.name = "Ewan";
    g_profile.rating = 1260;
    g_profile.wins = 14; g_profile.draws = 3; g_profile.losses = 9;
    g_profile.avatar = 9;
    g_profile.bio = "Knights before bishops. Usually.";
    g_avatarPick = 9;
    const char* names[] = {"Marlowe", "Ottilie", "Bram", "Seren"};
    for (int i = 0; i < 4; i++) {
        fb::Person p;
        p.uid = std::string("u") + std::to_string(i);
        p.name = names[i];
        p.avatar = (i * 7 + 2) % AVATAR_COUNT;
        if (i < 3) g_friends.push_back(p);
        else { p.kind = "request"; g_requests.push_back(p); }
    }
    g_selPerson = g_friends[1];
    g_viewed = fb::Profile();
    g_viewed.uid = "u1"; g_viewed.name = "Ottilie"; g_viewed.rating = 1410; g_viewed.avatar = g_friends[1].avatar;
    g_viewed.wins = 31; g_viewed.draws = 6; g_viewed.losses = 22; g_viewed.bio = "Sicilian or nothing.";
    g_viewed.updated = fb::nowMs() - 5 * 60 * 1000;
    g_viewedOk = g_socialLoaded = true;
    for (int i = 0; i < 3; i++) {
        fb::Chat c;
        c.a = "me"; c.b = g_friends[i].uid; c.aName = "Ewan"; c.bName = g_friends[i].name;
        c.id = fb::chatIdFor(c.a, c.b);
        c.bAvatar = g_friends[i].avatar;
        c.last = i == 0 ? "Rematch tonight? 5+3" : i == 1 ? "gg, that knight fork hurt" : "ok see you there";
        c.lastFrom = i == 1 ? "me" : g_friends[i].uid;
        c.lastAt = fb::nowMs() - (i + 1) * 9 * 60 * 1000;
        g_chats.push_back(c);
    }
    g_chatSeen[g_chats[2].id] = fb::nowMs();
    g_unread = countUnread();
    g_chatWith = g_friends[0];
    const char* talk[] = {"Good game earlier!", "Thanks. You had me until move 30.", "I should not have traded queens.",
                          "The endgame was lost either way, I think. Your rook was stuck behind the pawns the whole time.",
                          "Rematch tonight? 5+3"};
    for (int i = 0; i < 5; i++) {
        fb::Message m;
        m.from = i % 2 ? "me" : "u0";
        m.text = talk[i];
        m.at = fb::nowMs() - (40 - i * 7) * 60 * 1000;
        g_msgs.push_back(m);
    }
    for (int i = 0; i < 3; i++) {
        fb::GameDoc d;
        d.id = "t" + std::to_string(i);
        d.createdBy = "x";
        (i % 2 ? d.black : d.white) = "x";
        (i % 2 ? d.blackName : d.whiteName) = i == 0 ? "Marlowe" : i == 1 ? "Guest 4821" : "Seren";
        d.whiteRating = d.blackRating = 1180 + i * 130;
        d.baseMs = (i == 0 ? 5 : i == 1 ? 10 : 0) * 60000LL;
        d.incMs = i == 0 ? 3000 : 0;
        g_openTables.push_back(d);
    }
    g_lobbyLoaded = true;
    g_rec[0].w = g_rec[1].w = g_rec[2].w = 1;
    g_sel = 36;
    g_menuScroll = g_scrollTarget = 33 * 44.f * g_ui;

    g_loginMode = 1;
    shot(SCR_LOGIN, L"01_login");
    shot(SCR_MENU, L"02_ladder");
    shot(SCR_LOBBY, L"03_online_room");
    mockGame();
    shot(SCR_GAME, L"04_game_light");
    shot(SCR_SETTINGS, L"05_settings_light");
    shot(SCR_PROFILE, L"06_profile");
    shot(SCR_PEOPLE, L"07_friends");
    shot(SCR_CHAT, L"08_messages");
    theme(1, 2, 1, 1, 0);
    shot(SCR_GAME, L"09_game_dark");
    shot(SCR_MENU, L"10_ladder_dark");
    shot(SCR_CHAT, L"11_messages_dark");
    shot(SCR_SETTINGS, L"12_settings_dark");
    theme(2, 1, 4, 4, 1);
    shot(SCR_GAME, L"13_game_high_contrast");
    shot(SCR_PEOPLE, L"14_friends_high_contrast");
    shot(SCR_MENU, L"15_ladder_high_contrast");

    // ---- plans, engines, puzzles, teams, tournaments
    theme(0, 0, 0, 0, keep.texture);
    g_plan = 3;
    for (int i = 0; i < 4; i++) {
        EngineSpec e;
        e.id = "e" + std::to_string(i);
        e.owner = i == 0 ? "me" : "u1";
        e.ownerName = i == 0 ? "Ewan" : "Ottilie";
        static const char* nm[] = {"Berserker", "The Wall", "Coffeehouse Carl", "Glass Cannon"};
        static const char* bl[] = {"Throws everything at your king.", "Trades down and waits for you to crack.",
                                   "Plays fast, misses things, never gives up.", "Brilliant attacks, shaky defence."};
        e.name = nm[i];
        e.blurb = bl[i];
        e.depth = 3 + i * 3;
        e.noise = 200 - i * 60;
        e.blunder = 12 - i * 4;
        e.pos = 50 + i * 15;
        e.aggression = i == 1 ? 1 : 9 - i;
        e.plays = 40 - i * 9;
        g_engines.push_back(e);
    }
    g_engSel = 0;
    g_engLoaded = true;
    shot(SCR_ENGINES, L"16_engines");
    g_engDraft = g_engines[0];
    g_engEditing = true;
    shot(SCR_ENGINES, L"17_engine_workshop");
    g_engEditing = false;

    g_puzDay = today();
    for (int i = 0; i < 5; i++) {
        Puzzle pz;
        pz.id = "d" + std::to_string(g_puzDay) + "_" + std::to_string(i);
        pz.title = "Daily puzzle " + std::to_string(i + 1);
        pz.author = "the club";
        pz.mate = 2 + (i % 2);
        pz.fen = "6k1/5ppp/8/8/8/8/5PPP/3RR1K1 w - - 0 1";
        g_daily.push_back(pz);
    }
    g_puzSolved.push_back(g_daily[0].id);
    for (int i = 0; i < 3; i++) {
        Puzzle pz;
        pz.id = "c" + std::to_string(i);
        static const char* tt[] = {"Smothered", "The quiet rook", "Two bishops"};
        pz.title = tt[i];
        pz.author = i == 1 ? "Ottilie" : "Marlowe";
        pz.mate = 1 + i;
        pz.fen = "6k1/5ppp/8/8/8/8/5PPP/3RR1K1 w - - 0 1";
        g_community.push_back(pz);
    }
    g_puzCommunityLoaded = true;
    shot(SCR_PUZZLES, L"18_puzzles");
    {
        Puzzle pz = g_daily[1];
        pz.mate = 1;
        pz.title = "Back rank";
        startPuzzle(pz);
        g.puzMsg = L"Find the checkmate.";
        shot(SCR_GAME, L"19_puzzle_board");
        g.puzzle = false;
    }

    for (int i = 0; i < 3; i++) {
        Team t;
        t.id = "team" + std::to_string(i);
        static const char* tn[] = {"Knights of the Okanagan", "Pawn Stars", "Endgame Society"};
        static const char* tg[] = {"KNT", "PWN", "END"};
        t.name = tn[i];
        t.tag = tg[i];
        t.motto = i == 0 ? "Forks before breakfast." : "";
        t.captain = i == 0 ? "me" : "u1";
        t.captainName = i == 0 ? "Ewan" : "Ottilie";
        t.members = i == 0 ? std::vector<std::string>{"me", "u0", "u2"} : std::vector<std::string>{"u1", "u3"};
        t.names = i == 0 ? std::vector<std::string>{"Ewan", "Marlowe", "Bram"} : std::vector<std::string>{"Ottilie", "Seren"};
        g_teams.push_back(t);
    }
    g_teamSel = "team0";
    g_teamsLoaded = true;
    shot(SCR_TEAMS, L"20_teams");

    {
        Tourney t;
        t.id = "tour0";
        t.name = "Friday Night Swiss";
        t.host = "me";
        t.hostName = "Ewan";
        t.kind = "team";
        t.status = "running";
        t.rounds = 5;
        t.round = 2;
        t.maxPlayers = 16;
        t.players = {"me", "u0", "u1", "u2", "u3", "u4"};
        t.names = {"Ewan", "Marlowe", "Ottilie", "Bram", "Seren", "Quill"};
        t.teams = {"KNT", "KNT", "PWN", "KNT", "PWN", ""};
        t.pairings = {"1|me|u1", "1|u2|u0", "1|u3|u4", "2|u0|me", "2|u1|u3", "2|u4|u2"};
        g_tours.push_back(t);
        Tourney o;
        o.id = "tour1";
        o.name = "Sunday Blitz";
        o.host = "u1";
        o.hostName = "Ottilie";
        o.kind = "solo";
        o.status = "open";
        o.players = {"u1", "u3"};
        o.names = {"Ottilie", "Seren"};
        o.teams = {"", ""};
        o.baseMs = 180000;
        o.incMs = 2000;
        g_tours.push_back(o);
        g_tourResults = {{pairingGameId("tour0", 1, "me", "u1"), "me", "u1", "1-0"},
                         {pairingGameId("tour0", 1, "u2", "u0"), "u2", "u0", "1/2"},
                         {pairingGameId("tour0", 1, "u3", "u4"), "u3", "u4", "0-1"},
                         {pairingGameId("tour0", 2, "u1", "u3"), "u1", "u3", "1-0"}};
        g_tourSel = "tour0";
        g_toursLoaded = true;
    }
    shot(SCR_TOURNEYS, L"21_tournament");
    shot(SCR_PLANS, L"22_plans");
    g_buyUrl[1] = "https://buy.stripe.com/test_silver";
    g_buyUrl[2] = "https://buy.stripe.com/test_gold";
    g_buyUrl[3] = "https://buy.stripe.com/test_ruby";
    g_portalUrl = "https://billing.stripe.com/p/login/test";
    g_plan = 1;
    g_founder = false;
    shot(SCR_PLANS, L"31_plans_for_sale");
    g_planConfirm = 2;
    shot(SCR_PLANS, L"32_plan_confirm");
    g_planConfirm = -1;
    g_planPending = 2;
    g_planChecks = 3;
    shot(SCR_PLANS, L"33_plan_waiting");
    g_planPending = -1;
    g_plan = 3;
    g_founder = true;
    g_plan = 0;
    g_openTables.resize(1);
    g_openTables[0].invite = "me";
    shot(SCR_LOBBY, L"23_online_room_bronze");
    g_confirm = 1;
    g.over = false;
    shot(SCR_GAME, L"24_resign_confirm");
    theme(1, 0, 0, 0, 0);
    g_plan = 3;
    shot(SCR_TOURNEYS, L"25_tournament_dark");
    shot(SCR_PROFILE, L"26_profile_dark");
    theme(0, 1, 0, 0, 1);
    for (int i = 0; i < (int)g_pieceSets.size(); i++)
        if (g_pieceSets[i] == L"designer-art") g_theme.pieceSet = i;
    loadPieceSet();
    g.pos.setStart();
    g.sans.clear();
    g.last = 0;
    g_confirm = 0;
    shot(SCR_GAME, L"27_designer_pieces");
    shot(SCR_SETTINGS, L"28_settings_piece_set");
    for (int i = 0; i < (int)g_pieceSets.size(); i++)
        if (g_pieceSets[i] == L"buddies") g_theme.pieceSet = i;
    loadPieceSet();
    shot(SCR_GAME, L"29_buddies_pieces");
    theme(0, 0, 0, 0, keep.texture);
    loadPieceSet();
    mockGame();
    g_marks = {{12, 28, 0}, {57, 42, 0}, {36, 36, 1}, {5, 33, 2}, {62, 45, 3}};
    g.preFrom = 57;
    g.preTo = 42;
    g_thinking = true;
    shot(SCR_GAME, L"30_planning_and_premove");
    g_thinking = false;
    g_marks.clear();
    g.preFrom = g.preTo = -1;
    g_theme = keep;
    return saved == shots ? 0 : 1;
}

// ---------------------------------------------------------------- live server pass

static int runNetTest() {
    g_testOut = _wfopen((g_data + L"\\nettest.txt").c_str(), L"w");
    fb::Session s;
    if (!fb::loadSession(sessionPath(), s)) {
        check(false, "no saved membership in this data folder");
    } else {
        std::string msg;
        fb::Err e = fb::refresh(s, &msg);
        check(e == fb::OK, "session refresh (%d %s)", (int)e, msg.c_str());
        if (e == fb::OK) {
            g_sess = s;
            fb::Profile p;
            e = fb::getProfile(g_sess, s.uid, p);
            check(e == fb::OK, "read own profile: rating %d", p.rating);
            if (p.name.empty()) p.name = s.name;
            e = fb::putProfile(g_sess, p);
            check(e == fb::OK, "write own profile with the new fields (rules accept it): %d", (int)e);
            std::vector<fb::Profile> found;
            e = fb::searchUsers(g_sess, p.name.substr(0, 2), found);
            bool self = false;
            for (auto& f : found) self |= f.uid == s.uid;
            check(e == fb::OK && self, "member lookup finds you among %d result(s)", (int)found.size());
            std::vector<fb::Person> list;
            for (const char* sub : {"friends", "requests", "blocked"}) {
                e = fb::listPeople(g_sess, s.uid, sub, list);
                check(e == fb::OK, "list %s: %d", sub, (int)list.size());
            }
            std::vector<fb::Chat> chats;
            e = fb::listChats(g_sess, chats);
            check(e == fb::OK, "list conversations: %d", (int)chats.size());
            std::vector<fb::GameDoc> games;
            e = fb::listOpenGames(g_sess, games);
            check(e == fb::OK, "list open tables: %d", (int)games.size());
            int plan = fb::getPlan(g_sess, s.uid);
            check(true, "founding member: %s", fb::isFounder(g_sess, s.uid) ? "yes" : "no");
            check(plan >= 0, "your plan on the server: %s", plan == 3 ? "Ruby" : plan == 2 ? "Gold" : plan == 1 ? "Silver" : "Bronze");
            std::vector<fb::Doc> docs;
            for (const char* col : {"engines", "puzzles", "teams", "tournaments"}) {
                e = fb::docList(g_sess, col, docs, 20);
                check(e == fb::OK, "list %s: %d", col, (int)docs.size());
            }
            fb::Doc brain;
            e = fb::docGet(g_sess, "brain/meta", brain);
            check(e == fb::OK, "read the club brain's note: %s",
                  brain.exists ? ("version " + std::to_string(brain.num("version")) + ", " + std::to_string(brain.num("games")) + " games").c_str()
                               : "not started yet");
            fb::Doc cfg;
            e = fb::docGet(g_sess, "config/app", cfg);
            check(e == fb::OK, "read config/app (%s)", cfg.exists ? "set up" : "not created yet");
            std::vector<fb::Doc> secret;
            e = fb::docList(g_sess, "billingCustomers", secret, 5);
            check(e == fb::AUTH, "the billing customer map is closed to the app (%d)", (int)e);
            // the plan document must not be writable from the app
            e = fb::docSet(g_sess, "plans/" + s.uid, fb::fvBody({{"plan", fb::fvStr("ruby")}}));
            check(e == fb::AUTH, "members cannot give themselves a plan (%d)", (int)e);
            if (plan < 3) {
                e = fb::docCreate(g_sess, "tournaments", "", fb::fvBody({{"name", fb::fvStr("should be refused")}}));
                check(e != fb::OK, "hosting a tournament without Ruby is refused (%d)", (int)e);
            }
            // a stranger's private lists must be refused
            e = fb::listPeople(g_sess, "someone-else", "friends", list);
            check(e == fb::AUTH, "another member's friends list is refused (%d)", (int)e);
        }
    }
    if (g_testOut) {
        fprintf(g_testOut, "%s (%d failed)\n", g_testFails ? "SOME FAILED" : "ALL PASSED", g_testFails);
        fclose(g_testOut);
    }
    return g_testFails;
}
