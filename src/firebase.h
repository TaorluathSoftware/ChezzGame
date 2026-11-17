// Firebase over plain HTTPS (WinHTTP): email/password accounts through Firebase Auth and the
// online games through Cloud Firestore's REST API. Every call blocks, so the UI runs them on its
// network worker thread.
#pragma once
#include "json.h"
#include <string>
#include <vector>

namespace fb {

struct Session {
    std::string uid, email, name, idToken, refreshToken;
    long long expiresAt = 0;   // unix ms
    bool valid() const { return !uid.empty() && !refreshToken.empty(); }
};

struct Profile {
    std::string uid, name, bio, photo;   // photo: hosted image URL (Silver and up), empty = preset picture
    int avatar = 0, banner = 0, rating = 1200, wins = 0, draws = 0, losses = 0;
    long long updated = 0;   // last time they were seen, unix ms
};

// An entry in someone's friends / requests / blocked list.
struct Person {
    std::string uid, name, kind, photo;   // kind (requests only): "request", "accepted" or "removed"
    int avatar = 0;
    long long at = 0;
};

struct Chat {
    std::string id, a, b, aName, bName, last, lastFrom;
    int aAvatar = 0, bAvatar = 0;
    long long lastAt = 0;
};

struct Message {
    std::string from, text;
    long long at = 0;
};

struct GameDoc {
    std::string id, updateTime;
    std::string white, black, whiteName, blackName, createdBy, status, moves, result, reason;
    std::string invite, event;   // invite: only this uid may sit down; event: tournament id, if any
    int whiteRating = 1200, blackRating = 1200;
    long long baseMs = 0, incMs = 0, whiteMs = 0, blackMs = 0, lastMoveAt = 0, createdAt = 0;
};

enum Err { OK = 0, NET = 1, AUTH = 2, CONFLICT = 3, OTHER = 4 };

long long nowMs();   // unix time in ms

// accounts: the returned message is empty on success, otherwise readable text for the player
std::string signUp(Session& s, const std::string& email, const std::string& pass, const std::string& name);
std::string signIn(Session& s, const std::string& email, const std::string& pass);
std::string signInGuest(Session& s, const std::string& name);   // anonymous account, never saved to disk
Err refresh(Session& s, std::string* msg = nullptr);
Err setDisplayName(Session& s, const std::string& name);
bool loadSession(const std::wstring& path, Session& s);
void saveSession(const std::wstring& path, const Session& s);
void clearSession(const std::wstring& path);

// firestore
Err getProfile(Session& s, const std::string& uid, Profile& p);
Err putProfile(Session& s, const Profile& p);
Err createGame(Session& s, GameDoc& g);
Err getGame(Session& s, const std::string& id, GameDoc& g);
Err updateGame(Session& s, GameDoc& g);   // guarded by g.updateTime; CONFLICT if someone wrote first
Err deleteGame(Session& s, const std::string& id);
Err listOpenGames(Session& s, std::vector<GameDoc>& out);

// people: lookup, friends, requests, blocks (members only; the rules refuse guests)
Err searchUsers(Session& s, const std::string& namePrefix, std::vector<Profile>& out);
Err listPeople(Session& s, const std::string& ownerUid, const char* list, std::vector<Person>& out);   // "friends", "requests", "blocked"
Err putPerson(Session& s, const std::string& ownerUid, const char* list, const Person& p);            // doc id = p.uid
Err deletePerson(Session& s, const std::string& ownerUid, const char* list, const std::string& uid);

// direct messages
std::string chatIdFor(const std::string& uidA, const std::string& uidB);
Err listChats(Session& s, std::vector<Chat>& out);
Err listMessages(Session& s, const std::string& chatId, std::vector<Message>& out);   // oldest first, last 60
Err sendMessage(Session& s, const Profile& me, const Person& to, const std::string& text);

// ---- generic documents (teams, tournaments, engines, puzzles, plans, config)
struct Doc {
    std::string id, updateTime;
    Json fields;
    bool exists = false;
    std::string str(const char* key) const;
    long long num(const char* key) const;
    std::vector<std::string> list(const char* key) const;
    std::vector<unsigned char> bytes(const char* key) const;
};
std::string fvStr(const std::string& v);
std::string fvInt(long long v);
std::string fvList(const std::vector<std::string>& v);
std::string fvBytes(const void* data, size_t n);
// {"fields":{...}} from name/value pairs (values made with fvStr / fvInt / fvList)
std::string fvBody(const std::vector<std::pair<std::string, std::string>>& kv);
Err docGet(Session& s, const std::string& path, Doc& out);   // path like "teams/abc"; OK with exists=false if missing
// Writes (creates or replaces). A non-empty mask updates only those fields.
Err docSet(Session& s, const std::string& path, const std::string& body, const std::vector<std::string>& mask = {});
// Creates with a chosen id ("" = automatic). CONFLICT if that id is already taken.
Err docCreate(Session& s, const std::string& collection, const std::string& id, const std::string& body, Doc* out = nullptr);
Err docDelete(Session& s, const std::string& path);

// Several documents written together, all or none. `ifUpdateTime` guards one document against
// anyone else having written it since you read it ("" = no guard, "absent" = it must not exist).
struct Write {
    std::string path, body;
    std::vector<std::string> mask;
    std::string ifUpdateTime;
};
Err commitWrites(Session& s, const std::vector<Write>& writes);
Err docList(Session& s, const std::string& collection, std::vector<Doc>& out, int limit = 100);
// One-field filter. op: EQUAL, ARRAY_CONTAINS, GREATER_THAN_OR_EQUAL ... ; field "" = no filter.
Err docQuery(Session& s, const std::string& collection, const std::string& field, const std::string& op,
             const std::string& value, std::vector<Doc>& out, int limit = 50);
// 0 Bronze, 1 Silver, 2 Gold, 3 Ruby. Plans live in plans/{uid}, which only the project owner can write.
int getPlan(Session& s, const std::string& uid);
// Founding member (one of the first five accounts): Ruby for life.
bool isFounder(Session& s, const std::string& uid);
// Called once when a new account is created: takes a founder place if one of the five is still free.
bool claimFounder(Session& s);
// Plain HTTPS to another host (image hosting). Returns the HTTP status, 0 if unreachable.
int httpsRequest(const wchar_t* method, const std::wstring& host, const std::string& path, const std::string& body,
                 const char* contentType, std::string& out);

// Test hook: point every call at a local Firebase emulator (http://127.0.0.1) instead of Google.
void useEmulator(int authPort, int firestorePort);

}   // namespace fb
