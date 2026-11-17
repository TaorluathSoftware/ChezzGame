// Appearance: colours, fonts, board and table styles. Included by main.cpp right after the asset
// globals. applyTheme() turns g_theme into the palette and textures every draw call uses.

static std::wstring widen(const std::string& s);
static std::string narrow(const std::wstring& w);

static Color C_PRINT, C_FAINT, C_INK, C_RED, C_PAPER, C_CARD, C_RULE, C_CREAM, C_LCD(255, 244, 196, 112),
    C_HILITE, C_CARDTINT, C_EDGE, C_LEDGER;
static COLORREF g_fieldBg = RGB(251, 248, 240), g_fieldText = RGB(29, 45, 92);
static float g_textScale = 1.f, g_pieceFill = 0.8f, g_glideMul = 1.f, g_edgeWidth = 1.f;
static int g_iconSide = BLACK;          // colour of the little piece icons drawn on paper
static bool g_useBoardImage = false;    // "your image" board texture selected and available

struct NamedColor { const wchar_t* name; BYTE r, g, b; };
static const NamedColor ACCENTS[] = {{L"red pencil", 172, 44, 38},  {L"royal blue", 38, 84, 172}, {L"forest", 34, 120, 74},
                                     {L"violet", 120, 60, 160},     {L"amber", 196, 120, 20},     {L"teal", 20, 130, 140},
                                     {L"rose", 200, 60, 110},       {L"graphite", 70, 70, 78}};
static const NamedColor TABLES[] = {{L"green felt", 30, 72, 52}, {L"navy", 24, 44, 84}, {L"burgundy", 84, 28, 36},
                                    {L"charcoal", 40, 42, 46},   {L"teal", 16, 74, 78}};
struct BoardColors { const wchar_t* name; BYTE l[3], d[3]; };
static const BoardColors BOARDS[] = {{L"maple & walnut", {228, 204, 160}, {150, 102, 66}},
                                     {L"tournament green", {236, 236, 208}, {112, 148, 84}},
                                     {L"ocean blue", {222, 232, 240}, {92, 132, 170}},
                                     {L"slate", {214, 214, 214}, {120, 124, 132}},
                                     {L"rosewood", {238, 208, 190}, {150, 70, 60}},
                                     {L"lavender", {230, 222, 240}, {130, 104, 168}},
                                     {L"ice", {240, 246, 250}, {140, 170, 190}},
                                     {L"midnight", {160, 166, 180}, {58, 66, 88}}};
struct FontSet { const wchar_t* name; const wchar_t* fam[3]; };   // print, handwriting, typewriter
static const FontSet FONTSETS[] = {{L"classic", {L"Georgia", L"Segoe Print", L"Consolas"}},
                                   {L"modern", {L"Segoe UI", L"Segoe UI", L"Consolas"}},
                                   {L"typewriter", {L"Courier New", L"Courier New", L"Courier New"}},
                                   {L"elegant", {L"Palatino Linotype", L"Gabriola", L"Consolas"}},
                                   {L"readable", {L"Verdana", L"Verdana", L"Consolas"}},
                                   {L"friendly", {L"Comic Sans MS", L"Comic Sans MS", L"Consolas"}}};
static const wchar_t* MODES[] = {L"light", L"dark", L"high contrast"};
static const wchar_t* TEXTURES[] = {L"wood grain", L"flat", L"marble", L"your image"};
static const wchar_t* TEXTSIZES[] = {L"small", L"normal", L"large", L"extra large"};
static const wchar_t* PIECESIZES[] = {L"small", L"normal", L"large"};
static const wchar_t* GLIDES[] = {L"off", L"normal", L"slow"};
#define COUNT_OF(a) ((int)(sizeof(a) / sizeof(a[0])))

struct Theme {
    int mode = 0, accent = 0, table = 0, fonts = 0, textSize = 1, board = 0, texture = 0, pieces = 1;
    int volume = 10, glide = 1, coords = 1, lastMove = 1, dots = 1;
    int pieceSet = 0;   // index into g_pieceSets (the folders inside assets/pieces)
};
static Theme g_theme;
static std::vector<std::wstring> g_pieceSets;   // e.g. "classic", "designer-art"

// Procedural 256x256 tile. style: 0 felt, 1 paper, 2 dark wood (frame), 3 wood (squares), 4 flat, 5 marble.
static Bitmap* makeTile(int style, float r, float g, float bl, int seed) {
    const int W = 256, H = 256;
    Bitmap* b = new Bitmap(W, H, PixelFormat32bppARGB);
    Rect rc(0, 0, W, H);
    BitmapData bd;
    b->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd);
    std::mt19937 rng(seed * 977 + 11);
    std::uniform_real_distribution<float> U(-1.f, 1.f);
    const float TAU = 6.2831853f;
    float base[3] = {r, g, bl};
    for (int y = 0; y < H; y++) {
        UINT32* row = (UINT32*)((BYTE*)bd.Scan0 + y * bd.Stride);
        for (int x = 0; x < W; x++) {
            float k;
            if (style == 0) k = 1.f + U(rng) * 0.07f + 0.025f * sinf(x * TAU / W * 3 + sinf(y * TAU / H * 2) * 2);
            else if (style == 1) k = 1.f + U(rng) * 0.018f + 0.01f * sinf(y * TAU / H * 5 + x * TAU / W);
            else if (style == 4) k = 1.f + U(rng) * 0.012f;
            else if (style == 5) {
                float v = sinf(x * TAU / W * 3 + 4.f * sinf(y * TAU / H * 2) + 2.f * sinf((x + y) * TAU / W * 5));
                k = 1.f - 0.09f * powf(fabsf(v), 0.35f) + 0.06f + U(rng) * 0.015f;
            } else {
                float warp = 5.f * sinf(x * TAU / W * 2) + 2.f * sinf(x * TAU / W * 5 + y * TAU / H * 3);
                float ring = 0.5f + 0.5f * sinf((y + warp) * TAU / H * (style == 2 ? 30 : 22));
                float amt = style == 2 ? 0.28f : 0.12f;
                k = 1.f - amt * 0.5f + amt * ring * ring * ring + U(rng) * 0.03f;
            }
            int c[3];
            for (int i = 0; i < 3; i++) c[i] = max(0, min(255, (int)(base[i] * k)));
            row[x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    }
    b->UnlockBits(&bd);
    return b;
}

static void clampTheme() {
    Theme& t = g_theme;
    auto fix = [](int& v, int n) { if (v < 0 || v >= n) v = 0; };
    fix(t.mode, 3);
    fix(t.accent, COUNT_OF(ACCENTS));
    fix(t.table, COUNT_OF(TABLES));
    fix(t.fonts, COUNT_OF(FONTSETS));
    fix(t.textSize, 4);
    fix(t.board, COUNT_OF(BOARDS));
    fix(t.texture, 4);
    fix(t.pieces, 3);
    fix(t.glide, 3);
    if (t.pieceSet < 0 || t.pieceSet >= (int)g_pieceSets.size()) t.pieceSet = 0;
    t.volume = max(0, min(10, t.volume));
    t.coords = t.coords != 0;
    t.lastMove = t.lastMove != 0;
    t.dots = t.dots != 0;
}

static void applyTheme() {
    clampTheme();
    const Theme& t = g_theme;
    bool dark = t.mode == 1, hc = t.mode == 2;
    const NamedColor& ac = ACCENTS[t.accent];
    auto lift = [](BYTE c, float k) { return (BYTE)(c + (255 - c) * k); };   // brighten towards white
    float paper[3];
    if (hc) {
        C_PRINT = Color(255, 255, 255, 255); C_FAINT = Color(255, 214, 214, 214); C_INK = Color(255, 255, 236, 60);
        C_PAPER = Color(255, 0, 0, 0); C_CARD = Color(255, 0, 0, 0); C_RULE = Color(255, 255, 255, 255);
        C_CREAM = Color(255, 255, 255, 255); C_HILITE = Color(170, 0, 96, 220); C_CARDTINT = Color(0, 0, 0, 0);
        C_EDGE = Color(255, 255, 255, 255); C_LEDGER = Color(130, 255, 255, 255);
        C_RED = Color(255, lift(ac.r, 0.5f), lift(ac.g, 0.5f), lift(ac.b, 0.5f));
        paper[0] = paper[1] = paper[2] = 0;
        g_fieldBg = RGB(0, 0, 0); g_fieldText = RGB(255, 255, 255);
        g_iconSide = WHITE; g_edgeWidth = 2.f;
    } else if (dark) {
        C_PRINT = Color(255, 232, 230, 224); C_FAINT = Color(255, 150, 152, 160); C_INK = Color(255, 156, 184, 255);
        C_PAPER = Color(255, 40, 42, 48); C_CARD = Color(255, 52, 55, 62); C_RULE = Color(255, 86, 88, 98);
        C_CREAM = Color(255, 226, 222, 212); C_HILITE = Color(90, 250, 226, 92); C_CARDTINT = Color(16, 255, 255, 255);
        C_EDGE = Color(255, 74, 76, 86); C_LEDGER = Color(60, 140, 160, 220);
        C_RED = Color(255, lift(ac.r, 0.35f), lift(ac.g, 0.35f), lift(ac.b, 0.35f));
        paper[0] = 40; paper[1] = 42; paper[2] = 48;
        g_fieldBg = RGB(52, 55, 62); g_fieldText = RGB(232, 230, 224);
        g_iconSide = WHITE; g_edgeWidth = 1.f;
    } else {
        C_PRINT = Color(255, 43, 38, 32); C_FAINT = Color(255, 128, 118, 104); C_INK = Color(255, 29, 45, 92);
        C_PAPER = Color(255, 243, 235, 216); C_CARD = Color(255, 251, 248, 240); C_RULE = Color(255, 201, 191, 166);
        C_CREAM = Color(255, 240, 228, 200); C_HILITE = Color(150, 250, 226, 92); C_CARDTINT = Color(130, 255, 255, 255);
        C_EDGE = Color(255, 214, 203, 178); C_LEDGER = Color(70, 70, 110, 170);
        C_RED = Color(255, ac.r, ac.g, ac.b);
        paper[0] = 243; paper[1] = 235; paper[2] = 216;
        g_fieldBg = RGB(251, 248, 240); g_fieldText = RGB(29, 45, 92);
        g_iconSide = BLACK; g_edgeWidth = 1.f;
    }
    const NamedColor& tb = TABLES[t.table];
    float tk = hc ? 0.f : dark ? 0.5f : 1.f;
    const BoardColors& bc = BOARDS[t.board];
    int sqStyle = t.texture == 1 ? 4 : t.texture == 2 ? 5 : 3;
    float frame[3] = {78, 49, 32};   // walnut for the wooden board, a deep shade of the dark squares otherwise
    if (t.board != 0)
        for (int i = 0; i < 3; i++) frame[i] = bc.d[i] * 0.45f;
    for (Bitmap** p : {&g_felt, &g_paper, &g_walnut, &g_maple, &g_darkwood}) { delete *p; *p = nullptr; }
    g_felt = makeTile(0, tb.r * tk, tb.g * tk, tb.b * tk, 0);
    g_paper = makeTile(1, paper[0], paper[1], paper[2], 1);
    g_walnut = makeTile(2, frame[0], frame[1], frame[2], 2);
    g_maple = makeTile(sqStyle, bc.l[0], bc.l[1], bc.l[2], 3);
    g_darkwood = makeTile(sqStyle, bc.d[0], bc.d[1], bc.d[2], 4);
    g_useBoardImage = t.texture == 3 && (g_boardImg || g_lightImg || g_darkImg);

    for (auto& kv : g_fonts) delete kv.second;
    g_fonts.clear();
    static const float TS[4] = {0.9f, 1.f, 1.15f, 1.3f}, PF[3] = {0.7f, 0.8f, 0.9f}, GM[3] = {0.f, 1.f, 1.8f};
    g_textScale = TS[t.textSize];
    g_pieceFill = PF[t.pieces];
    g_glideMul = GM[t.glide];
    audioSetVolume(t.volume / 10.f);
    if (g_fieldBrush) DeleteObject(g_fieldBrush);
    g_fieldBrush = CreateSolidBrush(g_fieldBg);
    g_bgKey = -1;
    g_sceneDirty = true;
}

static void saveSettings() {
    FILE* f = _wfopen((g_data + L"\\settings.ini").c_str(), L"w");
    if (!f) return;
    const Theme& t = g_theme;
    fprintf(f, "mode %d\naccent %d\ntable %d\nfonts %d\ntextsize %d\nboard %d\ntexture %d\npieces %d\nvolume %d\nglide %d\n"
               "coords %d\nlastmove %d\ndots %d\n",
            t.mode, t.accent, t.table, t.fonts, t.textSize, t.board, t.texture, t.pieces, t.volume, t.glide, t.coords,
            t.lastMove, t.dots);
    fclose(f);
    if (t.pieceSet >= 0 && t.pieceSet < (int)g_pieceSets.size())
        if (FILE* p = _wfopen((g_data + L"\\pieceset.txt").c_str(), L"wb")) {
            std::string n = narrow(g_pieceSets[t.pieceSet]);
            fwrite(n.data(), 1, n.size(), p);
            fclose(p);
        }
}

static void loadSettings() {
    g_theme = Theme();
    if (g_boardImg || g_lightImg || g_darkImg) g_theme.texture = 3;   // board art in assets/ is used until you pick another
    {   // piece set: the saved name, else "classic", else the first folder
        std::wstring want = L"classic";
        if (FILE* p = _wfopen((g_data + L"\\pieceset.txt").c_str(), L"rb")) {
            char buf[260];
            size_t n = fread(buf, 1, sizeof buf - 1, p);
            fclose(p);
            if (n) want = widen(std::string(buf, n));
        }
        for (int i = 0; i < (int)g_pieceSets.size(); i++)
            if (g_pieceSets[i] == want) g_theme.pieceSet = i;
    }
    FILE* f = _wfopen((g_data + L"\\settings.ini").c_str(), L"r");
    if (!f) return;
    char key[32];
    int v;
    Theme& t = g_theme;
    while (fscanf(f, "%31s %d", key, &v) == 2) {
        struct { const char* k; int* p; } map[] = {{"mode", &t.mode}, {"accent", &t.accent}, {"table", &t.table},
            {"fonts", &t.fonts}, {"textsize", &t.textSize}, {"board", &t.board}, {"texture", &t.texture},
            {"pieces", &t.pieces}, {"volume", &t.volume}, {"glide", &t.glide}, {"coords", &t.coords},
            {"lastmove", &t.lastMove}, {"dots", &t.dots}};
        for (auto& m : map)
            if (!strcmp(key, m.k)) *m.p = v;
    }
    fclose(f);
}
