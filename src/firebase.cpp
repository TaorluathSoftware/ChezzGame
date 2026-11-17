#define NOMINMAX
#include "firebase.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace fb {

// Web API keys identify the project; they are not secrets. Access is enforced by firestore.rules.
static const char* API_KEY = "AIzaSyBRyzSwfiuIVYmF3-K8rU1Iz6uEaXxOqKI";
static const char* DOCS = "/v1/projects/chezz-game3/databases/(default)/documents";

long long nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)(t / 10000ULL - 11644473600000ULL);
}

// ---------------------------------------------------------------- HTTPS

static std::wstring wide(const std::string& s) {
    std::wstring w(s.size() + 1, 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}

static int g_emuAuth = 0, g_emuStore = 0;
void useEmulator(int authPort, int firestorePort) { g_emuAuth = authPort; g_emuStore = firestorePort; }

// Returns the HTTP status, or 0 when the server could not be reached.
static int http(const wchar_t* method, const wchar_t* host0, const std::string& path0, const std::string& body,
                const char* ctype, const std::string& bearer, std::string& out) {
    const wchar_t* host = host0;
    std::string path = path0;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    DWORD secure = WINHTTP_FLAG_SECURE;
    if (g_emuAuth) {
        bool store = !wcscmp(host0, L"firestore.googleapis.com");
        if (!store) path = "/" + std::string(host0, host0 + wcslen(host0)) + path0;
        host = L"127.0.0.1";
        port = (INTERNET_PORT)(store ? g_emuStore : g_emuAuth);
        secure = 0;
    }
    static HINTERNET session = nullptr;
    out.clear();
    if (!session) {
        session = WinHttpOpen(L"Chezz/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"Chezz/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return 0;
        WinHttpSetTimeouts(session, 8000, 8000, 10000, 10000);
    }
    int status = 0;
    HINTERNET c = WinHttpConnect(session, host, port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, method, wide(path).c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure)
                    : nullptr;
    if (r) {
        std::wstring hdr;
        if (ctype) hdr += L"Content-Type: " + wide(ctype) + L"\r\n";
        if (!bearer.empty()) hdr += L"Authorization: Bearer " + wide(bearer) + L"\r\n";
        DWORD len = (DWORD)body.size();
        if (WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), (DWORD)-1L,
                               len ? (LPVOID)body.data() : WINHTTP_NO_REQUEST_DATA, len, len, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD st = 0, sz = sizeof(st);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st,
                                &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            char buf[8192];
            DWORD got = 0;
            while (WinHttpReadData(r, buf, sizeof buf, &got) && got) out.append(buf, got);
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    return status;
}

static std::string urlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

// ---------------------------------------------------------------- accounts

static std::string friendlyAuthError(const std::string& body, int status) {
    if (status == 0) return "Can't reach the club's server. Check your internet connection.";
    std::string code = Json::parse(body)["error"]["message"].s;
    auto has = [&](const char* k) { return code.find(k) != std::string::npos; };
    if (has("EMAIL_EXISTS")) return "That email already has a membership. Sign in instead.";
    if (has("INVALID_LOGIN_CREDENTIALS") || has("INVALID_PASSWORD") || has("EMAIL_NOT_FOUND"))
        return "Wrong email or password.";
    if (has("WEAK_PASSWORD")) return "Pick a password of at least 6 characters.";
    if (has("INVALID_EMAIL") || has("MISSING_EMAIL")) return "That email doesn't look right.";
    if (has("MISSING_PASSWORD")) return "Enter a password.";
    if (has("TOO_MANY_ATTEMPTS")) return "Too many attempts. Wait a minute and try again.";
    if (has("USER_DISABLED")) return "This membership has been suspended.";
    if (has("TOKEN_EXPIRED") || has("INVALID_REFRESH_TOKEN") || has("USER_NOT_FOUND"))
        return "Your session has expired. Please sign in again.";
    return code.empty() ? "Something went wrong (" + std::to_string(status) + ")." : code;
}

static void takeTokens(Session& s, const Json& j) {
    s.idToken = j["idToken"].s;
    s.refreshToken = j["refreshToken"].s;
    s.uid = j["localId"].s;
    long long ttl = atoll(j["expiresIn"].s.c_str());
    s.expiresAt = nowMs() + (ttl > 0 ? ttl : 3600) * 1000;
}

std::string signUp(Session& s, const std::string& email, const std::string& pass, const std::string& name) {
    std::string out, path = std::string("/v1/accounts:signUp?key=") + API_KEY;
    std::string body = "{\"email\":" + jsonEscape(email) + ",\"password\":" + jsonEscape(pass) + ",\"returnSecureToken\":true}";
    int st = http(L"POST", L"identitytoolkit.googleapis.com", path, body, "application/json", "", out);
    if (st != 200) return friendlyAuthError(out, st);
    takeTokens(s, Json::parse(out));
    s.email = email;
    s.name = name;
    path = std::string("/v1/accounts:update?key=") + API_KEY;
    body = "{\"idToken\":" + jsonEscape(s.idToken) + ",\"displayName\":" + jsonEscape(name) + ",\"returnSecureToken\":false}";
    http(L"POST", L"identitytoolkit.googleapis.com", path, body, "application/json", "", out);
    Profile p;
    p.name = name;
    putProfile(s, p);
    claimFounder(s);   // the first five accounts ever created are Ruby for life
    return "";
}

std::string signIn(Session& s, const std::string& email, const std::string& pass) {
    std::string out, path = std::string("/v1/accounts:signInWithPassword?key=") + API_KEY;
    std::string body = "{\"email\":" + jsonEscape(email) + ",\"password\":" + jsonEscape(pass) + ",\"returnSecureToken\":true}";
    int st = http(L"POST", L"identitytoolkit.googleapis.com", path, body, "application/json", "", out);
    if (st != 200) return friendlyAuthError(out, st);
    Json j = Json::parse(out);
    takeTokens(s, j);
    s.email = email;
    s.name = j["displayName"].s;
    if (s.name.empty()) s.name = email.substr(0, email.find('@'));
    return "";
}

std::string signInGuest(Session& s, const std::string& name) {
    std::string out, path = std::string("/v1/accounts:signUp?key=") + API_KEY;
    int st = http(L"POST", L"identitytoolkit.googleapis.com", path, "{\"returnSecureToken\":true}", "application/json", "", out);
    if (st != 200) {
        if (Json::parse(out)["error"]["message"].s.find("ADMIN_ONLY_OPERATION") != std::string::npos ||
            Json::parse(out)["error"]["message"].s.find("OPERATION_NOT_ALLOWED") != std::string::npos)
            return "Guest seats aren't open at this club yet.";
        return friendlyAuthError(out, st);
    }
    takeTokens(s, Json::parse(out));
    s.email.clear();
    s.name = name;
    return "";
}

Err refresh(Session& s, std::string* msg) {
    std::string out, path = std::string("/v1/token?key=") + API_KEY;
    std::string body = "grant_type=refresh_token&refresh_token=" + urlEncode(s.refreshToken);
    int st = http(L"POST", L"securetoken.googleapis.com", path, body, "application/x-www-form-urlencoded", "", out);
    if (st == 0) {
        if (msg) *msg = friendlyAuthError(out, st);
        return NET;
    }
    if (st != 200) {
        if (msg) *msg = friendlyAuthError(out, st);
        return AUTH;
    }
    Json j = Json::parse(out);
    s.idToken = j["id_token"].s;
    s.refreshToken = j["refresh_token"].s;
    s.uid = j["user_id"].s;
    long long ttl = atoll(j["expires_in"].s.c_str());
    s.expiresAt = nowMs() + (ttl > 0 ? ttl : 3600) * 1000;
    return OK;
}

static Err ensureToken(Session& s) {
    if (!s.idToken.empty() && nowMs() < s.expiresAt - 5 * 60 * 1000) return OK;
    return refresh(s);
}

Err setDisplayName(Session& s, const std::string& name) {
    Err e = ensureToken(s);
    if (e != OK) return e;
    std::string out, path = std::string("/v1/accounts:update?key=") + API_KEY;
    std::string body = "{\"idToken\":" + jsonEscape(s.idToken) + ",\"displayName\":" + jsonEscape(name) + ",\"returnSecureToken\":false}";
    int st = http(L"POST", L"identitytoolkit.googleapis.com", path, body, "application/json", "", out);
    if (st == 0) return NET;
    if (st != 200) return OTHER;
    s.name = name;
    return OK;
}

bool loadSession(const std::wstring& path, Session& s) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "uid") s.uid = v;
        else if (k == "email") s.email = v;
        else if (k == "name") s.name = v;
        else if (k == "refresh") s.refreshToken = v;
    }
    fclose(f);
    return s.valid();
}

void saveSession(const std::wstring& path, const Session& s) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "uid=%s\nemail=%s\nname=%s\nrefresh=%s\n", s.uid.c_str(), s.email.c_str(), s.name.c_str(), s.refreshToken.c_str());
    fclose(f);
}

void clearSession(const std::wstring& path) { _wremove(path.c_str()); }

// ---------------------------------------------------------------- firestore

static std::string fStr(const std::string& v) { return "{\"stringValue\":" + jsonEscape(v) + "}"; }
static std::string fInt(long long v) { return "{\"integerValue\":\"" + std::to_string(v) + "\"}"; }
static std::string getStr(const Json& f, const char* k) { return f[k]["stringValue"].s; }
static long long getInt(const Json& f, const char* k) {
    const Json& v = f[k];
    if (v.has("integerValue")) return atoll(v["integerValue"].s.c_str());
    if (v.has("doubleValue")) return (long long)v["doubleValue"].n;
    return 0;
}

static Err classify(int st, const std::string& body) {
    if (st >= 200 && st < 300) return OK;
    if (st == 0) return NET;
    if (st == 401 || st == 403) return AUTH;
    if (st == 409 || body.find("FAILED_PRECONDITION") != std::string::npos) return CONFLICT;
    return OTHER;
}

static Err fsCall(Session& s, const wchar_t* method, const std::string& path, const std::string& body, std::string& out) {
    Err e = ensureToken(s);
    if (e != OK) return e;
    int st = http(method, L"firestore.googleapis.com", path, body, body.empty() ? nullptr : "application/json", s.idToken, out);
    return classify(st, out);
}

static std::string lastSegment(const std::string& name) {
    size_t k = name.find_last_of('/');
    return k == std::string::npos ? name : name.substr(k + 1);
}

static std::string lower(const std::string& in) {
    std::string o = in;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

static void parseProfile(const Json& doc, Profile& p) {
    const Json& f = doc["fields"];
    p.uid = lastSegment(doc["name"].s);
    p.name = getStr(f, "name");
    p.bio = getStr(f, "bio");
    p.photo = getStr(f, "photo");
    p.banner = (int)getInt(f, "banner");
    p.avatar = (int)getInt(f, "avatar");
    p.rating = (int)getInt(f, "rating");
    p.wins = (int)getInt(f, "wins");
    p.draws = (int)getInt(f, "draws");
    p.losses = (int)getInt(f, "losses");
    p.updated = getInt(f, "updated");
    if (p.rating <= 0) p.rating = 1200;
}

Err getProfile(Session& s, const std::string& uid, Profile& p) {
    std::string out;
    Err e = fsCall(s, L"GET", std::string(DOCS) + "/users/" + uid, "", out);
    if (e != OK) return e;
    parseProfile(Json::parse(out), p);
    return OK;
}

Err putProfile(Session& s, const Profile& p) {
    std::string body = "{\"fields\":{\"name\":" + fStr(p.name) + ",\"nameLower\":" + fStr(lower(p.name)) + ",\"bio\":" +
                       fStr(p.bio) + ",\"photo\":" + fStr(p.photo) + ",\"banner\":" + fInt(p.banner) + ",\"avatar\":" +
                       fInt(p.avatar) + ",\"rating\":" + fInt(p.rating) + ",\"wins\":" +
                       fInt(p.wins) + ",\"draws\":" + fInt(p.draws) + ",\"losses\":" + fInt(p.losses) + ",\"updated\":" +
                       fInt(nowMs()) + "}}";
    std::string out;
    return fsCall(s, L"PATCH", std::string(DOCS) + "/users/" + s.uid, body, out);
}

static const char* GAME_FIELDS[] = {"white", "black", "whiteName", "blackName", "createdBy", "status", "moves", "result",
                                    "reason", "whiteRating", "blackRating", "baseMs", "incMs", "whiteMs", "blackMs",
                                    "lastMoveAt", "createdAt", "invite", "event"};

static std::string gameBody(const GameDoc& g) {
    return "{\"fields\":{\"white\":" + fStr(g.white) + ",\"black\":" + fStr(g.black) + ",\"whiteName\":" + fStr(g.whiteName) +
           ",\"blackName\":" + fStr(g.blackName) + ",\"createdBy\":" + fStr(g.createdBy) + ",\"status\":" + fStr(g.status) +
           ",\"moves\":" + fStr(g.moves) + ",\"result\":" + fStr(g.result) + ",\"reason\":" + fStr(g.reason) +
           ",\"whiteRating\":" + fInt(g.whiteRating) + ",\"blackRating\":" + fInt(g.blackRating) + ",\"baseMs\":" + fInt(g.baseMs) +
           ",\"incMs\":" + fInt(g.incMs) + ",\"whiteMs\":" + fInt(g.whiteMs) + ",\"blackMs\":" + fInt(g.blackMs) +
           ",\"lastMoveAt\":" + fInt(g.lastMoveAt) + ",\"createdAt\":" + fInt(g.createdAt) + ",\"invite\":" + fStr(g.invite) +
           ",\"event\":" + fStr(g.event) + "}}";
}

static void parseGame(const Json& doc, GameDoc& g) {
    const Json& f = doc["fields"];
    g.id = lastSegment(doc["name"].s);
    g.updateTime = doc["updateTime"].s;
    g.white = getStr(f, "white");
    g.black = getStr(f, "black");
    g.whiteName = getStr(f, "whiteName");
    g.blackName = getStr(f, "blackName");
    g.createdBy = getStr(f, "createdBy");
    g.status = getStr(f, "status");
    g.moves = getStr(f, "moves");
    g.result = getStr(f, "result");
    g.reason = getStr(f, "reason");
    g.whiteRating = (int)getInt(f, "whiteRating");
    g.blackRating = (int)getInt(f, "blackRating");
    g.baseMs = getInt(f, "baseMs");
    g.incMs = getInt(f, "incMs");
    g.whiteMs = getInt(f, "whiteMs");
    g.blackMs = getInt(f, "blackMs");
    g.lastMoveAt = getInt(f, "lastMoveAt");
    g.createdAt = getInt(f, "createdAt");
    g.invite = getStr(f, "invite");
    g.event = getStr(f, "event");
}

Err createGame(Session& s, GameDoc& g) {
    std::string out;
    // a preset id (tournament pairings) makes both players land on the same table
    Err e = fsCall(s, L"POST", std::string(DOCS) + "/games" + (g.id.empty() ? "" : "?documentId=" + g.id), gameBody(g), out);
    if (e == OK) parseGame(Json::parse(out), g);
    return e;
}

Err getGame(Session& s, const std::string& id, GameDoc& g) {
    std::string out;
    Err e = fsCall(s, L"GET", std::string(DOCS) + "/games/" + id, "", out);
    if (e == OK) parseGame(Json::parse(out), g);
    return e;
}

Err updateGame(Session& s, GameDoc& g) {
    std::string path = std::string(DOCS) + "/games/" + g.id + "?";
    for (const char* k : GAME_FIELDS) path += std::string("updateMask.fieldPaths=") + k + "&";
    path += "currentDocument.updateTime=" + urlEncode(g.updateTime);
    std::string out;
    Err e = fsCall(s, L"PATCH", path, gameBody(g), out);
    if (e == OK) parseGame(Json::parse(out), g);
    return e;
}

Err deleteGame(Session& s, const std::string& id) {
    std::string out;
    return fsCall(s, L"DELETE", std::string(DOCS) + "/games/" + id, "", out);
}

Err listOpenGames(Session& s, std::vector<GameDoc>& list) {
    std::string body =
        "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"games\"}],\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":"
        "\"status\"},\"op\":\"EQUAL\",\"value\":{\"stringValue\":\"open\"}}},\"limit\":30}}";
    std::string out;
    Err e = fsCall(s, L"POST", std::string(DOCS) + ":runQuery", body, out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& item : j.a) {
        if (!item.has("document")) continue;
        GameDoc g;
        parseGame(item["document"], g);
        list.push_back(g);
    }
    return OK;
}

// ---------------------------------------------------------------- people

Err searchUsers(Session& s, const std::string& prefix, std::vector<Profile>& list) {
    std::string q = lower(prefix);
    // names starting with q: q <= nameLower < q + U+F8FF
    std::string body = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"users\"}],\"where\":{\"compositeFilter\":{\"op\":\"AND\","
                       "\"filters\":[{\"fieldFilter\":{\"field\":{\"fieldPath\":\"nameLower\"},\"op\":\"GREATER_THAN_OR_EQUAL\",\"value\":" +
                       fStr(q) + "}},{\"fieldFilter\":{\"field\":{\"fieldPath\":\"nameLower\"},\"op\":\"LESS_THAN\",\"value\":" +
                       fStr(q + "\xEF\xA3\xBF") + "}}]}},\"limit\":20}}";
    std::string out;
    Err e = fsCall(s, L"POST", std::string(DOCS) + ":runQuery", body, out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& item : j.a) {
        if (!item.has("document")) continue;
        Profile p;
        parseProfile(item["document"], p);
        list.push_back(p);
    }
    return OK;
}

Err listPeople(Session& s, const std::string& owner, const char* sub, std::vector<Person>& list) {
    std::string out;
    Err e = fsCall(s, L"GET", std::string(DOCS) + "/users/" + owner + "/" + sub + "?pageSize=200", "", out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& d : j["documents"].a) {
        const Json& f = d["fields"];
        Person p;
        p.uid = lastSegment(d["name"].s);
        p.name = getStr(f, "name");
        p.kind = getStr(f, "kind");
        p.photo = getStr(f, "photo");
        p.avatar = (int)getInt(f, "avatar");
        p.at = getInt(f, "at");
        list.push_back(p);
    }
    return OK;
}

Err putPerson(Session& s, const std::string& owner, const char* sub, const Person& p) {
    std::string body = "{\"fields\":{\"name\":" + fStr(p.name) + ",\"kind\":" + fStr(p.kind) + ",\"photo\":" + fStr(p.photo) +
                       ",\"avatar\":" + fInt(p.avatar) +
                       ",\"at\":" + fInt(p.at ? p.at : nowMs()) + "}}";
    std::string out;
    return fsCall(s, L"PATCH", std::string(DOCS) + "/users/" + owner + "/" + sub + "/" + p.uid, body, out);
}

Err deletePerson(Session& s, const std::string& owner, const char* sub, const std::string& uid) {
    std::string out;
    return fsCall(s, L"DELETE", std::string(DOCS) + "/users/" + owner + "/" + sub + "/" + uid, "", out);
}

// ---------------------------------------------------------------- direct messages

std::string chatIdFor(const std::string& x, const std::string& y) { return x < y ? x + "_" + y : y + "_" + x; }

static void parseChat(const Json& doc, Chat& c) {
    const Json& f = doc["fields"];
    c.id = lastSegment(doc["name"].s);
    c.a = getStr(f, "a");
    c.b = getStr(f, "b");
    c.aName = getStr(f, "aName");
    c.bName = getStr(f, "bName");
    c.aAvatar = (int)getInt(f, "aAvatar");
    c.bAvatar = (int)getInt(f, "bAvatar");
    c.last = getStr(f, "last");
    c.lastFrom = getStr(f, "lastFrom");
    c.lastAt = getInt(f, "lastAt");
}

Err listChats(Session& s, std::vector<Chat>& list) {
    list.clear();
    for (const char* side : {"a", "b"}) {
        std::string body = std::string("{\"structuredQuery\":{\"from\":[{\"collectionId\":\"chats\"}],\"where\":{\"fieldFilter\":{\"field\":"
                                       "{\"fieldPath\":\"") + side + "\"},\"op\":\"EQUAL\",\"value\":" + fStr(s.uid) + "}},\"limit\":50}}";
        std::string out;
        Err e = fsCall(s, L"POST", std::string(DOCS) + ":runQuery", body, out);
        if (e != OK) return e;
        Json j = Json::parse(out);
        for (auto& item : j.a) {
            if (!item.has("document")) continue;
            Chat c;
            parseChat(item["document"], c);
            list.push_back(c);
        }
    }
    return OK;
}

Err listMessages(Session& s, const std::string& chatId, std::vector<Message>& list) {
    std::string body = "{\"structuredQuery\":{\"from\":[{\"collectionId\":\"messages\"}],\"orderBy\":[{\"field\":{\"fieldPath\":\"at\"},"
                       "\"direction\":\"DESCENDING\"}],\"limit\":60}}";
    std::string out;
    Err e = fsCall(s, L"POST", std::string(DOCS) + "/chats/" + chatId + ":runQuery", body, out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& item : j.a) {
        if (!item.has("document")) continue;
        const Json& f = item["document"]["fields"];
        Message m;
        m.from = getStr(f, "from");
        m.text = getStr(f, "text");
        m.at = getInt(f, "at");
        list.push_back(m);
    }
    std::reverse(list.begin(), list.end());
    return OK;
}

Err sendMessage(Session& s, const Profile& me, const Person& to, const std::string& text) {
    bool meFirst = s.uid < to.uid;
    std::string id = chatIdFor(s.uid, to.uid);
    long long now = nowMs();
    std::string preview = text.size() > 80 ? text.substr(0, 80) : text;
    // the conversation header (created on the first message), then the message itself
    std::string body = "{\"fields\":{\"a\":" + fStr(meFirst ? s.uid : to.uid) + ",\"b\":" + fStr(meFirst ? to.uid : s.uid) +
                       ",\"aName\":" + fStr(meFirst ? me.name : to.name) + ",\"bName\":" + fStr(meFirst ? to.name : me.name) +
                       ",\"aAvatar\":" + fInt(meFirst ? me.avatar : to.avatar) + ",\"bAvatar\":" + fInt(meFirst ? to.avatar : me.avatar) +
                       ",\"last\":" + fStr(preview) + ",\"lastFrom\":" + fStr(s.uid) + ",\"lastAt\":" + fInt(now) + "}}";
    std::string out;
    Err e = fsCall(s, L"PATCH", std::string(DOCS) + "/chats/" + id, body, out);
    if (e != OK) return e;
    body = "{\"fields\":{\"from\":" + fStr(s.uid) + ",\"text\":" + fStr(text) + ",\"at\":" + fInt(now) + "}}";
    return fsCall(s, L"POST", std::string(DOCS) + "/chats/" + id + "/messages", body, out);
}

// ---------------------------------------------------------------- generic documents

std::string Doc::str(const char* key) const { return getStr(fields, key); }
long long Doc::num(const char* key) const { return getInt(fields, key); }
std::vector<unsigned char> Doc::bytes(const char* key) const { return base64Decode(fields[key]["bytesValue"].s); }

std::vector<std::string> Doc::list(const char* key) const {
    std::vector<std::string> v;
    for (auto& e : fields[key]["arrayValue"]["values"].a) v.push_back(e["stringValue"].s);
    return v;
}

std::string fvStr(const std::string& v) { return fStr(v); }
std::string fvInt(long long v) { return fInt(v); }
std::string fvBytes(const void* data, size_t n) { return "{\"bytesValue\":" + jsonEscape(base64Encode(data, n)) + "}"; }

std::string fvList(const std::vector<std::string>& v) {
    std::string o = "{\"arrayValue\":{\"values\":[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + fStr(v[i]);
    return o + "]}}";
}
std::string fvBody(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string o = "{\"fields\":{";
    for (size_t i = 0; i < kv.size(); i++) o += (i ? "," : "") + jsonEscape(kv[i].first) + ":" + kv[i].second;
    return o + "}}";
}

static void parseDoc(const Json& j, Doc& d) {
    d.id = lastSegment(j["name"].s);
    d.updateTime = j["updateTime"].s;
    d.fields = j["fields"];
    d.exists = !j["name"].s.empty();
}

Err docGet(Session& s, const std::string& path, Doc& d) {
    d = Doc();
    std::string out;
    Err e = ensureToken(s);
    if (e != OK) return e;
    int st = http(L"GET", L"firestore.googleapis.com", std::string(DOCS) + "/" + path, "", nullptr, s.idToken, out);
    if (st == 404) return OK;   // simply not there
    e = classify(st, out);
    if (e == OK) parseDoc(Json::parse(out), d);
    return e;
}

Err docSet(Session& s, const std::string& path, const std::string& body, const std::vector<std::string>& mask) {
    std::string url = std::string(DOCS) + "/" + path, out;
    for (size_t i = 0; i < mask.size(); i++) url += (i ? "&" : "?") + std::string("updateMask.fieldPaths=") + mask[i];
    return fsCall(s, L"PATCH", url, body, out);
}

Err docCreate(Session& s, const std::string& collection, const std::string& id, const std::string& body, Doc* d) {
    std::string out;
    Err e = fsCall(s, L"POST", std::string(DOCS) + "/" + collection + (id.empty() ? "" : "?documentId=" + id), body, out);
    if (e == OK && d) parseDoc(Json::parse(out), *d);
    return e;
}

Err docDelete(Session& s, const std::string& path) {
    std::string out;
    return fsCall(s, L"DELETE", std::string(DOCS) + "/" + path, "", out);
}

Err commitWrites(Session& s, const std::vector<Write>& writes) {
    std::string prefix = std::string(DOCS).substr(4) + "/";   // DOCS without its "/v1/" prefix
    std::string body = "{\"writes\":[";
    for (size_t i = 0; i < writes.size(); i++) {
        const Write& w = writes[i];
        if (i) body += ",";
        // the body already looks like {"fields":{...}}; the document name goes beside it
        std::string fields = w.body.substr(1);
        body += "{\"update\":{\"name\":" + jsonEscape(prefix + w.path) + "," + fields;
        if (!w.mask.empty()) {
            body += ",\"updateMask\":{\"fieldPaths\":[";
            for (size_t k = 0; k < w.mask.size(); k++) body += (k ? "," : "") + jsonEscape(w.mask[k]);
            body += "]}";
        }
        if (w.ifUpdateTime == "absent") body += ",\"currentDocument\":{\"exists\":false}";
        else if (!w.ifUpdateTime.empty()) body += ",\"currentDocument\":{\"updateTime\":" + jsonEscape(w.ifUpdateTime) + "}";
        body += "}";
    }
    body += "]}";
    std::string out;
    return fsCall(s, L"POST", std::string(DOCS) + ":commit", body, out);
}

Err docList(Session& s, const std::string& collection, std::vector<Doc>& list, int limit) {
    std::string out;
    Err e = fsCall(s, L"GET", std::string(DOCS) + "/" + collection + "?pageSize=" + std::to_string(limit), "", out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& item : j["documents"].a) {
        Doc d;
        parseDoc(item, d);
        list.push_back(d);
    }
    return OK;
}

Err docQuery(Session& s, const std::string& collection, const std::string& field, const std::string& op,
             const std::string& value, std::vector<Doc>& list, int limit) {
    // "tournaments/abc/results" -> parent "tournaments/abc", collection id "results"
    size_t cut = collection.find_last_of('/');
    std::string parent = cut == std::string::npos ? "" : "/" + collection.substr(0, cut);
    std::string cid = cut == std::string::npos ? collection : collection.substr(cut + 1);
    std::string body = "{\"structuredQuery\":{\"from\":[{\"collectionId\":" + jsonEscape(cid) + "}]";
    if (!field.empty())
        body += ",\"where\":{\"fieldFilter\":{\"field\":{\"fieldPath\":" + jsonEscape(field) + "},\"op\":" + jsonEscape(op) +
                ",\"value\":" + value + "}}";
    body += ",\"limit\":" + std::to_string(limit) + "}}";
    std::string out;
    Err e = fsCall(s, L"POST", std::string(DOCS) + parent + ":runQuery", body, out);
    if (e != OK) return e;
    list.clear();
    Json j = Json::parse(out);
    for (auto& item : j.a) {
        if (!item.has("document")) continue;
        Doc d;
        parseDoc(item["document"], d);
        list.push_back(d);
    }
    return OK;
}

// Must match firstFounders() in firestore.rules.
static const char* FIRST_FOUNDERS[] = {"TGZDeRhwX9Rz1sYmkWgYWqaAwex1", "qdWJEUZt32dOIHemNB2aTeQhzhK2"};
static const int FOUNDER_PLACES = 3;   // five in all: the two above plus three more

bool isFounder(Session& s, const std::string& uid) {
    for (const char* f : FIRST_FOUNDERS)
        if (uid == f) return true;
    Doc d;
    if (docGet(s, "config/founders", d) != OK || !d.exists) return false;
    for (auto& u : d.list("uids"))
        if (u == uid) return true;
    return false;
}

bool claimFounder(Session& s) {
    for (int attempt = 0; attempt < 3; attempt++) {   // someone else may take a place at the same moment
        Doc d;
        if (docGet(s, "config/founders", d) != OK) return false;
        std::vector<std::string> uids = d.exists ? d.list("uids") : std::vector<std::string>();
        for (auto& u : uids)
            if (u == s.uid) return true;
        if ((int)uids.size() >= FOUNDER_PLACES) return false;
        uids.push_back(s.uid);
        std::string body = fvBody({{"uids", fvList(uids)}});
        Err e = d.exists ? docSet(s, "config/founders", body) : docCreate(s, "config", "founders", body);
        if (e == OK) return true;
        if (e == NET) return false;
    }
    return false;
}

int getPlan(Session& s, const std::string& uid) {
    if (isFounder(s, uid)) return 3;
    Doc d;
    if (docGet(s, "plans/" + uid, d) != OK || !d.exists) return 0;
    std::string p = d.str("plan");
    return p == "ruby" ? 3 : p == "gold" ? 2 : p == "silver" ? 1 : 0;
}

int httpsRequest(const wchar_t* method, const std::wstring& host, const std::string& path, const std::string& body,
                 const char* contentType, std::string& out) {
    int ea = g_emuAuth;
    g_emuAuth = 0;   // never redirect third-party hosts to the emulator
    int st = http(method, host.c_str(), path, body, contentType, "", out);
    g_emuAuth = ea;
    return st;
}

}   // namespace fb
