// Teams and tournaments. Included by main.cpp after puzzles.inl.
//
// Teams (Silver and up): a name, a short tag, a captain and a roster of up to 20.
// Tournaments (Ruby hosts, Silver and up play): Swiss-style rounds. The host opens registration,
// starts round 1, and starts each later round once the results are in. Each pairing is an ordinary
// online game on a table only those two can sit at; when it ends, either player files the result
// and the server checks it against the finished game. "Solo" events rank players; "team battle"
// events also rank teams by their best four scores.

struct Team {
    std::string id, name, tag, motto, captain, captainName;
    std::vector<std::string> members, names;
    long long createdAt = 0;
};
struct Tourney {
    std::string id, name, host, hostName, kind, status;   // kind: solo | team; status: open | running | done
    std::vector<std::string> players, names, teams, pairings;   // pairings: "round|whiteUid|blackUid"
    int rounds = 5, round = 0, maxPlayers = 16;
    long long baseMs = 300000, incMs = 3000, createdAt = 0;
};
struct TourResult { std::string id, white, black, result; };

static std::vector<Team> g_teams;
static std::vector<Tourney> g_tours;
static std::vector<TourResult> g_tourResults;
static std::string g_teamSel, g_tourSel;
static bool g_teamCreating = false, g_tourCreating = false, g_teamsLoaded = false, g_toursLoaded = false, g_competeBusy = false;
static int g_tourKind = 0, g_tourRounds = 1, g_tourMax = 1;   // draft choices for a new tournament
static const int TOUR_ROUNDS[3] = {3, 5, 7}, TOUR_MAX[4] = {8, 16, 32, 64};

static Team teamFromDoc(const fb::Doc& d) {
    Team t;
    t.id = d.id;
    t.name = d.str("name");
    t.tag = d.str("tag");
    t.motto = d.str("motto");
    t.captain = d.str("captain");
    t.captainName = d.str("captainName");
    t.members = d.list("members");
    t.names = d.list("names");
    t.names.resize(t.members.size());
    t.createdAt = d.num("createdAt");
    return t;
}
static std::string teamBody(const Team& t) {
    return fb::fvBody({{"name", fb::fvStr(t.name)}, {"tag", fb::fvStr(t.tag)}, {"motto", fb::fvStr(t.motto)},
                       {"captain", fb::fvStr(t.captain)}, {"captainName", fb::fvStr(t.captainName)},
                       {"members", fb::fvList(t.members)}, {"names", fb::fvList(t.names)}, {"createdAt", fb::fvInt(t.createdAt)}});
}
static Tourney tourFromDoc(const fb::Doc& d) {
    Tourney t;
    t.id = d.id;
    t.name = d.str("name");
    t.host = d.str("host");
    t.hostName = d.str("hostName");
    t.kind = d.str("kind");
    t.status = d.str("status");
    t.players = d.list("players");
    t.names = d.list("names");
    t.teams = d.list("teams");
    t.names.resize(t.players.size());
    t.teams.resize(t.players.size());
    t.pairings = d.list("pairings");
    t.rounds = (int)d.num("rounds");
    t.round = (int)d.num("round");
    t.maxPlayers = (int)d.num("maxPlayers");
    t.baseMs = d.num("baseMs");
    t.incMs = d.num("incMs");
    t.createdAt = d.num("createdAt");
    return t;
}
static std::string tourBody(const Tourney& t) {
    return fb::fvBody({{"name", fb::fvStr(t.name)}, {"host", fb::fvStr(t.host)}, {"hostName", fb::fvStr(t.hostName)},
                       {"kind", fb::fvStr(t.kind)}, {"status", fb::fvStr(t.status)}, {"players", fb::fvList(t.players)},
                       {"names", fb::fvList(t.names)}, {"teams", fb::fvList(t.teams)}, {"pairings", fb::fvList(t.pairings)},
                       {"rounds", fb::fvInt(t.rounds)}, {"round", fb::fvInt(t.round)}, {"maxPlayers", fb::fvInt(t.maxPlayers)},
                       {"baseMs", fb::fvInt(t.baseMs)}, {"incMs", fb::fvInt(t.incMs)}, {"createdAt", fb::fvInt(t.createdAt)}});
}

static const Team* myTeam() {
    for (auto& t : g_teams)
        if (std::find(t.members.begin(), t.members.end(), g_myUid) != t.members.end()) return &t;
    return nullptr;
}
static const Team* selectedTeam() {
    for (auto& t : g_teams)
        if (t.id == g_teamSel) return &t;
    return nullptr;
}
static const Tourney* selectedTour() {
    for (auto& t : g_tours)
        if (t.id == g_tourSel) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- standings and pairings (pure)

static std::string pairingGameId(const std::string& tid, int round, const std::string& w, const std::string& b) {
    return "t_" + tid + "_" + std::to_string(round) + "_" + w + "_" + b;
}
static std::string byeId(int round, const std::string& uid) { return "bye_" + std::to_string(round) + "_" + uid; }

struct PairingView { int round; std::string white, black; };
static PairingView parsePairing(const std::string& s) {
    PairingView p{0, "", ""};
    size_t a = s.find('|'), b = s.find('|', a + 1);
    if (a == std::string::npos || b == std::string::npos) return p;
    p.round = atoi(s.substr(0, a).c_str());
    p.white = s.substr(a + 1, b - a - 1);
    p.black = s.substr(b + 1);
    return p;
}

// Points in half-points (win 2, draw 1), so everything stays in whole numbers.
static std::map<std::string, int> tourPoints(const Tourney& t, const std::vector<TourResult>& results) {
    std::map<std::string, int> pts;
    for (auto& p : t.players) pts[p] = 0;
    for (auto& r : results) {
        if (r.result == "1-0") pts[r.white] += 2;
        else if (r.result == "0-1") pts[r.black] += 2;
        else if (r.result == "1/2") { pts[r.white] += 1; pts[r.black] += 1; }
    }
    pts.erase("BYE");
    return pts;
}

static const TourResult* resultFor(const Tourney& t, const PairingView& p, const std::vector<TourResult>& results) {
    std::string id = p.black == "BYE" ? byeId(p.round, p.white) : pairingGameId(t.id, p.round, p.white, p.black);
    for (auto& r : results)
        if (r.id == id) return &r;
    return nullptr;
}

static bool roundComplete(const Tourney& t, const std::vector<TourResult>& results) {
    for (auto& s : t.pairings) {
        PairingView p = parsePairing(s);
        if (p.round == t.round && !resultFor(t, p, results)) return false;
    }
    return true;
}

// Swiss-style: highest scores first, each player meets the nearest-ranked opponent they haven't
// played yet; with an odd number the lowest-ranked player without a bye sits out for a full point.
static std::vector<std::string> makePairings(const Tourney& t, const std::vector<TourResult>& results, int round, unsigned seed) {
    std::map<std::string, int> pts = tourPoints(t, results);
    std::vector<std::string> order = t.players;
    std::mt19937 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) { return pts[a] > pts[b]; });
    auto played = [&](const std::string& a, const std::string& b) {
        for (auto& s : t.pairings) {
            PairingView p = parsePairing(s);
            if ((p.white == a && p.black == b) || (p.white == b && p.black == a)) return true;
        }
        return false;
    };
    auto whites = [&](const std::string& a) {
        int n = 0;
        for (auto& s : t.pairings) n += parsePairing(s).white == a;
        return n;
    };
    std::vector<std::string> out;
    std::vector<bool> used(order.size(), false);
    if (order.size() % 2) {   // bye
        for (int i = (int)order.size() - 1; i >= 0; i--)
            if (!played(order[i], "BYE")) {
                used[i] = true;
                out.push_back(std::to_string(round) + "|" + order[i] + "|BYE");
                break;
            }
    }
    for (size_t i = 0; i < order.size(); i++) {
        if (used[i]) continue;
        size_t pick = order.size();
        for (size_t j = i + 1; j < order.size(); j++)
            if (!used[j] && !played(order[i], order[j])) { pick = j; break; }
        if (pick == order.size())   // everyone left has met already: allow a rematch rather than leave them out
            for (size_t j = i + 1; j < order.size(); j++)
                if (!used[j]) { pick = j; break; }
        if (pick == order.size()) continue;
        used[i] = used[pick] = true;
        bool firstWhite = whites(order[i]) <= whites(order[pick]);
        const std::string& w = firstWhite ? order[i] : order[pick];
        const std::string& b = firstWhite ? order[pick] : order[i];
        out.push_back(std::to_string(round) + "|" + w + "|" + b);
    }
    return out;
}

// ---------------------------------------------------------------- syncing

static void refreshTeams() {
    if (g_offline || g_myUid.empty()) return;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::Doc> docs;
        fb::Err e = fb::docList(g_sess, "teams", docs, 200);
        return [e, docs]() {
            g_teamsLoaded = true;
            if (e != fb::OK) { if (g_screen == SCR_TEAMS) socialSay(L"Couldn't fetch the teams.", true); return; }
            g_teams.clear();
            for (auto& d : docs) g_teams.push_back(teamFromDoc(d));
            std::sort(g_teams.begin(), g_teams.end(), [](const Team& a, const Team& b) { return a.members.size() > b.members.size(); });
            if (!selectedTeam()) g_teamSel = myTeam() ? myTeam()->id : g_teams.empty() ? "" : g_teams[0].id;
            invalidate();
        };
    });
}

static void loadTourResults(const std::string& tid) {
    if (tid.empty()) return;
    netAsync([tid]() -> std::function<void()> {
        std::vector<fb::Doc> docs;
        fb::Err e = fb::docList(g_sess, "tournaments/" + tid + "/results", docs, 300);
        return [e, docs, tid]() {
            if (e != fb::OK || tid != g_tourSel) return;
            g_tourResults.clear();
            for (auto& d : docs) g_tourResults.push_back({d.id, d.str("white"), d.str("black"), d.str("result")});
            invalidate();
        };
    });
}

static void refreshTours() {
    if (g_offline || g_myUid.empty()) return;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::Doc> docs;
        fb::Err e = fb::docList(g_sess, "tournaments", docs, 100);
        return [e, docs]() {
            g_toursLoaded = true;
            if (e != fb::OK) { if (g_screen == SCR_TOURNEYS) socialSay(L"Couldn't fetch the tournaments.", true); return; }
            g_tours.clear();
            for (auto& d : docs) g_tours.push_back(tourFromDoc(d));
            auto rank = [](const Tourney& t) { return t.status == "running" ? 0 : t.status == "open" ? 1 : 2; };
            std::sort(g_tours.begin(), g_tours.end(), [&](const Tourney& a, const Tourney& b) {
                return rank(a) != rank(b) ? rank(a) < rank(b) : a.createdAt > b.createdAt;
            });
            if (!selectedTour()) { g_tourSel = g_tours.empty() ? "" : g_tours[0].id; g_tourResults.clear(); }
            loadTourResults(g_tourSel);
            invalidate();
        };
    });
}

static void openTeams() {
    g_socialMsg.clear();
    g_teamCreating = false;
    goScreen(SCR_TEAMS);
    refreshTeams();
}
static void openTours() {
    g_socialMsg.clear();
    g_tourCreating = false;
    goScreen(SCR_TOURNEYS);
    refreshTours();
    refreshTeams();   // team battles need to know which team you are in
}
// from netTick: keep an open tournament page current
static void competeTick() {
    if (g_screen == SCR_TOURNEYS && !g_tourCreating && g_netTick % 7 == 0) refreshTours();
}

// Writes a document change, then reloads. `what` is shown when the server says no.
static void competeWrite(std::function<fb::Err()> job, const std::wstring& done, const std::wstring& refused) {
    if (g_competeBusy) return;
    g_competeBusy = true;
    netAsync([job, done, refused]() -> std::function<void()> {
        fb::Err e = job();
        return [e, done, refused]() {
            g_competeBusy = false;
            if (e == fb::OK) socialSay(done);
            else socialSay(e == fb::AUTH ? refused : e == fb::NET ? L"Can't reach the club's server." : L"That didn't go through. Try again.", true);
            if (e == fb::OK) { g_teamCreating = false; g_tourCreating = false; showLoginEdits(false); }
            refreshTeams();
            refreshTours();
        };
    });
}

// ---------------------------------------------------------------- team actions

static RectF teamEditRect(int i) {   // 0 name, 1 tag, 2 motto (while founding a team)
    RectF r = socialRight();
    return RectF(r.X + S(24), r.Y + S(92) + i * S(70), i == 1 ? S(160) : r.Width - S(48), S(34));
}

static void createTeam() {
    if (!needPlan(1, L"Founding a team")) return;
    Team t;
    t.name = narrow(editText(g_edGen[0]));
    t.tag = narrow(editText(g_edGen[1]));
    t.motto = narrow(editText(g_edGen[2]));
    while (!t.name.empty() && t.name.back() == ' ') t.name.pop_back();
    for (char& c : t.tag) c = (char)toupper((unsigned char)c);
    bool tagOk = t.tag.size() >= 2 && t.tag.size() <= 5;
    for (char c : t.tag) tagOk = tagOk && isalnum((unsigned char)c);
    if (t.name.size() < 3 || t.name.size() > 24) { socialSay(L"A team name needs 3 to 24 characters.", true); return; }
    if (!tagOk) { socialSay(L"The tag is 2 to 5 letters or digits, like KNT.", true); return; }
    if (myTeam()) { socialSay(L"Leave your current team first.", true); return; }
    if (t.motto.size() > 100) t.motto.resize(100);
    t.captain = g_myUid;
    t.captainName = g_myName;
    t.members = {g_myUid};
    t.names = {g_myName};
    t.createdAt = fb::nowMs();
    competeWrite([t]() { return fb::docCreate(g_sess, "teams", "", teamBody(t)); }, L"Team founded. You are its captain.",
                 L"The server refused: founding a team needs Silver or above.");
}

static void joinTeam(const Team& t) {
    if (!needPlan(1, L"Joining a team")) return;
    if (myTeam()) { socialSay(L"You are already in a team. Leave it first.", true); return; }
    if (t.members.size() >= 20) { socialSay(L"That team is full (20 members).", true); return; }
    std::string id = t.id, me = g_myUid, name = g_myName;
    competeWrite([id, me, name]() {
        fb::Doc d;
        fb::Err e = fb::docGet(g_sess, "teams/" + id, d);   // fresh roster, so nobody is overwritten
        if (e != fb::OK || !d.exists) return fb::OTHER;
        Team t = teamFromDoc(d);
        t.members.push_back(me);
        t.names.push_back(name);
        return fb::docSet(g_sess, "teams/" + id, fb::fvBody({{"members", fb::fvList(t.members)}, {"names", fb::fvList(t.names)}}),
                          {"members", "names"});
    }, L"Welcome to the team.", L"The server refused: joining a team needs Silver or above.");
}

static void removeFromTeam(const Team& t, const std::string& uid, const std::wstring& done) {
    std::string id = t.id;
    competeWrite([id, uid]() {
        fb::Doc d;
        fb::Err e = fb::docGet(g_sess, "teams/" + id, d);
        if (e != fb::OK || !d.exists) return fb::OTHER;
        Team t = teamFromDoc(d);
        for (size_t i = 0; i < t.members.size(); i++)
            if (t.members[i] == uid) {
                t.members.erase(t.members.begin() + i);
                t.names.erase(t.names.begin() + i);
                break;
            }
        return fb::docSet(g_sess, "teams/" + id, fb::fvBody({{"members", fb::fvList(t.members)}, {"names", fb::fvList(t.names)}}),
                          {"members", "names"});
    }, done, L"The server refused that change.");
}

// ---------------------------------------------------------------- tournament actions

static RectF tourEditRect(int i) {   // 0 name (while setting up a tournament)
    RectF r = socialRight();
    return RectF(r.X + S(24), r.Y + S(92) + i * S(70), r.Width - S(48), S(34));
}

static void createTour() {
    if (!needPlan(3, L"Hosting tournaments")) return;
    Tourney t;
    t.name = narrow(editText(g_edGen[0]));
    while (!t.name.empty() && t.name.back() == ' ') t.name.pop_back();
    if (t.name.size() < 3 || t.name.size() > 40) { socialSay(L"Name the tournament (3 to 40 characters).", true); return; }
    if (TCS[g_tcIndex].minutes == 0) { socialSay(L"Tournament games need a clock. Pick a time control.", true); return; }
    t.host = g_myUid;
    t.hostName = g_myName;
    t.kind = g_tourKind ? "team" : "solo";
    t.status = "open";
    t.rounds = TOUR_ROUNDS[g_tourRounds];
    t.maxPlayers = TOUR_MAX[g_tourMax];
    t.baseMs = TCS[g_tcIndex].minutes * 60000LL;
    t.incMs = TCS[g_tcIndex].inc * 1000LL;
    t.createdAt = fb::nowMs();
    competeWrite([t]() { return fb::docCreate(g_sess, "tournaments", "", tourBody(t)); }, L"Tournament announced. Registration is open.",
                 L"The server refused: hosting tournaments needs the Ruby plan.");
}

static void registerTour(const Tourney& t, bool join) {
    if (join && !needPlan(1, L"Playing in tournaments")) return;
    std::string tag;
    if (join && t.kind == "team") {
        const Team* mine = myTeam();
        if (!mine) { socialSay(L"This is a team battle: join or found a team first (shield icon).", true); return; }
        tag = mine->tag;
    }
    std::string id = t.id, me = g_myUid, name = g_myName;
    competeWrite([id, me, name, tag, join]() {
        fb::Doc d;
        fb::Err e = fb::docGet(g_sess, "tournaments/" + id, d);
        if (e != fb::OK || !d.exists) return fb::OTHER;
        Tourney t = tourFromDoc(d);
        auto it = std::find(t.players.begin(), t.players.end(), me);
        if (join) {
            if (it != t.players.end()) return fb::OK;
            if ((int)t.players.size() >= t.maxPlayers) return fb::CONFLICT;
            t.players.push_back(me);
            t.names.push_back(name);
            t.teams.push_back(tag);
        } else if (it != t.players.end()) {
            size_t i = it - t.players.begin();
            t.players.erase(t.players.begin() + i);
            t.names.erase(t.names.begin() + i);
            t.teams.erase(t.teams.begin() + i);
        }
        return fb::docSet(g_sess, "tournaments/" + id,
                          fb::fvBody({{"players", fb::fvList(t.players)}, {"names", fb::fvList(t.names)}, {"teams", fb::fvList(t.teams)}}),
                          {"players", "names", "teams"});
    }, join ? L"You are registered." : L"You have withdrawn.", L"The server refused: playing in tournaments needs Silver or above.");
}

// Host: begin round 1, or the next round once every result is in; the last round closes the event.
static void advanceTour(const Tourney& t) {
    if (t.host != g_myUid) return;
    if (t.status == "open" && t.players.size() < 2) { socialSay(L"A tournament needs at least two players.", true); return; }
    if (t.status == "running" && !roundComplete(t, g_tourResults)) { socialSay(L"Some games of this round are still being played.", true); return; }
    Tourney next = t;
    std::vector<TourResult> results = g_tourResults;
    if (t.status == "running" && t.round >= t.rounds) {
        next.status = "done";
    } else {
        next.status = "running";
        next.round = t.round + 1;
        std::vector<std::string> made = makePairings(t, results, next.round, (unsigned)(t.createdAt % 100000) + next.round);
        next.pairings.insert(next.pairings.end(), made.begin(), made.end());
    }
    competeWrite([next]() {
        fb::Err e = fb::docSet(g_sess, "tournaments/" + next.id, tourBody(next));
        if (e != fb::OK) return e;
        for (auto& s : next.pairings) {   // byes score a full point straight away
            PairingView p = parsePairing(s);
            if (p.round == next.round && p.black == "BYE")
                fb::docCreate(g_sess, "tournaments/" + next.id + "/results", byeId(p.round, p.white),
                              fb::fvBody({{"white", fb::fvStr(p.white)}, {"black", fb::fvStr("BYE")}, {"result", fb::fvStr("1-0")}}));
        }
        return fb::OK;
    }, next.status == "done" ? L"The tournament is finished." : L"Round " + std::to_wstring(next.round) + L" is under way.",
       L"Only the host can do that.");
}

// Sit down for your game of the current round: open the table, join it, or carry on where it stands.
static void playTourGame(const Tourney& t) {
    for (auto& s : t.pairings) {
        PairingView p = parsePairing(s);
        if (p.round != t.round || (p.white != g_myUid && p.black != g_myUid) || p.black == "BYE") continue;
        std::string gid = pairingGameId(t.id, p.round, p.white, p.black);
        bool white = p.white == g_myUid;
        std::string opp = white ? p.black : p.white, oppName;
        for (size_t i = 0; i < t.players.size(); i++)
            if (t.players[i] == opp) oppName = t.names[i];
        fb::GameDoc fresh;
        fresh.id = gid;
        (white ? fresh.white : fresh.black) = g_myUid;
        (white ? fresh.whiteName : fresh.blackName) = g_myName;
        (white ? fresh.whiteRating : fresh.blackRating) = g_profile.rating;
        fresh.createdBy = g_myUid;
        fresh.status = "open";
        fresh.baseMs = t.baseMs;
        fresh.incMs = t.incMs;
        fresh.whiteMs = fresh.blackMs = t.baseMs;
        fresh.createdAt = fb::nowMs();
        fresh.invite = opp;
        fresh.event = t.id;
        socialSay(L"Finding your table\x2026");
        std::string me = g_myUid, myName = g_myName;
        int rating = g_profile.rating;
        netAsync([gid, fresh, me, myName, rating]() mutable -> std::function<void()> {
            fb::GameDoc d;
            fb::Err e = fb::getGame(g_sess, gid, d);
            int outcome = 0;   // 1 hosting, 2 playing, 3 already finished
            if (e != fb::OK || d.status.empty()) {   // no table yet: open it and wait
                d = fresh;
                e = fb::createGame(g_sess, d);
                outcome = 1;
            } else if (d.status == "open" && d.createdBy != me) {
                if (d.white.empty()) { d.white = me; d.whiteName = myName; d.whiteRating = rating; }
                else { d.black = me; d.blackName = myName; d.blackRating = rating; }
                d.status = "active";
                d.lastMoveAt = fb::nowMs();
                e = fb::updateGame(g_sess, d);
                outcome = 2;
            } else if (d.status == "open") outcome = 1;
            else if (d.status == "active") outcome = 2;
            else outcome = 3;
            if (e == fb::OK) g_latestUpdate[d.id] = d.updateTime;
            return [e, d, outcome]() {
                if (e != fb::OK) { socialSay(L"Couldn't get to your table. Try again.", true); return; }
                if (outcome == 1) {
                    g_hosting = true;
                    g_hostDoc = d;
                    g_lobbyMsg = L"Your tournament table is open. Waiting for your opponent to sit down.";
                    goScreen(SCR_LOBBY);
                } else if (outcome == 2) {
                    startOnlineGame(d);
                    onlineSync(d);
                } else socialSay(L"That game has already been played.");
            };
        });
        return;
    }
    socialSay(L"You have no game to play this round.", true);
}

// Called when one of my online games ends: tournament games file their result.
static void reportTourResult(const fb::GameDoc& g2, int resultWhite) {
    if (g2.event.empty()) return;
    std::string res = resultWhite > 0 ? "1-0" : resultWhite < 0 ? "0-1" : "1/2";
    std::string path = "tournaments/" + g2.event + "/results", id = g2.id;
    std::string body = fb::fvBody({{"white", fb::fvStr(g2.white)}, {"black", fb::fvStr(g2.black)}, {"result", fb::fvStr(res)}});
    netAsync([path, id, body]() -> std::function<void()> {
        fb::docCreate(g_sess, path, id, body);   // the other player may have filed it first: that's fine
        return nullptr;
    });
}

// ---------------------------------------------------------------- rendering

static void renderTeams(Graphics& gr) {
    drawTopBar(gr, L"teams  \x00B7  found one, join one, play team battles together");
    RectF L = socialLeft();
    paperSheet(gr, L);
    sheetHeader(gr, L, L"TEAMS");
    drawText(gr, L"members", RectF(L.X, L.Y + S(8), L.Width - S(18), S(30)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    float rh = S(56), y = L.Y + S(52);
    int maxRows = (int)((L.Y + L.Height - y - S(60)) / rh);
    if (g_teams.empty())
        drawText(gr, !g_teamsLoaded ? L"Fetching\x2026" : L"No teams yet. Found the first one.", RectF(L.X + S(24), y + S(10), L.Width - S(48), S(60)),
                 F_HAND, S(15), C_INK, 0, 0, 0, true);
    const Team* mine = myTeam();
    for (int i = 0; i < (int)g_teams.size() && i < maxRows; i++) {
        const Team& t = g_teams[i];
        RectF r(L.X + S(8), y + i * rh, L.Width - S(16), rh);
        if (t.id == g_teamSel && !g_teamCreating) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
        }
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, r.X, r.Y + rh - 1, r.X + r.Width, r.Y + rh - 1);
        drawIcon(gr, IC_TEAM, RectF(r.X + S(8), r.Y + S(8), rh - S(16), rh - S(16)), C_PRINT);
        drawText(gr, widen(t.name), RectF(r.X + S(60), r.Y + S(4), r.Width - S(150), rh * 0.52f), F_SERIF, S(16), C_PRINT, FontStyleBold, 0, 2);
        drawText(gr, L"[" + widen(t.tag) + L"]  captain " + widen(t.captainName) + (mine && mine->id == t.id ? L"  \x00B7  your team" : L""),
                 RectF(r.X + S(60), r.Y + rh * 0.54f, r.Width - S(120), rh * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
        drawText(gr, std::to_wstring(t.members.size()), RectF(r.X, r.Y, r.Width - S(14), rh), F_MONO, S(15), C_PRINT, FontStyleBold, 2);
        g_buttons.push_back({r, B_TEAMROW + i});
    }
    inkButton(gr, B_TEAM_NEW, RectF(L.X + S(18), L.Y + L.Height - S(50), S(210), S(36)), L"FOUND A TEAM", !mine);
    if (g_plan < 1) {
        drawBadge(gr, 1, RectF(L.X + S(238), L.Y + L.Height - S(44), S(22), S(22)));
        drawText(gr, L"Silver", RectF(L.X + S(266), L.Y + L.Height - S(50), S(100), S(36)), F_HAND, S(13), C_FAINT);
    }

    RectF R = socialRight();
    paperSheet(gr, R, true);
    float ix = R.X + S(24), iw = R.Width - S(48), yy = R.Y + S(18);
    if (g_teamCreating) {
        drawText(gr, L"FOUND A TEAM", RectF(ix, yy, iw, S(30)), F_MONO, S(16), C_PRINT, FontStyleBold);
        fieldBox(gr, teamEditRect(0), L"TEAM NAME");
        fieldBox(gr, teamEditRect(1), L"TAG (2-5 letters)");
        fieldBox(gr, teamEditRect(2), L"MOTTO");
        float by = teamEditRect(2).Y + S(56);
        stampButton(gr, B_TEAM_CREATE, RectF(ix + S(4), by, S(220), S(56)), L"FOUND IT", L"", !g_competeBusy);
        inkButton(gr, B_TEAM_CANCEL, RectF(ix + S(250), by + S(10), S(130), S(36)), L"CANCEL");
        statusLine(gr, RectF(ix, by + S(72), iw, S(60)));
        return;
    }
    const Team* t = selectedTeam();
    if (!t) {
        drawText(gr, L"Pick a team on the left to see its roster.", RectF(ix, yy, iw, S(60)), F_HAND, S(16), C_INK, 0, 0, 0, true);
        statusLine(gr, RectF(ix, yy + S(70), iw, S(60)));
        return;
    }
    drawText(gr, widen(t->name), RectF(ix, yy, iw - S(90), S(40)), F_SERIF, S(28), C_PRINT, FontStyleBold);
    drawText(gr, L"[" + widen(t->tag) + L"]", RectF(ix, yy, iw, S(40)), F_MONO, S(20), C_RED, FontStyleBold, 2);
    yy += S(44);
    if (!t->motto.empty()) {
        drawText(gr, L"\x201C" + widen(t->motto) + L"\x201D", RectF(ix, yy, iw, S(40)), F_HAND, S(14), C_INK, 0, 0, 0, true);
        yy += S(44);
    }
    drawText(gr, L"ROSTER  " + std::to_wstring(t->members.size()) + L" of 20", RectF(ix, yy, iw, S(24)), F_MONO, S(12), C_FAINT);
    yy += S(28);
    bool captain = t->captain == g_myUid, member = mine && mine->id == t->id;
    float rowh = S(30), footer = S(110);
    int maxm = (int)((R.Y + R.Height - yy - footer) / rowh);
    for (int i = 0; i < (int)t->members.size() && i < maxm; i++) {
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, ix, yy + rowh - 1, ix + iw, yy + rowh - 1);
        drawText(gr, widen(t->names[i]) + (t->members[i] == t->captain ? L"  (captain)" : L""), RectF(ix + S(4), yy, iw - S(110), rowh), F_SERIF,
                 S(15), C_PRINT, t->members[i] == g_myUid ? FontStyleBold : FontStyleRegular);
        if (captain && t->members[i] != g_myUid) inkButton(gr, B_TEAM_KICK + i, RectF(ix + iw - S(96), yy + S(3), S(96), rowh - S(6)), L"REMOVE");
        yy += rowh;
    }
    float by = R.Y + R.Height - footer + S(6);
    if (captain) inkButton(gr, B_TEAM_DISBAND, RectF(ix, by, S(200), S(36)), L"DISBAND TEAM", !g_competeBusy);
    else if (member) inkButton(gr, B_TEAM_LEAVE, RectF(ix, by, S(200), S(36)), L"LEAVE TEAM", !g_competeBusy);
    else inkButton(gr, B_TEAM_JOIN, RectF(ix, by, S(200), S(36)), L"JOIN THIS TEAM", !mine && !g_competeBusy);
    statusLine(gr, RectF(ix, by + S(44), iw, S(50)));
}

static std::wstring nameIn(const Tourney& t, const std::string& uid) {
    if (uid == "BYE") return L"bye";
    for (size_t i = 0; i < t.players.size(); i++)
        if (t.players[i] == uid) return widen(t.names[i]) + (t.teams[i].empty() ? L"" : L" [" + widen(t.teams[i]) + L"]");
    return L"?";
}
static std::wstring ptsText(int half) { return std::to_wstring(half / 2) + (half % 2 ? L"\x00BD" : L""); }

static void renderTours(Graphics& gr) {
    drawTopBar(gr, L"tournaments  \x00B7  solo events and team battles");
    RectF L = socialLeft();
    paperSheet(gr, L);
    sheetHeader(gr, L, L"TOURNAMENTS");
    float rh = S(60), y = L.Y + S(52);
    int maxRows = (int)((L.Y + L.Height - y - S(60)) / rh);
    if (g_tours.empty())
        drawText(gr, !g_toursLoaded ? L"Fetching\x2026" : L"Nothing on the calendar. Ruby members can host a tournament.",
                 RectF(L.X + S(24), y + S(10), L.Width - S(48), S(70)), F_HAND, S(15), C_INK, 0, 0, 0, true);
    for (int i = 0; i < (int)g_tours.size() && i < maxRows; i++) {
        const Tourney& t = g_tours[i];
        RectF r(L.X + S(8), y + i * rh, L.Width - S(16), rh);
        if (t.id == g_tourSel && !g_tourCreating) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
        }
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, r.X, r.Y + rh - 1, r.X + r.Width, r.Y + rh - 1);
        drawIcon(gr, t.kind == "team" ? IC_TEAM : IC_TROPHY, RectF(r.X + S(8), r.Y + S(10), rh - S(20), rh - S(20)), C_PRINT);
        drawText(gr, widen(t.name), RectF(r.X + S(60), r.Y + S(4), r.Width - S(170), rh * 0.5f), F_SERIF, S(16), C_PRINT, FontStyleBold, 0, 2);
        drawText(gr, std::wstring(t.kind == "team" ? L"team battle" : L"solo") + L"  \x00B7  " + tcLabel(t.baseMs, t.incMs) + L"  \x00B7  " +
                         std::to_wstring(t.players.size()) + L"/" + std::to_wstring(t.maxPlayers) + L" players",
                 RectF(r.X + S(60), r.Y + rh * 0.52f, r.Width - S(150), rh * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
        std::wstring st = t.status == "open" ? L"registering" : t.status == "running" ? L"round " + std::to_wstring(t.round) : L"finished";
        drawText(gr, st, RectF(r.X, r.Y, r.Width - S(14), rh), F_HAND, S(14), t.status == "running" ? C_RED : C_FAINT, 0, 2, 1);
        g_buttons.push_back({r, B_TOURROW + i});
    }
    inkButton(gr, B_TOUR_NEW, RectF(L.X + S(18), L.Y + L.Height - S(50), S(240), S(36)), L"HOST A TOURNAMENT");
    if (g_plan < 3) {
        drawBadge(gr, 3, RectF(L.X + S(268), L.Y + L.Height - S(44), S(22), S(22)));
        drawText(gr, L"Ruby", RectF(L.X + S(296), L.Y + L.Height - S(50), S(100), S(36)), F_HAND, S(13), C_FAINT);
    }

    RectF R = socialRight();
    paperSheet(gr, R, true);
    float ix = R.X + S(24), iw = R.Width - S(48), yy = R.Y + S(18);
    if (g_tourCreating) {
        drawText(gr, L"HOST A TOURNAMENT", RectF(ix, yy, iw, S(30)), F_MONO, S(16), C_PRINT, FontStyleBold);
        fieldBox(gr, tourEditRect(0), L"TOURNAMENT NAME");
        float cy = tourEditRect(0).Y + S(54), bs = S(30);
        auto chooser = [&](const wchar_t* label, int prev, int next, const std::wstring& value) {
            drawText(gr, label, RectF(ix, cy, S(100), S(34)), F_MONO, S(12), C_FAINT, 0, 0, 1);
            inkButton(gr, prev, RectF(ix + S(100), cy + S(2), bs, bs), L"\x2039");
            drawText(gr, value, RectF(ix + S(100) + bs, cy, S(150), S(34)), F_SERIF, S(16), C_INK, FontStyleRegular, 1, 1);
            inkButton(gr, next, RectF(ix + S(250) + bs, cy + S(2), bs, bs), L"\x203A");
            cy += S(44);
        };
        chooser(L"FORMAT", B_TOUR_KIND, B_TOUR_KIND, g_tourKind ? L"team battle" : L"solo");
        chooser(L"ROUNDS", B_TOUR_ROUNDS_DN, B_TOUR_ROUNDS_UP, std::to_wstring(TOUR_ROUNDS[g_tourRounds]));
        chooser(L"PLAYERS", B_TOUR_MAX_DN, B_TOUR_MAX_UP, L"up to " + std::to_wstring(TOUR_MAX[g_tourMax]));
        tcChooser(gr, ix, cy, iw);
        cy += S(54);
        stampButton(gr, B_TOUR_CREATE, RectF(ix + S(4), cy, S(240), S(56)), L"ANNOUNCE IT", L"", !g_competeBusy);
        inkButton(gr, B_TOUR_CANCEL, RectF(ix + S(270), cy + S(10), S(130), S(36)), L"CANCEL");
        statusLine(gr, RectF(ix, cy + S(72), iw, S(60)));
        return;
    }
    const Tourney* t = selectedTour();
    if (!t) {
        drawText(gr, L"Pick a tournament on the left.", RectF(ix, yy, iw, S(60)), F_HAND, S(16), C_INK, 0, 0, 0, true);
        statusLine(gr, RectF(ix, yy + S(70), iw, S(60)));
        return;
    }
    drawText(gr, widen(t->name), RectF(ix, yy, iw, S(38)), F_SERIF, S(26), C_PRINT, FontStyleBold);
    yy += S(40);
    drawText(gr, std::wstring(t->kind == "team" ? L"Team battle" : L"Solo event") + L", " + std::to_wstring(t->rounds) + L" rounds, " +
                     tcLabel(t->baseMs, t->incMs) + L", hosted by " + widen(t->hostName),
             RectF(ix, yy, iw, S(24)), F_SERIF, S(13), C_FAINT, FontStyleItalic);
    yy += S(32);
    bool host = t->host == g_myUid, in = std::find(t->players.begin(), t->players.end(), g_myUid) != t->players.end();
    float footer = S(104), bottom = R.Y + R.Height - footer;

    if (t->status == "open") {
        drawText(gr, L"REGISTERED  " + std::to_wstring(t->players.size()) + L" of " + std::to_wstring(t->maxPlayers), RectF(ix, yy, iw, S(24)),
                 F_MONO, S(12), C_FAINT);
        yy += S(28);
        float cw = iw / 2;
        for (size_t i = 0; i < t->players.size() && yy + S(24) * (float)(i / 2) < bottom - S(24); i++)
            drawText(gr, nameIn(*t, t->players[i]), RectF(ix + (i % 2) * cw, yy + S(24) * (float)(i / 2), cw - S(8), S(24)), F_SERIF, S(14),
                     C_PRINT, t->players[i] == g_myUid ? FontStyleBold : FontStyleRegular);
    } else {
        // standings on the left, this round's games on the right
        std::map<std::string, int> pts = tourPoints(*t, g_tourResults);
        std::vector<std::pair<int, std::string>> table;
        for (auto& p : t->players) table.push_back({pts[p], p});
        std::stable_sort(table.begin(), table.end(), [](auto& a, auto& b) { return a.first > b.first; });
        float cw = iw * 0.46f, sx = ix, px = ix + iw * 0.5f, pw = iw * 0.5f, top = yy;
        drawText(gr, L"STANDINGS", RectF(sx, yy, cw, S(22)), F_MONO, S(12), C_FAINT);
        yy += S(24);
        for (size_t i = 0; i < table.size() && yy < bottom - S(22); i++) {
            drawText(gr, std::to_wstring(i + 1) + L".  " + nameIn(*t, table[i].second), RectF(sx, yy, cw - S(44), S(22)), F_SERIF, S(14),
                     C_PRINT, table[i].second == g_myUid ? FontStyleBold : FontStyleRegular);
            drawText(gr, ptsText(table[i].first), RectF(sx, yy, cw, S(22)), F_MONO, S(14), C_PRINT, FontStyleBold, 2);
            yy += S(22);
        }
        if (t->kind == "team" && yy < bottom - S(60)) {
            std::map<std::string, std::vector<int>> byTeam;
            for (size_t i = 0; i < t->players.size(); i++)
                if (!t->teams[i].empty()) byTeam[t->teams[i]].push_back(pts[t->players[i]]);
            std::vector<std::pair<int, std::string>> teamTable;
            for (auto& kv : byTeam) {
                std::sort(kv.second.rbegin(), kv.second.rend());
                int sum = 0;
                for (size_t i = 0; i < kv.second.size() && i < 4; i++) sum += kv.second[i];   // best four count
                teamTable.push_back({sum, kv.first});
            }
            std::sort(teamTable.rbegin(), teamTable.rend());
            yy += S(10);
            drawText(gr, L"TEAMS (best four scores)", RectF(sx, yy, cw, S(22)), F_MONO, S(12), C_FAINT);
            yy += S(24);
            for (size_t i = 0; i < teamTable.size() && yy < bottom - S(22); i++) {
                drawText(gr, std::to_wstring(i + 1) + L".  [" + widen(teamTable[i].second) + L"]", RectF(sx, yy, cw - S(44), S(22)), F_SERIF, S(14), C_PRINT);
                drawText(gr, ptsText(teamTable[i].first), RectF(sx, yy, cw, S(22)), F_MONO, S(14), C_PRINT, FontStyleBold, 2);
                yy += S(22);
            }
        }
        float py = top;
        drawText(gr, t->status == "done" ? L"FINAL ROUND" : L"ROUND " + std::to_wstring(t->round) + L" OF " + std::to_wstring(t->rounds),
                 RectF(px, py, pw, S(22)), F_MONO, S(12), C_FAINT);
        py += S(24);
        for (auto& s : t->pairings) {
            PairingView p = parsePairing(s);
            if (p.round != t->round || py > bottom - S(40)) continue;
            const TourResult* res = resultFor(*t, p, g_tourResults);
            bool mine2 = p.white == g_myUid || p.black == g_myUid;
            drawText(gr, nameIn(*t, p.white) + L"  v  " + nameIn(*t, p.black), RectF(px, py, pw - S(54), S(22)), F_SERIF, S(13), C_PRINT,
                     mine2 ? FontStyleBold : FontStyleRegular);
            std::wstring rs = !res ? L"\x2026" : res->result == "1/2" ? L"\x00BD-\x00BD" : widen(res->result);
            drawText(gr, rs, RectF(px, py, pw, S(22)), F_HAND, S(13), res ? C_RED : C_FAINT, 0, 2, 1);
            py += S(22);
        }
    }

    float by = bottom + S(8);
    float bx = ix;
    auto btn = [&](int id, const wchar_t* label, float w2, bool enabled = true) {
        inkButton(gr, id, RectF(bx, by, w2, S(36)), label, enabled && !g_competeBusy);
        bx += w2 + S(10);
    };
    if (t->status == "open") {
        if (in) btn(B_TOUR_WITHDRAW, L"WITHDRAW", S(150));
        else btn(B_TOUR_REGISTER, L"REGISTER", S(150), (int)t->players.size() < t->maxPlayers);
        if (host) { btn(B_TOUR_ADVANCE, L"START ROUND 1", S(190), t->players.size() >= 2); btn(B_TOUR_DELETE, L"CANCEL EVENT", S(170)); }
    } else if (t->status == "running") {
        bool myGame = false;
        for (auto& s : t->pairings) {
            PairingView p = parsePairing(s);
            if (p.round == t->round && (p.white == g_myUid || p.black == g_myUid) && p.black != "BYE" && !resultFor(*t, p, g_tourResults)) myGame = true;
        }
        if (in) btn(B_TOUR_PLAY, L"PLAY MY GAME", S(190), myGame);
        if (host) btn(B_TOUR_ADVANCE, t->round >= t->rounds ? L"FINISH EVENT" : L"START NEXT ROUND", S(220), roundComplete(*t, g_tourResults));
    }
    statusLine(gr, RectF(ix, by + S(42), iw, S(48)));
}

static bool onCompeteButton(int id) {
    if (id >= B_TEAMROW && id < B_TEAMROW + 300) {
        if (id - B_TEAMROW < (int)g_teams.size()) g_teamSel = g_teams[id - B_TEAMROW].id;
        g_teamCreating = false;
        g_socialMsg.clear();
        showLoginEdits(false);
        invalidate();
        return true;
    }
    if (id >= B_TOURROW && id < B_TOURROW + 200) {
        if (id - B_TOURROW < (int)g_tours.size()) { g_tourSel = g_tours[id - B_TOURROW].id; g_tourResults.clear(); loadTourResults(g_tourSel); }
        g_tourCreating = false;
        g_socialMsg.clear();
        showLoginEdits(false);
        invalidate();
        return true;
    }
    const Team* tm = selectedTeam();
    const Tourney* tr = selectedTour();
    if (id >= B_TEAM_KICK && id < B_TEAM_KICK + 20) {
        int i = id - B_TEAM_KICK;
        if (tm && tm->captain == g_myUid && i < (int)tm->members.size()) removeFromTeam(*tm, tm->members[i], L"Removed from the roster.");
        return true;
    }
    switch (id) {
        case B_TEAM_NEW:
            if (needPlan(1, L"Founding a team")) { g_teamCreating = true; g_socialMsg.clear(); showLoginEdits(false); }
            invalidate();
            return true;
        case B_TEAM_CANCEL: g_teamCreating = false; showLoginEdits(false); invalidate(); return true;
        case B_TEAM_CREATE: createTeam(); return true;
        case B_TEAM_JOIN: if (tm) joinTeam(*tm); return true;
        case B_TEAM_LEAVE: if (tm) removeFromTeam(*tm, g_myUid, L"You have left the team."); return true;
        case B_TEAM_DISBAND:
            if (tm && tm->captain == g_myUid) {
                std::string tid = tm->id;
                competeWrite([tid]() { return fb::docDelete(g_sess, "teams/" + tid); }, L"The team is disbanded.", L"Only the captain can disband the team.");
            }
            return true;
        case B_TOUR_NEW:
            if (needPlan(3, L"Hosting tournaments")) { g_tourCreating = true; g_socialMsg.clear(); showLoginEdits(false); }
            invalidate();
            return true;
        case B_TOUR_CANCEL: g_tourCreating = false; showLoginEdits(false); invalidate(); return true;
        case B_TOUR_KIND: g_tourKind ^= 1; invalidate(); return true;
        case B_TOUR_ROUNDS_DN: g_tourRounds = (g_tourRounds + 2) % 3; invalidate(); return true;
        case B_TOUR_ROUNDS_UP: g_tourRounds = (g_tourRounds + 1) % 3; invalidate(); return true;
        case B_TOUR_MAX_DN: g_tourMax = (g_tourMax + 3) % 4; invalidate(); return true;
        case B_TOUR_MAX_UP: g_tourMax = (g_tourMax + 1) % 4; invalidate(); return true;
        case B_TOUR_CREATE: createTour(); return true;
        case B_TOUR_REGISTER: if (tr) registerTour(*tr, true); return true;
        case B_TOUR_WITHDRAW: if (tr) registerTour(*tr, false); return true;
        case B_TOUR_ADVANCE: if (tr) advanceTour(*tr); return true;
        case B_TOUR_PLAY: if (tr) playTourGame(*tr); return true;
        case B_TOUR_DELETE:
            if (tr && tr->host == g_myUid && tr->status == "open") {
                std::string tid = tr->id;
                competeWrite([tid]() { return fb::docDelete(g_sess, "tournaments/" + tid); }, L"The event is cancelled.", L"Only the host can cancel it.");
            }
            return true;
    }
    return false;
}
