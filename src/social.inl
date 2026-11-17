// Members' social side: profile card with preset pictures, member lookup, friends, blocks and
// direct messages. Included by main.cpp (after drawTopBar, before render()).
//
// Friendship is a handshake through each member's "requests" inbox in Firestore:
//   A adds B       -> A drops a "request" note in B's inbox
//   B accepts      -> B writes A into B's friends, drops an "accepted" note in A's inbox
//   A's next sync  -> A sees the note, writes B into A's friends, deletes the note
// "removed" notes undo it the same way. Nobody ever writes into someone else's friends list.

static const int AVATAR_COUNT = 48;   // 0-23 for everyone, 24-47 for Silver and up
static std::map<int, Bitmap*> g_presetImg;
static fb::Person g_selPerson;            // who the people screen is showing on the right
static fb::Profile g_viewed;
static bool g_viewedOk = false, g_socialBusy = false, g_searched = false, g_socialLoaded = false;
static std::vector<std::string> g_requestedUids;   // requests sent this session ("REQUEST SENT")
static int g_peopleTab = 0, g_avatarPick = 0, g_socialTicks = 0;
static float g_chatScroll = 0;
static int g_viewedPlan = 0;

// ---------------------------------------------------------------- layout shared with the text boxes

static RectF socialLeft() { return RectF(contentX(), contentY(), min(S(520), contentW() * 0.47f), (REAL)g_H - contentY() - S(24)); }
static RectF socialRight() {
    RectF l = socialLeft();
    float x = l.X + l.Width + S(24);
    return RectF(x, contentY(), g_W - x - S(24), l.Height);
}
static RectF peopleSearchRect() {
    RectF l = socialLeft();
    return RectF(l.X + S(18), l.Y + S(88), l.Width - S(36) - S(124), S(34));
}
static RectF chatInputRect() {
    RectF l = socialLeft();
    l.Width = min(l.Width, S(380));   // the conversation list is narrower than the other left sheets
    float x = l.X + l.Width + S(24);
    RectF r(x, contentY(), g_W - x - S(24), l.Height);
    return RectF(r.X + S(18), r.Y + r.Height - S(54), r.Width - S(36) - S(108), S(36));
}
static RectF profileNameRect() {
    RectF l = socialLeft();
    return RectF(l.X + S(24), l.Y + S(246), l.Width - S(48), S(34));
}
static RectF profileBioRect() {
    RectF l = socialLeft();
    return RectF(l.X + S(24), l.Y + S(320), l.Width - S(48), S(34));
}

// ---------------------------------------------------------------- small helpers

static bool isFriend(const std::string& uid) {
    for (auto& p : g_friends)
        if (p.uid == uid) return true;
    return false;
}
static bool isBlocked(const std::string& uid) {
    for (auto& p : g_blocked)
        if (p.uid == uid) return true;
    return false;
}
static bool hasRequestFrom(const std::string& uid) {
    for (auto& p : g_requests)
        if (p.uid == uid && p.kind == "request") return true;
    return false;
}
static int pendingRequests() {
    int n = 0;
    for (auto& p : g_requests) n += p.kind == "request";
    return n;
}
static fb::Person mePerson(const char* kind) {
    fb::Person p;
    p.uid = g_myUid;
    p.name = g_myName;
    p.avatar = g_profile.avatar;
    p.photo = g_profile.photo;
    p.kind = kind;
    return p;
}
static fb::Profile meProfile() {
    fb::Profile p = g_profile;
    p.uid = g_myUid;
    p.name = g_myName;
    return p;
}
static bool socialAllowed() { return !g_guest && !g_offline && !g_myUid.empty(); }

static std::wstring agoText(long long ms) {
    if (!ms) return L"not seen yet";
    long long d = (fb::nowMs() - ms) / 1000;
    if (d < 180) return L"here just now";
    if (d < 3600) return L"seen " + std::to_wstring(d / 60) + L" minutes ago";
    if (d < 86400) return L"seen " + std::to_wstring(d / 3600) + (d < 7200 ? L" hour ago" : L" hours ago");
    return L"seen " + std::to_wstring(d / 86400) + (d < 172800 ? L" day ago" : L" days ago");
}
static std::wstring clockText(long long ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm* lt = localtime(&t);
    wchar_t b[16];
    swprintf(b, 16, L"%02d:%02d", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0);
    return b;
}

// Preset profile pictures: six pieces on four backgrounds. A file assets/avatars/preset_NN.png
// (01..24) replaces the drawn one.
static void drawAvatar(Graphics& gr, int idx, RectF r) {
    idx = max(0, min(AVATAR_COUNT - 1, idx));
    auto it = g_presetImg.find(idx);
    if (it == g_presetImg.end()) {
        wchar_t n[40];
        swprintf(n, 40, L"avatars\\preset_%02d", idx + 1);
        it = g_presetImg.emplace(idx, loadFirst({n})).first;
    }
    static const BYTE BG[8][3] = {{186, 78, 58}, {52, 100, 156}, {226, 190, 116}, {196, 210, 224},
                                  {66, 68, 78}, {140, 36, 68}, {166, 218, 188}, {240, 212, 224}};
    int type = PAWN + (idx % 24) / 4, col = idx % 4 + (idx >= 24 ? 4 : 0);
    GraphicsPath circle;
    circle.AddEllipse(r);
    GraphicsState st = gr.Save();
    gr.SetClip(&circle, CombineModeIntersect);
    if (Bitmap* b = it->second) {
        gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gr.DrawImage(b, r);
    } else {
        SolidBrush bg(Color(255, BG[col][0], BG[col][1], BG[col][2]));
        gr.FillRectangle(&bg, r);
        int px = max(8, (int)r.Width);
        gr.DrawImage(pieceBitmap(mkpiece(col % 4 < 2 ? WHITE : BLACK, type), px, 0.7f, false), r);
    }
    gr.Restore(st);
    Pen rim(C_PRINT, max(1.f, r.Width / 48));
    gr.DrawEllipse(&rim, r);
}

static void saveChatSeen() {
    FILE* f = _wfopen((g_data + L"\\chatseen.txt").c_str(), L"w");
    if (!f) return;
    for (auto& kv : g_chatSeen) fprintf(f, "%s %lld\n", kv.first.c_str(), kv.second);
    fclose(f);
}
static void loadChatSeen() {
    FILE* f = _wfopen((g_data + L"\\chatseen.txt").c_str(), L"r");
    if (!f) return;
    char id[160];
    long long at;
    while (fscanf(f, "%159s %lld", id, &at) == 2) g_chatSeen[id] = at;
    fclose(f);
}
static int countUnread() {
    int n = 0;
    for (auto& c : g_chats)
        if (c.lastFrom != g_myUid && c.lastAt > g_chatSeen[c.id]) n++;
    return n;
}
static fb::Person otherOf(const fb::Chat& c) {
    fb::Person p;
    bool meA = c.a == g_myUid;
    p.uid = meA ? c.b : c.a;
    p.name = meA ? c.bName : c.aName;
    p.avatar = meA ? c.bAvatar : c.aAvatar;
    return p;
}

static void socialSay(const std::wstring& msg, bool err) {
    g_socialMsg = msg;
    g_socialMsgErr = err;
    invalidate();
}

static void socialReset() {
    g_friends.clear();
    g_requests.clear();
    g_blocked.clear();
    g_found.clear();
    g_chats.clear();
    g_msgs.clear();
    g_requestedUids.clear();
    g_chatWith = fb::Person();
    g_selPerson = fb::Person();
    g_viewedOk = g_searched = g_socialLoaded = g_socialBusy = false;
    g_unread = 0;
    g_socialMsg.clear();
}

// ---------------------------------------------------------------- syncing

// Friends, inbox, blocks and conversations in one background pass. Also completes handshakes.
static void refreshSocial() {
    if (!socialAllowed() || g_socialBusy) return;
    g_socialBusy = true;
    netAsync([]() -> std::function<void()> {
        std::vector<fb::Person> fr, rq, bl;
        std::vector<fb::Chat> ch;
        const std::string me = g_sess.uid;
        fb::Err e = fb::listPeople(g_sess, me, "friends", fr);
        if (e == fb::OK) e = fb::listPeople(g_sess, me, "requests", rq);
        if (e == fb::OK) e = fb::listPeople(g_sess, me, "blocked", bl);
        if (e == fb::OK) {
            for (size_t i = 0; i < rq.size();) {
                fb::Person p = rq[i];
                if (p.kind == "accepted" || p.kind == "removed") {
                    bool add = p.kind == "accepted";
                    p.kind.clear();
                    if (add) {
                        if (fb::putPerson(g_sess, me, "friends", p) == fb::OK) fr.push_back(p);
                    } else {
                        fb::deletePerson(g_sess, me, "friends", p.uid);
                        fr.erase(std::remove_if(fr.begin(), fr.end(), [&](const fb::Person& q) { return q.uid == p.uid; }), fr.end());
                    }
                    fb::deletePerson(g_sess, me, "requests", p.uid);
                    rq.erase(rq.begin() + i);
                } else {
                    i++;
                }
            }
            e = fb::listChats(g_sess, ch);
        }
        return [e, fr, rq, bl, ch]() {
            g_socialBusy = false;
            if (e != fb::OK) {
                if (g_screen == SCR_PEOPLE || g_screen == SCR_CHAT)
                    socialSay(e == fb::NET ? L"Can't reach the club's server." : L"The server refused that (are the rules deployed?).", true);
                return;
            }
            int before = g_unread + pendingRequests();
            g_friends = fr;
            g_requests = rq;
            g_blocked = bl;
            g_chats = ch;
            std::sort(g_chats.begin(), g_chats.end(), [](const fb::Chat& x, const fb::Chat& y) { return x.lastAt > y.lastAt; });
            // the conversation you are looking at counts as read
            if (g_screen == SCR_CHAT && !g_chatWith.uid.empty()) {
                std::string id = fb::chatIdFor(g_myUid, g_chatWith.uid);
                for (auto& c : g_chats)
                    if (c.id == id && c.lastAt > g_chatSeen[id]) { g_chatSeen[id] = c.lastAt; saveChatSeen(); }
            }
            g_unread = countUnread();
            if (g_socialLoaded && g_unread + pendingRequests() > before) audioPlay(SND_NOTIFY);
            g_socialLoaded = true;
            invalidate();
        };
    });
}

static void loadMessages() {
    if (g_chatWith.uid.empty() || !socialAllowed()) return;
    std::string id = fb::chatIdFor(g_myUid, g_chatWith.uid);
    netAsync([id]() -> std::function<void()> {
        std::vector<fb::Message> list;
        fb::Err e = fb::listMessages(g_sess, id, list);
        return [e, id, list]() {
            if (g_chatWith.uid.empty() || fb::chatIdFor(g_myUid, g_chatWith.uid) != id) return;
            if (e == fb::OK && (list.size() != g_msgs.size() || (!list.empty() && list.back().at != g_msgs.back().at))) {
                bool incoming = !list.empty() && list.back().from != g_myUid && list.size() > g_msgs.size() && !g_msgs.empty();
                g_msgs = list;
                if (incoming) audioPlay(SND_NOTIFY);
                invalidate();
            }
            if (!list.empty() && list.back().at > g_chatSeen[id]) {
                g_chatSeen[id] = list.back().at;
                saveChatSeen();
                g_unread = countUnread();
            }
        };
    });
}

// Called on every network tick (about once a second) while signed in.
static void socialTick() {
    if (!socialAllowed() || g_screen == SCR_LOGIN || g_screen == SCR_GAME) return;
    g_socialTicks++;
    if (g_socialTicks % 12 == 1) refreshSocial();
    if (g_screen == SCR_CHAT && g_socialTicks % 3 == 0) loadMessages();
}

static void viewPerson(const fb::Person& p) {
    g_selPerson = p;
    g_viewedOk = false;
    g_socialMsg.clear();
    std::string uid = p.uid;
    netAsync([uid]() -> std::function<void()> {
        fb::Profile prof;
        fb::Err e = fb::getProfile(g_sess, uid, prof);
        int plan = e == fb::OK ? fb::getPlan(g_sess, uid) : 0;
        return [e, prof, uid, plan]() {
            if (e == fb::OK && g_selPerson.uid == uid) {
                g_viewed = prof;
                g_viewedPlan = plan;
                g_viewedOk = true;
                invalidate();
            }
        };
    });
    invalidate();
}

// ---------------------------------------------------------------- actions

static void openPeople() {
    if (!socialAllowed()) return;
    g_socialMsg.clear();
    goScreen(SCR_PEOPLE);
    refreshSocial();
}

static void openChatWith(const fb::Person& p) {
    if (!socialAllowed() || p.uid == g_myUid) return;
    g_chatWith = p;
    g_msgs.clear();
    g_chatScroll = 0;
    g_socialMsg.clear();
    goScreen(SCR_CHAT);
    loadMessages();
}

static void openChat() {
    if (!socialAllowed()) return;
    g_socialMsg.clear();
    goScreen(SCR_CHAT);
    refreshSocial();
    if (!g_chatWith.uid.empty()) loadMessages();
}

static void openProfile() {
    if (g_guest || g_myUid.empty()) return;
    g_avatarPick = g_profile.avatar;
    g_photoDraft = g_profile.photo;
    g_bannerDraft = g_profile.banner;
    g_socialMsg.clear();
    SetWindowTextW(g_edPName, widen(g_myName).c_str());
    SetWindowTextW(g_edBio, widen(g_profile.bio).c_str());
    goScreen(SCR_PROFILE);
}

static void doSearch() {
    std::string q = narrow(editText(g_edSearch));
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.size() < 2) { socialSay(L"Type at least two letters of a member's name.", true); return; }
    socialSay(L"Looking through the register\x2026");
    netAsync([q]() -> std::function<void()> {
        std::vector<fb::Profile> list;
        fb::Err e = fb::searchUsers(g_sess, q, list);
        return [e, list]() {
            g_searched = true;
            g_found = list;
            socialSay(e == fb::OK ? (list.empty() ? L"Nobody by that name." : L"") : L"The lookup didn't go through.", e != fb::OK);
        };
    });
}

static void sendFriendRequest(const fb::Person& to) {
    if (to.uid == g_myUid || isFriend(to.uid)) return;
    fb::Person me = mePerson("request");
    std::string uid = to.uid;
    std::wstring name = widen(to.name);
    netAsync([me, uid, name]() -> std::function<void()> {
        fb::Err e = fb::putPerson(g_sess, uid, "requests", me);
        return [e, uid, name]() {
            if (e == fb::OK) {
                g_requestedUids.push_back(uid);
                socialSay(L"Friend request sent to " + name + L".");
            } else {
                socialSay(e == fb::AUTH ? name + L" isn't accepting requests from you." : L"Couldn't send the request.", true);
            }
        };
    });
}

static void acceptRequest(fb::Person from) {
    fb::Person me = mePerson("accepted");
    from.kind.clear();
    netAsync([me, from]() -> std::function<void()> {
        fb::Err e = fb::putPerson(g_sess, g_sess.uid, "friends", from);
        if (e == fb::OK) {
            fb::putPerson(g_sess, from.uid, "requests", me);
            fb::deletePerson(g_sess, g_sess.uid, "requests", from.uid);
        }
        return [e, from]() {
            socialSay(e == fb::OK ? L"You and " + widen(from.name) + L" are now friends." : L"Couldn't accept just now.", e != fb::OK);
            refreshSocial();
        };
    });
}

static void declineRequest(const std::string& uid) {
    netAsync([uid]() -> std::function<void()> {
        fb::deletePerson(g_sess, g_sess.uid, "requests", uid);
        return []() { socialSay(L"Request declined."); refreshSocial(); };
    });
}

static void removeFriend(const std::string& uid) {
    fb::Person me = mePerson("removed");
    netAsync([me, uid]() -> std::function<void()> {
        fb::deletePerson(g_sess, g_sess.uid, "friends", uid);
        fb::putPerson(g_sess, uid, "requests", me);   // lets their side drop us too
        return []() { socialSay(L"Removed from your friends."); refreshSocial(); };
    });
}

static void blockPerson(fb::Person p) {
    fb::Person me = mePerson("removed");
    p.kind.clear();
    netAsync([me, p]() -> std::function<void()> {
        fb::putPerson(g_sess, p.uid, "requests", me);
        fb::Err e = fb::putPerson(g_sess, g_sess.uid, "blocked", p);
        fb::deletePerson(g_sess, g_sess.uid, "friends", p.uid);
        fb::deletePerson(g_sess, g_sess.uid, "requests", p.uid);
        return [e, p]() {
            socialSay(e == fb::OK ? widen(p.name) + L" is blocked: no requests, no messages, and their tables are hidden."
                                  : L"Couldn't block just now.", e != fb::OK);
            if (g_chatWith.uid == p.uid) { g_chatWith = fb::Person(); g_msgs.clear(); }
            refreshSocial();
        };
    });
}

static void unblockPerson(const std::string& uid) {
    netAsync([uid]() -> std::function<void()> {
        fb::deletePerson(g_sess, g_sess.uid, "blocked", uid);
        return []() { socialSay(L"Unblocked."); refreshSocial(); };
    });
}

static void sendChat() {
    if (g_chatWith.uid.empty() || !socialAllowed()) return;
    std::wstring w = editText(g_edChat);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    if (w.empty()) return;
    if (w.size() > 500) w.resize(500);
    if (isBlocked(g_chatWith.uid)) { socialSay(L"Unblock them first to send a message.", true); return; }
    std::string text = narrow(w);
    SetWindowTextW(g_edChat, L"");
    fb::Message m;
    m.from = g_myUid;
    m.text = text;
    m.at = fb::nowMs();
    g_msgs.push_back(m);   // show it straight away; the next sync confirms it
    g_chatScroll = 0;
    g_socialMsg.clear();
    fb::Profile me = meProfile();
    fb::Person to = g_chatWith;
    netAsync([me, to, text]() -> std::function<void()> {
        fb::Err e = fb::sendMessage(g_sess, me, to, text);
        return [e, to]() {
            if (e != fb::OK)
                socialSay(e == fb::AUTH ? L"Not delivered: " + widen(to.name) + L" isn't accepting messages from you."
                                        : L"Not delivered. Check your connection.", true);
            loadMessages();
            refreshSocial();
        };
    });
    invalidate();
}

static void saveProfile() {
    if (g_socialBusy || g_guest) return;
    std::string name = narrow(editText(g_edPName)), bio = narrow(editText(g_edBio));
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() < 2 || name.size() > 20) { socialSay(L"Your ladder name needs 2 to 20 characters.", true); return; }
    if (bio.size() > 140) bio.resize(140);
    fb::Profile p = meProfile();
    bool renamed = name != g_myName;
    p.name = name;
    p.bio = bio;
    p.avatar = g_avatarPick;
    p.photo = g_plan >= 1 ? g_photoDraft : "";
    p.banner = g_plan >= 1 ? g_bannerDraft : 0;
    if (g_offline) { socialSay(L"You're offline; the card can't be saved right now.", true); return; }
    socialSay(L"Saving your card\x2026");
    netAsync([p, renamed]() -> std::function<void()> {
        if (renamed && fb::setDisplayName(g_sess, p.name) == fb::OK) fb::saveSession(sessionPath(), g_sess);
        fb::Err e = fb::putProfile(g_sess, p);
        return [e, p]() {
            if (e == fb::OK) {
                g_profile.bio = p.bio;
                g_profile.avatar = p.avatar;
                g_profile.photo = p.photo;
                g_profile.banner = p.banner;
                g_profile.name = p.name;
                g_myName = p.name;
                socialSay(L"Saved.");
            } else {
                socialSay(L"Couldn't save your card.", true);
            }
        };
    });
}

// ---------------------------------------------------------------- rendering

static void sheetHeader(Graphics& gr, RectF sheet, const std::wstring& title) {
    drawText(gr, title, RectF(sheet.X + S(18), sheet.Y + S(8), sheet.Width - S(36), S(30)), F_SERIF, S(15), C_PRINT, FontStyleBold);
    Pen dbl(C_PRINT, 1.f);
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(40), sheet.X + sheet.Width - S(12), sheet.Y + S(40));
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(43), sheet.X + sheet.Width - S(12), sheet.Y + S(43));
}

static void fieldBox(Graphics& gr, RectF r, const wchar_t* label) {
    drawText(gr, label, RectF(r.X, r.Y - S(22), r.Width + S(120), S(20)), F_MONO, S(12), C_FAINT, 0, 0, 1);
    SolidBrush box(C_CARD);
    gr.FillRectangle(&box, r);
    Pen line(C_PRINT, 1.f);
    gr.DrawLine(&line, r.X, r.Y + r.Height, r.X + r.Width, r.Y + r.Height);
}

static void statusLine(Graphics& gr, RectF r) {
    if (!g_socialMsg.empty()) drawText(gr, g_socialMsg, r, F_HAND, S(14), g_socialMsgErr ? C_RED : C_INK, 0, 0, 0, true);
}

static void renderProfile(Graphics& gr) {
    drawTopBar(gr, L"your member card  \x00B7  how other members see you");
    RectF L = socialLeft();
    paperSheet(gr, L, true);
    drawBanner(gr, g_bannerDraft, RectF(L.X + 1, L.Y + S(53), L.Width - 2, S(16)));
    {
        Pen red(Color(170, C_RED.GetR(), C_RED.GetG(), C_RED.GetB()), S(1.5f));
        gr.DrawLine(&red, L.X + 1, L.Y + S(52), L.X + L.Width - 1, L.Y + S(52));
    }
    drawText(gr, L"MY MEMBER CARD", RectF(L.X + S(24), L.Y + S(10), L.Width, S(36)), F_MONO, S(16), C_PRINT, FontStyleBold);
    drawText(gr, std::wstring(PLAN_NAME[g_plan]) + L" member", RectF(L.X, L.Y + S(10), L.Width - (g_plan ? S(60) : S(24)), S(36)), F_SERIF,
             S(14), C_FAINT, FontStyleItalic, 2);
    drawBadge(gr, g_plan, RectF(L.X + L.Width - S(52), L.Y + S(13), S(28), S(28)));
    float av = S(112);
    drawFace(gr, g_photoDraft, g_avatarPick, RectF(L.X + S(24), L.Y + S(80), av, av));
    float fx = L.X + S(24) + av + S(24), fw = L.X + L.Width - fx - S(20);
    drawText(gr, widen(g_myName), RectF(fx, L.Y + S(76), fw, S(38)), F_SERIF, S(26), C_PRINT, FontStyleBold);
    wchar_t buf[160];
    swprintf(buf, 160, L"online rating %d, record %d-%d-%d", g_profile.rating, g_profile.wins, g_profile.draws, g_profile.losses);
    drawText(gr, buf, RectF(fx, L.Y + S(114), fw, S(24)), F_MONO, S(13), C_PRINT);
    int beaten = 0, top = 0;
    for (int i = 0; i < 150; i++)
        if (g_rec[i].w > 0) { beaten++; top = allLevels()[i].rating; }
    swprintf(buf, 160, beaten ? L"ladder: %d of 150 bots beaten, best %d" : L"ladder: no bots beaten yet", beaten, top);
    drawText(gr, buf, RectF(fx, L.Y + S(136), fw, S(24)), F_MONO, S(13), C_PRINT);
    inkButton(gr, B_UPLOAD, RectF(fx, L.Y + S(166), S(176), S(30)), L"UPLOAD A PICTURE");
    if (!g_photoDraft.empty()) inkButton(gr, B_PRESETPIC, RectF(fx + S(184), L.Y + S(166), S(140), S(30)), L"USE A PRESET");
    else if (g_plan < 1) {
        drawBadge(gr, 1, RectF(fx + S(184), L.Y + S(170), S(22), S(22)));
        drawText(gr, L"Silver", RectF(fx + S(212), L.Y + S(166), S(80), S(30)), F_HAND, S(13), C_FAINT);
    }

    fieldBox(gr, profileNameRect(), L"NAME ON THE LADDER");
    fieldBox(gr, profileBioRect(), L"MOTTO (other members see this, up to 140 characters)");
    float y = profileBioRect().Y + S(48);
    drawText(gr, L"BANNER", RectF(L.X + S(24), y, S(90), S(34)), F_MONO, S(12), C_FAINT, 0, 0, 1);
    inkButton(gr, B_BANNER_DN, RectF(L.X + S(114), y + S(2), S(30), S(30)), L"\x2039");
    drawText(gr, BANNER_NAME[g_bannerDraft], RectF(L.X + S(144), y, S(120), S(34)), F_HAND, S(15), C_INK, 0, 1, 1);
    inkButton(gr, B_BANNER_UP, RectF(L.X + S(264), y + S(2), S(30), S(30)), L"\x203A");
    if (g_plan < 1) {
        drawBadge(gr, 1, RectF(L.X + S(306), y + S(6), S(22), S(22)));
        drawText(gr, L"Silver", RectF(L.X + S(334), y, S(80), S(34)), F_HAND, S(13), C_FAINT);
    }
    y += S(48);
    stampButton(gr, B_SAVEPROFILE, RectF(L.X + S(30), y, S(240), S(60)), L"SAVE CARD", L"");
    statusLine(gr, RectF(L.X + S(24), y + S(72), L.Width - S(48), S(50)));
    auto habits = g_brain.patterns();
    if (habits && y + S(180) < L.Y + L.Height) {
        auto traits = habits->traits();
        std::wstring t = L"The club brain on your play: ";
        if (traits.empty()) t += L"still getting to know you.";
        for (size_t i = 0; i < traits.size() && i < 3; i++) t += (i ? L"; " : L"") + widen(traits[i]);
        if (!traits.empty()) t += L".";
        drawText(gr, t, RectF(L.X + S(24), y + S(126), L.Width - S(48), S(60)), F_HAND, S(13), C_INK, 0, 0, 0, true);
    }

    RectF R = socialRight();
    paperSheet(gr, R);
    sheetHeader(gr, R, L"CHOOSE A PICTURE");
    int cols = 6, rows = 8;
    float gap = S(10), label = S(24);
    float cell = min((R.Width - S(40) - gap * (cols - 1)) / cols, (R.Height - S(100) - 2 * label - gap * (rows - 1)) / rows);
    cell = min(cell, S(84));
    float gx = R.X + (R.Width - (cell * cols + gap * (cols - 1))) / 2, gy = R.Y + S(52);
    for (int half = 0; half < 2; half++) {
        drawText(gr, half == 0 ? L"FOR EVERY MEMBER" : L"SILVER, GOLD AND RUBY", RectF(gx, gy, R.Width, label), F_MONO, S(11), C_FAINT, 0, 0, 1);
        if (half == 1) drawBadge(gr, 1, RectF(gx + S(176), gy + S(3), S(18), S(18)));
        gy += label;
        for (int i = 0; i < 24; i++) {
            // across: the six pieces; down: the four backgrounds
            int idx = half * 24 + (i % cols) * 4 + i / cols;
            RectF r(gx + (i % cols) * (cell + gap), gy + (i / cols) * (cell + gap), cell, cell);
            drawAvatar(gr, idx, r);
            if (half == 1 && g_plan < 1) {   // shown, but dimmed until you have the plan
                SolidBrush veil(Color(150, C_PAPER.GetR(), C_PAPER.GetG(), C_PAPER.GetB()));
                gr.FillEllipse(&veil, r);
            }
            if (idx == g_avatarPick && g_photoDraft.empty()) inkCircle(gr, r, C_RED);
            else if (r.Contains(g_mouseX, g_mouseY)) inkCircle(gr, r, Color(110, C_RED.GetR(), C_RED.GetG(), C_RED.GetB()));
            g_buttons.push_back({r, B_AVATAR + idx});
        }
        gy += 4 * (cell + gap) + S(4);
    }
    drawText(gr, L"Your own art: assets/avatars/preset_01.png to preset_48.png replace these.",
             RectF(R.X + S(18), R.Y + R.Height - S(30), R.Width - S(36), S(24)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 1);
}

static std::vector<fb::Person> peopleRows() {
    std::vector<fb::Person> rows;
    if (g_peopleTab == 0) rows = g_friends;
    else if (g_peopleTab == 1) {
        for (auto& p : g_requests)
            if (p.kind == "request") rows.push_back(p);
    } else if (g_peopleTab == 2) {
        for (auto& f : g_found) {
            fb::Person p;
            p.uid = f.uid;
            p.name = f.name;
            p.avatar = f.avatar;
            p.at = f.rating;   // "at" doubles as the rating for lookup rows
            rows.push_back(p);
        }
    } else rows = g_blocked;
    return rows;
}

static void renderPeople(Graphics& gr) {
    drawTopBar(gr, L"friends & members  \x00B7  look people up, add them, message them");
    RectF L = socialLeft();
    paperSheet(gr, L);
    wchar_t tb[4][40];
    swprintf(tb[0], 40, L"FRIENDS (%d)", (int)g_friends.size());
    swprintf(tb[1], 40, L"REQUESTS (%d)", pendingRequests());
    swprintf(tb[2], 40, L"FIND MEMBERS");
    swprintf(tb[3], 40, L"BLOCKED (%d)", (int)g_blocked.size());
    float tw = (L.Width - S(24)) / 4;
    for (int k = 0; k < 4; k++) {
        RectF tr(L.X + S(12) + k * tw, L.Y + S(6), tw, S(34));
        bool active = g_peopleTab == k, hover = tr.Contains(g_mouseX, g_mouseY);
        drawText(gr, tb[k], tr, F_SERIF, S(12), active ? C_PRINT : hover ? C_INK : C_FAINT, FontStyleBold, 1, 1);
        if (active) {
            Pen u(C_RED, S(2));
            gr.DrawLine(&u, tr.X + S(10), tr.Y + tr.Height, tr.X + tr.Width - S(10), tr.Y + tr.Height);
        }
        g_buttons.push_back({tr, B_TAB + k});
    }
    Pen dbl(C_PRINT, 1.f);
    gr.DrawLine(&dbl, L.X + S(12), L.Y + S(46), L.X + L.Width - S(12), L.Y + S(46));
    float y = L.Y + S(56);
    if (g_peopleTab == 2) {
        RectF sr = peopleSearchRect();
        fieldBox(gr, sr, L"MEMBER NAME STARTS WITH");
        inkButton(gr, B_SEARCH, RectF(sr.X + sr.Width + S(12), sr.Y, S(112), sr.Height), L"LOOK UP");
        y = sr.Y + sr.Height + S(16);
    }
    std::vector<fb::Person> rows = peopleRows();
    float rh = S(56);
    int maxRows = (int)((L.Y + L.Height - y - S(8)) / rh);
    if (rows.empty()) {
        const wchar_t* empty[4] = {L"No friends yet. Find members and add them.", L"No requests waiting.",
                                   L"Type a name above and press Look up.", L"Nobody is blocked."};
        std::wstring e = !g_socialLoaded && g_peopleTab != 2 ? L"Fetching\x2026" : empty[g_peopleTab];
        if (g_peopleTab == 2 && g_searched) e = L"Nobody by that name.";
        drawText(gr, e, RectF(L.X + S(30), y + S(10), L.Width - S(60), S(60)), F_HAND, S(15), C_INK, 0, 1, 0, true);
    }
    for (int i = 0; i < (int)rows.size() && i < maxRows; i++) {
        const fb::Person& p = rows[i];
        RectF r(L.X + S(8), y + i * rh, L.Width - S(16), rh);
        if (p.uid == g_selPerson.uid) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
        } else if (r.Contains(g_mouseX, g_mouseY)) {
            Pen pencil(C_FAINT, 1.f);
            gr.DrawLine(&pencil, r.X + S(64), r.Y + r.Height - S(8), r.X + S(240), r.Y + r.Height - S(8));
        }
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, r.X, r.Y + rh - 1, r.X + r.Width, r.Y + rh - 1);
        drawFace(gr, p.photo, p.avatar, RectF(r.X + S(10), r.Y + S(8), rh - S(16), rh - S(16)));
        drawText(gr, widen(p.name), RectF(r.X + S(64), r.Y + S(4), r.Width - S(74), rh * 0.52f), F_SERIF, S(16), C_PRINT,
                 FontStyleBold, 0, 2);
        std::wstring sub = g_peopleTab == 0 ? L"friend" : g_peopleTab == 1 ? L"would like to be friends"
                         : g_peopleTab == 2 ? L"rated " + std::to_wstring(p.at) : L"blocked";
        if (g_peopleTab == 2 && p.uid == g_myUid) sub += L"  (you)";
        if (g_peopleTab == 2 && isFriend(p.uid)) sub += L"  \x00B7  friend";
        drawText(gr, sub, RectF(r.X + S(64), r.Y + rh * 0.54f, r.Width - S(74), rh * 0.4f), F_SERIF, S(12), C_FAINT,
                 FontStyleItalic, 0, 0);
        g_buttons.push_back({r, B_PERSON + i});
    }

    RectF R = socialRight();
    paperSheet(gr, R, true);
    float ix = R.X + S(24), iw = R.Width - S(48), yy = R.Y + S(24);
    if (g_selPerson.uid.empty()) {
        drawText(gr, L"Pick someone on the left to see their card.", RectF(ix, yy, iw, S(60)), F_HAND, S(16), C_INK, 0, 0, 0, true);
        statusLine(gr, RectF(ix, yy + S(70), iw, S(60)));
        return;
    }
    const fb::Person& p = g_selPerson;
    float av = min(S(120), iw * 0.34f);
    if (g_viewedOk) drawBanner(gr, g_viewed.banner, RectF(R.X + 1, R.Y + 1, R.Width - 2, S(14)));
    drawFace(gr, g_viewedOk ? g_viewed.photo : p.photo, g_viewedOk ? g_viewed.avatar : p.avatar, RectF(ix, yy, av, av));
    if (g_viewedOk && g_viewedPlan > 0) drawBadge(gr, g_viewedPlan, RectF(R.X + R.Width - S(56), yy, S(30), S(30)));
    float fx = ix + av + S(22), fw = iw - av - S(22);
    drawText(gr, widen(g_viewedOk ? g_viewed.name : p.name), RectF(fx, yy, fw, S(38)), F_SERIF, S(26), C_PRINT, FontStyleBold);
    if (g_viewedOk) {
        wchar_t buf[120];
        swprintf(buf, 120, L"rated %d", g_viewed.rating);
        drawText(gr, buf, RectF(fx, yy + S(40), fw, S(24)), F_MONO, S(14), C_PRINT);
        swprintf(buf, 120, L"record %d-%d-%d", g_viewed.wins, g_viewed.draws, g_viewed.losses);
        drawText(gr, buf, RectF(fx, yy + S(64), fw, S(24)), F_MONO, S(14), C_PRINT);
        drawText(gr, agoText(g_viewed.updated), RectF(fx, yy + S(88), fw, S(24)), F_SERIF, S(13), C_FAINT, FontStyleItalic);
    } else {
        drawText(gr, L"fetching their card\x2026", RectF(fx, yy + S(40), fw, S(24)), F_HAND, S(13), C_FAINT);
    }
    yy += av + S(16);
    if (g_viewedOk && !g_viewed.bio.empty()) {
        drawText(gr, L"\x201C" + widen(g_viewed.bio) + L"\x201D", RectF(ix, yy, iw, S(66)), F_HAND, S(15), C_INK, 0, 0, 0, true);
        yy += S(72);
    }
    float bw = min(iw, S(250));
    auto btn = [&](int id, const wchar_t* label, bool enabled = true) {
        inkButton(gr, id, RectF(ix, yy, bw, S(38)), label, enabled);
        yy += S(46);
    };
    if (p.uid == g_myUid) {
        drawText(gr, L"This is you.", RectF(ix, yy, iw, S(30)), F_HAND, S(15), C_INK);
        yy += S(40);
    } else if (isBlocked(p.uid)) {
        btn(B_ACT_UNBLOCK, L"UNBLOCK");
    } else {
        bool sent = std::find(g_requestedUids.begin(), g_requestedUids.end(), p.uid) != g_requestedUids.end();
        if (hasRequestFrom(p.uid)) {
            btn(B_ACT_ACCEPT, L"ACCEPT FRIEND REQUEST");
            btn(B_ACT_DECLINE, L"DECLINE");
        } else if (isFriend(p.uid)) {
            btn(B_ACT_MSG, L"SEND A MESSAGE");
            btn(B_ACT_REMOVE, L"REMOVE FRIEND");
        } else {
            btn(B_ACT_ADD, sent ? L"REQUEST SENT" : L"ADD FRIEND", !sent);
            btn(B_ACT_MSG, L"SEND A MESSAGE");
        }
        btn(B_ACT_BLOCK, L"BLOCK");
    }
    statusLine(gr, RectF(ix, yy + S(6), iw, S(60)));
}

static void renderChat(Graphics& gr) {
    drawTopBar(gr, L"messages  \x00B7  private notes between members");
    RectF L = socialLeft();
    L.Width = min(L.Width, S(380));
    paperSheet(gr, L);
    sheetHeader(gr, L, L"CONVERSATIONS");
    float rh = S(62), y = L.Y + S(52);
    if (g_chats.empty())
        drawText(gr, g_socialLoaded ? L"No conversations yet. Open someone's card in Friends and send a message."
                                    : L"Fetching\x2026",
                 RectF(L.X + S(24), y + S(10), L.Width - S(48), S(80)), F_HAND, S(14), C_INK, 0, 0, 0, true);
    int maxRows = (int)((L.Y + L.Height - y - S(8)) / rh);
    for (int i = 0; i < (int)g_chats.size() && i < maxRows; i++) {
        const fb::Chat& c = g_chats[i];
        fb::Person o = otherOf(c);
        RectF r(L.X + S(8), y + i * rh, L.Width - S(16), rh);
        if (o.uid == g_chatWith.uid) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, RectF(r.X + S(4), r.Y + S(5), r.Width - S(8), r.Height - S(10)));
        }
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, r.X, r.Y + rh - 1, r.X + r.Width, r.Y + rh - 1);
        drawAvatar(gr, o.avatar, RectF(r.X + S(10), r.Y + S(10), rh - S(20), rh - S(20)));
        bool unread = c.lastFrom != g_myUid && c.lastAt > g_chatSeen[c.id];
        drawText(gr, widen(o.name), RectF(r.X + S(64), r.Y + S(6), r.Width - S(120), rh * 0.48f), F_SERIF, S(15), C_PRINT,
                 FontStyleBold, 0, 2);
        drawText(gr, clockText(c.lastAt), RectF(r.X, r.Y + S(6), r.Width - S(12), rh * 0.48f), F_MONO, S(11), C_FAINT, 0, 2, 2);
        drawText(gr, (c.lastFrom == g_myUid ? L"you: " : L"") + widen(c.last), RectF(r.X + S(64), r.Y + rh * 0.52f, r.Width - S(96), rh * 0.4f),
                 F_SERIF, S(12), unread ? C_PRINT : C_FAINT, unread ? FontStyleBold : FontStyleItalic, 0, 0);
        if (unread) {
            SolidBrush dot(C_RED);
            gr.FillEllipse(&dot, r.X + r.Width - S(22), r.Y + rh * 0.58f, S(10), S(10));
        }
        g_buttons.push_back({r, B_CHATROW + i});
    }

    float x = L.X + L.Width + S(24);
    RectF R(x, contentY(), g_W - x - S(24), L.Height);
    paperSheet(gr, R, true);
    if (g_chatWith.uid.empty()) {
        drawText(gr, L"Pick a conversation on the left, or message someone from their card in Friends.",
                 RectF(R.X + S(24), R.Y + S(24), R.Width - S(48), S(80)), F_HAND, S(16), C_INK, 0, 0, 0, true);
        statusLine(gr, RectF(R.X + S(24), R.Y + S(110), R.Width - S(48), S(60)));
        return;
    }
    drawAvatar(gr, g_chatWith.avatar, RectF(R.X + S(18), R.Y + S(10), S(36), S(36)));
    drawText(gr, widen(g_chatWith.name), RectF(R.X + S(64), R.Y + S(8), R.Width - S(84), S(40)), F_SERIF, S(20), C_PRINT, FontStyleBold);
    Pen rl(C_PRINT, 1.f);
    gr.DrawLine(&rl, R.X + S(12), R.Y + S(54), R.X + R.Width - S(12), R.Y + S(54));

    // bubbles, newest at the bottom
    RectF area(R.X + S(18), R.Y + S(62), R.Width - S(36), R.Height - S(62) - S(96));
    GraphicsState st = gr.Save();
    gr.SetClip(area, CombineModeIntersect);
    Font* f = getFont(F_SERIF, S(14), FontStyleRegular);
    StringFormat sf;
    float maxW = area.Width * 0.72f, yb = area.Y + area.Height + g_chatScroll;
    for (int i = (int)g_msgs.size() - 1; i >= 0; i--) {
        const fb::Message& m = g_msgs[i];
        std::wstring text = widen(m.text);
        RectF box;
        gr.MeasureString(text.c_str(), -1, f, RectF(0, 0, maxW, 4000), &sf, &box);
        float bw = box.Width + S(22), bh = box.Height + S(14);
        yb -= bh + S(22);
        if (yb + bh + S(22) < area.Y) break;
        if (yb > area.Y + area.Height) continue;
        bool mine = m.from == g_myUid;
        RectF b(mine ? area.X + area.Width - bw - S(2) : area.X + S(1), yb, bw, bh);
        SolidBrush fill(mine ? Color(48, C_RED.GetR(), C_RED.GetG(), C_RED.GetB()) : C_CARD);
        Pen edge(mine ? C_RED : C_RULE, g_edgeWidth);
        gr.FillRectangle(&fill, b);
        gr.DrawRectangle(&edge, b);
        SolidBrush ink(C_PRINT);
        gr.DrawString(text.c_str(), -1, f, RectF(b.X + S(11), b.Y + S(7), maxW + S(4), box.Height + S(4)), &sf, &ink);
        drawText(gr, clockText(m.at), RectF(b.X, b.Y + bh + S(1), bw, S(16)), F_MONO, S(10), C_FAINT, 0, mine ? 2 : 0, 0);
    }
    gr.Restore(st);
    if (g_msgs.empty())
        drawText(gr, L"No messages yet. Say hello.", area, F_HAND, S(15), C_FAINT, 0, 1, 1);

    RectF in = chatInputRect();
    SolidBrush box(C_CARD);
    gr.FillRectangle(&box, in);
    Pen line(C_PRINT, 1.f);
    gr.DrawRectangle(&line, in);
    inkButton(gr, B_SEND, RectF(in.X + in.Width + S(10), in.Y, S(98), in.Height), L"SEND");
    statusLine(gr, RectF(R.X + S(18), in.Y - S(30), R.Width - S(36), S(26)));
}

// ---------------------------------------------------------------- input

static bool onSocialButton(int id) {
    if (id >= B_AVATAR && id < B_AVATAR + AVATAR_COUNT) {
        if (id - B_AVATAR >= 24 && !needPlan(1, L"These pictures")) return true;
        g_avatarPick = id - B_AVATAR;
        g_photoDraft.clear();   // a preset replaces an uploaded picture
        invalidate();
        return true;
    }
    if (id == B_UPLOAD) { uploadPhoto(); return true; }
    if (id == B_PRESETPIC) { g_photoDraft.clear(); invalidate(); return true; }
    if (id == B_BANNER_DN || id == B_BANNER_UP) {
        if (needPlan(1, L"Profile banners")) g_bannerDraft = (g_bannerDraft + (id == B_BANNER_UP ? 1 : 7)) % 8;
        invalidate();
        return true;
    }
    if (id >= B_TAB && id < B_TAB + 4) {
        g_peopleTab = id - B_TAB;
        g_selPerson = fb::Person();
        g_socialMsg.clear();
        showLoginEdits(false);   // the lookup box only exists on the Find tab
        invalidate();
        return true;
    }
    if (id >= B_PERSON && id < B_PERSON + 200) {
        auto rows = peopleRows();
        if (id - B_PERSON < (int)rows.size()) {
            fb::Person p = rows[id - B_PERSON];
            if (g_peopleTab == 2) p.at = 0;
            viewPerson(p);
        }
        return true;
    }
    if (id >= B_CHATROW && id < B_CHATROW + 200) {
        if (id - B_CHATROW < (int)g_chats.size()) openChatWith(otherOf(g_chats[id - B_CHATROW]));
        return true;
    }
    fb::Person p = g_selPerson;
    if (g_viewedOk && g_viewed.uid == p.uid) { p.name = g_viewed.name; p.avatar = g_viewed.avatar; }
    switch (id) {
        case B_SAVEPROFILE: saveProfile(); return true;
        case B_SEARCH: doSearch(); return true;
        case B_SEND: sendChat(); return true;
        case B_ACT_MSG: openChatWith(p); return true;
        case B_ACT_ADD: sendFriendRequest(p); return true;
        case B_ACT_REMOVE: removeFriend(p.uid); return true;
        case B_ACT_ACCEPT: acceptRequest(p); return true;
        case B_ACT_DECLINE: declineRequest(p.uid); return true;
        case B_ACT_BLOCK: blockPerson(p); return true;
        case B_ACT_UNBLOCK: unblockPerson(p.uid); return true;
    }
    return false;
}
