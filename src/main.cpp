// Chezz: Win32 + GDI+ front end, styled as a chess club: felt table, wooden board, chess clock,
// paper scoresheet, a ladder of member cards, a membership desk (Firebase sign-in) and an online
// room. Redraws only on input, engine/network replies and clock ticks, to keep CPU/GPU load low.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <commctrl.h>
#include <algorithm>
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>
#include <mmsystem.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "brain.h"
#include "chess.h"
#include "eval.h"
#include "firebase.h"
#include "levels.h"
#include "search.h"
#include "sounds.h"
#include "audio.h"
#include <cstdarg>
#include <ctime>

using namespace Gdiplus;

#define WM_ENGINE (WM_APP + 1)
#define WM_NET (WM_APP + 2)
static const int TIMER_LEARN = 1, TIMER_CLOCK = 2, TIMER_ANIM = 3, TIMER_NET = 4;
typedef std::chrono::steady_clock Clock;

// ---------------------------------------------------------------- state

enum Screen { SCR_LOGIN, SCR_MENU, SCR_LOBBY, SCR_GAME, SCR_SETTINGS, SCR_PROFILE, SCR_PEOPLE, SCR_CHAT,
              SCR_ENGINES, SCR_PUZZLES, SCR_TEAMS, SCR_TOURNEYS, SCR_PLANS };
enum BtnId {
    B_NONE, B_PLAY, B_WHITE, B_RANDOM, B_BLACK, B_LEARN, B_UNDO, B_FLIP, B_RESIGN, B_MENU, B_REMATCH, B_NEXT,
    B_TC_PREV, B_TC_NEXT, B_THOUGHTS, B_ONLINE, B_SIGNOUT, B_MODE_IN, B_MODE_UP, B_SUBMIT, B_LOBBY_BACK, B_HOST,
    B_UNHOST, B_GUEST, B_JOINCLUB, B_NAV_LADDER, B_NAV_PEOPLE, B_NAV_CHAT, B_NAV_PROFILE, B_NAV_SETTINGS, B_SET_RESET,
    B_SAVEPROFILE, B_SEARCH, B_SEND, B_ACT_MSG, B_ACT_ADD, B_ACT_REMOVE, B_ACT_ACCEPT, B_ACT_DECLINE, B_ACT_BLOCK,
    B_ACT_UNBLOCK,
    B_NAV_ENGINES, B_NAV_PUZZLES, B_NAV_TOURS, B_NAV_TEAMS, B_NAV_PLANS, B_NAV_THEME, B_DEVCHAT, B_UPGRADE, B_CONFIRM_YES,
    B_PLAN_GO, B_PLAN_CANCEL, B_PLAN_CHECK, B_PLAN_STOP, B_PORTAL, B_PLAN_PICK = 2340,
    B_CONFIRM_NO, B_ENG_NEW, B_ENG_EDIT, B_ENG_SAVE, B_ENG_CANCEL, B_ENG_DELETE, B_ENG_PLAY, B_PUZ_MORE, B_PUZ_COMPOSE,
    B_PUZ_CANCEL, B_PUZ_USEPOS, B_PUZ_PUBLISH, B_PUZ_HINT, B_PUZ_SOLVE, B_PUZ_BACK, B_PUZ_NEXT, B_PUZ_AGAIN, B_TEAM_NEW,
    B_TEAM_CANCEL, B_TEAM_CREATE, B_TEAM_JOIN, B_TEAM_LEAVE, B_TEAM_DISBAND, B_TOUR_NEW, B_TOUR_CANCEL, B_TOUR_KIND,
    B_TOUR_ROUNDS_DN, B_TOUR_ROUNDS_UP, B_TOUR_MAX_DN, B_TOUR_MAX_UP, B_TOUR_CREATE, B_TOUR_REGISTER, B_TOUR_WITHDRAW,
    B_TOUR_ADVANCE, B_TOUR_PLAY, B_TOUR_DELETE, B_QUICK, B_INVITE_PREV, B_INVITE_NEXT, B_FILT_TIER_DN, B_FILT_TIER_UP,
    B_FILT_STATUS, B_UPLOAD, B_PRESETPIC, B_BANNER_DN, B_BANNER_UP, B_PIECEDIR, B_PIECERESET,
    // ranges: one id per row / swatch / list entry
    B_JOIN = 1000, B_SET_PREV = 2000, B_SET_NEXT = 2100, B_SWATCH_ACCENT = 2200, B_SWATCH_BOARD = 2300, B_AVATAR = 2400,
    B_TAB = 2500, B_PERSON = 2600, B_CHATROW = 2900, B_PUZROW = 3500, B_PUZCROW = 3600, B_TEAMROW = 4000, B_TEAM_KICK = 4400,
    B_TOURROW = 4500, B_ENG_DEC = 4800, B_ENG_INC = 4820, B_ENGROW = 5000
};
struct Button { RectF r; int id; };
struct Record { int w = 0, d = 0, l = 0; };

struct TimeControl { const wchar_t* label; int minutes, inc; };
static const TimeControl TCS[] = {{L"untimed", 0, 0}, {L"1 + 0", 1, 0},  {L"3 + 0", 3, 0},   {L"3 + 2", 3, 2},
                                  {L"5 + 0", 5, 0},   {L"5 + 3", 5, 3},  {L"10 + 0", 10, 0}, {L"10 + 5", 10, 5},
                                  {L"15 + 10", 15, 10}, {L"30 + 0", 30, 0}};
static const int TC_COUNT = sizeof(TCS) / sizeof(TCS[0]);

static HWND g_hwnd;
static int g_W = 1180, g_H = 800;
static float g_ui = 1.f;
static std::wstring g_base, g_assets, g_data;
static Brain g_brain;
static Screen g_screen = SCR_LOGIN;
static int g_sel = 0, g_colorPref = 1, g_tcIndex = 0;
static bool g_learn = true, g_showThoughts = true;
static bool g_brainShare = true;
static float g_menuScroll = 0, g_scrollTarget = 0;
static RectF g_listRect;
static std::vector<Button> g_buttons;
static int g_hoverBtn = 0;
static float g_mouseX = -1, g_mouseY = -1;
static std::vector<Record> g_rec(150);
static std::mt19937 g_rng;

// Three layers: bg (table + board wood, rebuilt on resize/flip/screen change), scene (everything
// static between events) and the final frame (scene + moving pieces + the bot's thoughts).
struct Layer { HDC dc = 0; HBITMAP bmp = 0, old = 0; int w = 0, h = 0; };
static Layer g_bgL, g_sceneL, g_memL;
// The ladder list is drawn once into its own tall strip; scrolling just copies a slice of it.
static Layer g_listL;
static bool g_listDirty = true;
static int g_listDrawnSel = -1;
static std::vector<int> g_ladder;                    // level indices that pass the filters, in order
static int g_filterTier = -1, g_filterStatus = 0;    // tier 0-14 or -1 = all; 0 all, 1 not beaten yet, 2 beaten
static std::string g_filterText;
static long long g_bgKey = -1;
static bool g_sceneDirty = true;

struct Anim {
    int piece, toSq;      // toSq: square hidden in the scene while gliding (-1 = none)
    RectF from, to;
    Clock::time_point t0;
    float dur;
    bool fade;            // captured piece fading out in place
    int sound;            // played on landing, -1 = none
};
static std::vector<Anim> g_anims;

// Planning marks drawn with the right mouse button: an arrow between two squares, or a ring when
// pressed and released on the same one. There is no limit; a left click or the opponent's move
// wipes them. Modifier keys pick the colour.
struct Mark { int from, to, colour; };
static std::vector<Mark> g_marks;
static int g_rightFrom = -1, g_rightTo = -1, g_markColour = 0;
static const Color MARK_COLOR[4] = {Color(176, 86, 158, 70), Color(176, 190, 56, 48), Color(176, 58, 112, 190),
                                    Color(176, 222, 142, 38)};
static Clock::time_point g_lastThinkPaint;

struct Game {
    Position pos;
    int human = WHITE, level = 0, gameId = 0;
    bool flipped = false, over = false;
    std::vector<std::wstring> sans;
    std::vector<std::string> ucis;
    std::vector<Sample> samples;
    Move last = 0;
    std::wstring resultText, reasonText, ratingText;
    int resultForHuman = 0, resultWhite = 0;
    int selected = -1;
    std::vector<Move> targets;
    bool dragging = false;
    // a move queued while the bot thinks (offline games only); -1 = none
    int preFrom = -1, preTo = -1;
    std::vector<Move> preTargets;   // where the held piece could go, as a hint
    bool promoOpen = false;
    int promoFrom = -1, promoTo = -1;
    bool timed = false;
    long long baseMs = 0, incMs = 0;
    long long clockMs[2] = {0, 0};   // remaining when timed, used otherwise
    Clock::time_point turnStart;
    bool online = false;
    fb::GameDoc doc;
    bool puzzle = false;                  // solving a puzzle: no clock, no engine thread, no result
    int puzMate = 0, puzState = 0, puzHint = -1, puzTries = 0;   // state: 0 solving, 1 solved, 2 solution shown
    std::string puzId;
    std::wstring puzTitle, puzBy, puzMsg;
    std::vector<PatternSample> myMoves;   // your moves this game, for the habit model
    Move predicted = 0;                   // what the habit model expects you to play next
    int predHits = 0, predTotal = 0;
} g;

static Engine* g_engine;
static std::thread g_engThread;
static std::atomic<bool> g_engStop{false}, g_thinking{false};
static std::atomic<int> g_pendingScore{0};
static std::atomic<bool> g_pendingKnown{false};
static ThinkInfo g_think;
static RectF g_thinkLine;
static std::mutex g_noteMx;
static std::wstring g_expectNote;   // "expects you to answer Nf6", written by the engine thread

// account + online (g_sess belongs to the network thread; the UI keeps copies)
static fb::Session g_sess;
static std::string g_myUid, g_myName;
static fb::Profile g_profile;
static bool g_offline = false;
static bool g_guest = false;   // anonymous visitor: online rooms only, never the AI ladder
static int g_loginMode = 0;   // 0 sign in, 1 join
static std::wstring g_loginMsg;
static bool g_loginBusy = false, g_loginErr = false;
static HWND g_edEmail, g_edPass, g_edName;
static HFONT g_edFont;
static HBRUSH g_fieldBrush;
static HWND g_edSearch, g_edChat, g_edPName, g_edBio;
// friends, inbox, blocks and conversations (filled by refreshSocial in social.inl)
static std::vector<fb::Person> g_friends, g_requests, g_blocked;
static std::vector<fb::Profile> g_found;
static std::vector<fb::Chat> g_chats;
static std::vector<fb::Message> g_msgs;
static std::map<std::string, long long> g_chatSeen;   // chat id -> newest message time already read
static fb::Person g_chatWith;                          // open conversation (uid empty = none)
static int g_unread = 0;
static std::wstring g_socialMsg;                       // one status line shared by the members' screens
static bool g_socialMsgErr = false;
static int g_plan = 0;                                 // 0 Bronze, 1 Silver, 2 Gold, 3 Ruby (from plans/{uid})
static bool g_founder = false;                         // one of the first five accounts: Ruby for life
static const wchar_t* PLAN_NAME[4] = {L"Bronze", L"Silver", L"Gold", L"Ruby"};
static HWND g_edGen[4];                                // general text boxes: filter, engine/team/tournament/puzzle forms
static bool g_customOn = false;                        // playing a member-built engine instead of a ladder bot
static Level g_customLevel;
static std::wstring g_customEvent;
static int g_confirm = 0;                              // 1 "resign?", 2 "leave and resign?"
static int g_inviteIdx = 0;                            // hosting for: 0 anyone, n = the n-th friend
static std::wstring g_pieceDir;                        // Gold: folder with the member's own piece images
static std::vector<fb::GameDoc> g_openTables;
static std::wstring g_lobbyMsg;
static bool g_hosting = false, g_pollBusy = false, g_lobbyLoaded = false;
static fb::GameDoc g_hostDoc;
static int g_netTick = 0, g_netWrites = 0;
static bool g_applyingRemote = false;   // replaying the opponent's moves: the server already has them
static std::map<std::string, std::string> g_latestUpdate;   // network thread: newest updateTime per game

// network worker: jobs run in order on one thread; each may hand back a UI-thread continuation
static std::mutex g_netMx;
static std::condition_variable g_netCv;
static std::deque<std::function<std::function<void()>()>> g_netQ;
static std::thread g_netThread;
static bool g_netQuit = false;
static std::atomic<bool> g_netIdle{true};

// assets
static Bitmap* g_pieceSrc[16];
static std::map<long long, Bitmap*> g_pieceCache;
static Bitmap *g_boardImg, *g_lightImg, *g_darkImg, *g_bgImg;
static Bitmap *g_felt, *g_paper, *g_walnut, *g_maple, *g_darkwood;
static std::map<int, Bitmap*> g_avatars;
static std::map<int, Font*> g_fonts;

#include "theme.inl"

enum { F_SERIF, F_HAND, F_MONO };
static const int TIER_PIECE[15] = {PAWN, PAWN, PAWN, KNIGHT, KNIGHT, KNIGHT, BISHOP, BISHOP, BISHOP,
                                   ROOK, ROOK, ROOK, QUEEN, QUEEN, KING};

static inline float S(float v) { return v * g_ui; }
// Layout: a navigation sidebar on the left, the page heading on top, the page itself in the rest.
static float sidebarW() { return S(198); }
static float contentX() { return sidebarW() + S(24); }
static float contentY() { return S(62); }
static float contentW() { return g_W - contentX() - S(24); }
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static const Level& curLevel() { return g_customOn ? g_customLevel : allLevels()[g.level]; }
static int levelForRating(int r) {
    int best = 0;
    for (int i = 1; i < 150; i++)
        if (std::abs(allLevels()[i].rating - r) < std::abs(allLevels()[best].rating - r)) best = i;
    return best;
}
static std::wstring oppName() {
    if (!g.online) return widen(curLevel().name);
    return widen(g.human == WHITE ? g.doc.blackName : g.doc.whiteName);
}
static int oppRating() {
    if (!g.online) return curLevel().rating;
    return g.human == WHITE ? g.doc.blackRating : g.doc.whiteRating;
}
static int oppPortrait() { return g.online ? levelForRating(oppRating()) : g.level; }
static std::wstring tcLabel(long long baseMs, long long incMs) {
    if (baseMs <= 0) return L"untimed";
    return std::to_wstring(baseMs / 60000) + L" + " + std::to_wstring(incMs / 1000);
}

// ---------------------------------------------------------------- network worker

static void netAsync(std::function<std::function<void()>()> job) {
    {
        std::lock_guard<std::mutex> lk(g_netMx);
        g_netQ.push_back(std::move(job));
    }
    g_netCv.notify_one();
}

static void netLoop() {
    for (;;) {
        std::function<std::function<void()>()> job;
        {
            std::unique_lock<std::mutex> lk(g_netMx);
            g_netCv.wait(lk, [] { return g_netQuit || !g_netQ.empty(); });
            if (g_netQ.empty()) return;
            job = std::move(g_netQ.front());
            g_netQ.pop_front();
            g_netIdle = false;
        }
        std::function<void()> ui = job();
        {
            std::lock_guard<std::mutex> lk(g_netMx);
            g_netIdle = g_netQ.empty();
        }
        if (ui && g_hwnd) PostMessageW(g_hwnd, WM_NET, 0, (LPARAM) new std::function<void()>(std::move(ui)));
    }
}

// ---------------------------------------------------------------- assets

// Every file under assets\ is also compiled into the exe (see tools\make_rc.ps1), so Chezz.exe runs
// on its own. A real file in assets\ still wins, which is how anyone replaces the artwork without
// rebuilding.

static Bitmap* loadImage(const std::wstring& path) {
    if (!fileExists(path)) return nullptr;
    Bitmap* b = new Bitmap(path.c_str());
    if (b->GetLastStatus() != Ok || b->GetWidth() == 0) { delete b; return nullptr; }
    return b;
}

// The resource name for an asset: upper case, every awkward character turned into an underscore.
// tools\make_rc.ps1 does the same, because windres cannot cope with quoted resource names.
static std::wstring assetKey(const std::wstring& rel) {
    std::wstring key = L"ASSET_";
    for (wchar_t c : rel) {
        wchar_t u = (wchar_t)towupper(c);
        key += (u >= L'A' && u <= L'Z') || (u >= L'0' && u <= L'9') ? u : L'_';
    }
    return key;
}

static std::vector<unsigned char> resourceBytes(const std::wstring& name) {
    std::vector<unsigned char> out;
    HRSRC r = FindResourceW(NULL, name.c_str(), (LPCWSTR)RT_RCDATA);
    HGLOBAL h = r ? LoadResource(NULL, r) : NULL;
    const unsigned char* p = h ? (const unsigned char*)LockResource(h) : nullptr;
    if (p) out.assign(p, p + SizeofResource(NULL, r));
    return out;
}

// Which assets are inside the exe, by their paths relative to assets\ ("pieces/classic/wK.png").
static const std::vector<std::wstring>& embeddedAssets() {
    static std::vector<std::wstring> names;
    static bool done = false;
    if (!done) {
        done = true;
        std::vector<unsigned char> list = resourceBytes(L"ASSET_MANIFEST");
        std::string line;
        for (size_t i = 0; i <= list.size(); i++) {
            char c = i < list.size() ? (char)list[i] : '\n';
            if (c == '\n' || c == '\r') {
                if (!line.empty()) names.push_back(widen(line));
                line.clear();
            } else line += c;
        }
    }
    return names;
}

// The bytes of one asset: the loose file first, then the copy inside the exe.
static std::vector<unsigned char> assetBytes(const std::wstring& rel) {
    std::vector<unsigned char> out;
    std::wstring path = g_assets + L"\\" + rel;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 64 * 1024 * 1024) {
            out.resize(n);
            if (fread(out.data(), 1, n, f) != (size_t)n) out.clear();
        }
        fclose(f);
        if (!out.empty()) return out;
    }
    return resourceBytes(assetKey(rel));
}

static Bitmap* bitmapFromBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return nullptr;
    void* dst = GlobalLock(mem);
    memcpy(dst, bytes.data(), bytes.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    Bitmap* b = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK) {   // the stream owns `mem` from here
        b = Bitmap::FromStream(stream);
        stream->Release();
        if (b && (b->GetLastStatus() != Ok || b->GetWidth() == 0)) { delete b; b = nullptr; }
    } else {
        GlobalFree(mem);
    }
    return b;
}

// An image named relative to assets\, without its extension: "pieces\\classic\\white_king".
static Bitmap* loadFirst(const std::vector<std::wstring>& names) {
    static const wchar_t* EXT[] = {L".png", L".jpg", L".jpeg", L".bmp"};
    for (auto& n : names)
        for (auto e : EXT)
            if (Bitmap* b = bitmapFromBytes(assetBytes(n + e))) return b;
    return nullptr;
}

static void loadPieceSet();

// audio.cpp asks for its clips through this, so a .wav in assets\sounds beats the embedded copy.
std::vector<unsigned char> chezzAssetBytes(const std::wstring& rel) { return assetBytes(rel); }

static void savePieceDir() {
    FILE* f = _wfopen((g_data + L"\\piecedir.txt").c_str(), L"wb");
    if (!f) return;
    std::string s = narrow(g_pieceDir);
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

// The 12 piece images: from the member's own folder if one is set, otherwise from assets/.
static void loadPieceSet() {
    static const wchar_t* TN[7] = {L"", L"pawn", L"knight", L"bishop", L"rook", L"queen", L"king"};
    static const wchar_t* TL[7] = {L"", L"P", L"N", L"B", L"R", L"Q", L"K"};
    static const wchar_t* EXT[] = {L".png", L".jpg", L".jpeg", L".bmp"};
    for (auto& kv : g_pieceCache) delete kv.second;
    g_pieceCache.clear();
    for (int c = 0; c < 2; c++)
        for (int t = PAWN; t <= KING; t++) {
            std::wstring cl = c == WHITE ? L"w" : L"b", cn = c == WHITE ? L"white" : L"black";
            std::vector<std::wstring> base = {cl + TL[t], cl + L"_" + TN[t], cn + L"_" + TN[t], std::wstring(TN[t]) + L"_" + cn,
                                              cn + L"-" + TN[t], cn + TN[t]};
            Bitmap*& slot = g_pieceSrc[mkpiece(c, t)];
            delete slot;
            slot = nullptr;
            if (!g_pieceDir.empty())
                for (auto& n : base)
                    for (auto e : EXT)
                        if (!slot) slot = loadImage(g_pieceDir + L"\\" + n + e);
            if (slot) continue;
            // the chosen set's folder first, then loose files in assets\pieces and assets
            std::vector<std::wstring> names, dirs;
            if (g_theme.pieceSet >= 0 && g_theme.pieceSet < (int)g_pieceSets.size())
                dirs.push_back(L"pieces\\" + g_pieceSets[g_theme.pieceSet] + L"\\");
            dirs.push_back(L"pieces\\");
            dirs.push_back(L"");
            for (auto& d : dirs)
                for (auto& n : base) names.push_back(d + n);
            slot = loadFirst(names);
        }
}

static void loadAssets() {
    if (FILE* f = _wfopen((g_data + L"\\piecedir.txt").c_str(), L"rb")) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        g_pieceDir = widen(std::string(buf, n));
    }
    {   // a piece set is a folder inside assets\pieces, on disk or embedded in the exe
        std::set<std::wstring> sets;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g_assets + L"\\pieces\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') sets.insert(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        const std::wstring prefix = L"pieces/";
        for (auto& rel : embeddedAssets()) {
            if (_wcsnicmp(rel.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
            size_t slash = rel.find(L'/', prefix.size());
            if (slash == std::wstring::npos) continue;
            std::wstring set = rel.substr(prefix.size(), slash - prefix.size());
            bool have = false;   // keep the folder's own spelling if it is also on disk
            for (auto& s : sets)
                if (_wcsicmp(s.c_str(), set.c_str()) == 0) have = true;
            if (!have) sets.insert(set);
        }
        g_pieceSets.assign(sets.begin(), sets.end());
    }
    g_boardImg = loadFirst({L"board", L"boards\\board", L"textures\\board"});
    g_lightImg = loadFirst({L"light", L"square_light", L"light_square", L"textures\\light", L"textures\\square_light"});
    g_darkImg = loadFirst({L"dark", L"square_dark", L"dark_square", L"textures\\dark", L"textures\\square_dark"});
    g_bgImg = loadFirst({L"background", L"bg", L"textures\\background"});
    loadSettings();
    applyTheme();
    loadPieceSet();   // after the settings: they say which set
}

static void playSound(int id) { audioPlay(id); }

static Bitmap* avatarFor(int lvl) {
    auto it = g_avatars.find(lvl);
    if (it != g_avatars.end()) return it->second;
    const Level& L = allLevels()[lvl];
    wchar_t a[64], b[64], c[64];
    swprintf(a, 64, L"avatars\\level_%03d", L.number);
    swprintf(b, 64, L"avatars\\%d", L.number);
    swprintf(c, 64, L"avatars\\tier_%02d", L.tier + 1);
    Bitmap* bm = loadFirst({a, b, L"avatars\\" + widen(L.name), c});
    g_avatars[lvl] = bm;
    return bm;
}

// ---------------------------------------------------------------- pieces

static ColorMatrix solidMatrix(float r, float gg, float b, float a) {
    ColorMatrix m = {};
    m.m[3][3] = a;
    m.m[4][0] = r;
    m.m[4][1] = gg;
    m.m[4][2] = b;
    m.m[4][4] = 1;
    return m;
}

static void drawWithMatrix(Graphics& gg, Bitmap* src, RectF dst, const ColorMatrix* cm) {
    ImageAttributes ia;
    if (cm) ia.SetColorMatrix(cm);
    gg.DrawImage(src, dst, 0, 0, (REAL)src->GetWidth(), (REAL)src->GetHeight(), UnitPixel, cm ? &ia : nullptr);
}

// A piece rendered at px x px, taking up `fill` of the square, optionally with a drop shadow.
// A missing colour is derived from the other set: black art becomes ivory with a dark rim.
static Bitmap* pieceBitmap(int piece, int px, float fill, bool shadow) {
    long long key = ((long long)piece << 32) | ((long long)px << 8) | ((int)(fill * 100) << 1) | (shadow ? 1 : 0);
    auto it = g_pieceCache.find(key);
    if (it != g_pieceCache.end()) return it->second;
    if (g_pieceCache.size() > 400) {
        for (auto& kv : g_pieceCache) delete kv.second;
        g_pieceCache.clear();
    }
    bool white = pcolor(piece) == WHITE;
    Bitmap* src = g_pieceSrc[piece];
    bool derived = false;
    if (!src && g_pieceSrc[piece ^ 8]) { src = g_pieceSrc[piece ^ 8]; derived = true; }

    Bitmap* b = new Bitmap(px, px, PixelFormat32bppPARGB);
    Graphics gg(b);
    gg.Clear(Color(0, 0, 0, 0));
    gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gg.SetSmoothingMode(SmoothingModeAntiAlias);
    gg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    float d = px * fill;
    if (src) {
        float w = (float)src->GetWidth(), h = (float)src->GetHeight(), sc = min(d / w, d / h);
        int bw = max(1, (int)(w * sc)), bh = max(1, (int)(h * sc));
        // Downscale once, then build shadow/rim/body from the small copy.
        Bitmap small(bw, bh, PixelFormat32bppPARGB);
        {
            Graphics sg(&small);
            sg.Clear(Color(0, 0, 0, 0));
            sg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            sg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            sg.DrawImage(src, RectF(0, 0, (REAL)bw, (REAL)bh));
        }
        float x = (px - bw) / 2.f, y = (px - bh) / 2.f + px * 0.02f;
        if (shadow) {
            ColorMatrix sm = solidMatrix(0, 0, 0, 0.32f);
            drawWithMatrix(gg, &small, RectF(x + px * 0.03f, y + px * 0.04f, (REAL)bw, (REAL)bh), &sm);
        }
        float rim = white ? max(1.f, px * 0.02f) : max(0.8f, px * 0.012f);
        ColorMatrix rm = white ? solidMatrix(0.16f, 0.12f, 0.09f, 1.f) : solidMatrix(0.93f, 0.87f, 0.75f, 0.45f);
        if (derived || !white)
            for (int i = 0; i < 8; i++) {
                float a = i * 0.785398f;
                drawWithMatrix(gg, &small, RectF(x + cosf(a) * rim, y + sinf(a) * rim, (REAL)bw, (REAL)bh), &rm);
            }
        if (derived && white) {
            ColorMatrix iv = {};   // invert, tinted towards ivory
            iv.m[0][0] = -0.80f; iv.m[4][0] = 0.96f;
            iv.m[1][1] = -0.78f; iv.m[4][1] = 0.93f;
            iv.m[2][2] = -0.74f; iv.m[4][2] = 0.86f;
            iv.m[3][3] = 1.f; iv.m[4][4] = 1.f;
            drawWithMatrix(gg, &small, RectF(x, y, (REAL)bw, (REAL)bh), &iv);
        } else if (derived) {
            ColorMatrix dk = {};   // white art pressed into ebony
            dk.m[0][0] = 0.25f; dk.m[1][1] = 0.24f; dk.m[2][2] = 0.24f; dk.m[3][3] = 1.f; dk.m[4][4] = 1.f;
            drawWithMatrix(gg, &small, RectF(x, y, (REAL)bw, (REAL)bh), &dk);
        } else {
            gg.DrawImage(&small, RectF(x, y, (REAL)bw, (REAL)bh));
        }
    } else {
        static const wchar_t GLYPH[7] = {0, 0x265F, 0x265E, 0x265D, 0x265C, 0x265B, 0x265A};
        GraphicsPath path;
        FontFamily ff(L"Segoe UI Symbol");
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        path.AddString(&GLYPH[ptype(piece)], 1, &ff, FontStyleRegular, d, RectF(0, px * 0.03f, (REAL)px, (REAL)px), &sf);
        if (shadow) {
            Matrix mx;
            mx.Translate(px * 0.03f, px * 0.04f);
            GraphicsPath sp;
            sp.AddPath(&path, FALSE);
            sp.Transform(&mx);
            SolidBrush sb(Color(80, 0, 0, 0));
            gg.FillPath(&sb, &sp);
        }
        SolidBrush fillB(white ? Color(255, 245, 238, 220) : Color(255, 34, 30, 28));
        Pen edge(white ? Color(255, 40, 32, 26) : Color(200, 230, 220, 196), max(1.f, px / 40.f));
        edge.SetLineJoin(LineJoinRound);
        gg.FillPath(&fillB, &path);
        gg.DrawPath(&edge, &path);
    }
    g_pieceCache[key] = b;
    return b;
}

// ---------------------------------------------------------------- drawing helpers

static Font* getFont(int fam, float px, int style) {
    px *= g_textScale;
    int key = ((int)(px * 10) * 8 + style) * 4 + fam;
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    Font* f = new Font(FONTSETS[g_theme.fonts].fam[fam], px, style, UnitPixel);
    if (f->GetLastStatus() != Ok) { delete f; f = new Font(L"Segoe UI", px, style, UnitPixel); }
    g_fonts[key] = f;
    return f;
}

// halign/valign: 0 near, 1 center, 2 far
static void drawText(Graphics& gr, const std::wstring& s, RectF r, int fam, float px, const Color& c,
                     int style = FontStyleRegular, int halign = 0, int valign = 1, bool wrap = false) {
    StringFormat sf;
    StringAlignment A[3] = {StringAlignmentNear, StringAlignmentCenter, StringAlignmentFar};
    sf.SetAlignment(A[halign]);
    sf.SetLineAlignment(A[valign]);
    if (!wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(wrap ? StringTrimmingWord : StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    gr.DrawString(s.c_str(), -1, getFont(fam, px, style), r, &sf, &b);
}

static void fillTexture(Graphics& gr, Bitmap* tex, RectF r, float ox = 0, float oy = 0) {
    TextureBrush tb(tex, WrapModeTile);
    tb.TranslateTransform(r.X + ox, r.Y + oy);
    gr.FillRectangle(&tb, r);
}

static void dropShadow(Graphics& gr, RectF r, float off) {
    for (int i = 3; i >= 1; i--) {
        SolidBrush sb(Color((BYTE)(26 * i), 0, 0, 0));
        float k = off * i / 3.f;
        gr.FillRectangle(&sb, RectF(r.X + k * 0.6f, r.Y + k, r.Width, r.Height));
    }
}

// A sheet of paper with a soft shadow.
static void paperSheet(Graphics& gr, RectF r, bool card = false) {
    dropShadow(gr, r, S(6));
    fillTexture(gr, g_paper, r);
    if (card) {
        SolidBrush w(C_CARDTINT);
        gr.FillRectangle(&w, r);
    }
    Pen edge(C_EDGE, g_edgeWidth);
    gr.DrawRectangle(&edge, r);
}

static void drawTick(Graphics& gr, float x, float y, float s, const Color& c) {
    Pen p(c, max(1.5f, s * 0.16f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    PointF pts[3] = {PointF(x, y + s * 0.55f), PointF(x + s * 0.35f, y + s * 0.95f), PointF(x + s * 1.05f, y - s * 0.05f)};
    gr.DrawLines(&p, pts, 3);
}

// Two loose pencil loops, like someone circled it by hand.
static void inkCircle(Graphics& gr, RectF r, const Color& c) {
    Pen p(c, S(1.8f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    gr.DrawArc(&p, RectF(r.X - S(4), r.Y - S(3), r.Width + S(8), r.Height + S(6)), 200, 335);
    gr.DrawArc(&p, RectF(r.X - S(6), r.Y - S(1), r.Width + S(10), r.Height + S(4)), 170, 70);
}

// Printed box button; inverts to solid ink on hover.
static void inkButton(Graphics& gr, int id, RectF r, const std::wstring& label, bool enabled = true) {
    bool hover = enabled && r.Contains(g_mouseX, g_mouseY);
    if (hover) {
        SolidBrush ib(C_INK);
        gr.FillRectangle(&ib, r);
    }
    Pen p(enabled ? C_INK : Color(120, 128, 118, 104), S(1.2f));
    if (!enabled) p.SetDashStyle(DashStyleDash);
    gr.DrawRectangle(&p, r);
    drawText(gr, label, r, F_SERIF, S(13), hover ? C_PAPER : enabled ? C_INK : C_FAINT, FontStyleBold, 1, 1);
    if (enabled) g_buttons.push_back({r, id});
}

// The same idea chalked onto the felt: cream outline, fills cream on hover.
static void feltButton(Graphics& gr, int id, RectF r, const std::wstring& label, bool enabled = true, bool active = false,
                       float px = 0) {
    bool lit = active || (enabled && r.Contains(g_mouseX, g_mouseY));
    if (lit) {
        SolidBrush b(C_CREAM);
        gr.FillRectangle(&b, r);
    }
    Pen p(enabled ? C_CREAM : Color(110, 240, 228, 200), S(1.2f));
    gr.DrawRectangle(&p, r);
    drawText(gr, label, r, F_SERIF, px > 0 ? px : S(13), lit ? Color(255, 24, 40, 34) : enabled ? C_CREAM : Color(130, 240, 228, 200),
             FontStyleBold, 1, 1);
    if (enabled && !active) g_buttons.push_back({r, id});
}

// A red rubber stamp, slightly crooked.
static void stampButton(Graphics& gr, int id, RectF stamp, const std::wstring& big, const std::wstring& small,
                        bool enabled = true) {
    bool hov = enabled && stamp.Contains(g_mouseX, g_mouseY);
    GraphicsState s2 = gr.Save();
    gr.TranslateTransform(stamp.X + stamp.Width / 2, stamp.Y + stamp.Height / 2);
    gr.RotateTransform(-3.f);
    RectF lr(-stamp.Width / 2, -stamp.Height / 2, stamp.Width, stamp.Height);
    Color red(!enabled ? 90 : hov ? 255 : 215, 176, 40, 34);
    if (hov) {
        SolidBrush f(Color(40, 176, 40, 34));
        gr.FillRectangle(&f, lr);
    }
    Pen p1(red, S(3)), p2(red, S(1.2f));
    gr.DrawRectangle(&p1, lr);
    gr.DrawRectangle(&p2, RectF(lr.X + S(6), lr.Y + S(6), lr.Width - S(12), lr.Height - S(12)));
    float bigH = small.empty() ? lr.Height : lr.Height * 0.6f;
    drawText(gr, big, RectF(lr.X, lr.Y + (small.empty() ? 0 : S(6)), lr.Width, bigH), F_SERIF,
             min(S(28), lr.Height * 0.38f), red, FontStyleBold, 1, 1);
    if (!small.empty())
        drawText(gr, small, RectF(lr.X, lr.Y + lr.Height * 0.58f, lr.Width, lr.Height * 0.32f), F_MONO, S(12), red,
                 FontStyleRegular, 1, 1);
    gr.Restore(s2);
    if (enabled) g_buttons.push_back({stamp, id});
}

// Portrait: the user's avatar if supplied, otherwise the tier's piece as a mounted photo.
static void drawPortrait(Graphics& gr, int lvl, RectF r, bool corners) {
    const Level& L = allLevels()[lvl];
    SolidBrush mount(Color(255, 252, 250, 244));
    gr.FillRectangle(&mount, r);
    float m = max(2.f, r.Width * 0.05f);
    RectF in(r.X + m, r.Y + m, r.Width - 2 * m, r.Height - 2 * m);
    if (Bitmap* b = g.online && g_screen == SCR_GAME ? nullptr : avatarFor(lvl)) {
        float w = (float)b->GetWidth(), h = (float)b->GetHeight(), s = min(w, h);
        gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gr.DrawImage(b, in, (w - s) / 2, (h - s) / 2, s, s, UnitPixel);
    } else {
        SolidBrush bg(Color(255, 222, 214, 196));
        gr.FillRectangle(&bg, in);
        gr.DrawImage(pieceBitmap(mkpiece(BLACK, TIER_PIECE[L.tier]), (int)in.Width, 0.78f, false), in);
    }
    if (corners) {
        SolidBrush cb(Color(255, 46, 40, 34));
        float c = r.Width * 0.16f;
        PointF a[3] = {PointF(r.X - 2, r.Y - 2), PointF(r.X + c, r.Y - 2), PointF(r.X - 2, r.Y + c)};
        PointF b2[3] = {PointF(r.X + r.Width + 2, r.Y - 2), PointF(r.X + r.Width - c, r.Y - 2), PointF(r.X + r.Width + 2, r.Y + c)};
        PointF c2[3] = {PointF(r.X - 2, r.Y + r.Height + 2), PointF(r.X + c, r.Y + r.Height + 2), PointF(r.X - 2, r.Y + r.Height - c)};
        PointF d2[3] = {PointF(r.X + r.Width + 2, r.Y + r.Height + 2), PointF(r.X + r.Width - c, r.Y + r.Height + 2),
                        PointF(r.X + r.Width + 2, r.Y + r.Height - c)};
        gr.FillPolygon(&cb, a, 3);
        gr.FillPolygon(&cb, b2, 3);
        gr.FillPolygon(&cb, c2, 3);
        gr.FillPolygon(&cb, d2, 3);
    } else {
        Pen p(Color(255, 190, 178, 152), 1.f);
        gr.DrawRectangle(&p, r);
    }
}

// The clock chooser: "CLOCK  < 5 + 0 >"
static void tcChooser(Graphics& gr, float x, float y, float w) {
    drawText(gr, L"CLOCK", RectF(x, y, S(100), S(34)), F_MONO, S(12), C_FAINT, FontStyleRegular, 0, 1);
    float bx = x + S(100), bs = S(30);
    inkButton(gr, B_TC_PREV, RectF(bx, y + S(2), bs, bs), L"\x2039");
    drawText(gr, TCS[g_tcIndex].label, RectF(bx + bs, y, min(S(130), w - S(100) - 2 * bs), S(34)), F_SERIF, S(16), C_INK,
             FontStyleRegular, 1, 1);
    inkButton(gr, B_TC_NEXT, RectF(bx + bs + min(S(130), w - S(100) - 2 * bs), y + S(2), bs, bs), L"\x203A");
}

// White / either / black tokens, the chosen one circled in red pencil.
static void colourTokens(Graphics& gr, float x, float y, const wchar_t* label) {
    drawText(gr, label, RectF(x, y, S(100), S(56)), F_MONO, S(12), C_FAINT, FontStyleRegular, 0, 1);
    float tok = S(52), tx = x + S(100);
    for (int k = 0; k < 3; k++) {
        RectF tr(tx + k * (tok + S(26)), y + S(2), tok, tok);
        int id = k == 0 ? B_WHITE : k == 1 ? B_RANDOM : B_BLACK;
        if (k == 1) {
            SolidBrush lb(Color(255, 240, 232, 214)), db(Color(255, 40, 34, 30));
            gr.FillPie(&lb, tr, 90, 180);
            gr.FillPie(&db, tr, 270, 180);
            Pen rim(C_PRINT, S(1.2f));
            gr.DrawEllipse(&rim, tr);
        } else {
            gr.DrawImage(pieceBitmap(mkpiece(k == 0 ? WHITE : BLACK, KING), (int)tok, 0.9f, false), tr);
        }
        if (g_colorPref == k) inkCircle(gr, tr, C_RED);
        else if (tr.Contains(g_mouseX, g_mouseY)) inkCircle(gr, tr, Color(90, 172, 44, 38));
        g_buttons.push_back({tr, id});
        const wchar_t* cap = k == 0 ? L"white" : k == 1 ? L"either" : L"black";
        drawText(gr, cap, RectF(tr.X - S(10), tr.Y + tok + S(4), tok + S(20), S(18)), F_SERIF, S(12), C_INK, FontStyleItalic, 1, 1);
    }
}

static void checkbox(Graphics& gr, int id, float x, float y, bool on, const std::wstring& label) {
    RectF box(x, y + S(4), S(16), S(16));
    Pen bp(C_PRINT, S(1.2f));
    gr.DrawRectangle(&bp, box);
    if (on) drawTick(gr, box.X + S(1), box.Y - S(2), S(17), C_INK);
    drawText(gr, label, RectF(box.X + S(28), y, S(320), S(24)), F_MONO, S(14), C_PRINT, 0, 0, 1);
    g_buttons.push_back({RectF(box.X - S(4), y, S(300), S(24)), id});
}

// Hand-drawn arrow for the bot's thoughts.
static RectF squareRect(int s);

// A planning mark: a ring when both squares are the same, an L for a knight's move, else a
// straight arrow.
static void drawMark(Graphics& gr, int from, int to, const Color& c, float sq) {
    RectF a = squareRect(from), b = squareRect(to);
    PointF p1(a.X + a.Width / 2, a.Y + a.Height / 2), p2(b.X + b.Width / 2, b.Y + b.Height / 2);
    float w = sq * 0.17f;
    if (from == to) {
        Pen ring(c, sq * 0.09f);
        gr.DrawEllipse(&ring, RectF(a.X + sq * 0.07f, a.Y + sq * 0.07f, sq - sq * 0.14f, sq - sq * 0.14f));
        return;
    }
    Pen p(c, w);
    AdjustableArrowCap cap(2.4f, 2.2f, TRUE);
    p.SetCustomEndCap(&cap);
    p.SetStartCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    int df = sqfile(to) - sqfile(from), dr = sqrank(to) - sqrank(from);
    if (std::abs(df) + std::abs(dr) == 3 && df && dr && std::abs(df) != std::abs(dr)) {
        // knight's move: along the long side first, then turn
        PointF mid = std::abs(df) == 2 ? PointF(p2.X, p1.Y) : PointF(p1.X, p2.Y);
        float dx = p2.X - mid.X, dy = p2.Y - mid.Y, len = sqrtf(dx * dx + dy * dy);
        PointF end(p2.X - dx / len * sq * 0.34f, p2.Y - dy / len * sq * 0.34f);
        PointF pts3[3] = {p1, mid, end};
        gr.DrawLines(&p, pts3, 3);
        return;
    }
    float dx = p2.X - p1.X, dy = p2.Y - p1.Y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    PointF end(p2.X - dx / len * sq * 0.34f, p2.Y - dy / len * sq * 0.34f);
    gr.DrawLine(&p, p1, end);
}

static void drawArrow(Graphics& gr, RectF a, RectF b, const Color& c, float width) {
    PointF p1(a.X + a.Width / 2, a.Y + a.Height / 2), p2(b.X + b.Width / 2, b.Y + b.Height / 2);
    float dx = p2.X - p1.X, dy = p2.Y - p1.Y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float pull = a.Width * 0.3f;   // stop short of the target's centre
    p2.X -= dx / len * pull;
    p2.Y -= dy / len * pull;
    Pen p(c, width);
    AdjustableArrowCap cap(3.2f, 2.6f, TRUE);
    p.SetCustomEndCap(&cap);
    p.SetStartCap(LineCapRound);
    gr.DrawLine(&p, p1, p2);
}

// ---------------------------------------------------------------- board geometry

struct Geom { float x, y, size, sq, frame; };
static Geom boardGeom() {
    // room above and below the board for the two player bars (puzzles have none)
    float m = S(18), bars = g.puzzle ? 0.f : 2 * (S(60) + S(10)), rightW = max(S(320), g_W * 0.27f);
    float total = min((float)g_H - 2 * m - bars, (float)g_W - rightW - 3 * m);
    total = max(total, 220.f);
    float sq = floorf(total / 8.7f), frame = floorf((total - 8 * sq) / 2);
    return {m + frame, floorf((g_H - 8 * sq) / 2), 8 * sq, sq, frame};
}
static RectF squareRect(int s) {
    Geom b = boardGeom();
    int col = g.flipped ? 7 - sqfile(s) : sqfile(s), row = g.flipped ? sqrank(s) : 7 - sqrank(s);
    return RectF(b.x + col * b.sq, b.y + row * b.sq, b.sq, b.sq);
}
static int squareAt(float x, float y) {
    Geom b = boardGeom();
    if (x < b.x || y < b.y || x >= b.x + b.size || y >= b.y + b.size) return -1;
    int col = (int)((x - b.x) / b.sq), row = (int)((y - b.y) / b.sq);
    int file = g.flipped ? 7 - col : col, rank = g.flipped ? row : 7 - row;
    return rank * 8 + file;
}
static RectF promoRect(int k) {
    RectF t = squareRect(g.promoTo);
    Geom b = boardGeom();
    bool top = t.Y < b.y + b.size / 2;
    return RectF(t.X, top ? t.Y + k * b.sq : t.Y - k * b.sq, b.sq, b.sq);
}
static const int PROMO_TYPES[4] = {QUEEN, KNIGHT, ROOK, BISHOP};

// ---------------------------------------------------------------- persistence

static void saveProgress() {
    FILE* f = _wfopen((g_data + L"\\progress.txt").c_str(), L"w");
    if (!f) return;
    fprintf(f, "sel %d\ncolor %d\nlearn %d\ntc %d\nthoughts %d\nsharebrain %d\n", g_sel, g_colorPref, g_learn ? 1 : 0,
            g_tcIndex, g_showThoughts ? 1 : 0, g_brainShare ? 1 : 0);
    for (int i = 0; i < 150; i++)
        if (g_rec[i].w || g_rec[i].d || g_rec[i].l) fprintf(f, "rec %d %d %d %d\n", i, g_rec[i].w, g_rec[i].d, g_rec[i].l);
    fclose(f);
}

static void loadProgress() {
    FILE* f = _wfopen((g_data + L"\\progress.txt").c_str(), L"r");
    if (!f) return;
    char key[32];
    while (fscanf(f, "%31s", key) == 1) {
        int a = 0, b = 0, c = 0, d = 0;
        if (!strcmp(key, "sel") && fscanf(f, "%d", &a) == 1) g_sel = max(0, min(149, a));
        else if (!strcmp(key, "color") && fscanf(f, "%d", &a) == 1) g_colorPref = max(0, min(2, a));
        else if (!strcmp(key, "learn") && fscanf(f, "%d", &a) == 1) g_learn = a != 0;
        else if (!strcmp(key, "tc") && fscanf(f, "%d", &a) == 1) g_tcIndex = max(0, min(TC_COUNT - 1, a));
        else if (!strcmp(key, "thoughts") && fscanf(f, "%d", &a) == 1) g_showThoughts = a != 0;
        else if (!strcmp(key, "sharebrain") && fscanf(f, "%d", &a) == 1) g_brainShare = a != 0;
        else if (!strcmp(key, "rec") && fscanf(f, "%d %d %d %d", &a, &b, &c, &d) == 4 && a >= 0 && a < 150)
            g_rec[a] = {b, c, d};
    }
    fclose(f);
}

static std::wstring sessionPath() { return g_data + L"\\account.ini"; }

// ---------------------------------------------------------------- invalidation + animation helpers

static void invalidate() {   // something in the scene changed
    g_sceneDirty = true;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void invalidateOverlay() { InvalidateRect(g_hwnd, NULL, FALSE); }   // only moving things changed
static void startAnimTimer() { SetTimer(g_hwnd, TIMER_ANIM, 15, NULL); }

static void addGlide(int piece, RectF from, RectF to, int toSq, float dur, bool fade = false) {
    g_anims.push_back({piece, toSq, from, to, Clock::now(), dur, fade, -1});
    startAnimTimer();
}
static void clearMarks() {
    if (g_marks.empty() && g_rightFrom < 0) return;
    g_marks.clear();
    g_rightFrom = g_rightTo = -1;
    invalidate();
}

static void clearPreMove() {
    if (g.preFrom < 0) return;
    g.preFrom = g.preTo = -1;
    g.preTargets.clear();
    invalidate();
}

static bool squareGliding(int s) {
    for (auto& a : g_anims)
        if (!a.fade && a.toSq == s) return true;
    return false;
}

// ---------------------------------------------------------------- login edit boxes

static void showLoginEdits(bool show);
static void layoutLoginEdits();

static void goScreen(Screen s) {
    Screen was = g_screen;
    g_screen = s;
    if (s == SCR_MENU && was != SCR_MENU) SetWindowTextW(g_edGen[0], widen(g_filterText).c_str());   // the filter survives a trip elsewhere
    else if (was == SCR_MENU && s != SCR_MENU) SetWindowTextW(g_edGen[0], L"");
    showLoginEdits(s == SCR_LOGIN);
    // the network tick drives the lobby, online games, and the friends/messages sync on every other signed-in screen
    if (s != SCR_LOGIN && !(s == SCR_GAME && !g.online)) SetTimer(g_hwnd, TIMER_NET, 900, NULL);
    else KillTimer(g_hwnd, TIMER_NET);
    invalidate();
}

// ---------------------------------------------------------------- game logic

static void stopEngine() {
    g_engStop = true;
    if (g_engThread.joinable()) g_engThread.join();
    g_engStop = false;
    g_thinking = false;
}

static long long clockFor(int c) {
    long long run = 0;
    if (!g.over && g_screen == SCR_GAME && g.pos.side == c)
        run = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g.turnStart).count();
    return g.timed ? g.clockMs[c] - run : g.clockMs[c] + run;
}

static void maybeEngine() {
    if (g.puzzle || g.online || g.over || g.pos.side == g.human || g_thinking) return;
    if (g_engThread.joinable()) g_engThread.join();
    g_thinking = true;
    g_think.reset();
    {
        std::lock_guard<std::mutex> lk(g_noteMx);
        g_expectNote.clear();
    }
    Position copy = g.pos;
    Level L = curLevel();
    int gid = g.gameId;
    auto net = g_brain.net();
    unsigned seed = g_rng();
    bool timed = g.timed;
    long long clk = clockFor(g.human ^ 1), inc = g.incMs;
    g_lastThinkPaint = Clock::now();
    startAnimTimer();
    g_engThread = std::thread([copy, L, gid, net, seed, timed, clk, inc]() {
        auto t0 = Clock::now();
        auto ms = [&]() { return (double)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); };
        std::mt19937 rng(seed);
        // Decide how long a player of this strength would sit over this position. On a clock the
        // search gets most of that budget; whatever it doesn't need is spent mulling it over.
        Position p = copy;
        MoveList ml;
        p.genLegal(ml);
        double want = botThinkMs(L, copy, ml.n, timed, clk, inc, rng);
        int searchMs = timed ? (int)(want * 0.85) : 0;
        BotMove bm = botChoose(*g_engine, copy, L, &g_engStop, net.get(), rng, &g_think, searchMs);

        // Think about *you*: how would you most likely answer each candidate?
        auto habits = g_brain.patterns();
        if (bm.known && habits && habits->ready() && !g_engStop) {
            Move cands[4];
            int nc = std::min(4, g_think.ncand.load());
            for (int i = 0; i < nc; i++) cands[i] = g_think.cand[i];
            Move reply = 0;
            Move chosen = anticipate(*g_engine, copy, L, bm.move, cands, nc, *habits, net.get(), &g_engStop, &reply);
            if (chosen) bm.move = chosen;
            if (reply) {
                Position after = copy;
                if (after.make(bm.move)) {
                    std::string san = after.san(reply);
                    std::lock_guard<std::mutex> lk(g_noteMx);
                    g_expectNote = L"expects you to answer " + std::wstring(san.begin(), san.end());
                }
            }
        }

        // ...mull over the candidates while the clock runs
        int nc = g_think.ncand, k = 0;
        double lastSwitch = ms();
        while (!g_engStop && ms() < want) {
            if (nc > 1 && ms() - lastSwitch > 420) {
                g_think.current = g_think.cand[k++ % nc].load();
                lastSwitch = ms();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        if (g_engStop) return;
        g_think.best = bm.move;
        g_think.current = bm.move;
        g_pendingScore = bm.score;
        g_pendingKnown = bm.known && L.noise < 40;
        PostMessageW(g_hwnd, WM_ENGINE, (WPARAM)gid, (LPARAM)bm.move);
    });
}

// Write the local game state to Firestore. Runs on the network thread with the newest updateTime.
static void pushOnline() {
    if (!g.online) return;
    fb::GameDoc d = g.doc;
    std::string moves;
    for (auto& u : g.ucis) moves += (moves.empty() ? "" : " ") + u;
    d.moves = moves;
    long long wMs = g.timed ? max(0LL, clockFor(WHITE)) : 0, bMs = g.timed ? max(0LL, clockFor(BLACK)) : 0;
    d.whiteMs = wMs;
    d.blackMs = bMs;
    d.lastMoveAt = fb::nowMs();
    if (g.over) {
        d.status = "done";
        d.result = g.resultWhite > 0 ? "1-0" : g.resultWhite < 0 ? "0-1" : "1/2";
        d.reason = narrow(g.reasonText);
    }
    g_netWrites++;
    netAsync([d]() mutable -> std::function<void()> {
        auto it = g_latestUpdate.find(d.id);
        if (it != g_latestUpdate.end()) d.updateTime = it->second;
        fb::Err e = fb::updateGame(g_sess, d);
        if (e == fb::CONFLICT) {   // someone wrote first (usually our own poll was stale): re-read and retry once
            fb::GameDoc cur;
            if (fb::getGame(g_sess, d.id, cur) == fb::OK && cur.status != "done") {
                d.updateTime = cur.updateTime;
                e = fb::updateGame(g_sess, d);
            }
        }
        if (e == fb::OK) g_latestUpdate[d.id] = d.updateTime;
        fb::GameDoc saved = d;
        return [e, saved]() {
            g_netWrites--;
            if (e == fb::OK && g.online && g.doc.id == saved.id) g.doc.updateTime = saved.updateTime;
            if (e != fb::OK) g_lobbyMsg = L"Lost touch with the server; your last move may not have been sent.";
            invalidate();
        };
    });
}

static void reportTourResult(const fb::GameDoc& game, int resultWhite);

static bool hasMatingMaterial(int c) {
    int minors = 0;
    for (int s = 0; s < 64; s++) {
        int p = g.pos.sq[s];
        if (!p || pcolor(p) != c) continue;
        int t = ptype(p);
        if (t == PAWN || t == ROOK || t == QUEEN) return true;
        if (t == KNIGHT || t == BISHOP) minors++;
    }
    return minors >= 2;
}

static void finishGame(int resultWhite, const std::string& reason, bool push = true) {
    g.clockMs[g.pos.side] = clockFor(g.pos.side);
    g.over = true;
    g_thinking = false;
    KillTimer(g_hwnd, TIMER_CLOCK);
    g.resultWhite = resultWhite;
    int fh = g.human == WHITE ? resultWhite : -resultWhite;
    g.resultForHuman = fh;
    g.reasonText = widen(reason);
    if (fh > 0) g.resultText = L"You win";
    else if (fh < 0) g.resultText = oppName() + L" wins";
    else g.resultText = L"Drawn";
    g.ratingText.clear();
    if (!g.online) {
        Record scratch;   // member-built engines don't count towards the ladder
        Record& r = g_customOn ? scratch : g_rec[g.level];
        if (fh > 0) r.w++;
        else if (fh < 0) r.l++;
        else r.d++;
        saveProgress();
    } else {
        // Elo for my own profile (each player updates their own)
        int me = g.human == WHITE ? g.doc.whiteRating : g.doc.blackRating, opp = oppRating();
        double expect = 1.0 / (1.0 + pow(10.0, (opp - me) / 400.0)), score = fh > 0 ? 1 : fh < 0 ? 0 : 0.5;
        int nr = max(100, (int)lround(me + 32 * (score - expect)));
        g.ratingText = L"rating " + std::to_wstring(me) + L" \x2192 " + std::to_wstring(nr);
        g_profile.rating = nr;
        if (fh > 0) g_profile.wins++;
        else if (fh < 0) g_profile.losses++;
        else g_profile.draws++;
        if (!g_guest) {   // a guest's rating lasts only for this visit
            fb::Profile p = g_profile;
            p.name = g_myName;
            netAsync([p]() -> std::function<void()> {
                fb::putProfile(g_sess, p);
                return nullptr;
            });
        }
        if (push && !g_applyingRemote) pushOnline();
        reportTourResult(g.doc, resultWhite);   // tournament games file their result (queued after the push)
    }
    playSound(SND_END);
    if (g_learn && g.samples.size() >= 2) {
        for (Sample& s : g.samples) s.result = (int8_t)(s.side == WHITE ? resultWhite : -resultWhite);
        g_brain.learnAsync(g.samples, g.myMoves);
        SetTimer(g_hwnd, TIMER_LEARN, 400, NULL);
    }
}

static void checkEnd() {
    std::string reason;
    int r = g.pos.outcome(&reason);
    if (r != 2) finishGame(r, reason);
}

static void puzzleMove(Move m);

// Quietly guess your next move from your habits, to score the guess once you've played.
static void predictNext() {
    g.predicted = 0;
    if (g.over || g.pos.side != g.human) return;
    auto habits = g_brain.patterns();
    if (habits && habits->ready()) g.predicted = habits->best(g.pos);
}

static void playMove(Move m, int score, bool known, bool glide) {
    if (g.puzzle) { puzzleMove(m); return; }
    if (g.pos.side != g.human) clearMarks();   // the opponent has moved: the plan is out of date
    if (g.pos.side == g.human) {
        g.myMoves.push_back(makePatternSample(g.pos, m));
        if (g.predicted) {
            g.predTotal++;
            if (g.predicted == m) g.predHits++;
        }
        g.predicted = 0;
    }
    if (g_glideMul == 0.f) glide = false;
    int from = mfrom(m), to = mto(m);
    int capSq = (m & MF_EP) ? to + (g.pos.side == WHITE ? -8 : 8) : to;
    int capPiece = (m & MF_CAP) ? g.pos.sq[capSq] : 0;
    Sample s;
    memcpy(s.sq, g.pos.sq, 64);
    s.side = (uint8_t)g.pos.side;
    s.result = 0;
    s.search = known ? (int16_t)max(-2000, min(2000, score)) : NO_SCORE;
    g.samples.push_back(s);
    std::string san = g.pos.san(m), uci = g.pos.uci(m);
    int mover = g.pos.side;
    if (!g.pos.make(m)) { g.samples.pop_back(); return; }
    long long spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g.turnStart).count();
    if (g.timed) g.clockMs[mover] += g.incMs - spent;
    else g.clockMs[mover] += spent;
    g.turnStart = Clock::now();
    g.sans.push_back(widen(san));
    g.ucis.push_back(uci);
    g.last = m;
    g.selected = -1;
    g.targets.clear();
    int snd = g.pos.inCheck() ? SND_CHECK : (m & MF_CASTLE) ? SND_CASTLE : mpromo(m) ? SND_PROMOTE
            : (m & MF_CAP) ? SND_CAPTURE : SND_MOVE;
    float dist = sqrtf((float)((sqfile(from) - sqfile(to)) * (sqfile(from) - sqfile(to)) +
                               (sqrank(from) - sqrank(to)) * (sqrank(from) - sqrank(to))));
    float dur = glide ? (0.13f + 0.035f * dist) * g_glideMul : 0.f;   // longer moves take a little longer
    if (capPiece) addGlide(capPiece, squareRect(capSq), squareRect(capSq), -1, glide ? dur : 0.16f, true);
    bool wasOver = g.over;
    checkEnd();
    if (g.online && mover == g.human && !(g.over && !wasOver)) pushOnline();   // finishGame already pushed
    if (glide) {
        addGlide(g.pos.sq[to], squareRect(from), squareRect(to), to, dur);
        if (m & MF_CASTLE) {
            int rf = to > from ? from + 3 : from - 4, rt = to > from ? from + 1 : from - 1;
            addGlide(g.pos.sq[rt], squareRect(rf), squareRect(rt), rt, dur + 0.05f);
        }
    }
    if (!g.over) playSound(snd);   // at once: the mixer starts it within a few milliseconds
    predictNext();
    if (!g.over) maybeEngine();
    invalidate();
}

// Pre-moves: offline only, because an online opponent's move arrives when it arrives and a queued
// reply would be played before you had seen it.
static bool preMoveAllowed() {
    return g_screen == SCR_GAME && !g.over && !g.online && !g.puzzle && g.pos.side != g.human;
}

// The squares that piece could reach, worked out as though it were already your turn. Only a hint:
// the real move is checked for legality when it is played.
static void setPreTargets(int from) {
    g.preTargets.clear();
    Position p = g.pos;
    p.side = g.human;
    p.ep = -1;
    p.hist.clear();
    p.hash = p.computeHash();
    MoveList ml;
    p.genLegal(ml);
    for (int i = 0; i < ml.n; i++)
        if (mfrom(ml.m[i]) == from) g.preTargets.push_back(ml.m[i]);
}

// Called once the bot has moved and it is your turn again.
static void runPreMove() {
    if (g.preFrom < 0) return;
    int from = g.preFrom, to = g.preTo;
    g.preFrom = g.preTo = -1;
    g.preTargets.clear();
    if (g.over || g.pos.side != g.human) return;
    MoveList ml;
    g.pos.genLegal(ml);
    Move pick = 0;
    for (int i = 0; i < ml.n; i++) {
        Move c = ml.m[i];
        if (mfrom(c) != from || mto(c) != to) continue;
        if (!pick || mpromo(c) == QUEEN) pick = c;   // a pre-moved pawn always promotes to a queen
    }
    if (pick) playMove(pick, 0, false, true);
    else invalidate();   // no longer legal: it is quietly dropped
}

static void resetBoardState() {
    stopEngine();
    g.pos.setStart();
    g.over = false;
    g.sans.clear();
    g.ucis.clear();
    g.samples.clear();
    g.last = 0;
    g.resultText.clear();
    g.reasonText.clear();
    g.ratingText.clear();
    g.selected = -1;
    g.targets.clear();
    g.dragging = false;
    g.promoOpen = false;
    g.turnStart = Clock::now();
    g_anims.clear();
    g.myMoves.clear();
    g.predicted = 0;
    g.predHits = g.predTotal = 0;
    g.puzzle = false;
    g_confirm = 0;
    g.preFrom = g.preTo = -1;
    g.preTargets.clear();
    g_marks.clear();
    g_rightFrom = g_rightTo = -1;
    g.gameId++;
}

static void startGame(int level) {
    if (g_guest) return;   // the bots are for members only
    resetBoardState();
    g.online = false;
    g_customOn = false;
    g.level = level;
    g_sel = level;
    g.human = g_colorPref == 0 ? WHITE : g_colorPref == 2 ? BLACK : (int)(g_rng() & 1);
    g.flipped = g.human == BLACK;
    g.timed = TCS[g_tcIndex].minutes > 0;
    g.baseMs = TCS[g_tcIndex].minutes * 60000LL;
    g.incMs = TCS[g_tcIndex].inc * 1000LL;
    g.clockMs[0] = g.clockMs[1] = g.timed ? g.baseMs : 0;
    g_engine->clear();
    goScreen(SCR_GAME);
    SetTimer(g_hwnd, TIMER_CLOCK, g.timed ? 200 : 1000, NULL);
    saveProgress();
    playSound(SND_START);
    predictNext();
    maybeEngine();
    invalidate();
}

// A game against a member-built engine: same board and clocks, but its own dials and no ladder record.
static void startCustomGame(const Level& L, const std::wstring& event) {
    if (g_guest) return;
    resetBoardState();
    g.online = false;
    g_customOn = true;
    g_customLevel = L;
    g_customEvent = event;
    g.level = levelForRating(L.rating);
    g.human = g_colorPref == 0 ? WHITE : g_colorPref == 2 ? BLACK : (int)(g_rng() & 1);
    g.flipped = g.human == BLACK;
    g.timed = TCS[g_tcIndex].minutes > 0;
    g.baseMs = TCS[g_tcIndex].minutes * 60000LL;
    g.incMs = TCS[g_tcIndex].inc * 1000LL;
    g.clockMs[0] = g.clockMs[1] = g.timed ? g.baseMs : 0;
    g_engine->clear();
    goScreen(SCR_GAME);
    SetTimer(g_hwnd, TIMER_CLOCK, g.timed ? 200 : 1000, NULL);
    playSound(SND_START);
    predictNext();
    maybeEngine();
    invalidate();
}

static void startOnlineGame(const fb::GameDoc& d) {
    resetBoardState();
    g_customOn = false;
    g.online = true;
    g.doc = d;
    g_hosting = false;
    g.human = d.white == g_myUid ? WHITE : BLACK;
    g.flipped = g.human == BLACK;
    g.timed = d.baseMs > 0;
    g.baseMs = d.baseMs;
    g.incMs = d.incMs;
    g.clockMs[0] = g.clockMs[1] = g.timed ? g.baseMs : 0;
    g_lobbyMsg.clear();
    goScreen(SCR_GAME);
    SetTimer(g_hwnd, TIMER_CLOCK, g.timed ? 200 : 1000, NULL);
    playSound(SND_START);
    predictNext();
    invalidate();
}

// Bring the local game in line with the server copy.
static void onlineSync(const fb::GameDoc& d) {
    if (!g.online || d.id != g.doc.id || g_netWrites > 0) return;
    std::vector<std::string> remote;
    {
        std::istringstream ss(d.moves);
        std::string t;
        while (ss >> t) remote.push_back(t);
    }
    bool applied = false;
    while (!g.over && remote.size() > g.ucis.size()) {
        const std::string& want = remote[g.ucis.size()];
        MoveList ml;
        g.pos.genLegal(ml);
        Move found = 0;
        for (int i = 0; i < ml.n; i++)
            if (g.pos.uci(ml.m[i]) == want) found = ml.m[i];
        if (!found) break;
        bool lastOne = g.ucis.size() + 1 == remote.size();
        int mover = g.pos.side;
        g_applyingRemote = true;
        playMove(found, 0, false, lastOne);
        g_applyingRemote = false;
        if (g.timed) g.clockMs[mover] = mover == WHITE ? d.whiteMs : d.blackMs;   // trust the mover's own clock
        applied = true;
    }
    g.doc.updateTime = d.updateTime;
    g.doc.status = d.status;
    if (!g.over && d.status == "done") {
        int rw = d.result == "1-0" ? 1 : d.result == "0-1" ? -1 : 0;
        finishGame(rw, d.reason.empty() ? "resignation" : d.reason, false);
    }
    if (applied || g.over) invalidate();
}

static void undoPly() {
    if (g.pos.hist.empty()) return;
    g.pos.unmake();
    if (g.pos.side == g.human && !g.myMoves.empty()) g.myMoves.pop_back();   // a taken-back move isn't a habit
    if (!g.sans.empty()) g.sans.pop_back();
    if (!g.ucis.empty()) g.ucis.pop_back();
    if (!g.samples.empty()) g.samples.pop_back();
    g.last = g.pos.hist.empty() ? 0 : g.pos.hist.back().m;
}

static void doUndo() {
    if (g.over || g.online) return;
    if (g_thinking) {
        stopEngine();
        g.gameId++;
        undoPly();
    } else if (g.pos.side == g.human && g.pos.hist.size() >= 2) {
        undoPly();
        undoPly();
    }
    g_anims.clear();
    g.turnStart = Clock::now();
    g.selected = -1;
    g.targets.clear();
    clearPreMove();
    clearMarks();
    predictNext();
    maybeEngine();
    invalidate();
}

static void resign() {
    if (g.over) return;
    stopEngine();
    g.gameId++;
    finishGame(g.human == WHITE ? -1 : 1, "resignation");
}

// Flag fall. The side that still has time only wins if it could ever deliver mate.
static void clockCheck() {
    if (!g.timed || g.over || g_screen != SCR_GAME) return;
    int c = g.pos.side;
    long long grace = (g.online && c != g.human) ? 2500 : 0;   // allow for network delay before claiming
    if (clockFor(c) > -grace) return;
    if (!g.online) stopEngine();
    g.gameId += g.online ? 0 : 1;
    int winner = c ^ 1;
    if (!hasMatingMaterial(winner)) finishGame(0, "time (no mating material)");
    else finishGame(winner == WHITE ? 1 : -1, "time");
    invalidate();
}

static bool humanCanMove() { return g_screen == SCR_GAME && !g.over && g.pos.side == g.human && !g_thinking; }

static void selectSquare(int s) {
    g.selected = s;
    g.targets.clear();
    MoveList ml;
    g.pos.genLegal(ml);
    for (int i = 0; i < ml.n; i++)
        if (mfrom(ml.m[i]) == s) g.targets.push_back(ml.m[i]);
}

static bool isTarget(int s) {
    for (Move m : g.targets)
        if (mto(m) == s) return true;
    return false;
}

static void tryMove(int from, int to, bool glide) {
    std::vector<Move> c;
    for (Move m : g.targets)
        if (mfrom(m) == from && mto(m) == to) c.push_back(m);
    if (c.empty()) return;
    if (mpromo(c[0])) {
        g.promoOpen = true;
        g.promoFrom = from;
        g.promoTo = to;
        invalidate();
        return;
    }
    playMove(c[0], 0, false, glide);
}

static void openLobby();
static bool isBlocked(const std::string& uid);
static void socialReset();
static void socialTick();
static void refreshSocial();
static void loadChatSeen();
static void loadPlanAndConfig();
static void billingTick();
static void cloudBrainTick();
static void cloudBrainPull(bool force);
static void cloudBrainAfterLearn();
static void loadCloudState();
static bool onPlansButton(int id);
static void loadPuzzleState();
static void competeTick();
static void rebuildLadder();
static void drawBadge(Graphics& gr, int plan, RectF r);

static void goMenu() {
    stopEngine();
    g.gameId++;
    KillTimer(g_hwnd, TIMER_CLOCK);
    g_anims.clear();
    if (g_guest) { openLobby(); return; }   // guests have no ladder to go back to
    rebuildLadder();   // a win may change what the filters show
    g_scrollTarget = g_menuScroll;
    goScreen(SCR_MENU);
}

// ---------------------------------------------------------------- account + lobby actions

static void enterSignedIn(const fb::Session& s, const fb::Profile& p, bool offline, bool guest = false) {
    g_myUid = s.uid;
    g_myName = s.name.empty() ? p.name : s.name;
    g_profile = p;
    if (g_profile.name.empty()) g_profile.name = g_myName;
    g_offline = offline;
    g_guest = guest;
    g_loginBusy = false;
    g_loginMsg.clear();
    socialReset();
    g_plan = 0;
    loadPuzzleState();
    loadCloudState();
    if (guest) openLobby();
    else {
        goScreen(SCR_MENU);
        if (!offline) { loadChatSeen(); refreshSocial(); loadPlanAndConfig(); cloudBrainPull(false); }
    }
}

static void submitGuest() {
    if (g_loginBusy) return;
    g_loginBusy = true;
    g_loginErr = false;
    g_loginMsg = L"Finding you a visitor's badge\x2026";
    showLoginEdits(true);
    invalidate();
    std::string name = "Guest " + std::to_string(1000 + g_rng() % 9000);
    netAsync([name]() -> std::function<void()> {
        fb::Session s;
        std::string err = fb::signInGuest(s, name);
        if (err.empty()) g_sess = s;   // deliberately not saved: a guest visit ends when the app closes
        return [err, s]() {
            if (err.empty()) {
                fb::Profile p;
                p.name = s.name;
                enterSignedIn(s, p, false, true);
            } else {
                g_loginBusy = false;
                g_loginErr = true;
                g_loginMsg = widen(err);
                showLoginEdits(true);
                invalidate();
            }
        };
    });
}

static std::wstring editText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(n + 1, 0);
    GetWindowTextW(h, &w[0], n + 1);
    w.resize(n);
    return w;
}

static void submitLogin() {
    if (g_loginBusy) return;
    std::string email = narrow(editText(g_edEmail)), pass = narrow(editText(g_edPass)), name = narrow(editText(g_edName));
    while (!email.empty() && email.back() == ' ') email.pop_back();
    while (!name.empty() && name.back() == ' ') name.pop_back();
    g_loginErr = true;
    if (email.find('@') == std::string::npos) { g_loginMsg = L"Enter your email address."; invalidate(); return; }
    if (pass.size() < 6) { g_loginMsg = L"Passwords are at least 6 characters."; invalidate(); return; }
    if (g_loginMode == 1 && (name.size() < 2 || name.size() > 20)) {
        g_loginMsg = L"Your ladder name needs 2 to 20 characters.";
        invalidate();
        return;
    }
    g_loginErr = false;
    g_loginBusy = true;
    g_loginMsg = g_loginMode ? L"Writing you into the register\x2026" : L"Checking the register\x2026";
    showLoginEdits(true);
    invalidate();
    int mode = g_loginMode;
    netAsync([email, pass, name, mode]() -> std::function<void()> {
        fb::Session s;
        std::string err = mode ? fb::signUp(s, email, pass, name) : fb::signIn(s, email, pass);
        fb::Profile p;
        p.name = s.name;
        if (err.empty()) {
            g_sess = s;
            fb::saveSession(sessionPath(), s);
            fb::Profile got;
            if (fb::getProfile(g_sess, s.uid, got) == fb::OK && !got.name.empty()) p = got;
            fb::putProfile(g_sess, p);   // creates the card, or stamps "last seen" on an existing one
        }
        return [err, s, p]() {
            if (err.empty()) {
                SetWindowTextW(g_edPass, L"");
                enterSignedIn(s, p, false);
            } else {
                g_loginBusy = false;
                g_loginErr = true;
                g_loginMsg = widen(err);
                showLoginEdits(true);
                invalidate();
            }
        };
    });
}

static void resumeSession() {
    fb::Session s;
    if (!fb::loadSession(sessionPath(), s)) return;
    g_loginBusy = true;
    g_loginErr = false;
    g_loginMsg = L"Welcome back. Checking your membership\x2026";
    netAsync([s]() mutable -> std::function<void()> {
        std::string msg;
        fb::Err e = fb::refresh(s, &msg);
        fb::Profile p;
        p.name = s.name;
        if (e == fb::OK) {
            g_sess = s;
            fb::saveSession(sessionPath(), s);
            fb::Profile got;
            if (fb::getProfile(g_sess, s.uid, got) == fb::OK && !got.name.empty()) {
                p = got;
                fb::putProfile(g_sess, p);   // stamps "last seen"
            }
        }
        return [e, s, p, msg]() {
            if (e == fb::OK) enterSignedIn(s, p, false);
            else if (e == fb::NET) enterSignedIn(s, p, true);   // members may keep playing the ladder offline
            else {
                fb::clearSession(sessionPath());
                g_loginBusy = false;
                g_loginErr = true;
                g_loginMsg = widen(msg);
                showLoginEdits(true);
                invalidate();
            }
        };
    });
}

static void signOut() {
    if (g_hosting) {
        std::string id = g_hostDoc.id;
        netAsync([id]() -> std::function<void()> { fb::deleteGame(g_sess, id); return nullptr; });
        g_hosting = false;
    }
    netAsync([]() -> std::function<void()> {
        g_sess = fb::Session();
        fb::clearSession(sessionPath());
        return nullptr;
    });
    g_myUid.clear();
    g_myName.clear();
    g_loginMsg = g_guest ? L"Thanks for visiting." : L"Signed out. See you at the next club night.";
    g_guest = false;
    g_plan = 0;
    g_founder = false;
    g_profile = fb::Profile();
    socialReset();
    g_loginErr = false;
    g_loginBusy = false;
    goScreen(SCR_LOGIN);
}

static void refreshLobby() {
    if (g_pollBusy) return;
    g_pollBusy = true;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::GameDoc> list;
        fb::Err e = fb::listOpenGames(g_sess, list);
        return [e, list]() {
            g_pollBusy = false;
            g_lobbyLoaded = true;
            if (e == fb::OK) {
                g_openTables.clear();
                long long now = fb::nowMs();
                for (auto& d : list)
                    if (d.createdBy != g_myUid && now - d.createdAt < 2LL * 3600 * 1000 && !isBlocked(d.createdBy) &&
                        (d.invite == g_myUid || (d.invite.empty() && g_plan >= 1)))   // Bronze and guests: invitations only
                        g_openTables.push_back(d);
                if (g_lobbyMsg.find(L"reach") != std::wstring::npos) g_lobbyMsg.clear();
            } else {
                g_lobbyMsg = e == fb::NET ? L"Can't reach the club's server." : L"The online room isn't open yet (server said no).";
            }
            if (g_screen == SCR_LOBBY) invalidate();
        };
    });
}

static void openLobby() {
    if (g_offline) {
        // try to get back online first
        fb::Session s;
        fb::loadSession(sessionPath(), s);
        netAsync([s]() mutable -> std::function<void()> {
            fb::Err e = fb::refresh(s);
            if (e == fb::OK) { g_sess = s; fb::saveSession(sessionPath(), s); }
            return [e]() {
                if (e == fb::OK) { g_offline = false; g_lobbyLoaded = false; goScreen(SCR_LOBBY); refreshLobby(); }
                else { g_lobbyMsg = L"Still offline. The online room needs an internet connection."; invalidate(); }
            };
        });
        return;
    }
    g_lobbyLoaded = false;
    goScreen(SCR_LOBBY);
    refreshLobby();
}

static void hostTable() {
    if (g_hosting || g_offline) return;
    fb::GameDoc d;
    int mine = g_colorPref == 0 ? WHITE : g_colorPref == 2 ? BLACK : (int)(g_rng() & 1);
    (mine == WHITE ? d.white : d.black) = g_myUid;
    (mine == WHITE ? d.whiteName : d.blackName) = g_myName;
    (mine == WHITE ? d.whiteRating : d.blackRating) = g_profile.rating;
    d.createdBy = g_myUid;
    d.status = "open";
    d.baseMs = TCS[g_tcIndex].minutes * 60000LL;
    d.incMs = TCS[g_tcIndex].inc * 1000LL;
    d.whiteMs = d.blackMs = d.baseMs;
    d.createdAt = fb::nowMs();
    if (g_inviteIdx > 0 && g_inviteIdx <= (int)g_friends.size()) d.invite = g_friends[g_inviteIdx - 1].uid;
    g_hosting = true;
    g_hostDoc = d;
    g_lobbyMsg.clear();
    invalidate();
    netAsync([d]() mutable -> std::function<void()> {
        fb::Err e = fb::createGame(g_sess, d);
        return [e, d]() {
            if (e == fb::OK) g_hostDoc = d;
            else {
                g_hosting = false;
                g_lobbyMsg = L"Couldn't open a table right now.";
            }
            invalidate();
        };
    });
}

static void unhostTable() {
    if (!g_hosting) return;
    std::string id = g_hostDoc.id;
    g_hosting = false;
    if (!id.empty()) netAsync([id]() -> std::function<void()> { fb::deleteGame(g_sess, id); return nullptr; });
    invalidate();
}

static void joinTable(const fb::GameDoc& t) {
    if (g_hosting) unhostTable();
    g_lobbyMsg = L"Pulling up a chair\x2026";
    invalidate();
    std::string id = t.id, me = g_myUid, name = g_myName;
    int rating = g_profile.rating;
    netAsync([id, me, name, rating]() -> std::function<void()> {
        fb::GameDoc d;
        fb::Err e = fb::getGame(g_sess, id, d);
        if (e == fb::OK && d.status == "open") {
            if (d.white.empty()) { d.white = me; d.whiteName = name; d.whiteRating = rating; }
            else { d.black = me; d.blackName = name; d.blackRating = rating; }
            d.status = "active";
            d.lastMoveAt = fb::nowMs();
            e = fb::updateGame(g_sess, d);
            if (e == fb::OK) g_latestUpdate[d.id] = d.updateTime;
        } else if (e == fb::OK) {
            e = fb::CONFLICT;
        }
        return [e, d]() {
            if (e == fb::OK) startOnlineGame(d);
            else {
                g_lobbyMsg = e == fb::CONFLICT ? L"Someone else sat down first." : L"Couldn't join that table.";
                refreshLobby();
                invalidate();
            }
        };
    });
}

// Bronze members and guests are seated with whoever is waiting; if nobody is, a table opens in their name.
static void quickMatch() {
    if (g_offline || g_hosting) return;
    g_lobbyMsg = L"Looking for an opponent\x2026";
    invalidate();
    std::string me = g_myUid;
    netAsync([me]() -> std::function<void()> {
        std::vector<fb::GameDoc> list;
        fb::Err e = fb::listOpenGames(g_sess, list);
        return [e, list, me]() {
            if (e != fb::OK) { g_lobbyMsg = L"Can't reach the club's server."; invalidate(); return; }
            std::vector<fb::GameDoc> ok;
            long long now = fb::nowMs();
            for (auto& d : list)
                if (d.createdBy != me && d.invite.empty() && now - d.createdAt < 2LL * 3600 * 1000 && !isBlocked(d.createdBy)) ok.push_back(d);
            if (!ok.empty()) { joinTable(ok[g_rng() % ok.size()]); return; }
            g_inviteIdx = 0;
            hostTable();
            g_lobbyMsg = L"Nobody is waiting, so a table is open in your name. You'll be seated when someone arrives.";
            invalidate();
        };
    });
}

// Called on TIMER_NET: lobby refresh, waiting for an opponent, or following an online game.
static void netTick() {
    g_netTick++;
    socialTick();
    competeTick();
    billingTick();
    cloudBrainTick();
    if (g_screen == SCR_LOBBY) {
        if (g_hosting && !g_hostDoc.id.empty() && !g_pollBusy) {
            g_pollBusy = true;
            std::string id = g_hostDoc.id;
            netAsync([id]() -> std::function<void()> {
                fb::GameDoc d;
                fb::Err e = fb::getGame(g_sess, id, d);
                if (e == fb::OK) g_latestUpdate[d.id] = d.updateTime;
                return [e, d]() {
                    g_pollBusy = false;
                    if (e == fb::OK && g_hosting && d.id == g_hostDoc.id && d.status == "active") startOnlineGame(d);
                };
            });
        } else if (g_netTick % 3 == 0) {
            refreshLobby();
        }
    } else if (g_screen == SCR_GAME && g.online && !g.over && !g_pollBusy && g_netWrites == 0) {
        g_pollBusy = true;
        std::string id = g.doc.id;
        netAsync([id]() -> std::function<void()> {
            fb::GameDoc d;
            fb::Err e = fb::getGame(g_sess, id, d);
            if (e == fb::OK) g_latestUpdate[d.id] = d.updateTime;
            return [e, d]() {
                g_pollBusy = false;
                if (e == fb::OK) onlineSync(d);
            };
        });
    }
}

// ---------------------------------------------------------------- rendering: shared

static float rowH() { return S(44); }
static float maxScroll() { return max(0.f, (float)g_ladder.size() * rowH() - g_listRect.Height); }
static RectF ladderFilterRect() { return RectF(contentX() + S(18), contentY() + S(50), S(132), S(28)); }

// Applies the name / tier / beaten filters to the 150 bots.
static void rebuildLadder() {
    const auto& lv = allLevels();
    g_ladder.clear();
    for (int i = 0; i < 150; i++) {
        const Level& L = lv[i];
        if (g_filterTier >= 0 && L.tier != g_filterTier) continue;
        if (g_filterStatus == 1 && g_rec[i].w > 0) continue;
        if (g_filterStatus == 2 && g_rec[i].w == 0) continue;
        if (!g_filterText.empty()) {
            std::string hay = L.name + " " + std::to_string(L.rating) + " " + L.title;
            for (char& c : hay) c = (char)tolower((unsigned char)c);
            if (hay.find(g_filterText) == std::string::npos) continue;
        }
        g_ladder.push_back(i);
    }
    g_listDirty = true;
    g_menuScroll = max(0.f, min(maxScroll(), g_menuScroll));
    g_scrollTarget = max(0.f, min(maxScroll(), g_scrollTarget));
}

static void ensureVisible(int level, bool instant = false) {
    int i = (int)(std::find(g_ladder.begin(), g_ladder.end(), level) - g_ladder.begin());
    if (i >= (int)g_ladder.size()) return;   // filtered out
    float top = i * rowH(), bot = top + rowH();
    if (top < g_scrollTarget) g_scrollTarget = top;
    if (bot > g_scrollTarget + g_listRect.Height) g_scrollTarget = bot - g_listRect.Height;
    g_scrollTarget = max(0.f, min(maxScroll(), g_scrollTarget));
    if (instant) g_menuScroll = g_scrollTarget;
    else startAnimTimer();
}

static void drawTable(Graphics& gr) {
    RectF all(0, 0, (REAL)g_W, (REAL)g_H);
    if (g_bgImg) {
        gr.SetInterpolationMode(InterpolationModeBilinear);
        gr.DrawImage(g_bgImg, all);
    } else {
        fillTexture(gr, g_felt, all);
    }
    // lamp-light vignette
    GraphicsPath p;
    p.AddEllipse(RectF(-g_W * 0.25f, -g_H * 0.3f, g_W * 1.5f, g_H * 1.6f));
    PathGradientBrush pg(&p);
    pg.SetCenterColor(Color(0, 0, 0, 0));
    Color edge(g_bgImg ? 150 : 120, 6, 12, 8);
    int n = 1;
    pg.SetSurroundColors(&edge, &n);
    gr.FillRectangle(&pg, all);
}

// "Chezz" on the felt, plus who is signed in and the room buttons.
#include "icons.inl"
#include "cloudbrain.inl"   // the club brain, kept on the server

// ---------------------------------------------------------------- rendering: membership desk (login)

static RectF loginForm() {
    float w = min(S(560), g_W - S(80)), h = min(S(680), g_H - S(40));
    return RectF((g_W - w) / 2, (g_H - h) / 2, w, h);
}
static RectF loginField(int i) {   // 0 email, 1 password, 2 ladder name
    RectF f = loginForm();
    float y = f.Y + S(196) + i * S(76);
    return RectF(f.X + S(48), y + S(22), f.Width - S(96), S(34));
}

static void renderLogin(Graphics& gr) {
    RectF f = loginForm();
    paperSheet(gr, f, true);
    {
        Pen red(Color(170, 190, 60, 50), S(1.5f));
        gr.DrawLine(&red, f.X + 1, f.Y + S(118), f.X + f.Width - 1, f.Y + S(118));
    }
    drawText(gr, L"Chezz Chess Club", RectF(f.X, f.Y + S(22), f.Width, S(48)), F_SERIF, S(32), C_PRINT, FontStyleBold, 1, 1);
    drawText(gr, L"Members only. Please sign the register.", RectF(f.X, f.Y + S(70), f.Width, S(34)), F_HAND, S(15), C_INK,
             0, 1, 1);

    // tabs
    float tw = (f.Width - S(96)) / 2, ty = f.Y + S(132);
    for (int k = 0; k < 2; k++) {
        RectF tr(f.X + S(48) + k * tw, ty, tw, S(36));
        bool active = g_loginMode == k, hover = tr.Contains(g_mouseX, g_mouseY);
        drawText(gr, k == 0 ? L"I'M A MEMBER" : L"NEW MEMBER", tr, F_SERIF, S(14), active ? C_PRINT : hover ? C_INK : C_FAINT,
                 FontStyleBold, 1, 1);
        if (active) {
            Pen u(C_RED, S(2));
            gr.DrawLine(&u, tr.X + S(30), tr.Y + tr.Height, tr.X + tr.Width - S(30), tr.Y + tr.Height - S(1));
        }
        if (!g_loginBusy) g_buttons.push_back({tr, k == 0 ? B_MODE_IN : B_MODE_UP});
    }

    const wchar_t* labels[3] = {L"EMAIL", L"PASSWORD", L"NAME ON THE LADDER"};
    int nf = g_loginMode == 1 ? 3 : 2;
    for (int i = 0; i < nf; i++) {
        RectF r = loginField(i);
        drawText(gr, labels[i], RectF(r.X, r.Y - S(22), r.Width, S(20)), F_MONO, S(12), C_FAINT, 0, 0, 1);
        SolidBrush box(C_CARD);
        gr.FillRectangle(&box, r);
        Pen line(C_PRINT, 1.f);
        gr.DrawLine(&line, r.X, r.Y + r.Height, r.X + r.Width, r.Y + r.Height);
    }
    float sy = loginField(nf - 1).Y + S(58);
    RectF stamp(f.X + (f.Width - S(300)) / 2, sy, S(300), S(64));
    stampButton(gr, B_SUBMIT, stamp, g_loginMode ? L"JOIN THE CLUB" : L"SIGN IN", L"", !g_loginBusy);

    // the visitors' book: no account, online rooms only
    RectF guest(f.X + S(40), sy + S(76), f.Width - S(80), S(26));
    bool gh = !g_loginBusy && guest.Contains(g_mouseX, g_mouseY);
    std::wstring gl = L"or sign the visitors' book and play as a guest (online rooms only)";
    drawText(gr, gl, guest, F_HAND, S(13), gh ? C_RED : C_INK, 0, 1, 1);
    if (gh) {
        Pen u(C_RED, 1.f);
        gr.DrawLine(&u, guest.X + S(40), guest.Y + guest.Height - S(2), guest.X + guest.Width - S(40), guest.Y + guest.Height - S(2));
    }
    if (!g_loginBusy) g_buttons.push_back({guest, B_GUEST});

    if (!g_loginMsg.empty())
        drawText(gr, g_loginMsg, RectF(f.X + S(40), sy + S(106), f.Width - S(80), S(50)), F_HAND, S(14),
                 g_loginErr ? C_RED : C_INK, 0, 1, 0, true);
    if (sy + S(160) < f.Y + f.Height - S(48))   // small print only when it doesn't crowd the messages
        drawText(gr, L"Members play the 150-bot ladder and keep an online rating. Guests can sit at online tables.",
                 RectF(f.X + S(30), f.Y + f.Height - S(48), f.Width - S(60), S(40)), F_SERIF, S(12), C_FAINT,
                 FontStyleItalic, 1, 1, true);
}

// ---------------------------------------------------------------- rendering: ladder (menu)

static void renderMenu(Graphics& gr) {
    const auto& lv = allLevels();
    drawTopBar(gr, L"chess club  \x00B7  one hundred and fifty members, one shared brain");

    // the ladder sheet: heading and filters here; the rows come from a cached strip (renderLadderCache)
    RectF sheet(contentX(), contentY(), min(S(480), contentW() * 0.45f), (REAL)g_H - contentY() - S(24));
    paperSheet(gr, sheet);
    drawText(gr, L"THE LADDER", RectF(sheet.X + S(18), sheet.Y + S(8), S(200), S(30)), F_SERIF, S(15), C_PRINT, FontStyleBold);
    drawText(gr, g_ladder.size() == 150 ? std::wstring(L"rating") : std::to_wstring(g_ladder.size()) + L" of 150 shown",
             RectF(sheet.X, sheet.Y + S(8), sheet.Width - S(18), S(30)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    {
        Pen dbl(C_PRINT, 1.f);
        gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(40), sheet.X + sheet.Width - S(12), sheet.Y + S(40));
        gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(43), sheet.X + sheet.Width - S(12), sheet.Y + S(43));
    }
    {   // filters: name or rating, tier, beaten or not
        RectF fr = ladderFilterRect();
        SolidBrush box(C_CARD);
        gr.FillRectangle(&box, fr);
        Pen line(C_PRINT, 1.f);
        gr.DrawLine(&line, fr.X, fr.Y + fr.Height, fr.X + fr.Width, fr.Y + fr.Height);
        if (g_filterText.empty() && GetFocus() != g_edGen[0])
            drawText(gr, L"name or rating", RectF(fr.X + S(6), fr.Y, fr.Width, fr.Height), F_SERIF, S(12), C_FAINT, FontStyleItalic);
        float bs = S(26), sw = S(84), right = sheet.X + sheet.Width - S(14);
        float fx = fr.X + fr.Width + S(8), tw = right - sw - S(8) - fx - 2 * bs;
        inkButton(gr, B_FILT_TIER_DN, RectF(fx, fr.Y + S(1), bs, bs), L"\x2039");
        drawText(gr, g_filterTier < 0 ? std::wstring(L"all tiers") : widen(lv[g_filterTier * 10].title), RectF(fx + bs, fr.Y, tw, fr.Height),
                 F_HAND, S(13), C_INK, FontStyleRegular, 1, 1);
        inkButton(gr, B_FILT_TIER_UP, RectF(fx + bs + tw, fr.Y + S(1), bs, bs), L"\x203A");
        static const wchar_t* ST[3] = {L"ALL", L"TO BEAT", L"BEATEN"};
        inkButton(gr, B_FILT_STATUS, RectF(right - sw, fr.Y + S(1), sw, bs), ST[g_filterStatus]);
    }
    g_listRect = RectF(floorf(sheet.X + S(4)), floorf(sheet.Y + S(88)), floorf(sheet.Width - S(8)), floorf(sheet.Height - S(94)));
    g_menuScroll = max(0.f, min(maxScroll(), g_menuScroll));
    g_scrollTarget = max(0.f, min(maxScroll(), g_scrollTarget));
    if (g_ladder.empty())
        drawText(gr, L"Nobody matches. Clear the filters.", RectF(g_listRect.X, g_listRect.Y + S(20), g_listRect.Width, S(40)), F_HAND,
                 S(15), C_INK, FontStyleRegular, 1, 1);

    // the member card
    const Level& L = lv[g_sel];
    float x0 = sheet.X + sheet.Width + S(24), w = g_W - x0 - S(24);
    RectF card(x0, contentY(), w, (REAL)g_H - contentY() - S(24));
    paperSheet(gr, card, true);
    float pad = S(22), ix = x0 + pad, iw = w - 2 * pad;
    {
        Pen blue(C_LEDGER, 1.f);
        for (float ly = card.Y + S(86); ly < card.Y + card.Height - S(10); ly += S(28))
            gr.DrawLine(&blue, card.X + 1, ly, card.X + card.Width - 1, ly);
        Pen red(Color(170, 190, 60, 50), S(1.5f));
        gr.DrawLine(&red, card.X + 1, card.Y + S(52), card.X + card.Width - 1, card.Y + S(52));
    }
    wchar_t buf[256];
    swprintf(buf, 256, L"MEMBER No. %03d", L.number);
    drawText(gr, buf, RectF(ix, card.Y + S(10), iw, S(36)), F_MONO, S(16), C_PRINT, FontStyleBold);
    drawText(gr, widen(L.title), RectF(ix, card.Y + S(10), iw, S(36)), F_SERIF, S(15), C_RED, FontStyleItalic, 2);

    float y = card.Y + S(66);
    float ph = min(S(130), iw * 0.3f);
    drawPortrait(gr, g_sel, RectF(ix + S(4), y, ph, ph), true);
    float fx = ix + ph + S(28), fw = iw - ph - S(28);
    auto field = [&](float fy, const wchar_t* label, const std::wstring& value, const Color& c) {
        drawText(gr, label, RectF(fx, fy, S(90), S(28)), F_MONO, S(12), C_FAINT, FontStyleRegular, 0, 2);
        drawText(gr, value, RectF(fx + S(86), fy - S(4), fw - S(86), S(32)), F_MONO, S(19), c, FontStyleBold, 0, 2);
    };
    const Record& rc = g_rec[g_sel];
    field(y, L"NAME", widen(L.name), C_PRINT);
    swprintf(buf, 256, L"%d", L.rating);
    field(y + S(34), L"RATING", buf, C_PRINT);
    swprintf(buf, 256, L"%d-%d-%d", rc.w, rc.d, rc.l);
    field(y + S(68), L"W-D-L", buf, rc.w ? C_RED : C_PRINT);
    y += ph + S(14);

    drawText(gr, L"Notes:", RectF(ix, y, S(80), S(26)), F_MONO, S(12), C_FAINT, FontStyleRegular, 0, 2);
    drawText(gr, widen(L.blurb), RectF(ix + S(64), y - S(2), iw - S(64), S(70)), F_HAND, S(14), C_INK, FontStyleRegular, 0, 0, true);
    y += S(76);

    colourTokens(gr, ix, y, L"YOU PLAY");
    y += S(84);
    tcChooser(gr, ix, y, iw);
    y += S(46);

    stampButton(gr, B_PLAY, RectF(ix + S(6), y, min(iw - S(12), S(330)), S(72)), L"CHALLENGE",
                widen(L.name) + L"  \x00B7  board one");
    y += S(90);

    checkbox(gr, B_LEARN, ix + S(6), y, g_learn, L"learn from my games");
    y += S(26);
    checkbox(gr, B_THOUGHTS, ix + S(6), y, g_showThoughts, L"show what the bot is considering");
    y += S(30);
    auto habits = g_brain.patterns();
    if (brainCloudOn() || g_brainVersion > 0)
        swprintf(buf, 256, L"club brain: version %d, %d games from all members", g_brainVersion, g_brainServerGames);
    else
        swprintf(buf, 256, L"brain on this computer: %d games studied, %zu positions", g_brain.games(), g_brain.positions());
    drawText(gr, buf, RectF(ix + S(6), y, iw, S(22)), F_MONO, S(12), C_FAINT);
    y += S(20);
    swprintf(buf, 256, L"from you: %d games, %zu positions, %zu moves", g_brain.games(), g_brain.positions(), g_brain.yourMoves());
    drawText(gr, buf, RectF(ix + S(6), y, iw, S(22)), F_MONO, S(12), C_FAINT);
    y += S(20);
    if (habits && habits->ready()) {
        swprintf(buf, 256, L"predicts your next move %d%% of the time (top 3: %d%%)", (int)lround(habits->acc1 * 100),
                 (int)lround(habits->acc3 * 100));
        drawText(gr, buf, RectF(ix + S(6), y, iw, S(22)), F_MONO, S(12), C_FAINT);
        y += S(20);
    }
    if (g_brain.busy()) drawText(gr, L"studying your last game\x2026", RectF(ix + S(6), y, iw, S(22)), F_HAND, S(13), C_RED);
    else if (g_brainBusy) drawText(gr, L"syncing with the club brain\x2026", RectF(ix + S(6), y, iw, S(22)), F_HAND, S(13), C_RED);
    else if (!g_brainMsg.empty()) drawText(gr, g_brainMsg, RectF(ix + S(6), y, iw, S(40)), F_HAND, S(13), C_INK, 0, 0, 0, true);
    else if (!g_lobbyMsg.empty() && g_offline) drawText(gr, g_lobbyMsg, RectF(ix + S(6), y, iw, S(22)), F_HAND, S(13), C_RED);
    else if (habits) {
        // what the club has noticed about you, in pencil
        auto traits = habits->traits();
        std::wstring t;
        for (size_t i = 0; i < traits.size() && i < 3; i++) t += (i ? L"; " : L"You ") + widen(traits[i]);
        if (t.empty() && habits->trained < 150)
            t = L"Still getting to know you (" + std::to_wstring(habits->trained) + L" of 150 moves).";
        else if (!t.empty()) t += L".";
        drawText(gr, t, RectF(ix + S(6), y, iw, S(44)), F_HAND, S(13), C_INK, 0, 0, 0, true);
    }
}

// ---------------------------------------------------------------- rendering: online room

static void renderLobby(Graphics& gr) {
    drawTopBar(gr, L"the online room  \x00B7  sit down with another member");
    RectF sheet(contentX(), contentY(), min(S(540), contentW() * 0.5f), (REAL)g_H - contentY() - S(24));
    paperSheet(gr, sheet);
    drawText(gr, L"OPEN TABLES", RectF(sheet.X + S(18), sheet.Y + S(8), S(240), S(30)), F_SERIF, S(15), C_PRINT, FontStyleBold);
    drawText(gr, L"refreshes by itself", RectF(sheet.X, sheet.Y + S(8), sheet.Width - S(18), S(30)), F_SERIF, S(12), C_FAINT,
             FontStyleItalic, 2);
    Pen dbl(C_PRINT, 1.f);
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(40), sheet.X + sheet.Width - S(12), sheet.Y + S(40));
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(43), sheet.X + sheet.Width - S(12), sheet.Y + S(43));
    float y = sheet.Y + S(52), rh = S(58);
    if (!g_lobbyLoaded) {
        drawText(gr, L"Looking around the room\x2026", RectF(sheet.X, y + S(20), sheet.Width, S(40)), F_HAND, S(15), C_INK, 0, 1, 1);
    } else if (g_openTables.empty()) {
        drawText(gr, g_plan >= 1 ? L"No open tables at the moment.\nOpen one yourself and wait for a challenger."
                                 : L"Tables your friends open for you appear here.\nThe stamp below seats you with a random opponent.",
                 RectF(sheet.X + S(30), y + S(20), sheet.Width - S(60), S(80)), F_HAND, S(15), C_INK, 0, 1, 0, true);
    }
    int maxRows = (int)((sheet.Y + sheet.Height - y - S(100)) / rh);
    for (int i = 0; i < (int)g_openTables.size() && i < maxRows; i++) {
        const fb::GameDoc& t = g_openTables[i];
        float ry = y + i * rh;
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, sheet.X + S(10), ry + rh - 1, sheet.X + sheet.Width - S(10), ry + rh - 1);
        bool hostWhite = !t.white.empty();
        std::wstring host = widen(hostWhite ? t.whiteName : t.blackName);
        int hr = hostWhite ? t.whiteRating : t.blackRating;
        float ic = rh - S(18);
        gr.DrawImage(pieceBitmap(mkpiece(hostWhite ? WHITE : BLACK, KING), (int)ic, 0.85f, false),
                     RectF(sheet.X + S(16), ry + S(9), ic, ic));
        drawText(gr, host, RectF(sheet.X + S(24) + ic, ry + S(4), S(220), rh * 0.55f), F_SERIF, S(16), C_PRINT, FontStyleBold, 0, 2);
        drawText(gr, (t.invite == g_myUid ? L"invited you  \x00B7  " : L"") + (L"rated " + std::to_wstring(hr)) + L"  \x00B7  plays " + (hostWhite ? L"white" : L"black"),
                 RectF(sheet.X + S(24) + ic, ry + rh * 0.55f, S(260), rh * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
        drawText(gr, tcLabel(t.baseMs, t.incMs), RectF(sheet.X + sheet.Width - S(250), ry, S(100), rh), F_HAND, S(15), C_INK, 0, 1, 1);
        inkButton(gr, B_JOIN + i, RectF(sheet.X + sheet.Width - S(136), ry + S(12), S(116), rh - S(24)), L"SIT DOWN");
    }

    // everyone can be seated with a random opponent; choosing from the list is for Silver and up
    stampButton(gr, B_QUICK, RectF(sheet.X + S(22), sheet.Y + sheet.Height - S(82), S(300), S(60)), L"RANDOM OPPONENT", L"", !g_hosting);
    if (g_plan < 1) {
        drawBadge(gr, 1, RectF(sheet.X + S(340), sheet.Y + sheet.Height - S(64), S(22), S(22)));
        drawText(gr, L"Silver members choose from everyone waiting.", RectF(sheet.X + S(368), sheet.Y + sheet.Height - S(84), sheet.Width - S(384), S(60)),
                 F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 1, true);
    }

    // hosting card
    float x0 = sheet.X + sheet.Width + S(24), w = g_W - x0 - S(24);
    RectF card(x0, contentY(), w, (REAL)g_H - contentY() - S(24));
    paperSheet(gr, card, true);
    float pad = S(22), ix = x0 + pad, iw = w - 2 * pad;
    {
        Pen red(Color(170, 190, 60, 50), S(1.5f));
        gr.DrawLine(&red, card.X + 1, card.Y + S(52), card.X + card.Width - 1, card.Y + S(52));
    }
    drawText(gr, L"YOUR MEMBERSHIP", RectF(ix, card.Y + S(10), iw, S(36)), F_MONO, S(16), C_PRINT, FontStyleBold);
    float yy = card.Y + S(70);
    drawText(gr, widen(g_myName), RectF(ix, yy, iw, S(36)), F_SERIF, S(26), C_PRINT, FontStyleBold);
    wchar_t buf[128];
    swprintf(buf, 128, L"rating %d   \x00B7   %d-%d-%d", g_profile.rating, g_profile.wins, g_profile.draws, g_profile.losses);
    drawText(gr, buf, RectF(ix, yy + S(38), iw, S(26)), F_MONO, S(15), C_PRINT);
    yy += S(90);
    drawText(gr, L"HOST A TABLE", RectF(ix, yy, iw, S(28)), F_SERIF, S(15), C_PRINT, FontStyleBold);
    yy += S(36);
    if (!g_hosting) {
        colourTokens(gr, ix, yy, L"YOU PLAY");
        yy += S(84);
        tcChooser(gr, ix, yy, iw);
        yy += S(44);
        {   // who the table is for: anyone, or one friend
            if (g_inviteIdx > (int)g_friends.size()) g_inviteIdx = 0;
            drawText(gr, L"FOR", RectF(ix, yy, S(100), S(34)), F_MONO, S(12), C_FAINT, FontStyleRegular, 0, 1);
            inkButton(gr, B_INVITE_PREV, RectF(ix + S(100), yy + S(2), S(30), S(30)), L"\x2039", !g_friends.empty());
            drawText(gr, g_inviteIdx == 0 ? std::wstring(L"anyone") : widen(g_friends[g_inviteIdx - 1].name),
                     RectF(ix + S(130), yy, min(S(130), iw - S(160)), S(34)), F_HAND, S(16), C_INK, FontStyleRegular, 1, 1);
            inkButton(gr, B_INVITE_NEXT, RectF(ix + S(130) + min(S(130), iw - S(160)), yy + S(2), S(30), S(30)), L"\x203A", !g_friends.empty());
        }
        yy += S(50);
        stampButton(gr, B_HOST, RectF(ix + S(6), yy, min(iw - S(12), S(330)), S(68)), L"OPEN A TABLE", TCS[g_tcIndex].label);
        yy += S(86);
    } else {
        drawText(gr, L"Your table is open (" + tcLabel(g_hostDoc.baseMs, g_hostDoc.incMs) +
                     L").\nWaiting for someone to sit down\x2026",
                 RectF(ix, yy, iw, S(70)), F_HAND, S(16), C_INK, 0, 0, 0, true);
        yy += S(84);
        inkButton(gr, B_UNHOST, RectF(ix, yy, S(220), S(40)), L"TAKE DOWN TABLE");
        yy += S(60);
    }
    if (!g_lobbyMsg.empty())
        drawText(gr, g_lobbyMsg, RectF(ix, yy, iw, S(48)), F_HAND, S(14), C_RED, 0, 0, 0, true);
}

#include "plans.inl"
#include "social.inl"
#include "settings_ui.inl"
#include "engines.inl"
#include "puzzles.inl"
#include "compete.inl"

// ---------------------------------------------------------------- rendering: board

// Wood, squares and coordinates: cached in the bg layer.
static void renderBoardBase(Graphics& gr) {
    Geom b = boardGeom();
    RectF br(b.x, b.y, b.size, b.size);
    RectF fr(b.x - b.frame, b.y - b.frame, b.size + 2 * b.frame, b.size + 2 * b.frame);
    dropShadow(gr, fr, S(10));
    fillTexture(gr, g_walnut, fr);
    {
        Pen outer(Color(255, 30, 18, 12), S(1.5f)), bevel(Color(90, 255, 230, 190), 1.f), inner(Color(255, 34, 20, 13), S(2));
        gr.DrawRectangle(&outer, fr);
        gr.DrawRectangle(&bevel, RectF(fr.X + 2, fr.Y + 2, fr.Width - 4, fr.Height - 4));
        gr.DrawRectangle(&inner, RectF(br.X - 1, br.Y - 1, br.Width + 2, br.Height + 2));
    }
    gr.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    if (g_useBoardImage && g_boardImg) {
        gr.DrawImage(g_boardImg, br);
    } else {
        for (int s = 0; s < 64; s++) {
            RectF r = squareRect(s);
            bool light = (sqfile(s) + sqrank(s)) & 1;
            Bitmap* tex = g_useBoardImage ? (light ? g_lightImg : g_darkImg) : nullptr;
            if (tex) gr.DrawImage(tex, r);
            else fillTexture(gr, light ? g_maple : g_darkwood, r, (float)((s * 37) % 256), (float)((s * 91) % 256));
        }
    }
    // coordinates live on the frame, like a real board
    float cs = max(S(10), min(S(14), b.frame * 0.5f));
    Color cc(200, 236, 214, 170);
    for (int i = 0; i < 8 && g_theme.coords; i++) {
        int file = g.flipped ? 7 - i : i, rank = g.flipped ? i : 7 - i;
        wchar_t fl[2] = {(wchar_t)(L'a' + file), 0}, rk[2] = {(wchar_t)(L'1' + rank), 0};
        drawText(gr, fl, RectF(b.x + i * b.sq, b.y + b.size, b.sq, b.frame), F_SERIF, cs, cc, FontStyleBold, 1, 1);
        drawText(gr, rk, RectF(b.x - b.frame, b.y + i * b.sq, b.frame, b.sq), F_SERIF, cs, cc, FontStyleBold, 1, 1);
    }
}

static void renderBoard(Graphics& gr) {
    Geom b = boardGeom();
    RectF br(b.x, b.y, b.size, b.size);
    if (g.last && g_theme.lastMove) {
        SolidBrush hl(Color(95, 236, 170, 40));
        gr.FillRectangle(&hl, squareRect(mfrom(g.last)));
        gr.FillRectangle(&hl, squareRect(mto(g.last)));
    }
    for (int s : {g.preFrom, g.preTo}) {   // the queued move, waiting its turn
        if (s < 0) continue;
        SolidBrush pre(Color(120, 92, 150, 230));
        gr.FillRectangle(&pre, squareRect(s));
    }
    if (g.preFrom >= 0 && g.preTo < 0 && !g.dragging) {
        Geom bb = boardGeom();
        SolidBrush dot(Color(80, 92, 150, 230));
        for (Move pm : g.preTargets) {
            RectF r = squareRect(mto(pm));
            gr.FillEllipse(&dot, r.X + bb.sq * 0.39f, r.Y + bb.sq * 0.39f, bb.sq * 0.22f, bb.sq * 0.22f);
        }
    }
    if (g.selected >= 0) {
        RectF r = squareRect(g.selected);
        SolidBrush sb(Color(70, 255, 245, 200));
        gr.FillRectangle(&sb, r);
        Pen p(Color(200, 60, 40, 20), S(2));
        gr.DrawRectangle(&p, RectF(r.X + S(1), r.Y + S(1), r.Width - S(2), r.Height - S(2)));
    }
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    if (g.puzzle && g.puzHint >= 0) {   // the hint: circle the piece to move
        RectF r = squareRect(g.puzHint);
        Pen ring(C_RED, S(3));
        gr.DrawEllipse(&ring, r.X + S(4), r.Y + S(4), r.Width - S(8), r.Height - S(8));
    }
    if (g.pos.inCheck()) {
        RectF r = squareRect(g.pos.king[g.pos.side]);
        GraphicsPath p;
        p.AddEllipse(r);
        PathGradientBrush pg(&p);
        pg.SetCenterColor(Color(210, 220, 30, 20));
        Color edge(0, 220, 30, 20);
        int cnt = 1;
        pg.SetSurroundColors(&edge, &cnt);
        gr.FillPath(&pg, &p);
    }
    int px = (int)b.sq;
    for (int s = 0; s < 64; s++) {
        int p = g.pos.sq[s];
        if (!p || (g.dragging && (s == g.selected || s == g.preFrom)) || squareGliding(s)) continue;
        if (g.promoOpen && s == g.promoFrom) continue;
        gr.DrawImage(pieceBitmap(p, px, g_pieceFill, true), squareRect(s));
    }
    for (Move m : g.targets) {
        if (!g_theme.dots) break;
        RectF r = squareRect(mto(m));
        if (g.pos.sq[mto(m)] || (m & MF_EP)) {
            Pen ring(Color(120, 40, 22, 12), b.sq * 0.06f);
            gr.DrawEllipse(&ring, r.X + b.sq * 0.08f, r.Y + b.sq * 0.08f, b.sq * 0.84f, b.sq * 0.84f);
        } else {
            SolidBrush dot(Color(105, 40, 22, 12));
            gr.FillEllipse(&dot, r.X + b.sq * 0.39f, r.Y + b.sq * 0.39f, b.sq * 0.22f, b.sq * 0.22f);
        }
    }
    if (g.promoOpen) {
        SolidBrush dim(Color(110, 0, 0, 0));
        gr.FillRectangle(&dim, br);
        RectF a = promoRect(0), z = promoRect(3);
        RectF strip(min(a.X, z.X), min(a.Y, z.Y), b.sq, b.sq * 4);
        paperSheet(gr, strip, true);
        for (int k = 0; k < 4; k++) {
            RectF r = promoRect(k);
            if (r.Contains(g_mouseX, g_mouseY)) {
                SolidBrush hl(C_HILITE);
                gr.FillRectangle(&hl, r);
            }
            gr.DrawImage(pieceBitmap(mkpiece(g.human, PROMO_TYPES[k]), px, g_pieceFill, false), r);
        }
    }
    if (g.over) {
        // a result slip tucked onto the board
        RectF slip(br.X + br.Width * 0.2f, br.Y + br.Height * 0.38f, br.Width * 0.6f, br.Height * 0.24f);
        GraphicsState s2 = gr.Save();
        gr.TranslateTransform(slip.X + slip.Width / 2, slip.Y + slip.Height / 2);
        gr.RotateTransform(-2.5f);
        RectF lr(-slip.Width / 2, -slip.Height / 2, slip.Width, slip.Height);
        paperSheet(gr, lr, true);
        std::wstring score = g.resultWhite > 0 ? L"1 \x2013 0" : g.resultWhite < 0 ? L"0 \x2013 1" : L"\x00BD \x2013 \x00BD";
        drawText(gr, score, RectF(lr.X, lr.Y + lr.Height * 0.06f, lr.Width, lr.Height * 0.42f), F_SERIF, lr.Height * 0.28f,
                 C_PRINT, FontStyleBold, 1, 1);
        drawText(gr, g.resultText + L" by " + g.reasonText, RectF(lr.X + S(8), lr.Y + lr.Height * 0.48f, lr.Width - S(16), lr.Height * 0.28f),
                 F_HAND, max(S(13), lr.Height * 0.11f), g.resultForHuman > 0 ? C_RED : C_INK, 0, 1, 1);
        if (!g.ratingText.empty())
            drawText(gr, g.ratingText, RectF(lr.X, lr.Y + lr.Height * 0.74f, lr.Width, lr.Height * 0.22f), F_MONO, S(13),
                     C_PRINT, 0, 1, 1);
        gr.Restore(s2);
    }
}

// ---------------------------------------------------------------- rendering: clock + scoresheet

static std::wstring fmtClock(long long ms, bool countdown) {
    if (ms < 0) ms = 0;
    wchar_t b[32];
    if (countdown && ms < 10000) {   // last ten seconds show tenths
        swprintf(b, 32, L"%lld.%lld", ms / 1000, (ms / 100) % 10);
        return b;
    }
    long long s = countdown ? (ms + 999) / 1000 : ms / 1000;
    if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%02lld:%02lld", s / 60, s % 60);
    return b;
}

static void renderClock(Graphics& gr, RectF r) {
    int opp = g.human ^ 1;
    bool humanTurn = !g.over && g.pos.side == g.human, oppTurn = !g.over && g.pos.side == opp;
    // plungers: the side to move has its button up
    float pw = r.Width * 0.16f;
    SolidBrush metal(Color(255, 58, 52, 48)), cap(Color(255, 196, 176, 132));
    for (int k = 0; k < 2; k++) {
        bool up = k == 0 ? humanTurn : oppTurn;
        float px = k == 0 ? r.X + r.Width * 0.2f : r.X + r.Width * 0.64f;
        float top = r.Y + (up ? 0 : S(8));
        gr.FillRectangle(&metal, RectF(px + pw * 0.35f, top + S(6), pw * 0.3f, r.Y + S(18) - top));
        gr.FillRectangle(&cap, RectF(px, top, pw, S(7)));
    }
    RectF body(r.X, r.Y + S(16), r.Width, r.Height - S(16));
    dropShadow(gr, body, S(6));
    fillTexture(gr, g_walnut, body);
    Pen edge(Color(255, 30, 18, 12), S(1.2f));
    gr.DrawRectangle(&edge, body);
    float fw = body.Width * 0.4f, fh = body.Height * 0.46f;
    for (int k = 0; k < 2; k++) {
        RectF face(k == 0 ? body.X + body.Width * 0.06f : body.X + body.Width * 0.54f, body.Y + S(12), fw, fh);
        SolidBrush lcd(Color(255, 26, 24, 20));
        gr.FillRectangle(&lcd, face);
        Pen bez(Color(255, 150, 130, 96), S(1.5f));
        gr.DrawRectangle(&bez, face);
        int c = k == 0 ? g.human : opp;
        bool running = k == 0 ? humanTurn : oppTurn;
        long long t = clockFor(c);
        Color digits = g.timed && t < 10000 ? Color(255, 255, 96, 72) : running ? C_LCD : Color(150, 244, 196, 112);
        drawText(gr, fmtClock(t, g.timed), face, F_MONO, fh * 0.55f, digits, FontStyleBold, 1, 1);
        std::wstring who = k == 0 ? std::wstring(L"you") : oppName();
        drawText(gr, who, RectF(face.X, face.Y + fh + S(2), fw, body.Height - fh - S(14)), F_SERIF, S(13), C_CREAM,
                 FontStyleItalic, 1, 1);
    }
    // the opponent's "thinking" lamp
    bool lit = g_thinking || (g.online && oppTurn);
    RectF lamp(body.X + body.Width / 2 - S(5), body.Y + S(14), S(10), S(10));
    SolidBrush lb(lit ? Color(255, 255, 90, 60) : Color(255, 70, 30, 24));
    gr.FillEllipse(&lb, lamp);
}

static void renderSheet(Graphics& gr, RectF sh) {
    paperSheet(gr, sh);
    float pad = S(16), ix = sh.X + pad, iw = sh.Width - 2 * pad, y = sh.Y + S(10);
    drawText(gr, L"CHEZZ CHESS CLUB", RectF(ix, y, iw, S(24)), F_SERIF, S(14), C_PRINT, FontStyleBold);
    if (g.predTotal > 0) {   // pencilled in the corner: how often the club brain saw your move coming
        std::wstring read = L"read " + std::to_wstring(g.predHits) + L" of " + std::to_wstring(g.predTotal) + L" of your moves";
        drawText(gr, read, RectF(ix, y - S(2), iw, S(26)), F_HAND, S(12), C_RED, FontStyleRegular, 2);
    } else {
        drawText(gr, L"scoresheet", RectF(ix, y, iw, S(24)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 2);
    }
    y += S(26);
    Pen print(C_PRINT, 1.f), rule(C_RULE, 1.f);
    gr.DrawLine(&print, ix, y, ix + iw, y);
    gr.DrawLine(&print, ix, y + 3, ix + iw, y + 3);
    y += S(8);

    // name fields, filled in by hand
    auto field = [&](float fx, float fy, float fw, const wchar_t* label, const std::wstring& value) {
        drawText(gr, label, RectF(fx, fy, fw, S(14)), F_SERIF, S(10), C_FAINT, FontStyleItalic, 0, 0);
        drawText(gr, value, RectF(fx + S(4), fy + S(6), fw - S(4), S(26)), F_HAND, S(15), C_INK, 0, 0, 1);
        gr.DrawLine(&rule, fx, fy + S(31), fx + fw - S(8), fy + S(31));
    };
    std::wstring me = g.online ? widen(g_myName) : std::wstring(L"Me"), opp = oppName();
    field(ix, y, iw / 2, L"White", g.human == WHITE ? me : opp);
    field(ix + iw / 2, y, iw / 2, L"Black", g.human == BLACK ? me : opp);
    y += S(36);
    std::wstring event = g.online ? L"Online, " + tcLabel(g.baseMs, g.incMs)
                 : g_customOn ? g_customEvent
                                  : L"Ladder board " + std::to_wstring(curLevel().number) + L", " + tcLabel(g.baseMs, g.incMs);
    field(ix, y, iw / 2, L"Event", event);
    field(ix + iw / 2, y, iw / 2, L"Opp. rating", std::to_wstring(oppRating()));
    y += S(42);

    // move table
    float btnArea = S(102), resH = S(40);
    RectF tbl(ix, y, iw, sh.Y + sh.Height - pad - btnArea - resH - y);
    float hh = S(20), lh = S(23), c0 = S(34), cw = (tbl.Width - c0) / 2;
    {
        SolidBrush head(Color(40, 43, 38, 32));
        gr.FillRectangle(&head, RectF(tbl.X, tbl.Y, tbl.Width, hh));
        drawText(gr, L"No.", RectF(tbl.X, tbl.Y, c0, hh), F_SERIF, S(10), C_PRINT, FontStyleBold, 1, 1);
        drawText(gr, L"WHITE", RectF(tbl.X + c0, tbl.Y, cw, hh), F_SERIF, S(10), C_PRINT, FontStyleBold, 1, 1);
        drawText(gr, L"BLACK", RectF(tbl.X + c0 + cw, tbl.Y, cw, hh), F_SERIF, S(10), C_PRINT, FontStyleBold, 1, 1);
    }
    wchar_t buf[128];
    int rows = max(1, (int)((tbl.Height - hh) / lh)), total = ((int)g.sans.size() + 1) / 2;
    int firstRow = max(0, total - rows);
    for (int r = 0; r < rows; r++) {
        float ry = tbl.Y + hh + r * lh;
        gr.DrawLine(&rule, tbl.X, ry + lh, tbl.X + tbl.Width, ry + lh);
        int n = firstRow + r;
        swprintf(buf, 128, L"%d", n + 1);
        drawText(gr, buf, RectF(tbl.X, ry, c0 - S(6), lh), F_SERIF, S(11), C_FAINT, 0, 2, 1);
        for (int k = 0; k < 2; k++) {
            size_t i = n * 2 + k;
            if (i >= g.sans.size()) break;
            bool latest = i + 1 == g.sans.size();
            drawText(gr, g.sans[i], RectF(tbl.X + c0 + k * cw + S(10), ry - S(3), cw - S(12), lh + S(2)), F_HAND, S(14),
                     latest ? C_RED : C_INK);
        }
    }
    gr.DrawRectangle(&print, tbl);
    gr.DrawLine(&print, tbl.X + c0, tbl.Y, tbl.X + c0, tbl.Y + tbl.Height);
    gr.DrawLine(&print, tbl.X + c0 + cw, tbl.Y, tbl.X + c0 + cw, tbl.Y + tbl.Height);
    gr.DrawLine(&print, tbl.X, tbl.Y + hh, tbl.X + tbl.Width, tbl.Y + hh);
    y = tbl.Y + tbl.Height + S(6);

    // result line
    drawText(gr, L"Result", RectF(ix, y, S(60), resH), F_SERIF, S(11), C_FAINT, FontStyleItalic, 0, 1);
    gr.DrawLine(&rule, ix + S(54), y + resH * 0.75f, ix + iw, y + resH * 0.75f);
    g_thinkLine = RectF(ix + S(64), y, iw - S(64), resH);
    if (g.over) {
        std::wstring score = g.resultWhite > 0 ? L"1\x2013" L"0" : g.resultWhite < 0 ? L"0\x2013" L"1" : L"\x00BD\x2013\x00BD";
        RectF sr(ix + S(64), y + S(2), S(56), resH - S(6));
        drawText(gr, score, sr, F_HAND, S(17), C_RED, FontStyleBold, 1, 1);
        inkCircle(gr, RectF(sr.X + S(4), sr.Y + S(6), sr.Width - S(8), sr.Height - S(10)), C_RED);
        drawText(gr, g.reasonText, RectF(ix + S(132), y, iw - S(132), resH), F_HAND, S(13), C_INK);
    } else if (g_thinking) {
        if (!g_showThoughts) drawText(gr, oppName() + L" is thinking\x2026", g_thinkLine, F_HAND, S(13), C_FAINT);
        // with thoughts on, the overlay writes the live depth/eval here
    } else if (g.online && g.pos.side != g.human) {
        drawText(gr, L"waiting for " + oppName() + L"\x2026", g_thinkLine, F_HAND, S(13), C_FAINT);
    } else {
        drawText(gr, L"your move", g_thinkLine, F_HAND, S(13), C_FAINT);
    }
    y += resH + S(8);

    float bw = (iw - S(10)) / 2, bh = S(38);
    if (g_confirm && !g.over) {
        drawText(gr, g_confirm == 2 ? L"Leaving concedes the game. Are you sure you want to resign?" : L"Are you sure you want to resign?",
                 RectF(ix, y - S(6), iw, bh + S(8)), F_HAND, S(15), C_RED, FontStyleRegular, 1, 1, true);
        inkButton(gr, B_CONFIRM_YES, RectF(ix, y + bh + S(8), bw, bh), L"YES, RESIGN");
        inkButton(gr, B_CONFIRM_NO, RectF(ix + bw + S(10), y + bh + S(8), bw, bh), L"KEEP PLAYING");
    } else if (!g.over) {
        inkButton(gr, B_UNDO, RectF(ix, y, bw, bh), L"TAKE BACK",
                  !g.online && g.pos.hist.size() >= 1 && !(g.pos.side == g.human && g.pos.hist.size() < 2));
        inkButton(gr, B_FLIP, RectF(ix + bw + S(10), y, bw, bh), L"TURN BOARD");
        inkButton(gr, B_RESIGN, RectF(ix, y + bh + S(8), bw, bh), L"RESIGN");
        inkButton(gr, B_MENU, RectF(ix + bw + S(10), y + bh + S(8), bw, bh), L"LEAVE TABLE");
    } else if (g.online) {
        inkButton(gr, B_ONLINE, RectF(ix, y, bw, bh), L"BACK TO THE ROOM");
        inkButton(gr, B_FLIP, RectF(ix + bw + S(10), y, bw, bh), L"TURN BOARD");
        if (g_guest) inkButton(gr, B_JOINCLUB, RectF(ix, y + bh + S(8), iw, bh), L"JOIN THE CLUB");
        else inkButton(gr, B_MENU, RectF(ix, y + bh + S(8), iw, bh), L"THE LADDER");
    } else {
        inkButton(gr, B_REMATCH, RectF(ix, y, bw, bh), L"REMATCH");
        inkButton(gr, B_NEXT, RectF(ix + bw + S(10), y, bw, bh), L"NEXT ON LADDER", g.level < 149 && !g_customOn);
        inkButton(gr, B_FLIP, RectF(ix, y + bh + S(8), bw, bh), L"TURN BOARD");
        inkButton(gr, B_MENU, RectF(ix + bw + S(10), y + bh + S(8), bw, bh), L"LEAVE TABLE");
    }
    std::wstring footer = g_brain.busy() ? L"the club brain is studying this game\x2026" : g.online ? g_lobbyMsg : L"";
    if (!footer.empty())
        drawText(gr, footer, RectF(sh.X, sh.Y + sh.Height + S(2), sh.Width, S(20)), F_SERIF, S(12), C_CREAM, FontStyleItalic, 2, 0);
}

// The piece counter: what each side has captured, and who is ahead on material (pawn 1, knight and
// bishop 3, rook 5, queen 9), like the tray of taken pieces beside a real board.
static void renderMaterial(Graphics& gr, RectF r) {
    paperSheet(gr, r);
    static const int START[7] = {0, 8, 2, 2, 2, 1, 1}, VAL[7] = {0, 1, 3, 3, 5, 9, 0};
    int cnt[2][7] = {}, mat[2] = {0, 0};
    for (int s = 0; s < 64; s++)
        if (int p = g.pos.sq[s]) {
            cnt[pcolor(p)][ptype(p)]++;
            mat[pcolor(p)] += VAL[ptype(p)];
        }
    float rowH = r.Height / 2, icon = min(S(22), rowH - S(4));
    GraphicsState st = gr.Save();
    gr.SetClip(r, CombineModeIntersect);
    for (int k = 0; k < 2; k++) {
        int side = k == 0 ? g.human : g.human ^ 1, enemy = side ^ 1;   // row k: what `side` has taken
        float y = r.Y + k * rowH;
        drawText(gr, k == 0 ? std::wstring(L"you took") : oppName() + L" took", RectF(r.X + S(10), y, S(112), rowH), F_SERIF,
                 S(11), C_FAINT, FontStyleItalic, 0, 1);
        float x = r.X + S(122);
        static const int ORDER[5] = {QUEEN, ROOK, BISHOP, KNIGHT, PAWN};
        for (int t : ORDER) {
            int lost = max(0, START[t] - cnt[enemy][t]);
            for (int i = 0; i < lost; i++) {
                gr.DrawImage(pieceBitmap(mkpiece(enemy, t), (int)icon, 0.95f, false), RectF(x, y + (rowH - icon) / 2, icon, icon));
                x += icon * 0.6f;
            }
            if (lost) x += icon * 0.4f;
        }
        int diff = mat[side] - mat[enemy];
        if (diff > 0)
            drawText(gr, L"+" + std::to_wstring(diff), RectF(r.X + r.Width - S(58), y, S(48), rowH), F_MONO, S(15), C_RED,
                     FontStyleBold, 2, 1);
    }
    if (mat[0] == mat[1])
        drawText(gr, L"level", RectF(r.X + r.Width - S(70), r.Y, S(60), r.Height), F_HAND, S(12), C_FAINT, 0, 2, 1);
    gr.Restore(st);
}

#include "hud.inl"

static float easeOut(float p) { return 1.f - (1.f - p) * (1.f - p) * (1.f - p); }

static bool thoughtsVisible() { return g_screen == SCR_GAME && g_thinking && g_showThoughts && !g.over && g.preFrom < 0; }
// true when something on top of the scene has to be drawn (moving pieces, marks, the bot's thoughts)
static bool overlayActive() {
    return g_screen == SCR_GAME && (!g_anims.empty() || g.dragging || thoughtsVisible() || !g_marks.empty() || g_rightFrom >= 0);
}

static void renderOverlay(Graphics& gr) {
    if (g_screen != SCR_GAME) return;
    Geom b = boardGeom();
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    for (auto& k : g_marks) drawMark(gr, k.from, k.to, MARK_COLOR[k.colour], b.sq);
    if (g_rightFrom >= 0 && g_rightTo >= 0) drawMark(gr, g_rightFrom, g_rightTo, MARK_COLOR[g_markColour], b.sq);
    if (thoughtsVisible()) {
        Move best = g_think.best, cur = g_think.current;
        if (cur && cur != best) drawArrow(gr, squareRect(mfrom(cur)), squareRect(mto(cur)), Color(110, 70, 60, 50), b.sq * 0.07f);
        if (best) drawArrow(gr, squareRect(mfrom(best)), squareRect(mto(best)), Color(150, 172, 44, 38), b.sq * 0.12f);
        int d = g_think.depth, sc = g_think.score;
        std::wstring line = oppName() + L" is thinking\x2026";
        if (d > 0) {
            wchar_t buf[64];
            if (std::abs(sc) >= MATE_SCORE - 200) swprintf(buf, 64, L"  depth %d, sees mate", d);
            else swprintf(buf, 64, L"  depth %d, %+.1f", d, sc / 100.0);
            line += buf;
        }
        {
            std::lock_guard<std::mutex> lk(g_noteMx);
            if (!g_expectNote.empty()) line = oppName() + L" " + g_expectNote + L"\x2026";
        }
        drawText(gr, line, g_thinkLine, F_HAND, S(13), C_FAINT);
    }
    gr.SetInterpolationMode(InterpolationModeNearestNeighbor);
    gr.SetPixelOffsetMode(PixelOffsetModeHalf);
    auto now = Clock::now();
    for (auto& a : g_anims) {
        if (!a.fade) continue;
        float p = a.dur > 0 ? std::chrono::duration<float>(now - a.t0).count() / a.dur : 1.f;
        p = max(0.f, min(1.f, p));
        Bitmap* bm = pieceBitmap(a.piece, (int)b.sq, g_pieceFill, true);
        // captured piece shrinks a touch and fades once the attacker is most of the way there
        float q = max(0.f, (p - 0.45f) / 0.55f), k = 1.f - 0.15f * q;
        ColorMatrix cm = {};
        cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = cm.m[4][4] = 1.f;
        cm.m[3][3] = 1.f - q;
        RectF r(a.to.X + a.to.Width * (1 - k) / 2, a.to.Y + a.to.Height * (1 - k) / 2, a.to.Width * k, a.to.Height * k);
        ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        gr.SetInterpolationMode(InterpolationModeBilinear);
        gr.DrawImage(bm, r, 0, 0, (REAL)bm->GetWidth(), (REAL)bm->GetHeight(), UnitPixel, &ia);
        gr.SetInterpolationMode(InterpolationModeNearestNeighbor);
    }
    for (auto& a : g_anims) {
        if (a.fade) continue;
        float p = a.dur > 0 ? std::chrono::duration<float>(now - a.t0).count() / a.dur : 1.f;
        float e = easeOut(max(0.f, min(1.f, p)));
        float x = a.from.X + (a.to.X - a.from.X) * e, y = a.from.Y + (a.to.Y - a.from.Y) * e;
        gr.DrawImage(pieceBitmap(a.piece, (int)b.sq, g_pieceFill, true), RectF(floorf(x), floorf(y), b.sq, b.sq));
    }
    int held = g.dragging ? (g.selected >= 0 ? g.selected : g.preFrom) : -1;
    if (held >= 0 && g.pos.sq[held]) {
        int px = (int)(b.sq * 1.08f);
        gr.DrawImage(pieceBitmap(g.pos.sq[held], px, g_pieceFill, true),
                     RectF(floorf(g_mouseX - b.sq * 0.54f), floorf(g_mouseY - b.sq * 0.6f), (REAL)px, (REAL)px));
    }
}

static void setupGraphics(Graphics& gr) {
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gr.SetCompositingQuality(CompositingQualityHighSpeed);
}

// Draws all filtered ladder rows once into a tall off-screen strip. Scrolling then copies a slice of
// it instead of laying out text again, which is what made the list feel heavy.
static void renderLadderCache() {
    const auto& lv = allLevels();
    int w = max(1, (int)g_listRect.Width), h = max(1, (int)(g_ladder.size() * rowH()));
    if (!g_listL.dc || g_listL.w != w || g_listL.h < h) {
        HDC dc = GetDC(g_hwnd);
        if (g_listL.dc) {
            SelectObject(g_listL.dc, g_listL.old);
            DeleteObject(g_listL.bmp);
            DeleteDC(g_listL.dc);
        }
        g_listL.dc = CreateCompatibleDC(dc);
        g_listL.w = w;
        g_listL.h = max(h, (int)(150 * rowH()));
        g_listL.bmp = CreateCompatibleBitmap(dc, g_listL.w, g_listL.h);
        g_listL.old = (HBITMAP)SelectObject(g_listL.dc, g_listL.bmp);
        ReleaseDC(g_hwnd, dc);
    }
    Graphics gr(g_listL.dc);
    setupGraphics(gr);
    fillTexture(gr, g_paper, RectF(0, 0, (REAL)w, (REAL)g_listL.h));
    float margin = S(46), rh = rowH();
    Pen red(Color(110, 190, 60, 50), 1.f);
    gr.DrawLine(&red, margin, 0.f, margin, (REAL)h);
    for (int k = 0; k < (int)g_ladder.size(); k++) {
        int i = g_ladder[k];
        const Level& L = lv[i];
        float ry = k * rh;
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, 0.f, ry + rh - 1, (REAL)w, ry + rh - 1);
        if (k && lv[g_ladder[k - 1]].tier != L.tier) {
            Pen tierRule(Color(170, C_PRINT.GetR(), C_PRINT.GetG(), C_PRINT.GetB()), S(1.5f));
            gr.DrawLine(&tierRule, S(8), ry, w - S(8), ry);
        }
        float iconS = rh - S(14), nx = margin + S(10) + iconS + S(10);
        if (i == g_sel) {   // highlighter stroke with slanted ends
            float hy = ry + rh * 0.2f, hh = rh * 0.62f, x1 = nx - S(8), x2 = w - S(10);
            PointF poly[4] = {PointF(x1 + S(4), hy), PointF(x2, hy + S(2)), PointF(x2 - S(5), hy + hh), PointF(x1, hy + hh - S(1))};
            SolidBrush hl(C_HILITE);
            gr.FillPolygon(&hl, poly, 4);
        }
        drawText(gr, std::to_wstring(L.number), RectF(0, ry, S(38), rh), F_MONO, S(13), C_FAINT, FontStyleRegular, 2);
        gr.DrawImage(pieceBitmap(mkpiece(g_iconSide, TIER_PIECE[L.tier]), (int)iconS, 0.86f, false), RectF(margin + S(10), ry + S(7), iconS, iconS));
        drawText(gr, widen(L.name), RectF(nx, ry + S(3), w - nx - S(84), rh * 0.55f), F_SERIF, S(16), C_PRINT, FontStyleBold, 0, 2);
        drawText(gr, widen(L.title), RectF(nx, ry + rh * 0.55f, S(200), rh * 0.4f), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0);
        drawText(gr, std::to_wstring(L.rating), RectF(0, ry, w - S(16), rh), F_MONO, S(16), C_PRINT, FontStyleBold, 2);
    }
    g_listDirty = false;
    g_listDrawnSel = g_sel;
}

static void render(HDC target) {
    if (!g_memL.dc) return;
    long long key = ((long long)g_W << 32) ^ ((long long)g_H << 8) ^ (g_screen << 2) ^ (g.flipped ? 1 : 0);
    if (key != g_bgKey) {
        g_bgKey = key;
        Graphics gr(g_bgL.dc);
        setupGraphics(gr);
        drawTable(gr);
        if (g_screen == SCR_GAME) renderBoardBase(gr);
        g_sceneDirty = true;
        g_listDirty = true;   // theme or size changed
    }
    if (g_sceneDirty) {
        g_sceneDirty = false;
        BitBlt(g_sceneL.dc, 0, 0, g_W, g_H, g_bgL.dc, 0, 0, SRCCOPY);
        Graphics gr(g_sceneL.dc);
        setupGraphics(gr);
        g_buttons.clear();
        switch (g_screen) {
            case SCR_LOGIN: renderLogin(gr); break;
            case SCR_MENU: renderMenu(gr); break;
            case SCR_LOBBY: renderLobby(gr); break;
            case SCR_SETTINGS: renderSettings(gr); break;
            case SCR_PROFILE: renderProfile(gr); break;
            case SCR_PEOPLE: renderPeople(gr); break;
            case SCR_CHAT: renderChat(gr); break;
            case SCR_ENGINES: renderEngines(gr); break;
            case SCR_PUZZLES: renderPuzzles(gr); break;
            case SCR_TEAMS: renderTeams(gr); break;
            case SCR_TOURNEYS: renderTours(gr); break;
            case SCR_PLANS: renderPlans(gr); break;
            case SCR_GAME:
                renderBoard(gr);
                renderGameSide(gr);
                break;
        }
    }
    if (g_screen == SCR_MENU) {
        // scene (everything but the rows) + the visible slice of the cached list
        if (g_listDirty || g_listDrawnSel != g_sel || g_listL.w != max(1, (int)g_listRect.Width)) renderLadderCache();
        BitBlt(g_memL.dc, 0, 0, g_W, g_H, g_sceneL.dc, 0, 0, SRCCOPY);
        int total = (int)(g_ladder.size() * rowH()), sc = (int)g_menuScroll;
        int h = min((int)g_listRect.Height, total - sc);
        if (h > 0) BitBlt(g_memL.dc, (int)g_listRect.X, (int)g_listRect.Y, (int)g_listRect.Width, h, g_listL.dc, 0, sc, SRCCOPY);
        if (maxScroll() > 0) {
            float frac = g_listRect.Height / max(1.f, (float)total), bh = max(S(30), g_listRect.Height * frac);
            RECT thumb;
            thumb.left = (LONG)(g_listRect.X + g_listRect.Width - S(4));
            thumb.right = thumb.left + (LONG)max(2.f, S(3));
            thumb.top = (LONG)(g_listRect.Y + (g_listRect.Height - bh) * (g_menuScroll / maxScroll()));
            thumb.bottom = thumb.top + (LONG)bh;
            HBRUSH tb = CreateSolidBrush(RGB(C_FAINT.GetR(), C_FAINT.GetG(), C_FAINT.GetB()));
            FillRect(g_memL.dc, &thumb, tb);
            DeleteObject(tb);
        }
        BitBlt(target, 0, 0, g_W, g_H, g_memL.dc, 0, 0, SRCCOPY);
        return;
    }
    if (overlayActive()) {
        BitBlt(g_memL.dc, 0, 0, g_W, g_H, g_sceneL.dc, 0, 0, SRCCOPY);
        {
            Graphics gr(g_memL.dc);
            setupGraphics(gr);
            renderOverlay(gr);
        }
        BitBlt(target, 0, 0, g_W, g_H, g_memL.dc, 0, 0, SRCCOPY);
    } else {
        BitBlt(target, 0, 0, g_W, g_H, g_sceneL.dc, 0, 0, SRCCOPY);
    }
}

// Runs every ~15 ms while something is moving; stops itself when everything has settled.
static void animTick() {
    bool active = false;
    if (!g_anims.empty()) {
        auto now = Clock::now();
        bool landed = false;
        for (size_t i = 0; i < g_anims.size();) {
            Anim& a = g_anims[i];
            if (std::chrono::duration<float>(now - a.t0).count() >= a.dur) {
                if (a.sound >= 0) playSound(a.sound);
                if (!a.fade) landed = true;
                g_anims.erase(g_anims.begin() + i);
            } else {
                i++;
            }
        }
        if (landed || g_anims.empty()) invalidate();
        else invalidateOverlay();
        active = !g_anims.empty();
    }
    if (thoughtsVisible()) {
        // the bot's arrows only need ~12 frames a second
        if (std::chrono::duration<float>(Clock::now() - g_lastThinkPaint).count() > 0.08f) {
            g_lastThinkPaint = Clock::now();
            invalidateOverlay();
        }
        active = true;
    }
    if (g_screen == SCR_MENU && fabsf(g_scrollTarget - g_menuScroll) > 0.5f) {
        g_menuScroll += (g_scrollTarget - g_menuScroll) * 0.28f;
        invalidateOverlay();   // only the list slice moves
        active = true;
    } else if (g_screen == SCR_MENU && g_menuScroll != g_scrollTarget) {
        g_menuScroll = g_scrollTarget;
        invalidateOverlay();
    }
    if (!active) KillTimer(g_hwnd, TIMER_ANIM);
}

// ---------------------------------------------------------------- login edit boxes

static void placeEdit(HWND h, RectF r, bool show) {
    int pad = (int)S(6);
    MoveWindow(h, (int)r.X + pad, (int)(r.Y + (r.Height - S(22)) / 2), max(10, (int)r.Width - 2 * pad), (int)S(22), TRUE);
    ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

// Lays out every text box for the screen being shown (the name is historical: it began login-only).
static void showLoginEdits(bool) {
    if (!g_edEmail) return;
    bool login = g_screen == SCR_LOGIN;
    placeEdit(g_edEmail, loginField(0), login);
    placeEdit(g_edPass, loginField(1), login);
    placeEdit(g_edName, loginField(2), login && g_loginMode == 1);
    for (HWND h : {g_edEmail, g_edPass, g_edName}) EnableWindow(h, !g_loginBusy);
    bool find = g_screen == SCR_PEOPLE && g_peopleTab == 2, chat = g_screen == SCR_CHAT && !g_chatWith.uid.empty();
    placeEdit(g_edSearch, peopleSearchRect(), find);
    placeEdit(g_edChat, chatInputRect(), chat);
    placeEdit(g_edPName, profileNameRect(), g_screen == SCR_PROFILE);
    placeEdit(g_edBio, profileBioRect(), g_screen == SCR_PROFILE);
    // general-purpose boxes: which ones show, and where, depends on the screen and what is open
    RectF gen[4];
    int limit[4] = {0, 0, 0, 0};
    switch (g_screen) {
        case SCR_MENU: gen[0] = ladderFilterRect(); limit[0] = 20; break;
        case SCR_ENGINES:
            if (g_engEditing) { gen[0] = engineEditRect(0); limit[0] = 24; gen[1] = engineEditRect(1); limit[1] = 140; }
            break;
        case SCR_PUZZLES:
            if (g_puzComposing) { gen[0] = puzzleEditRect(0); limit[0] = 120; gen[1] = puzzleEditRect(1); limit[1] = 40; }
            break;
        case SCR_TEAMS:
            if (g_teamCreating) {
                gen[0] = teamEditRect(0); limit[0] = 24;
                gen[1] = teamEditRect(1); limit[1] = 5;
                gen[2] = teamEditRect(2); limit[2] = 100;
            }
            break;
        case SCR_TOURNEYS:
            if (g_tourCreating) { gen[0] = tourEditRect(0); limit[0] = 40; }
            break;
        default: break;
    }
    for (int i = 0; i < 4; i++) {
        bool show = gen[i].Width > 0;
        if (show) SendMessageW(g_edGen[i], EM_SETLIMITTEXT, limit[i], 0);
        else if (IsWindowVisible(g_edGen[i]) && !(i == 0 && g_screen == SCR_MENU)) SetWindowTextW(g_edGen[i], L"");
        placeEdit(g_edGen[i], show ? gen[i] : RectF(0, 0, 20, 20), show);
    }
    HWND f = GetFocus();
    bool inBox = false;
    for (HWND h : {g_edEmail, g_edPass, g_edName, g_edSearch, g_edChat, g_edPName, g_edBio, g_edGen[0], g_edGen[1], g_edGen[2], g_edGen[3]})
        if (f == h && IsWindowVisible(h)) inBox = true;
    if (login) { if (!g_loginBusy && f != g_edPass && f != g_edName) SetFocus(g_edEmail); }
    else if (find) SetFocus(g_edSearch);
    else if (chat) SetFocus(g_edChat);
    else if (!inBox) SetFocus(g_hwnd);
}

static void layoutLoginEdits() { showLoginEdits(false); }

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    bool login = h == g_edEmail || h == g_edPass || h == g_edName;
    if (m == WM_KEYDOWN && w == VK_TAB) {
        std::vector<HWND> order;
        if (login) {
            order = {g_edEmail, g_edPass};
            if (g_loginMode == 1) order.push_back(g_edName);
        } else if (h == g_edPName || h == g_edBio) {
            order = {g_edPName, g_edBio};
        }
        if (order.size() > 1) {
            size_t i = 0;
            while (i < order.size() && order[i] != h) i++;
            bool back = GetKeyState(VK_SHIFT) < 0;
            SetFocus(order[(i + (back ? order.size() - 1 : 1)) % order.size()]);
        }
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        if (login) submitLogin();
        else if (h == g_edSearch) doSearch();
        else if (h == g_edChat) sendChat();
        else if (h == g_edPName || h == g_edBio) saveProfile();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE && !login) { SetFocus(g_hwnd); return 0; }
    if (m == WM_CHAR && (w == '\t' || w == '\r' || w == 27)) return 0;
    return DefSubclassProc(h, m, w, l);
}

static void createLoginEdits(HWND parent, HINSTANCE hi) {
    g_edFont = CreateFontW(-(int)S(17), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL;
    g_edEmail = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edPass = CreateWindowExW(0, L"EDIT", L"", style | ES_PASSWORD, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edName = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edSearch = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edChat = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edPName = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    g_edBio = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    for (HWND& h : g_edGen) h = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent, NULL, hi, NULL);
    for (HWND h : {g_edEmail, g_edPass, g_edName, g_edSearch, g_edChat, g_edPName, g_edBio, g_edGen[0], g_edGen[1], g_edGen[2], g_edGen[3]}) {
        SendMessageW(h, WM_SETFONT, (WPARAM)g_edFont, TRUE);
        SendMessageW(h, EM_SETLIMITTEXT, 128, 0);
        SetWindowSubclass(h, EditProc, 1, 0);
    }
    SendMessageW(g_edName, EM_SETLIMITTEXT, 20, 0);
    SendMessageW(g_edPName, EM_SETLIMITTEXT, 20, 0);
    SendMessageW(g_edSearch, EM_SETLIMITTEXT, 20, 0);
    SendMessageW(g_edBio, EM_SETLIMITTEXT, 140, 0);
    SendMessageW(g_edChat, EM_SETLIMITTEXT, 500, 0);
}

// ---------------------------------------------------------------- input

static void onButton(int id) {
    if (onSettingsButton(id) || onSocialButton(id) || onEnginesButton(id) || onPuzzleButton(id) || onCompeteButton(id) ||
        onPlansButton(id))
        return;
    if (id >= B_JOIN && id < B_SET_PREV) {
        int i = id - B_JOIN;
        if (i < (int)g_openTables.size()) joinTable(g_openTables[i]);
        return;
    }
    switch (id) {
        case B_PLAY: startGame(g_sel); break;
        case B_WHITE: g_colorPref = 0; saveProgress(); break;
        case B_RANDOM: g_colorPref = 1; saveProgress(); break;
        case B_BLACK: g_colorPref = 2; saveProgress(); break;
        case B_LEARN: g_learn = !g_learn; saveProgress(); break;
        case B_THOUGHTS: g_showThoughts = !g_showThoughts; saveProgress(); break;
        case B_TC_PREV: g_tcIndex = (g_tcIndex + TC_COUNT - 1) % TC_COUNT; saveProgress(); break;
        case B_TC_NEXT: g_tcIndex = (g_tcIndex + 1) % TC_COUNT; saveProgress(); break;
        case B_UNDO: doUndo(); break;
        case B_FLIP: g.flipped = !g.flipped; break;
        case B_RESIGN: if (!g.over) g_confirm = 1; break;
        case B_CONFIRM_NO: g_confirm = 0; break;
        case B_CONFIRM_YES: {
            int why = g_confirm;
            g_confirm = 0;
            resign();
            if (why == 2) goMenu();
            break;
        }
        case B_QUICK: quickMatch(); break;
        case B_INVITE_PREV:
        case B_INVITE_NEXT: {
            int n = (int)g_friends.size() + 1;
            g_inviteIdx = (g_inviteIdx + (id == B_INVITE_NEXT ? 1 : n - 1)) % n;
            break;
        }
        case B_FILT_TIER_DN: g_filterTier = g_filterTier < 0 ? 14 : g_filterTier - 1; rebuildLadder(); break;
        case B_FILT_TIER_UP: g_filterTier = g_filterTier >= 14 ? -1 : g_filterTier + 1; rebuildLadder(); break;
        case B_FILT_STATUS: g_filterStatus = (g_filterStatus + 1) % 3; rebuildLadder(); break;
        case B_NAV_ENGINES: unhostTable(); openEngines(); break;
        case B_NAV_PUZZLES: unhostTable(); openPuzzles(); break;
        case B_NAV_TOURS: unhostTable(); openTours(); break;
        case B_NAV_TEAMS: unhostTable(); openTeams(); break;
        case B_NAV_PLANS:
            unhostTable();
            g_socialMsg.clear();
            goScreen(SCR_PLANS);
            loadPlanAndConfig();
            break;
        case B_NAV_THEME:   // one tap between light and dark
            g_theme.mode = g_theme.mode ? 0 : 1;
            applyTheme();
            saveSettings();
            break;
        case B_DEVCHAT:
            if (g_plan >= 3 && !g_devUid.empty()) {
                fb::Person dev;
                dev.uid = g_devUid;
                dev.name = g_devName.empty() ? "The developers" : g_devName;
                openChatWith(dev);
            }
            break;
        case B_UPGRADE:
            if (g_upgradeUrl.compare(0, 8, "https://") == 0) ShellExecuteW(NULL, L"open", widen(g_upgradeUrl).c_str(), NULL, NULL, SW_SHOWNORMAL);
            break;
        case B_MENU:
            if (g_screen == SCR_GAME && g.online && !g.over) { g_confirm = 2; break; }   // leaving a live online game concedes it: ask first
            goMenu();
            break;
        case B_REMATCH:
            if (g_customOn) startCustomGame(Level(g_customLevel), std::wstring(g_customEvent));
            else startGame(g.level);
            break;
        case B_NEXT: startGame(min(149, g.level + 1)); break;
        case B_ONLINE:
            if (g_screen == SCR_GAME) { stopEngine(); KillTimer(g_hwnd, TIMER_CLOCK); g_anims.clear(); }
            openLobby();
            break;
        case B_LOBBY_BACK:
        case B_NAV_LADDER: unhostTable(); goMenu(); break;
        case B_NAV_PEOPLE: unhostTable(); openPeople(); break;
        case B_NAV_CHAT: unhostTable(); openChat(); break;
        case B_NAV_PROFILE: unhostTable(); openProfile(); break;
        case B_NAV_SETTINGS: unhostTable(); goScreen(SCR_SETTINGS); break;
        case B_SIGNOUT:
            if (g_screen == SCR_GAME && g.online && !g.over) resign();
            stopEngine();
            KillTimer(g_hwnd, TIMER_CLOCK);
            signOut();
            break;
        case B_MODE_IN: g_loginMode = 0; g_loginMsg.clear(); showLoginEdits(true); break;
        case B_MODE_UP: g_loginMode = 1; g_loginMsg.clear(); showLoginEdits(true); break;
        case B_SUBMIT: submitLogin(); break;
        case B_HOST: hostTable(); break;
        case B_UNHOST: unhostTable(); break;
        case B_GUEST: submitGuest(); break;
        case B_JOINCLUB:   // a guest who wants the bots: leave and come back as a member
            if (g_screen == SCR_GAME && g.online && !g.over) resign();
            KillTimer(g_hwnd, TIMER_CLOCK);
            signOut();
            g_loginMode = 1;
            g_loginMsg = L"Become a member to play the ladder's 150 bots.";
            showLoginEdits(true);
            break;
    }
    invalidate();
}

static int buttonAt(float x, float y) {
    for (auto& b : g_buttons)
        if (b.r.Contains(x, y)) return b.id;
    return 0;
}

static void onMouseDown(float x, float y, bool dbl) {
    g_mouseX = x;
    g_mouseY = y;
    if (g_screen == SCR_GAME && g.promoOpen) {
        for (int k = 0; k < 4; k++)
            if (promoRect(k).Contains(x, y)) {
                for (Move m : g.targets)
                    if (mfrom(m) == g.promoFrom && mto(m) == g.promoTo && mpromo(m) == PROMO_TYPES[k]) {
                        g.promoOpen = false;
                        playMove(m, 0, false, true);
                        return;
                    }
            }
        g.promoOpen = false;
        g.selected = -1;
        g.targets.clear();
        invalidate();
        return;
    }
    if (int id = buttonAt(x, y)) { onButton(id); return; }
    if (g_screen == SCR_MENU) {
        if (g_listRect.Contains(x, y)) {
            int i = (int)((y - g_listRect.Y + g_menuScroll) / rowH());
            if (i >= 0 && i < (int)g_ladder.size()) {
                g_sel = g_ladder[i];
                if (dbl) startGame(g_sel);
                invalidate();
            }
        }
        return;
    }
    if (g_screen != SCR_GAME) return;
    clearMarks();   // any left click wipes the planning arrows
    int s = squareAt(x, y);
    if (s < 0) { g.selected = -1; g.targets.clear(); invalidate(); return; }
    if (preMoveAllowed()) {   // queue a move while the bot is still thinking
        if (g.preFrom >= 0 && s != g.preFrom) {
            for (Move pm : g.preTargets)
                if (mto(pm) == s) {
                    g.preTo = s;
                    g.preTargets.clear();
                    invalidate();
                    return;
                }
        }
        int p = g.pos.sq[s];
        if (p && pcolor(p) == g.human) {
            g.preFrom = s;
            g.preTo = -1;
            setPreTargets(s);
            g.selected = -1;
            g.dragging = true;
            SetCapture(g_hwnd);
        } else {
            clearPreMove();
        }
        invalidate();
        return;
    }
    if (!humanCanMove()) return;
    if (g.selected >= 0 && isTarget(s)) { tryMove(g.selected, s, true); return; }
    int p = g.pos.sq[s];
    if (p && pcolor(p) == g.human) {
        selectSquare(s);
        g.dragging = true;
        SetCapture(g_hwnd);
    } else {
        g.selected = -1;
        g.targets.clear();
    }
    invalidate();
}

static void onMouseUp(float x, float y) {
    if (!g.dragging) return;
    g.dragging = false;
    ReleaseCapture();
    int s = squareAt(x, y);
    if (g.preFrom >= 0 && g.preTo < 0) {   // dropped a pre-move
        if (s >= 0 && s != g.preFrom)
            for (Move pm : g.preTargets)
                if (mto(pm) == s) { g.preTo = s; g.preTargets.clear(); break; }
        invalidate();
        return;
    }
    if (s >= 0 && s != g.selected && isTarget(s)) {
        tryMove(g.selected, s, false);
    } else if (s != g.selected && g.selected >= 0) {
        // dropped somewhere illegal: glide back home
        Geom b = boardGeom();
        float sz = b.sq * 1.08f;
        RectF home = squareRect(g.selected);
        RectF at(x - sz / 2 + (sz - b.sq) / 2, y - sz * 0.55f + (sz - b.sq) / 2, b.sq, b.sq);
        addGlide(g.pos.sq[g.selected], at, home, g.selected, 0.16f);
    }
    invalidate();
}

// The right button draws planning marks; it never presses anything.
static void onRightDown(float x, float y) {
    g_mouseX = x;
    g_mouseY = y;
    if (g_screen != SCR_GAME) return;
    if (g.promoOpen) { g.promoOpen = false; invalidate(); return; }
    if (g.preFrom >= 0) { clearPreMove(); return; }   // right click also takes back a queued move
    int s = squareAt(x, y);
    if (s < 0) return;
    g_rightFrom = g_rightTo = s;
    g_markColour = GetKeyState(VK_SHIFT) < 0 ? 1 : GetKeyState(VK_CONTROL) < 0 ? 2 : GetKeyState(VK_MENU) < 0 ? 3 : 0;
    SetCapture(g_hwnd);
    invalidateOverlay();
}

static void onRightUp(float x, float y) {
    if (g_rightFrom < 0) return;
    int from = g_rightFrom, to = squareAt(x, y), colour = g_markColour;
    g_rightFrom = g_rightTo = -1;
    ReleaseCapture();
    if (to >= 0) {
        // drawing the same mark again rubs it out
        for (size_t i = 0; i < g_marks.size(); i++)
            if (g_marks[i].from == from && g_marks[i].to == to) {
                if (g_marks[i].colour == colour) { g_marks.erase(g_marks.begin() + i); invalidate(); return; }
                g_marks.erase(g_marks.begin() + i);
                break;
            }
        g_marks.push_back({from, to, colour});
    }
    invalidate();
}

static void onMouseMove(float x, float y) {
    g_mouseX = x;
    g_mouseY = y;
    if (g_rightFrom >= 0) {
        int s = squareAt(x, y);
        if (s != g_rightTo) { g_rightTo = s; invalidateOverlay(); }
        return;
    }
    if (g.dragging) { invalidateOverlay(); return; }
    int h = buttonAt(x, y);
    static int lastRow = -1;
    int row = -1;
    bool promoHover = g_screen == SCR_GAME && g.promoOpen;
    if (h != g_hoverBtn || row != lastRow || promoHover) {
        g_hoverBtn = h;
        lastRow = row;
        invalidate();
    }
}

static void onKey(WPARAM k) {
    if (g_screen == SCR_MENU) {
        // arrows step through whatever the filters are showing
        int n = (int)g_ladder.size(), old = g_sel;
        int at = (int)(std::find(g_ladder.begin(), g_ladder.end(), g_sel) - g_ladder.begin());
        if (k == VK_RETURN) { startGame(g_sel); return; }
        if (!n) return;
        if (at >= n) at = 0;
        else if (k == VK_UP) at--;
        else if (k == VK_DOWN) at++;
        else if (k == VK_PRIOR) at -= 10;
        else if (k == VK_NEXT) at += 10;
        else if (k == VK_HOME) at = 0;
        else if (k == VK_END) at = n - 1;
        g_sel = g_ladder[max(0, min(n - 1, at))];
        if (g_sel != old) { ensureVisible(g_sel); invalidate(); }
    } else if (g_screen == SCR_GAME) {
        if (k == VK_ESCAPE) {
            if (g.promoOpen) { g.promoOpen = false; invalidate(); }
            else if (!g_marks.empty()) clearMarks();
            else if (g.preFrom >= 0) clearPreMove();
            else if (g_confirm) { g_confirm = 0; invalidate(); }
            else if (g.puzzle) onPuzzleButton(B_PUZ_BACK);
            else if (!g.online || g.over) goMenu();
        } else if (k == 'F') { g.flipped = !g.flipped; invalidate(); }
    } else if (g_screen == SCR_LOGIN && k == VK_RETURN) {
        submitLogin();
    } else if (g_screen != SCR_LOGIN && g_screen != SCR_LOBBY && k == VK_ESCAPE) {
        goMenu();
    }
}

// ---------------------------------------------------------------- window

static void resizeBuffer() {
    HDC dc = GetDC(g_hwnd);
    for (Layer* L : {&g_bgL, &g_sceneL, &g_memL}) {
        if (L->dc) {
            SelectObject(L->dc, L->old);
            DeleteObject(L->bmp);
            DeleteDC(L->dc);
        }
        L->dc = CreateCompatibleDC(dc);
        L->bmp = CreateCompatibleBitmap(dc, max(1, g_W), max(1, g_H));
        L->old = (HBITMAP)SelectObject(L->dc, L->bmp);
    }
    ReleaseDC(g_hwnd, dc);
    g_bgKey = -1;
}

static void shutdownNet() {
    if (g_hosting && !g_hostDoc.id.empty()) {
        std::string id = g_hostDoc.id;
        netAsync([id]() -> std::function<void()> { fb::deleteGame(g_sess, id); return nullptr; });
    }
    {
        std::lock_guard<std::mutex> lk(g_netMx);
        g_netQuit = true;
    }
    g_netCv.notify_one();
    // give pending writes (a resignation, taking down a table) a moment, but never hang on exit
    for (int i = 0; i < 40 && !(g_netIdle && g_netQ.empty()); i++) Sleep(100);
    if (g_netThread.joinable()) g_netThread.detach();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            g_W = LOWORD(lp);
            g_H = HIWORD(lp);
            if (g_W > 0 && g_H > 0) resizeBuffer();
            if (g_edEmail) layoutLoginEdits();
            invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (LONG)S(1100);
            mm->ptMinTrackSize.y = (LONG)S(660);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            render(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_fieldText);
            SetBkColor(dc, g_fieldBg);
            return (LRESULT)g_fieldBrush;
        }
        case WM_CTLCOLORSTATIC: {   // disabled edits while busy
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(128, 118, 104));
            SetBkColor(dc, g_fieldBg);
            return (LRESULT)g_fieldBrush;
        }
        case WM_LBUTTONDOWN: onMouseDown((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), false); return 0;
        case WM_LBUTTONDBLCLK: onMouseDown((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp), true); return 0;
        case WM_LBUTTONUP: onMouseUp((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)); return 0;
        case WM_RBUTTONDOWN: onRightDown((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)); return 0;
        case WM_RBUTTONUP: onRightUp((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEMOVE: onMouseMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEWHEEL:
            if (g_screen == SCR_MENU) {
                g_scrollTarget -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * rowH() * 3;
                g_scrollTarget = max(0.f, min(maxScroll(), g_scrollTarget));
                startAnimTimer();
            } else if (g_screen == SCR_CHAT) {   // scroll back through the conversation
                g_chatScroll = max(0.f, g_chatScroll + GET_WHEEL_DELTA_WPARAM(wp) / 120.f * S(60));
                invalidate();
            }
            return 0;
        case WM_KEYDOWN: onKey(wp); return 0;
        case WM_COMMAND:   // typing in the ladder's filter box
            if (HIWORD(wp) == EN_CHANGE && (HWND)lp == g_edGen[0] && g_screen == SCR_MENU) {
                g_filterText = narrow(editText(g_edGen[0]));
                for (char& c : g_filterText) c = (char)tolower((unsigned char)c);
                rebuildLadder();
                invalidate();
            } else if ((HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) && (HWND)lp == g_edGen[0]) {
                invalidate();   // show or hide the "name or rating" hint
            }
            return 0;
        case WM_ENGINE:
            if (g_engThread.joinable()) g_engThread.join();
            g_thinking = false;
            if ((int)wp == g.gameId && !g.over && g_screen == SCR_GAME && lp) {
                playMove((Move)lp, g_pendingScore, g_pendingKnown, true);
                runPreMove();   // your queued reply, if it is still legal
            }
            invalidate();
            return 0;
        case WM_NET: {
            auto* fn = (std::function<void()>*)lp;
            (*fn)();
            delete fn;
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_ANIM) { animTick(); return 0; }
            if (wp == TIMER_NET) { netTick(); return 0; }
            if (wp == TIMER_CLOCK) clockCheck();
            if (wp == TIMER_LEARN && !g_brain.busy()) {
                KillTimer(h, TIMER_LEARN);
                cloudBrainAfterLearn();
            }
            invalidate();
            return 0;
        case WM_CLOSE:
            if (g_screen == SCR_GAME && g.online && !g.over) resign();
            stopEngine();
            g_brain.shutdown();
            shutdownNet();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- command-line tools (no window)

static int runPerft() {
    struct T { const char* fen; int depth; uint64_t expect; };
    T tests[] = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379},
    };
    FILE* f = _wfopen((g_data + L"\\perft.txt").c_str(), L"w");
    int fails = 0;
    for (auto& t : tests) {
        Position p;
        p.setFEN(t.fen);
        uint64_t n = perft(p, t.depth);
        if (n != t.expect) fails++;
        if (f) fprintf(f, "%s  d%d  %llu  expected %llu  %s\n", t.fen, t.depth, (unsigned long long)n,
                       (unsigned long long)t.expect, n == t.expect ? "OK" : "FAIL");
    }
    if (f) { fprintf(f, "%s\n", fails ? "SOME FAILED" : "ALL OK"); fclose(f); }
    return fails;
}

static int runSelfPlay(int games) {
    FILE* log = _wfopen((g_data + L"\\selfplay.log").c_str(), L"a");
    Engine* eng = new Engine();
    std::mt19937 rng(GetTickCount());
    for (int gi = 0; gi < games; gi++) {
        Position p;
        p.setStart();
        std::vector<Sample> samples;
        int randomPlies = 2 + rng() % 5, result = 0;
        std::string reason = "move limit";
        eng->clear();
        for (int ply = 0; ply < 300; ply++) {
            int r = p.outcome(&reason);
            if (r != 2) { result = r; break; }
            Sample s;
            memcpy(s.sq, p.sq, 64);
            s.side = (uint8_t)p.side;
            s.result = 0;
            s.search = NO_SCORE;
            Move m;
            if (ply < randomPlies) {
                MoveList ml;
                p.genLegal(ml);
                m = ml.m[rng() % ml.n];
            } else {
                SearchLimits lim;
                lim.moveTimeMs = 120;
                lim.noiseCp = 12;
                lim.noiseSeed = rng();
                auto net = g_brain.net();
                SearchResult sr = eng->search(p, lim, nullptr, net.get());
                m = sr.best;
                s.search = (int16_t)max(-2000, min(2000, sr.score));
            }
            samples.push_back(s);
            p.make(m);
        }
        for (Sample& s : samples) s.result = (int8_t)(s.side == WHITE ? result : -result);
        float loss = g_brain.learnNow(samples);
        if (log) {
            fprintf(log, "game %d/%d  result %+d (%s)  plies %zu  loss %.4f\n", gi + 1, games, result, reason.c_str(),
                    samples.size(), loss);
            fflush(log);
        }
    }
    if (log) fclose(log);
    delete eng;
    return 0;
}

#include "tests.inl"

// ---------------------------------------------------------------- entry

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
    chessInit();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    g_base = exe;
    g_base = g_base.substr(0, g_base.find_last_of(L"\\/"));
    g_assets = g_base + L"\\assets";
    g_data = g_base + L"\\data";
    g_rng.seed((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId());

    // "--data <dir>" points the whole app at another data folder (the tests use a scratch one)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--data") && i + 1 < argc) g_data = argv[++i];
        else args.push_back(argv[i]);
    }
    std::wstring mode = args.empty() ? L"" : args[0], arg1 = args.size() > 1 ? args[1] : L"";
    CreateDirectoryW(g_data.c_str(), NULL);
    {   // a single downloaded exe may sit somewhere unwritable: fall back to the user's app data
        std::wstring probe = g_data + L"\\write_test.tmp";
        if (FILE* f = _wfopen(probe.c_str(), L"wb")) {
            fclose(f);
            _wremove(probe.c_str());
        } else {
            wchar_t appdata[MAX_PATH];
            if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata) == S_OK) {
                g_data = std::wstring(appdata) + L"\\Chezz";
                CreateDirectoryW(g_data.c_str(), NULL);
            }
        }
    }
    // names.txt (150 lines, top = AI 1) beside the exe or in the data folder replaces the built-in names
    if (loadLevelNames(g_base + L"\\names.txt") < 150) loadLevelNames(g_data + L"\\names.txt");
    g_brain.open(g_data);
    if (mode == L"--perft") return runPerft();
    if (mode == L"--selfplay") return runSelfPlay(arg1.empty() ? 20 : max(1, _wtoi(arg1.c_str())));
    if (mode == L"--nettest") return runNetTest();

    SetProcessDPIAware();
    HDC sdc = GetDC(NULL);
    g_ui = max(1.f, GetDeviceCaps(sdc, LOGPIXELSY) / 96.f);
    ReleaseDC(NULL, sdc);

    GdiplusStartupInput gsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, NULL);
    loadAssets();
    loadProgress();
    rebuildLadder();
    if (mode == L"--selftest") return runSelfTest();
    if (mode != L"--shots") audioInit(g_assets);
    g_engine = new Engine();
    g_netThread = std::thread(netLoop);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));   // compiled in from assets\icon.ico
    std::wstring ico = g_assets + L"\\icon.ico";
    if (fileExists(ico)) wc.hIcon = (HICON)LoadImageW(NULL, ico.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    wc.lpszClassName = L"ChezzWindow";
    RegisterClassExW(&wc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = min((int)S(1360), (int)(work.right - work.left) * 9 / 10);
    int wh = min((int)S(860), (int)(work.bottom - work.top) * 9 / 10);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Chezz", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             work.left + ((work.right - work.left) - ww) / 2, work.top + ((work.bottom - work.top) - wh) / 2,
                             ww, wh, NULL, NULL, hi, NULL);
    createLoginEdits(g_hwnd, hi);
    {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_listRect = RectF(S(32), S(126), min(S(500), rc.right * 0.44f) - S(8), (REAL)rc.bottom - S(160));
        ensureVisible(g_sel, true);
    }
    if (mode == L"--shots") {   // hidden renders of every screen; the window is never shown
        int rc = runShots(arg1.empty() ? g_data + L"\\shots" : arg1);
        ExitProcess(rc);
    }
    ShowWindow(g_hwnd, show);
    goScreen(SCR_LOGIN);   // nobody gets past the membership desk without an account
    resumeSession();
    showLoginEdits(true);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    audioShutdown();
    for (auto& kv : g_pieceCache) delete kv.second;
    for (auto& b : g_pieceSrc) delete b;
    for (auto& kv : g_avatars) delete kv.second;
    for (auto& kv : g_fonts) delete kv.second;
    for (Bitmap* b : {g_boardImg, g_lightImg, g_darkImg, g_bgImg, g_felt, g_paper, g_walnut, g_maple, g_darkwood}) delete b;
    GdiplusShutdown(token);
    return 0;
}
