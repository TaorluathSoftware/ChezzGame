// Membership plans (Bronze / Silver / Gold / Ruby), their badges and the plans page, plus the
// hosted-image helpers (uploaded profile pictures) and profile banners. Included by main.cpp before
// social.inl.
//
// A member's plan is the document plans/{uid} in Firestore. Members can read it but only the project
// owner can write it (Firebase console, or a payment webhook), and the server rules check it before
// allowing tournaments, teams, engines or puzzle publishing.

#include "config.h"

static const Color PLAN_COLOR[4] = {Color(255, 176, 112, 62), Color(255, 168, 176, 188), Color(255, 214, 168, 44),
                                    Color(255, 196, 30, 66)};
static const wchar_t* PLAN_PRICE[4] = {L"free", L"4.99 / month", L"9.99 / month", L"17.99 / month"};
static const wchar_t* PLAN_FEATURES[4][8] = {
    {L"All 150 ladder engines", L"1 daily puzzle", L"24 preset profile pictures", L"Play a random opponent or a friend online",
     L"Play engines built by Ruby members", nullptr},
    {L"Everything in Bronze", L"5 daily puzzles", L"Publish 1 puzzle a day", L"More pictures, your own photo, banners",
     L"Silver badge on your card", L"Play in tournaments; create, join and run teams", L"Choose any waiting opponent", nullptr},
    {L"Everything in Silver", L"Unlimited daily puzzles", L"Publish 12 puzzles a day", L"Your own images for the pieces",
     L"Gold badge on your card", L"Early access to new features", nullptr},
    {L"Everything in Gold", L"Publish unlimited puzzles", L"Host tournaments", L"Build engines for the Engines tab",
     L"Ruby badge on your card", L"Chat with the developers", nullptr}};

static std::string g_devUid, g_devName, g_upgradeUrl;   // from the config/app document
// Payment pages, one per paid plan, plus the page where a member manages or cancels. They live in
// the config/app document so prices and links can change without a new build of the app.
static std::string g_buyUrl[4], g_portalUrl;
static int g_planConfirm = -1;     // a plan the member has picked but not yet confirmed
static int g_planPending = -1;     // the payment page is open: waiting for the plan to switch on
static int g_planTicks = 0, g_planChecks = 0;
static bool g_planChecking = false;
static std::map<std::string, Bitmap*> g_remote;         // hosted images by URL (nullptr while fetching)
static std::string g_photoDraft;                        // picture chosen on the profile screen, saved with the card
static int g_bannerDraft = 0;

static void socialSay(const std::wstring& msg, bool err = false);
static void drawAvatar(Graphics& gr, int idx, RectF r);

// A cut gem in the plan's colour.
static void drawBadge(Graphics& gr, int plan, RectF r) {
    if (plan <= 0) return;
    float x = r.X, y = r.Y, s = r.Width;
    PointF pts[5] = {PointF(x + s * 0.25f, y + s * 0.1f), PointF(x + s * 0.75f, y + s * 0.1f), PointF(x + s, y + s * 0.4f),
                     PointF(x + s * 0.5f, y + s * 0.96f), PointF(x, y + s * 0.4f)};
    SolidBrush fill(PLAN_COLOR[plan]);
    gr.FillPolygon(&fill, pts, 5);
    Pen edge(Color(200, 20, 20, 24), max(1.f, s / 16));
    gr.DrawPolygon(&edge, pts, 5);
    Pen facet(Color(120, 255, 255, 255), max(1.f, s / 20));
    gr.DrawLine(&facet, x, y + s * 0.4f, x + s, y + s * 0.4f);
    gr.DrawLine(&facet, x + s * 0.36f, y + s * 0.4f, x + s * 0.5f, y + s * 0.96f);
    gr.DrawLine(&facet, x + s * 0.64f, y + s * 0.4f, x + s * 0.5f, y + s * 0.96f);
}

// True if your plan covers it; otherwise explains what is needed.
static bool needPlan(int rank, const std::wstring& what) {
    if (g_plan >= rank) return true;
    socialSay(what + L" is for " + PLAN_NAME[rank] + L" members and up. The gem icon shows the plans.", true);
    return false;
}

static void loadPlanAndConfig() {
    if (g_guest || g_offline || g_myUid.empty()) return;
    netAsync([]() -> std::function<void()> {
        int plan = fb::getPlan(g_sess, g_sess.uid);
        bool founder = fb::isFounder(g_sess, g_sess.uid);
        fb::Doc cfg;
        fb::docGet(g_sess, "config/app", cfg);
        return [plan, cfg, founder]() {
            g_plan = plan;
            g_founder = founder;
            if (cfg.exists) {
                g_devUid = cfg.str("devUid");
                g_devName = cfg.str("devName");
                g_upgradeUrl = cfg.str("upgradeUrl");
                g_buyUrl[1] = cfg.str("buySilver");
                g_buyUrl[2] = cfg.str("buyGold");
                g_buyUrl[3] = cfg.str("buyRuby");
                g_portalUrl = cfg.str("portalUrl");
            }
            invalidate();
        };
    });
}

// ---------------------------------------------------------------- choosing a plan

static bool billingReady() { return !g_guest && !g_offline && !g_myUid.empty() && !g_buyUrl[1].empty(); }
static bool safeLink(const std::string& u) { return u.compare(0, 8, "https://") == 0 && u.size() < 400; }

static void openLink(const std::string& url) {
    if (safeLink(url)) ShellExecuteW(NULL, L"open", widen(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// Asks the server what plan this member is on now: while waiting for a payment, and on demand.
static void checkPlanNow() {
    if (g_planChecking || g_guest || g_offline) return;
    g_planChecking = true;
    g_planChecks++;
    netAsync([]() -> std::function<void()> {
        int plan = fb::getPlan(g_sess, g_sess.uid);
        return [plan]() {
            g_planChecking = false;
            bool changed = plan != g_plan;
            g_plan = plan;
            if (changed && g_planPending >= 0) {
                socialSay(std::wstring(L"Thank you. You are a ") + PLAN_NAME[plan] + L" member now.", false);
                g_planPending = -1;
                audioPlay(SND_NOTIFY);
            }
            invalidate();
        };
    });
}

// While a payment page is open, watch for the plan to switch on: every four seconds for ten minutes.
static void billingTick() {
    if (g_planPending < 0) return;
    g_planTicks++;
    if (g_planTicks % 4 == 0) checkPlanNow();
    if (g_planTicks > 600) {   // they have probably closed the page
        g_planPending = -1;
        socialSay(L"Still nothing from the payment page. If you did pay, press Check again in a minute.", true);
    }
    if (g_screen == SCR_PLANS) invalidate();
}

// Opens the payment page with the membership id attached, so the webhook knows whose plan to set.
static void startCheckout(int plan) {
    g_planConfirm = -1;
    if (plan < 1 || plan > 3 || !safeLink(g_buyUrl[plan])) {
        socialSay(L"That plan has no payment page set up yet.", true);
        return;
    }
    std::string url = g_buyUrl[plan];
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "client_reference_id=" + g_myUid;
    openLink(url);
    g_planPending = plan;
    g_planTicks = 0;
    g_planChecks = 0;
    socialSay(std::wstring(L"The payment page for ") + PLAN_NAME[plan] + L" is open in your browser.", false);
}

static bool onPlansButton(int id) {
    if (id >= B_PLAN_PICK && id < B_PLAN_PICK + 4) {
        int plan = id - B_PLAN_PICK;
        if (g_guest) { socialSay(L"Join the club first: plans belong to a membership.", true); return true; }
        if (g_offline) { socialSay(L"Changing plan needs a connection.", true); return true; }
        if (plan == 0) {   // going back to the free plan means cancelling, which happens at the provider
            if (safeLink(g_portalUrl)) { openLink(g_portalUrl); socialSay(L"Cancel the subscription on that page; Bronze returns when it ends.", false); }
            else socialSay(L"To go back to Bronze, cancel your subscription on the club's payment page.", true);
            return true;
        }
        g_planConfirm = plan;
        g_socialMsg.clear();
        invalidate();
        return true;
    }
    switch (id) {
        case B_PLAN_GO: startCheckout(g_planConfirm); return true;
        case B_PLAN_CANCEL: g_planConfirm = -1; g_socialMsg.clear(); invalidate(); return true;
        case B_PLAN_CHECK: socialSay(L"Asking the server\x2026", false); checkPlanNow(); return true;
        case B_PLAN_STOP: g_planPending = -1; g_socialMsg.clear(); invalidate(); return true;
        case B_PORTAL:
            if (safeLink(g_portalUrl)) openLink(g_portalUrl);
            else socialSay(L"No billing page is set up yet.", true);
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- hosted images

static bool hostedUrl(const std::string& url) { return url.compare(0, 27, "https://res.cloudinary.com/") == 0 && url.size() < 300; }

// The picture behind a hosted URL, fetched once and kept in data/cache. Only the image host is ever
// contacted: a profile can't make the app download from anywhere else.
static Bitmap* remoteImage(const std::string& url) {
    if (!hostedUrl(url)) return nullptr;
    auto it = g_remote.find(url);
    if (it != g_remote.end()) return it->second;
    g_remote[url] = nullptr;
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : url) h = (h ^ c) * 1099511628211ULL;
    wchar_t name[40];
    swprintf(name, 40, L"\\cache\\%016llx.img", h);
    CreateDirectoryW((g_data + L"\\cache").c_str(), NULL);
    std::wstring file = g_data + name;
    if (fileExists(file)) {
        g_remote[url] = loadImage(file);
        return g_remote[url];
    }
    std::string path = url.substr(26);
    netAsync([url, path, file]() -> std::function<void()> {
        std::string out;
        int st = fb::httpsRequest(L"GET", L"res.cloudinary.com", path, "", nullptr, out);
        bool ok = st == 200 && out.size() > 64 && out.size() < 4u * 1024 * 1024;
        if (ok) {
            FILE* f = _wfopen(file.c_str(), L"wb");
            ok = f && fwrite(out.data(), 1, out.size(), f) == out.size();
            if (f) fclose(f);
        }
        return [url, file, ok]() {
            if (ok) {
                g_remote[url] = loadImage(file);
                invalidate();
            }
        };
    });
    return nullptr;
}

// A member's face: their uploaded picture if there is one and it has arrived, else their preset.
static void drawFace(Graphics& gr, const std::string& photo, int avatar, RectF r) {
    Bitmap* b = photo.empty() ? nullptr : remoteImage(photo);
    if (!b) { drawAvatar(gr, avatar, r); return; }
    GraphicsPath circle;
    circle.AddEllipse(r);
    GraphicsState st = gr.Save();
    gr.SetClip(&circle, CombineModeIntersect);
    gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    float w = (float)b->GetWidth(), h = (float)b->GetHeight(), s = min(w, h);
    gr.DrawImage(b, r, (w - s) / 2, (h - s) / 2, s, s, UnitPixel);
    gr.Restore(st);
    Pen rim(C_PRINT, max(1.f, r.Width / 48));
    gr.DrawEllipse(&rim, r);
}

// Lets a Silver+ member pick an image file and sends it to the image host. The returned URL is
// kept as a draft until they press SAVE CARD.
static void uploadPhoto() {
    if (!needPlan(1, L"Uploading your own picture")) return;
    if (!CHEZZ_CLOUDINARY_CLOUD[0] || !CHEZZ_CLOUDINARY_PRESET[0]) {
        socialSay(L"Image hosting isn't set up yet. README.md, \x201CImage hosting\x201D, has the three steps.", true);
        return;
    }
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string bytes;
    if (FILE* f = _wfopen(file, L"rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() < 3u * 1024 * 1024 + 1) bytes.append(buf, n);
        fclose(f);
    }
    if (bytes.empty() || bytes.size() > 3u * 1024 * 1024) { socialSay(L"Pick a picture under 3 MB.", true); return; }
    socialSay(L"Sending your picture\x2026", false);
    netAsync([bytes]() -> std::function<void()> {
        const std::string bd = "----chezzUpload7d4a9b";
        std::string body = "--" + bd + "\r\nContent-Disposition: form-data; name=\"upload_preset\"\r\n\r\n" + CHEZZ_CLOUDINARY_PRESET +
                           "\r\n--" + bd + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"picture\"\r\n"
                           "Content-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + bd + "--\r\n";
        std::string out, ctype = "multipart/form-data; boundary=" + bd;
        int st = fb::httpsRequest(L"POST", L"api.cloudinary.com", std::string("/v1_1/") + CHEZZ_CLOUDINARY_CLOUD + "/image/upload",
                                  body, ctype.c_str(), out);
        std::string url = st == 200 ? Json::parse(out)["secure_url"].s : "";
        // ask the host for a 256 px square centred on the face, as PNG
        size_t k = url.find("/upload/");
        if (k != std::string::npos) url.insert(k + 8, "c_fill,g_auto,w_256,h_256,f_png/");
        return [url, st]() {
            if (hostedUrl(url)) {
                g_photoDraft = url;
                socialSay(L"Picture uploaded. Press SAVE CARD to keep it.", false);
            } else {
                socialSay(st == 0 ? L"Couldn't reach the image host." : L"The image host refused the picture (check the upload preset).", true);
            }
        };
    });
}

// ---------------------------------------------------------------- banners

static const wchar_t* BANNER_NAME[8] = {L"none", L"walnut", L"midnight", L"emerald", L"royal", L"sunset", L"checks", L"ruby"};

static void drawBanner(Graphics& gr, int idx, RectF r) {
    if (idx <= 0 || idx > 7) return;
    static const BYTE C1[8][3] = {{0, 0, 0}, {92, 58, 36}, {22, 28, 56}, {18, 92, 66}, {44, 58, 150}, {208, 96, 40}, {60, 60, 66}, {150, 20, 48}};
    static const BYTE C2[8][3] = {{0, 0, 0}, {168, 120, 76}, {70, 84, 150}, {70, 170, 120}, {130, 80, 190}, {240, 190, 80}, {200, 200, 206}, {236, 90, 110}};
    LinearGradientBrush lg(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y + r.Height), Color(255, C1[idx][0], C1[idx][1], C1[idx][2]),
                           Color(255, C2[idx][0], C2[idx][1], C2[idx][2]));
    gr.FillRectangle(&lg, r);
    // a row of faint chequers along the band
    SolidBrush sq(Color(idx == 6 ? 110 : 36, 255, 255, 255));
    float c = r.Height / 2;
    for (int i = 0; i * c < r.Width; i++) gr.FillRectangle(&sq, RectF(r.X + i * c, r.Y + (i % 2) * c, min(c, r.X + r.Width - (r.X + i * c)), c));
}

// ---------------------------------------------------------------- the plans page

static void renderPlans(Graphics& gr) {
    drawTopBar(gr, L"membership plans  \x00B7  what each one unlocks");
    float left = contentX(), top = contentY(), gap = S(14), w = (contentW() - 3 * gap) / 4, h = (REAL)g_H - top - S(96);
    for (int p = 0; p < 4; p++) {
        RectF c(left + p * (w + gap), top, w, h);
        paperSheet(gr, c, true);
        SolidBrush band(PLAN_COLOR[p]);
        gr.FillRectangle(&band, RectF(c.X + 1, c.Y + 1, c.Width - 2, S(8)));
        if (p > 0) drawBadge(gr, p, RectF(c.X + c.Width - S(46), c.Y + S(22), S(28), S(28)));
        drawText(gr, PLAN_NAME[p], RectF(c.X + S(18), c.Y + S(16), c.Width - S(70), S(40)), F_SERIF, S(26), C_PRINT, FontStyleBold);
        drawText(gr, PLAN_PRICE[p], RectF(c.X + S(18), c.Y + S(56), c.Width - S(36), S(26)), F_MONO, S(14), C_FAINT);
        Pen rule(C_PRINT, 1.f);
        gr.DrawLine(&rule, c.X + S(14), c.Y + S(88), c.X + c.Width - S(14), c.Y + S(88));
        float y = c.Y + S(100);
        for (int i = 0; PLAN_FEATURES[p][i]; i++) {
            SolidBrush dot(PLAN_COLOR[p]);
            gr.FillEllipse(&dot, c.X + S(18), y + S(9), S(7), S(7));
            RectF tr(c.X + S(34), y, c.Width - S(48), S(64));
            RectF box;
            StringFormat sf;
            std::wstring t = PLAN_FEATURES[p][i];
            gr.MeasureString(t.c_str(), -1, getFont(F_SERIF, S(14), FontStyleRegular), tr, &sf, &box);
            drawText(gr, t, tr, F_SERIF, S(14), C_PRINT, FontStyleRegular, 0, 0, true);
            y += box.Height + S(12);
        }
        RectF tag(c.X + S(18), c.Y + c.Height - S(50), c.Width - S(36), S(34));
        if (p == g_plan) {
            SolidBrush hl(C_HILITE);
            gr.FillRectangle(&hl, tag);
            drawText(gr, g_founder && p == 3 ? L"YOURS FOR LIFE" : L"YOUR PLAN", tag, F_SERIF, S(13), C_PRINT, FontStyleBold, 1, 1);
        } else if (g_founder) {
            // nothing to buy: founding members keep Ruby whatever happens
        } else if (g_guest) {
            if (p == 0) drawText(gr, L"join the club, free", tag, F_SERIF, S(12), C_FAINT, FontStyleItalic, 1, 1);
        } else if (p == 0) {
            inkButton(gr, B_PLAN_PICK + 0, tag, L"GO BACK TO BRONZE", safeLink(g_portalUrl) || g_plan > 0);
        } else if (billingReady()) {
            std::wstring label = std::wstring(p > g_plan ? L"CHOOSE " : L"SWITCH TO ") + PLAN_NAME[p];
            for (auto& ch : label) ch = (wchar_t)towupper(ch);
            inkButton(gr, B_PLAN_PICK + p, tag, label, g_planPending < 0);
        } else {
            drawText(gr, g_offline ? L"offline" : L"not for sale yet", tag, F_SERIF, S(12), C_FAINT, FontStyleItalic, 1, 1);
        }
    }
    // the strip underneath: confirming a choice, waiting for a payment, or what to do instead
    RectF foot(left, top + h + S(12), contentW(), S(60));
    paperSheet(gr, foot);
    float bx = foot.X + foot.Width - S(16);
    std::wstring note;
    bool warn = false;
    if (g_planConfirm >= 1) {
        bx -= S(150);
        inkButton(gr, B_PLAN_CANCEL, RectF(bx, foot.Y + S(12), S(140), S(38)), L"NOT NOW");
        bx -= S(240);
        inkButton(gr, B_PLAN_GO, RectF(bx, foot.Y + S(12), S(230), S(38)), L"OPEN THE PAYMENT PAGE");
        note = std::wstring(PLAN_NAME[g_planConfirm]) + L", " + PLAN_PRICE[g_planConfirm] +
               L". Your browser opens the club's payment page, with your membership id attached so the plan switches on by itself.";
    } else if (g_planPending >= 1) {
        bx -= S(150);
        inkButton(gr, B_PLAN_STOP, RectF(bx, foot.Y + S(12), S(140), S(38)), L"STOP WAITING");
        bx -= S(160);
        inkButton(gr, B_PLAN_CHECK, RectF(bx, foot.Y + S(12), S(150), S(38)), L"CHECK NOW", !g_planChecking);
        note = std::wstring(L"Waiting for the payment to be confirmed") +
               (g_planChecks > 1 ? L" (checked " + std::to_wstring(g_planChecks) + L" times)" : L"") +
               L". Finish in your browser; this page switches over by itself.";
    } else {
    if (g_plan >= 3 && !g_devUid.empty()) {
        bx -= S(250);
        inkButton(gr, B_DEVCHAT, RectF(bx, foot.Y + S(12), S(240), S(38)), L"MESSAGE THE DEVELOPERS");
    }
    if (g_plan >= 1 && !g_founder && safeLink(g_portalUrl)) {
        bx -= S(210);
        inkButton(gr, B_PORTAL, RectF(bx, foot.Y + S(12), S(200), S(38)), L"MANAGE OR CANCEL");
    }
        note = g_founder ? L"You are a founding member: one of the first five accounts, with Ruby for life."
             : g_guest ? L"Guests play online rooms only. Join the club (free) to get the Bronze plan."
             : g_offline ? L"You are offline. Changing plan needs a connection."
             : billingReady() ? L"Pick a plan above. Payment is handled by the club's payment provider; the app never sees your card."
             : L"Paid plans are not for sale yet: ask the club's developers to switch one on for your account.";
    }
    if (!g_socialMsg.empty()) { note = g_socialMsg; warn = g_socialMsgErr; }
    drawText(gr, note, RectF(foot.X + S(18), foot.Y, bx - foot.X - S(30), foot.Height), F_SERIF, S(13), warn ? C_RED : C_INK,
             FontStyleRegular, 0, 1, true);
}
