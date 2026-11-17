// The club brain, kept on the server instead of on one computer.
//
// Firestore holds the weights in `brain/w0..w3` (the network is 824 KB, more than one document can
// take) and a note in `brain/meta` saying which version they are. A member signs in, pulls the
// newest brain, plays, trains on that game as before, and pushes the result back. The push is one
// all-or-nothing write guarded by the version it started from, so two members finishing a game at
// the same moment can never blend half of one brain into the other: the second one is refused and
// simply pulls the first one's work instead.
//
// Training itself still runs on the player's own machine, because Firestore only stores data and
// nothing here needs a server that is always awake. What lives on the server is the engine.

static const int BRAIN_CHUNKS = 4;
static const int BRAIN_CHUNK_FLOATS = (Net::SIZE + BRAIN_CHUNKS - 1) / BRAIN_CHUNKS;

static int g_brainVersion = 0;          // the version this computer holds
static int g_brainServerGames = 0;      // games the club brain has studied, over all members
static std::string g_brainMetaTime;     // when brain/meta last changed, as the server saw it
static bool g_brainBusy = false, g_brainWanted = false;
static std::wstring g_brainMsg;
static int g_brainTicks = 0;

static bool brainCloudOn() { return g_brainShare && !g_guest && !g_offline && !g_myUid.empty(); }

static void saveCloudState() {
    FILE* f = _wfopen((g_data + L"\\cloud.txt").c_str(), L"w");
    if (!f) return;
    fprintf(f, "version %d\ngames %d\nmeta %s\n", g_brainVersion, g_brainServerGames, g_brainMetaTime.c_str());
    fclose(f);
}

static void loadCloudState() {
    g_brainVersion = 0;
    g_brainServerGames = 0;
    g_brainMetaTime.clear();
    FILE* f = _wfopen((g_data + L"\\cloud.txt").c_str(), L"r");
    if (!f) return;
    char key[32], value[128];
    while (fscanf(f, "%31s %127s", key, value) == 2) {
        if (!strcmp(key, "version")) g_brainVersion = atoi(value);
        else if (!strcmp(key, "games")) g_brainServerGames = atoi(value);
        else if (!strcmp(key, "meta")) g_brainMetaTime = value;
    }
    fclose(f);
}

// Fetch the club brain if the server has a newer one than we hold.
static void cloudBrainPull(bool force) {
    if (!brainCloudOn() || g_brainBusy || g_brain.busy()) return;
    g_brainBusy = true;
    int have = force ? -1 : g_brainVersion;
    netAsync([have]() -> std::function<void()> {
        struct Result {
            fb::Err err = fb::OTHER;
            bool fresh = false;        // the server has nothing yet
            bool newer = false;
            int version = 0, games = 0;
            std::string metaTime;
            std::vector<float> weights;
        } r;
        fb::Doc meta;
        r.err = fb::docGet(g_sess, "brain/meta", meta);
        if (r.err != fb::OK) return [r]() {
            g_brainBusy = false;
            if (r.err == fb::AUTH) g_brainMsg = L"The club brain is not shared on this server yet.";
            invalidate();
        };
        if (!meta.exists) {
            r.fresh = true;
            return [r]() { g_brainBusy = false; invalidate(); };
        }
        r.version = (int)meta.num("version");
        r.games = (int)meta.num("games");
        r.metaTime = meta.updateTime;
        if (r.version <= have) return [r]() {   // ours is already up to date
            g_brainBusy = false;
            g_brainServerGames = r.games;
            g_brainMetaTime = r.metaTime;
            saveCloudState();
            invalidate();
        };
        r.weights.resize(Net::SIZE);
        bool ok = true;
        for (int i = 0; i < BRAIN_CHUNKS && ok; i++) {
            fb::Doc chunk;
            if (fb::docGet(g_sess, "brain/w" + std::to_string(i), chunk) != fb::OK || !chunk.exists) { ok = false; break; }
            std::vector<unsigned char> raw = chunk.bytes("d");
            int from = i * BRAIN_CHUNK_FLOATS, count = min(BRAIN_CHUNK_FLOATS, Net::SIZE - from);
            if ((int)raw.size() != count * (int)sizeof(float)) { ok = false; break; }
            memcpy(&r.weights[from], raw.data(), raw.size());
        }
        for (size_t i = 0; i < r.weights.size() && ok; i++)
            if (!std::isfinite(r.weights[i])) ok = false;   // never adopt a damaged brain
        r.newer = ok;
        return [r]() {
            g_brainBusy = false;
            if (!r.newer) {
                g_brainMsg = L"The club brain on the server looks damaged; keeping the one here.";
                invalidate();
                return;
            }
            Net net;
            net.P = r.weights;
            net.blank = false;
            for (int j = 0; j <= Net::HID; j++)
                if (net.P[Net::OFF_OUT + j] != 0.f) net.blank = false;
            g_brain.adopt(net);
            g_brainVersion = r.version;
            g_brainServerGames = r.games;
            g_brainMetaTime = r.metaTime;
            saveCloudState();
            g_brainMsg = L"Club brain version " + std::to_wstring(r.version) + L" downloaded.";
            invalidate();
        };
    });
}

// Send what this computer just learned back to the club.
static void cloudBrainPush() {
    if (!brainCloudOn() || g_brainBusy || g_brain.busy()) return;
    g_brainBusy = true;
    auto net = g_brain.net();                      // a snapshot; the trainer has finished with it
    std::vector<float> weights = net->P;
    int version = g_brainVersion + 1, games = g_brainServerGames + 1;
    float loss = g_brain.lastLoss();
    std::string guard = g_brainMetaTime.empty() ? std::string("absent") : g_brainMetaTime;
    std::string me = g_myUid;
    netAsync([weights, version, games, loss, guard, me]() -> std::function<void()> {
        std::vector<fb::Write> writes;
        for (int i = 0; i < BRAIN_CHUNKS; i++) {
            int from = i * BRAIN_CHUNK_FLOATS, count = min(BRAIN_CHUNK_FLOATS, Net::SIZE - from);
            fb::Write w;
            w.path = "brain/w" + std::to_string(i);
            w.body = fb::fvBody({{"d", fb::fvBytes(&weights[from], count * sizeof(float))}});
            w.mask = {"d"};
            writes.push_back(w);
        }
        fb::Write meta;
        meta.path = "brain/meta";
        meta.body = fb::fvBody({{"version", fb::fvInt(version)}, {"games", fb::fvInt(games)}, {"inputs", fb::fvInt(Net::NIN)},
                                {"hidden", fb::fvInt(Net::HID)}, {"layers", fb::fvInt(Net::LAYERS)},
                                {"loss", fb::fvInt((long long)(loss * 10000))}, {"by", fb::fvStr(me)},
                                {"updated", fb::fvInt(fb::nowMs())}});
        meta.mask = {"version", "games", "inputs", "hidden", "layers", "loss", "by", "updated"};
        meta.ifUpdateTime = guard;
        writes.push_back(meta);
        fb::Err e = fb::commitWrites(g_sess, writes);
        std::string newTime;
        if (e == fb::OK) {   // read back when the server says it changed, to guard the next push
            fb::Doc after;
            if (fb::docGet(g_sess, "brain/meta", after) == fb::OK) newTime = after.updateTime;
        }
        return [e, version, games, newTime]() {
            g_brainBusy = false;
            if (e == fb::OK) {
                g_brainVersion = version;
                g_brainServerGames = games;
                g_brainMetaTime = newTime;
                saveCloudState();
                g_brainMsg = L"Your game went into club brain version " + std::to_wstring(version) + L".";
            } else if (e == fb::CONFLICT) {
                // another member got there first; take theirs rather than overwrite it
                g_brainMsg = L"Another member trained the club brain first; fetching theirs.";
                g_brainWanted = true;
            } else if (e == fb::AUTH) {
                g_brainMsg = L"The server would not take the brain (check the rules are deployed).";
            } else {
                g_brainMsg = L"Couldn't reach the club brain; your training stays on this computer.";
            }
            invalidate();
        };
    });
}

// The learning thread has finished with this game.
static void cloudBrainAfterLearn() {
    if (!brainCloudOn()) return;
    if (g_brainVersion == 0 && g_brainMetaTime.empty()) g_brainWanted = true;   // never synced: look first
    else cloudBrainPush();
}

// From the network tick, about once a second.
static void cloudBrainTick() {
    if (!brainCloudOn() || g_brain.busy()) return;
    if (g_brainWanted && !g_brainBusy) {
        g_brainWanted = false;
        cloudBrainPull(true);
        return;
    }
    if (++g_brainTicks % 600 == 0) cloudBrainPull(false);   // and a quiet look every ten minutes
}
