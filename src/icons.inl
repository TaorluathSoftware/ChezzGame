// Line icons for the navigation row, and the top bar itself. Included by main.cpp before the
// screen renderers. Hovering an icon shows its name underneath.

enum IconKind { IC_LADDER, IC_ENGINE, IC_PUZZLE, IC_ONLINE, IC_TROPHY, IC_TEAM, IC_FRIENDS, IC_CHAT, IC_CARD, IC_GEM,
                IC_GEAR, IC_MOON, IC_SUN, IC_EXIT, IC_JOIN, IC_UNDO, IC_FLIP, IC_FLAG, IC_NEXT, IC_AGAIN };

static void drawIcon(Graphics& gr, int kind, RectF box, const Color& c) {
    float m = box.Width * 0.22f, x = box.X + m, y = box.Y + m, s = box.Width - 2 * m;
    Pen p(c, max(1.4f, s * 0.085f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    SolidBrush b(c);
    auto L = [&](float x1, float y1, float x2, float y2) { gr.DrawLine(&p, x + x1 * s, y + y1 * s, x + x2 * s, y + y2 * s); };
    auto E = [&](float ex, float ey, float ew, float eh) { gr.DrawEllipse(&p, x + ex * s, y + ey * s, ew * s, eh * s); };
    auto R = [&](float rx, float ry, float rw, float rh) { gr.DrawRectangle(&p, x + rx * s, y + ry * s, rw * s, rh * s); };
    auto A = [&](float ax, float ay, float aw, float ah, float st, float sw) { gr.DrawArc(&p, x + ax * s, y + ay * s, aw * s, ah * s, st, sw); };
    auto P = [&](std::initializer_list<PointF> pts, bool close) {
        std::vector<PointF> v;
        for (auto& q : pts) v.push_back(PointF(x + q.X * s, y + q.Y * s));
        if (close) gr.DrawPolygon(&p, v.data(), (INT)v.size());
        else gr.DrawLines(&p, v.data(), (INT)v.size());
    };
    switch (kind) {
        case IC_LADDER:
            L(0.25f, 0, 0.25f, 1); L(0.75f, 0, 0.75f, 1);
            L(0.25f, 0.2f, 0.75f, 0.2f); L(0.25f, 0.5f, 0.75f, 0.5f); L(0.25f, 0.8f, 0.75f, 0.8f);
            break;
        case IC_ENGINE:   // a chip
            R(0.22f, 0.22f, 0.56f, 0.56f); R(0.4f, 0.4f, 0.2f, 0.2f);
            for (float t : {0.36f, 0.64f}) { L(t, 0, t, 0.22f); L(t, 0.78f, t, 1); L(0, t, 0.22f, t); L(0.78f, t, 1, t); }
            break;
        case IC_PUZZLE:   // jigsaw piece
            P({{0.05f, 0.3f}, {0.35f, 0.3f}}, false); A(0.33f, 0.08f, 0.26f, 0.26f, 160, 220); P({{0.57f, 0.3f}, {0.85f, 0.3f}, {0.85f, 0.52f}}, false);
            A(0.76f, 0.5f, 0.24f, 0.24f, 250, 220); P({{0.85f, 0.72f}, {0.85f, 0.98f}, {0.05f, 0.98f}, {0.05f, 0.3f}}, false);
            break;
        case IC_ONLINE:   // globe
            E(0, 0, 1, 1); E(0.3f, 0, 0.4f, 1); L(0, 0.5f, 1, 0.5f);
            break;
        case IC_TROPHY:
            P({{0.25f, 0.05f}, {0.75f, 0.05f}}, false); A(0.25f, -0.4f, 0.5f, 0.95f, 0, 180);
            A(0.02f, 0.08f, 0.3f, 0.34f, 90, 180); A(0.68f, 0.08f, 0.3f, 0.34f, 270, 180);
            L(0.5f, 0.56f, 0.5f, 0.82f); L(0.28f, 0.95f, 0.72f, 0.95f); L(0.36f, 0.82f, 0.64f, 0.82f);
            break;
        case IC_TEAM:   // shield
            P({{0.5f, 0}, {0.92f, 0.16f}, {0.92f, 0.52f}, {0.5f, 1}, {0.08f, 0.52f}, {0.08f, 0.16f}}, true);
            L(0.5f, 0.2f, 0.5f, 0.74f); L(0.26f, 0.42f, 0.74f, 0.42f);
            break;
        case IC_FRIENDS:   // two people
            E(0.12f, 0.08f, 0.32f, 0.32f); A(0, 0.5f, 0.56f, 0.8f, 180, 180);
            E(0.58f, 0.2f, 0.26f, 0.26f); A(0.5f, 0.56f, 0.46f, 0.7f, 200, 160);
            break;
        case IC_CHAT:   // speech bubble
            P({{0.3f, 0.72f}, {0.08f, 0.72f}, {0.08f, 0.1f}, {0.92f, 0.1f}, {0.92f, 0.72f}, {0.52f, 0.72f}, {0.28f, 0.96f}, {0.3f, 0.72f}}, false);
            L(0.26f, 0.32f, 0.74f, 0.32f); L(0.26f, 0.5f, 0.6f, 0.5f);
            break;
        case IC_CARD:   // member card
            R(0, 0.14f, 1, 0.72f); E(0.14f, 0.3f, 0.24f, 0.24f); L(0.52f, 0.36f, 0.86f, 0.36f); L(0.52f, 0.56f, 0.78f, 0.56f);
            L(0.14f, 0.7f, 0.38f, 0.7f);
            break;
        case IC_GEM:
            P({{0.25f, 0.08f}, {0.75f, 0.08f}, {1, 0.38f}, {0.5f, 0.98f}, {0, 0.38f}}, true);
            L(0, 0.38f, 1, 0.38f); P({{0.25f, 0.08f}, {0.36f, 0.38f}, {0.5f, 0.98f}}, false); P({{0.75f, 0.08f}, {0.64f, 0.38f}, {0.5f, 0.98f}}, false);
            break;
        case IC_GEAR:
            E(0.2f, 0.2f, 0.6f, 0.6f); E(0.4f, 0.4f, 0.2f, 0.2f);
            for (int i = 0; i < 8; i++) {
                float a = i * 0.785398f, cx = 0.5f + cosf(a) * 0.32f, cy = 0.5f + sinf(a) * 0.32f;
                L(cx, cy, 0.5f + cosf(a) * 0.5f, 0.5f + sinf(a) * 0.5f);
            }
            break;
        case IC_MOON: {
            GraphicsPath gp;
            gp.AddArc(x + 0.08f * s, y + 0.05f * s, 0.9f * s, 0.9f * s, 60, 240);
            gp.AddArc(x + 0.36f * s, y - 0.02f * s, 0.74f * s, 0.74f * s, 260, -160);
            gp.CloseFigure();
            gr.FillPath(&b, &gp);
            break;
        }
        case IC_SUN:
            E(0.3f, 0.3f, 0.4f, 0.4f);
            for (int i = 0; i < 8; i++) {
                float a = i * 0.785398f;
                L(0.5f + cosf(a) * 0.36f, 0.5f + sinf(a) * 0.36f, 0.5f + cosf(a) * 0.5f, 0.5f + sinf(a) * 0.5f);
            }
            break;
        case IC_EXIT:   // door with an arrow leaving
            P({{0.56f, 0.2f}, {0.56f, 0.02f}, {0.04f, 0.02f}, {0.04f, 0.98f}, {0.56f, 0.98f}, {0.56f, 0.8f}}, false);
            L(0.34f, 0.5f, 1, 0.5f); P({{0.8f, 0.3f}, {1, 0.5f}, {0.8f, 0.7f}}, false);
            break;
        case IC_UNDO:   // arrow curling back
            A(0.12f, 0.24f, 0.78f, 0.7f, 190, 230);
            P({{0.02f, 0.3f}, {0.14f, 0.62f}, {0.44f, 0.5f}}, false);
            break;
        case IC_FLIP:   // one arrow up, one down
            L(0.3f, 0.12f, 0.3f, 0.92f); P({{0.1f, 0.32f}, {0.3f, 0.1f}, {0.5f, 0.32f}}, false);
            L(0.72f, 0.08f, 0.72f, 0.88f); P({{0.52f, 0.68f}, {0.72f, 0.9f}, {0.92f, 0.68f}}, false);
            break;
        case IC_FLAG:
            L(0.2f, 0.04f, 0.2f, 1);
            P({{0.2f, 0.08f}, {0.92f, 0.26f}, {0.2f, 0.52f}}, true);
            break;
        case IC_NEXT:
            L(0.08f, 0.5f, 0.9f, 0.5f); P({{0.6f, 0.2f}, {0.92f, 0.5f}, {0.6f, 0.8f}}, false);
            break;
        case IC_AGAIN:   // going round again
            A(0.1f, 0.1f, 0.8f, 0.8f, 300, 300);
            P({{0.62f, 0.02f}, {0.9f, 0.2f}, {0.62f, 0.42f}}, false);
            break;
        case IC_JOIN:   // person with a plus
            E(0.14f, 0.06f, 0.36f, 0.36f); A(0, 0.52f, 0.64f, 0.84f, 180, 180);
            L(0.82f, 0.3f, 0.82f, 0.66f); L(0.64f, 0.48f, 1, 0.48f);
            break;
    }
}

static int pendingRequests();
static void drawFace(Graphics& gr, const std::string& photo, int avatar, RectF r);

// The left sidebar (navigation, who is signed in) and the page heading above the content.
// `subtitle` is "page name  .  one line about it".
static void drawTopBar(Graphics& gr, const wchar_t* subtitle) {
    float sw = sidebarW();
    {
        SolidBrush shade(Color(g_theme.mode == 2 ? 255 : 104, 0, 0, 0));
        gr.FillRectangle(&shade, RectF(0, 0, sw, (REAL)g_H));
        Pen edge(Color(70, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB()), 1.f);
        gr.DrawLine(&edge, sw, 0.f, sw, (REAL)g_H);
    }
    drawText(gr, L"Chezz", RectF(S(20), S(8), sw - S(24), S(50)), F_SERIF, S(34), C_CREAM, FontStyleBoldItalic, 0, 1);

    struct Nav { int id, icon; const wchar_t* label; bool active, enabled; int badge; };
    std::vector<Nav> nav, foot;
    bool dark = g_theme.mode != 0, on = !g_offline;
    if (g_guest) {
        nav = {{B_ONLINE, IC_ONLINE, L"Online room", g_screen == SCR_LOBBY, true, 0},
               {B_NAV_SETTINGS, IC_GEAR, L"Settings", g_screen == SCR_SETTINGS, true, 0},
               {B_JOINCLUB, IC_JOIN, L"Join the club", false, true, 0}};
    } else {
        nav = {{B_NAV_LADDER, IC_LADDER, L"The ladder", g_screen == SCR_MENU, true, 0},
               {B_NAV_ENGINES, IC_ENGINE, L"Engines", g_screen == SCR_ENGINES, on, 0},
               {B_NAV_PUZZLES, IC_PUZZLE, L"Puzzles", g_screen == SCR_PUZZLES, true, 0},
               {B_ONLINE, IC_ONLINE, L"Online room", g_screen == SCR_LOBBY, true, 0},
               {B_NAV_TOURS, IC_TROPHY, L"Tournaments", g_screen == SCR_TOURNEYS, on, 0},
               {B_NAV_TEAMS, IC_TEAM, L"Teams", g_screen == SCR_TEAMS, on, 0},
               {B_NAV_PEOPLE, IC_FRIENDS, L"Friends", g_screen == SCR_PEOPLE, on, pendingRequests()},
               {B_NAV_CHAT, IC_CHAT, L"Messages", g_screen == SCR_CHAT, on, g_unread},
               {B_NAV_PROFILE, IC_CARD, L"My card", g_screen == SCR_PROFILE, true, 0},
               {B_NAV_PLANS, IC_GEM, L"Plans", g_screen == SCR_PLANS, true, 0},
               {B_NAV_SETTINGS, IC_GEAR, L"Settings", g_screen == SCR_SETTINGS, true, 0}};
    }
    foot = {{B_NAV_THEME, dark ? IC_SUN : IC_MOON, dark ? L"Light theme" : L"Dark theme", false, true, 0},
            {B_SIGNOUT, IC_EXIT, g_guest ? L"Leave" : L"Sign out", false, true, 0}};

    float memberH = S(64), footTop = g_H - S(12) - memberH - foot.size() * S(34) - S(10);
    float rh = min(S(38), (footTop - S(70)) / max(1, (int)nav.size()));
    auto row = [&](const Nav& v, float y, float h) {
        RectF r(S(10), y, sw - S(20), h - S(4));
        bool hover = v.enabled && r.Contains(g_mouseX, g_mouseY);
        if (v.active) {
            SolidBrush fill(C_CREAM);
            gr.FillRectangle(&fill, r);
        } else if (hover) {
            SolidBrush fill(Color(46, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB()));
            gr.FillRectangle(&fill, r);
        }
        Color c = v.active ? Color(255, 24, 40, 34) : v.enabled ? C_CREAM : Color(110, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB());
        float is = min(S(28), r.Height);
        drawIcon(gr, v.icon, RectF(r.X + S(6), r.Y + (r.Height - is) / 2, is, is), c);
        drawText(gr, v.label, RectF(r.X + S(42), r.Y, r.Width - S(72), r.Height), F_SERIF, S(14), c, v.active ? FontStyleBold : FontStyleRegular);
        if (v.badge > 0) {
            RectF d(r.X + r.Width - S(28), r.Y + (r.Height - S(20)) / 2, S(22), S(20));
            SolidBrush dot(C_RED);
            gr.FillEllipse(&dot, d);
            drawText(gr, std::to_wstring(min(v.badge, 99)), d, F_MONO, S(11), Color(255, 255, 255, 255), FontStyleBold, 1, 1);
        }
        if (v.enabled && !v.active) g_buttons.push_back({r, v.id});
    };
    float y = S(70);
    for (auto& v : nav) { row(v, y, rh); y += rh; }
    y = footTop;
    for (auto& v : foot) { row(v, y, S(34)); y += S(34); }

    // who is signed in
    RectF mb(S(10), g_H - S(12) - memberH, sw - S(20), memberH);
    Pen line(Color(70, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB()), 1.f);
    gr.DrawLine(&line, mb.X, mb.Y, mb.X + mb.Width, mb.Y);
    float av = S(40);
    drawFace(gr, g_guest ? std::string() : g_profile.photo, g_profile.avatar, RectF(mb.X + S(4), mb.Y + S(12), av, av));
    drawText(gr, widen(g_myName), RectF(mb.X + S(52), mb.Y + S(10), mb.Width - S(78), S(24)), F_SERIF, S(15), C_CREAM, FontStyleBold);
    std::wstring sub = g_guest ? std::wstring(L"visitor") : std::to_wstring(g_profile.rating) + L" \x00B7 " + PLAN_NAME[g_plan];
    if (g_offline) sub += L" \x00B7 offline";
    drawText(gr, sub, RectF(mb.X + S(52), mb.Y + S(32), mb.Width - S(56), S(20)), F_MONO, S(11), Color(210, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB()));
    if (!g_guest) drawBadge(gr, g_plan, RectF(mb.X + mb.Width - S(24), mb.Y + S(12), S(20), S(20)));

    // page heading
    std::wstring full = subtitle, title = full, tag;
    size_t cut = full.find(L"  \x00B7  ");
    if (cut != std::wstring::npos) { title = full.substr(0, cut); tag = full.substr(cut + 5); }
    if (!title.empty()) title[0] = (wchar_t)towupper(title[0]);
    float hx = contentX(), hw = contentW();
    drawText(gr, title, RectF(hx, S(10), hw * 0.5f, S(44)), F_SERIF, S(26), C_CREAM, FontStyleBold, 0, 1);
    drawText(gr, tag, RectF(hx + hw * 0.4f, S(18), hw * 0.6f, S(34)), F_SERIF, S(14), Color(200, C_CREAM.GetR(), C_CREAM.GetG(), C_CREAM.GetB()),
             FontStyleItalic, 2, 1);
}
