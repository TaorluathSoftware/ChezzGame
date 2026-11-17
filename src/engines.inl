// The Engines tab: chess engines built by Ruby members, playable by everyone. An engine is a set of
// dials (search depth, thinking time, mistakes, blunders, positional sense, aggression) stored in
// Firestore; the game runs it locally through the same search and brain as the ladder bots.
// Included by main.cpp after social.inl.

struct EngineSpec {
    std::string id, owner, ownerName, name, blurb;
    int depth = 4, timeMs = 1000, noise = 80, blunder = 4, pos = 70, aggression = 3, plays = 0;
    long long createdAt = 0;
};

static std::vector<EngineSpec> g_engines;
static int g_engSel = -1;
static bool g_engEditing = false, g_engLoaded = false;
static EngineSpec g_engDraft;
static const int ENG_MAX_OWNED = 6;

enum { ED_DEPTH, ED_TIME, ED_NOISE, ED_BLUNDER, ED_POS, ED_AGGR, ED_COUNT };
static const wchar_t* ED_LABEL[ED_COUNT] = {L"LOOKS AHEAD", L"THINKING TIME", L"MISJUDGEMENTS", L"OUTRIGHT BLUNDERS", L"POSITIONAL SENSE",
                                            L"AGGRESSION"};

static EngineSpec specFromDoc(const fb::Doc& d) {
    EngineSpec e;
    e.id = d.id;
    e.owner = d.str("owner");
    e.ownerName = d.str("ownerName");
    e.name = d.str("name");
    e.blurb = d.str("blurb");
    e.depth = max(1, min(14, (int)d.num("depth")));
    e.timeMs = max(200, min(5000, (int)d.num("timeMs")));
    e.noise = max(0, min(400, (int)d.num("noise")));
    e.blunder = max(0, min(30, (int)d.num("blunder")));
    e.pos = max(20, min(100, (int)d.num("pos")));
    e.aggression = max(0, min(10, (int)d.num("aggression")));
    e.plays = (int)d.num("plays");
    e.createdAt = d.num("createdAt");
    return e;
}

static std::string specBody(const EngineSpec& e) {
    return fb::fvBody({{"owner", fb::fvStr(e.owner)}, {"ownerName", fb::fvStr(e.ownerName)}, {"name", fb::fvStr(e.name)},
                       {"blurb", fb::fvStr(e.blurb)}, {"depth", fb::fvInt(e.depth)}, {"timeMs", fb::fvInt(e.timeMs)},
                       {"noise", fb::fvInt(e.noise)}, {"blunder", fb::fvInt(e.blunder)}, {"pos", fb::fvInt(e.pos)},
                       {"aggression", fb::fvInt(e.aggression)}, {"plays", fb::fvInt(e.plays)}, {"createdAt", fb::fvInt(e.createdAt)}});
}

// A rough strength guess from the dials, on the same 250-3500 scale as the ladder (rounded to 10).
static int engineRating(const EngineSpec& e) {
    float fd = min(1.f, (e.depth - 1) / 13.f);                 // how far it sees
    float fn = 1.f - powf(e.noise / 400.f, 0.5f);              // how often it misjudges
    float fb2 = 1.f - e.blunder / 30.f;                        // how often it throws a piece away
    float fp = (e.pos - 20) / 80.f;
    float f = 0.42f * fd + 0.26f * fn + 0.2f * fb2 + 0.12f * fp;
    f *= 0.55f + 0.45f * fb2;                                   // frequent blunders cap everything else
    return (int)lround((250 + 3250 * max(0.f, min(1.f, f))) / 10.f) * 10;
}

static Level levelFromSpec(const EngineSpec& e) {
    Level L = allLevels()[levelForRating(engineRating(e))];   // borrows that tier's title
    L.name = e.name;
    L.rating = engineRating(e);
    L.blurb = e.blurb;
    L.depth = e.depth >= 14 ? 64 : e.depth;
    L.timeMs = e.timeMs;
    L.noise = e.noise + 4;
    L.blunder = e.blunder / 100.f;
    L.quiesce = e.depth >= 2;
    L.posScale = e.pos / 100.f;
    L.aggression = e.aggression;
    return L;
}

static std::wstring dialText(const EngineSpec& e, int row) {
    wchar_t b[64];
    switch (row) {
        case ED_DEPTH: swprintf(b, 64, e.depth >= 14 ? L"as far as time allows" : L"%d moves", e.depth); break;
        case ED_TIME: swprintf(b, 64, L"%.1f s a move", e.timeMs / 1000.0); break;
        case ED_NOISE: swprintf(b, 64, e.noise == 0 ? L"none" : e.noise < 60 ? L"rare (%d)" : e.noise < 180 ? L"some (%d)" : L"often (%d)", e.noise); break;
        case ED_BLUNDER: swprintf(b, 64, e.blunder ? L"%d%% of moves" : L"never", e.blunder); break;
        case ED_POS: swprintf(b, 64, L"%d%%", e.pos); break;
        default: swprintf(b, 64, e.aggression == 0 ? L"calm" : e.aggression < 4 ? L"measured (%d)" : e.aggression < 8 ? L"attacking (%d)" : L"all-out (%d)", e.aggression);
    }
    return b;
}

static void dialStep(EngineSpec& e, int row, int dir) {
    switch (row) {
        case ED_DEPTH: e.depth = max(1, min(14, e.depth + dir)); break;
        case ED_TIME: e.timeMs = max(200, min(5000, e.timeMs + dir * 200)); break;
        case ED_NOISE: e.noise = max(0, min(400, e.noise + dir * 20)); break;
        case ED_BLUNDER: e.blunder = max(0, min(30, e.blunder + dir * 2)); break;
        case ED_POS: e.pos = max(20, min(100, e.pos + dir * 10)); break;
        default: e.aggression = max(0, min(10, e.aggression + dir));
    }
}

static RectF engineEditRect(int i) {   // 0 name, 1 description (while building)
    RectF r = socialRight();
    return RectF(r.X + S(24), r.Y + S(92) + i * S(70), r.Width - S(48), S(34));
}

static void refreshEngines() {
    if (g_offline || g_myUid.empty()) return;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::Doc> docs;
        fb::Err e = fb::docList(g_sess, "engines", docs, 200);
        return [e, docs]() {
            g_engLoaded = true;
            if (e != fb::OK) { socialSay(e == fb::NET ? L"Can't reach the club's server." : L"Couldn't fetch the engines.", true); return; }
            std::string keep = g_engSel >= 0 && g_engSel < (int)g_engines.size() ? g_engines[g_engSel].id : "";
            g_engines.clear();
            for (auto& d : docs) g_engines.push_back(specFromDoc(d));
            std::sort(g_engines.begin(), g_engines.end(), [](const EngineSpec& a, const EngineSpec& b) {
                return a.plays != b.plays ? a.plays > b.plays : a.createdAt > b.createdAt;
            });
            g_engSel = g_engines.empty() ? -1 : 0;
            for (int i = 0; i < (int)g_engines.size(); i++)
                if (g_engines[i].id == keep) g_engSel = i;
            invalidate();
        };
    });
}

static void openEngines() {
    g_socialMsg.clear();
    g_engEditing = false;
    goScreen(SCR_ENGINES);
    refreshEngines();
}

static void beginEngineEdit(const EngineSpec* existing) {
    if (!needPlan(3, L"Building engines")) return;
    int owned = 0;
    for (auto& e : g_engines) owned += e.owner == g_myUid;
    if (!existing && owned >= ENG_MAX_OWNED) { socialSay(L"You already have six engines. Retire one first.", true); return; }
    g_engDraft = existing ? *existing : EngineSpec();
    g_engEditing = true;
    g_socialMsg.clear();
    showLoginEdits(false);
    SetWindowTextW(g_edGen[0], widen(g_engDraft.name).c_str());
    SetWindowTextW(g_edGen[1], widen(g_engDraft.blurb).c_str());
    invalidate();
}

static void saveEngine() {
    EngineSpec e = g_engDraft;
    e.name = narrow(editText(g_edGen[0]));
    e.blurb = narrow(editText(g_edGen[1]));
    while (!e.name.empty() && e.name.back() == ' ') e.name.pop_back();
    if (e.name.size() < 2 || e.name.size() > 24) { socialSay(L"Give the engine a name of 2 to 24 characters.", true); return; }
    if (e.blurb.size() > 140) e.blurb.resize(140);
    e.owner = g_myUid;
    e.ownerName = g_myName;
    if (!e.createdAt) e.createdAt = fb::nowMs();
    socialSay(L"Saving the engine\x2026");
    netAsync([e]() -> std::function<void()> {
        fb::Err err = e.id.empty() ? fb::docCreate(g_sess, "engines", "", specBody(e)) : fb::docSet(g_sess, "engines/" + e.id, specBody(e));
        return [err]() {
            if (err == fb::OK) {
                g_engEditing = false;
                showLoginEdits(false);
                socialSay(L"Engine saved. Anyone can play it now.");
                refreshEngines();
            } else {
                socialSay(err == fb::AUTH ? L"The server refused: building engines needs the Ruby plan." : L"Couldn't save the engine.", true);
            }
        };
    });
}

static void deleteEngine(const std::string& id) {
    netAsync([id]() -> std::function<void()> {
        fb::Err e = fb::docDelete(g_sess, "engines/" + id);
        return [e]() {
            socialSay(e == fb::OK ? L"Engine retired." : L"Couldn't retire it.", e != fb::OK);
            g_engEditing = false;
            showLoginEdits(false);
            refreshEngines();
        };
    });
}

static void playEngine(const EngineSpec& e) {
    std::string id = e.id;
    int plays = e.plays + 1;
    netAsync([id, plays]() -> std::function<void()> {   // count the game; the rules allow exactly +1
        fb::docSet(g_sess, "engines/" + id, fb::fvBody({{"plays", fb::fvInt(plays)}}), {"plays"});
        return nullptr;
    });
    startCustomGame(levelFromSpec(e), L"Engine by " + widen(e.ownerName));
}

static void dialBars(Graphics& gr, const EngineSpec& e, float x, float y, float w, bool editable) {
    float rh = S(40);
    for (int r = 0; r < ED_COUNT; r++) {
        float ry = y + r * rh;
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, x, ry + rh - 1, x + w, ry + rh - 1);
        drawText(gr, ED_LABEL[r], RectF(x, ry, w * 0.42f, rh), F_MONO, S(12), C_FAINT, 0, 0, 1);
        float vx = x + w * 0.42f, vw = w * 0.58f, bs = S(28);
        if (editable) {
            inkButton(gr, B_ENG_DEC + r, RectF(vx, ry + (rh - bs) / 2, bs, bs), L"\x2039");
            inkButton(gr, B_ENG_INC + r, RectF(vx + vw - bs, ry + (rh - bs) / 2, bs, bs), L"\x203A");
            drawText(gr, dialText(e, r), RectF(vx + bs, ry, vw - 2 * bs, rh), F_SERIF, S(14), C_INK, FontStyleRegular, 1, 1);
        } else {
            drawText(gr, dialText(e, r), RectF(vx, ry, vw, rh), F_SERIF, S(14), C_INK, FontStyleRegular, 0, 1);
        }
    }
}

static void renderEngines(Graphics& gr) {
    drawTopBar(gr, L"engines  \x00B7  built by Ruby members, playable by everyone");
    RectF L = socialLeft();
    paperSheet(gr, L);
    sheetHeader(gr, L, L"THE ENGINE ROOM");
    drawText(gr, L"games played", RectF(L.X, L.Y + S(8), L.Width - S(18), S(30)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    float rh = S(56), y = L.Y + S(52);
    int maxRows = (int)((L.Y + L.Height - y - S(60)) / rh);
    if (g_engines.empty())
        drawText(gr, !g_engLoaded ? L"Fetching\x2026" : L"No engines have been built yet. Ruby members can build the first.",
                 RectF(L.X + S(24), y + S(10), L.Width - S(48), S(70)), F_HAND, S(15), C_INK, 0, 0, 0, true);
    for (int i = 0; i < (int)g_engines.size() && i < maxRows; i++) {
        const EngineSpec& e = g_engines[i];
        RectF r(L.X + S(8), y + i * rh, L.Width - S(16), rh);
        if (i == g_engSel && !g_engEditing) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
        }
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, r.X, r.Y + rh - 1, r.X + r.Width, r.Y + rh - 1);
        drawIcon(gr, IC_ENGINE, RectF(r.X + S(8), r.Y + S(8), rh - S(16), rh - S(16)), C_PRINT);
        drawText(gr, widen(e.name), RectF(r.X + S(60), r.Y + S(4), r.Width - S(190), rh * 0.52f), F_SERIF, S(16), C_PRINT, FontStyleBold, 0, 2);
        drawText(gr, L"by " + widen(e.ownerName) + L"  \x00B7  about " + std::to_wstring(engineRating(e)),
                 RectF(r.X + S(60), r.Y + rh * 0.54f, r.Width - S(150), rh * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
        drawText(gr, std::to_wstring(e.plays), RectF(r.X, r.Y, r.Width - S(14), rh), F_MONO, S(15), C_PRINT, FontStyleBold, 2);
        g_buttons.push_back({r, B_ENGROW + i});
    }
    inkButton(gr, B_ENG_NEW, RectF(L.X + S(18), L.Y + L.Height - S(50), S(230), S(36)), L"BUILD AN ENGINE");
    if (g_plan < 3) {
        drawBadge(gr, 3, RectF(L.X + S(258), L.Y + L.Height - S(44), S(22), S(22)));
        drawText(gr, L"Ruby", RectF(L.X + S(286), L.Y + L.Height - S(50), S(100), S(36)), F_HAND, S(13), C_FAINT);
    }

    RectF R = socialRight();
    paperSheet(gr, R, true);
    float ix = R.X + S(24), iw = R.Width - S(48), yy = R.Y + S(18);
    if (g_engEditing) {
        drawText(gr, g_engDraft.id.empty() ? L"NEW ENGINE" : L"REBUILD ENGINE", RectF(ix, yy, iw, S(30)), F_MONO, S(16), C_PRINT, FontStyleBold);
        drawText(gr, L"about " + std::to_wstring(engineRating(g_engDraft)), RectF(ix, yy, iw, S(30)), F_MONO, S(15), C_RED, FontStyleBold, 2);
        fieldBox(gr, engineEditRect(0), L"NAME");
        fieldBox(gr, engineEditRect(1), L"DESCRIPTION (how it plays, up to 140 characters)");
        float dy = engineEditRect(1).Y + S(50);
        dialBars(gr, g_engDraft, ix, dy, iw, true);
        float by = dy + ED_COUNT * S(40) + S(16);
        stampButton(gr, B_ENG_SAVE, RectF(ix + S(4), by, S(210), S(56)), L"SAVE", L"");
        inkButton(gr, B_ENG_CANCEL, RectF(ix + S(240), by + S(10), S(130), S(36)), L"CANCEL");
        if (!g_engDraft.id.empty()) inkButton(gr, B_ENG_DELETE, RectF(ix + S(380), by + S(10), S(130), S(36)), L"RETIRE");
        statusLine(gr, RectF(ix, by + S(70), iw, S(50)));
        return;
    }
    if (g_engSel < 0 || g_engSel >= (int)g_engines.size()) {
        drawText(gr, L"Pick an engine on the left to see how it is built, then challenge it.", RectF(ix, yy, iw, S(60)), F_HAND,
                 S(16), C_INK, 0, 0, 0, true);
        statusLine(gr, RectF(ix, yy + S(70), iw, S(60)));
        return;
    }
    const EngineSpec& e = g_engines[g_engSel];
    drawText(gr, widen(e.name), RectF(ix, yy, iw - S(120), S(40)), F_SERIF, S(28), C_PRINT, FontStyleBold);
    drawText(gr, L"about " + std::to_wstring(engineRating(e)), RectF(ix, yy, iw, S(40)), F_MONO, S(16), C_RED, FontStyleBold, 2);
    drawText(gr, L"built by " + widen(e.ownerName), RectF(ix, yy + S(40), iw, S(24)), F_SERIF, S(13), C_FAINT, FontStyleItalic);
    yy += S(70);
    if (!e.blurb.empty()) {
        drawText(gr, L"\x201C" + widen(e.blurb) + L"\x201D", RectF(ix, yy, iw, S(50)), F_HAND, S(14), C_INK, 0, 0, 0, true);
        yy += S(56);
    }
    dialBars(gr, e, ix, yy, iw, false);
    yy += ED_COUNT * S(40) + S(14);
    colourTokens(gr, ix, yy, L"YOU PLAY");
    yy += S(82);
    tcChooser(gr, ix, yy, iw);
    yy += S(46);
    stampButton(gr, B_ENG_PLAY, RectF(ix + S(6), yy, min(iw - S(12), S(300)), S(64)), L"CHALLENGE", widen(e.name));
    if (e.owner == g_myUid) inkButton(gr, B_ENG_EDIT, RectF(ix + S(330), yy + S(14), S(150), S(36)), L"REBUILD");
    statusLine(gr, RectF(ix, yy + S(74), iw, S(44)));
}

static bool onEnginesButton(int id) {
    if (id >= B_ENGROW && id < B_ENGROW + 300) {
        g_engSel = id - B_ENGROW;
        g_engEditing = false;
        g_socialMsg.clear();
        showLoginEdits(false);
        invalidate();
        return true;
    }
    if (id >= B_ENG_DEC && id < B_ENG_DEC + ED_COUNT) { dialStep(g_engDraft, id - B_ENG_DEC, -1); invalidate(); return true; }
    if (id >= B_ENG_INC && id < B_ENG_INC + ED_COUNT) { dialStep(g_engDraft, id - B_ENG_INC, 1); invalidate(); return true; }
    bool has = g_engSel >= 0 && g_engSel < (int)g_engines.size();
    switch (id) {
        case B_ENG_NEW: beginEngineEdit(nullptr); return true;
        case B_ENG_EDIT: if (has) beginEngineEdit(&g_engines[g_engSel]); return true;
        case B_ENG_SAVE: saveEngine(); return true;
        case B_ENG_CANCEL: g_engEditing = false; showLoginEdits(false); invalidate(); return true;
        case B_ENG_DELETE: if (!g_engDraft.id.empty()) deleteEngine(g_engDraft.id); return true;
        case B_ENG_PLAY: if (has) playEngine(g_engines[g_engSel]); return true;
    }
    return false;
}
