// The settings screen: one row per appearance option, with a live preview card. Included by main.cpp.

enum { SR_MODE, SR_ACCENT, SR_TABLE, SR_FONTS, SR_TEXT, SR_BOARD, SR_TEXTURE, SR_PIECES, SR_VOLUME, SR_GLIDE, SR_COORDS,
       SR_LASTMOVE, SR_DOTS, SR_PIECESET, SR_SHAREBRAIN, SR_COUNT };
static const wchar_t* SR_LABEL[SR_COUNT] = {L"THEME", L"ACCENT COLOUR", L"TABLE", L"FONTS", L"TEXT SIZE", L"BOARD COLOURS",
                                            L"BOARD TEXTURE", L"PIECE SIZE", L"SOUND VOLUME", L"PIECE GLIDING",
                                            L"COORDINATES", L"LAST-MOVE HIGHLIGHT", L"LEGAL-MOVE DOTS", L"PIECE SET", L"SHARE THE CLUB BRAIN"};

static int g_shareBrainSetting = 1;   // mirrors g_brainShare, so the settings rows stay uniform

static int* setPtr(int row) {
    Theme& t = g_theme;
    g_shareBrainSetting = g_brainShare ? 1 : 0;
    int* p[SR_COUNT] = {&t.mode, &t.accent, &t.table, &t.fonts, &t.textSize, &t.board, &t.texture, &t.pieces, &t.volume,
                        &t.glide, &t.coords, &t.lastMove, &t.dots, &t.pieceSet, &g_shareBrainSetting};
    return p[row];
}
static int setCount(int row) {
    static const int n[SR_COUNT] = {3, COUNT_OF(ACCENTS), COUNT_OF(TABLES), COUNT_OF(FONTSETS), 4, COUNT_OF(BOARDS), 4, 3, 11,
                                    3, 2, 2, 2, 1, 2};
    if (row == SR_PIECESET) return max(1, (int)g_pieceSets.size());
    return n[row];
}
static std::wstring setName(int row) {
    int v = *setPtr(row);
    switch (row) {
        case SR_MODE: return MODES[v];
        case SR_ACCENT: return ACCENTS[v].name;
        case SR_TABLE: return TABLES[v].name;
        case SR_FONTS: return FONTSETS[v].name;
        case SR_TEXT: return TEXTSIZES[v];
        case SR_BOARD: return BOARDS[v].name;
        case SR_TEXTURE:
            if (v == 3 && !(g_boardImg || g_lightImg || g_darkImg)) return L"your image (none in assets)";
            return TEXTURES[v];
        case SR_PIECES: return PIECESIZES[v];
        case SR_VOLUME: return v == 0 ? std::wstring(L"muted") : std::to_wstring(v * 10) + L"%";
        case SR_GLIDE: return GLIDES[v];
        case SR_PIECESET: {
            if (g_pieceSets.empty()) return L"built-in";
            std::wstring n = g_pieceSets[v];
            for (auto& c : n)
                if (c == L'-' || c == L'_') c = L' ';
            return n;
        }
        default: return v ? L"on" : L"off";
    }
}

static void settingsChanged(int row) {
    if (row == SR_SHAREBRAIN) {
        g_brainShare = g_shareBrainSetting != 0;
        saveProgress();
        if (g_brainShare) cloudBrainPull(false);
    }
    applyTheme();
    if (row == SR_PIECESET || row < 0) loadPieceSet();   // swap the art
    saveSettings();
    if (row == SR_VOLUME) audioPlay(SND_MOVE);   // let them hear the new level
    invalidate();
}

static bool onSettingsButton(int id) {
    if (id >= B_SET_PREV && id < B_SET_PREV + SR_COUNT) {
        int row = id - B_SET_PREV, n = setCount(row);
        *setPtr(row) = (*setPtr(row) + n - 1) % n;
        settingsChanged(row);
        return true;
    }
    if (id >= B_SET_NEXT && id < B_SET_NEXT + SR_COUNT) {
        int row = id - B_SET_NEXT, n = setCount(row);
        *setPtr(row) = (*setPtr(row) + 1) % n;
        settingsChanged(row);
        return true;
    }
    if (id >= B_SWATCH_ACCENT && id < B_SWATCH_ACCENT + COUNT_OF(ACCENTS)) {
        g_theme.accent = id - B_SWATCH_ACCENT;
        settingsChanged(SR_ACCENT);
        return true;
    }
    if (id >= B_SWATCH_BOARD && id < B_SWATCH_BOARD + COUNT_OF(BOARDS)) {
        g_theme.board = id - B_SWATCH_BOARD;
        settingsChanged(SR_BOARD);
        return true;
    }
    if (id == B_PIECEDIR) {
        if (!needPlan(2, L"Using your own piece images")) { invalidate(); return true; }
        BROWSEINFOW bi = {};
        bi.hwndOwner = g_hwnd;
        bi.lpszTitle = L"Choose the folder with your 12 piece images (white_king.png, black_pawn.png ...)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (LPITEMIDLIST pid = SHBrowseForFolderW(&bi)) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pid, path)) {
                g_pieceDir = path;
                savePieceDir();
                loadPieceSet();
                int found = 0;
                for (auto* b : g_pieceSrc) found += b != nullptr;
                socialSay(L"Piece images loaded from that folder (" + std::to_wstring(found) + L" of 12 found; the rest use the built-in art).", false);
            }
            CoTaskMemFree(pid);
        }
        g_bgKey = -1;
        invalidate();
        return true;
    }
    if (id == B_PIECERESET) {
        g_pieceDir.clear();
        savePieceDir();
        loadPieceSet();
        socialSay(L"Back to the built-in pieces.", false);
        return true;
    }
    if (id == B_SET_RESET) {
        g_theme = Theme();
        if (g_boardImg || g_lightImg || g_darkImg) g_theme.texture = 3;
        for (int i = 0; i < (int)g_pieceSets.size(); i++)
            if (g_pieceSets[i] == L"classic") g_theme.pieceSet = i;
        settingsChanged(-1);
        return true;
    }
    return false;
}

static void renderSettings(Graphics& gr) {
    drawTopBar(gr, L"settings  \x00B7  make the club your own");
    RectF sheet(contentX(), contentY(), min(S(580), contentW() * 0.52f), (REAL)g_H - contentY() - S(24));
    paperSheet(gr, sheet);
    drawText(gr, L"APPEARANCE & SOUND", RectF(sheet.X + S(18), sheet.Y + S(8), sheet.Width, S(30)), F_SERIF, S(15), C_PRINT,
             FontStyleBold);
    Pen dbl(C_PRINT, 1.f);
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(40), sheet.X + sheet.Width - S(12), sheet.Y + S(40));
    gr.DrawLine(&dbl, sheet.X + S(12), sheet.Y + S(43), sheet.X + sheet.Width - S(12), sheet.Y + S(43));

    float avail = sheet.Height - S(56) - S(92);
    float rh = min(S(42), avail / SR_COUNT), y = sheet.Y + S(52);
    float lx = sheet.X + S(22), cx = sheet.X + sheet.Width * 0.42f, bs = min(S(30), rh - S(6));
    float vw = sheet.X + sheet.Width - S(22) - bs - (cx + bs);
    for (int r = 0; r < SR_COUNT; r++) {
        float ry = y + r * rh;
        Pen rule(C_LEDGER, 1.f);
        gr.DrawLine(&rule, sheet.X + S(10), ry + rh - 1, sheet.X + sheet.Width - S(10), ry + rh - 1);
        drawText(gr, SR_LABEL[r], RectF(lx, ry, cx - lx, rh), F_MONO, S(12), C_FAINT, 0, 0, 1);
        inkButton(gr, B_SET_PREV + r, RectF(cx, ry + (rh - bs) / 2, bs, bs), L"\x2039");
        drawText(gr, setName(r), RectF(cx + bs, ry, vw, rh), F_SERIF, S(15), C_INK, FontStyleRegular, 1, 1);
        inkButton(gr, B_SET_NEXT + r, RectF(cx + bs + vw, ry + (rh - bs) / 2, bs, bs), L"\x203A");
    }
    float fy = sheet.Y + sheet.Height - S(50);
    inkButton(gr, B_SET_RESET, RectF(lx, fy, S(190), S(36)), L"RESET TO DEFAULTS");
    inkButton(gr, B_PIECEDIR, RectF(lx + S(200), fy, S(190), S(36)), L"MY PIECE IMAGES\x2026");
    if (!g_pieceDir.empty()) inkButton(gr, B_PIECERESET, RectF(lx + S(400), fy, S(150), S(36)), L"BUILT-IN");
    else if (g_plan < 2) {
        drawBadge(gr, 2, RectF(lx + S(398), fy + S(7), S(22), S(22)));
        drawText(gr, L"Gold", RectF(lx + S(426), fy, S(80), S(36)), F_HAND, S(13), C_FAINT);
    }
    statusLine(gr, RectF(lx, fy - S(34), sheet.Width - S(44), S(30)));

    // preview card
    float x0 = sheet.X + sheet.Width + S(24), w = g_W - x0 - S(24);
    RectF card(x0, contentY(), w, (REAL)g_H - contentY() - S(24));
    paperSheet(gr, card, true);
    float pad = S(22), ix = x0 + pad, iw = w - 2 * pad, yy = card.Y + S(14);
    drawText(gr, L"PREVIEW", RectF(ix, yy, iw, S(28)), F_MONO, S(15), C_PRINT, FontStyleBold);
    yy += S(40);

    // a corner of the board
    float sq = min(S(64), min(iw / 4.6f, (card.Height - S(360)) / 4));
    if (sq > S(24)) {
        float fr = sq * 0.3f, bx = ix + fr, by = yy + fr;
        RectF frame(ix, yy, sq * 4 + 2 * fr, sq * 4 + 2 * fr);
        fillTexture(gr, g_walnut, frame);
        static const int demo[16] = {mkpiece(BLACK, ROOK), 0, mkpiece(BLACK, KING), 0, mkpiece(BLACK, PAWN), mkpiece(BLACK, PAWN), 0,
                                     mkpiece(BLACK, KNIGHT), 0, 0, mkpiece(WHITE, BISHOP), 0, mkpiece(WHITE, QUEEN), 0,
                                     mkpiece(WHITE, KING), mkpiece(WHITE, PAWN)};
        for (int i = 0; i < 16; i++) {
            int cx2 = i % 4, cy2 = i / 4;
            RectF r(bx + cx2 * sq, by + cy2 * sq, sq, sq);
            bool light = (cx2 + cy2) % 2 == 0;
            Bitmap* tex = g_useBoardImage ? (light ? g_lightImg : g_darkImg) : nullptr;
            if (tex) gr.DrawImage(tex, r);
            else fillTexture(gr, light ? g_maple : g_darkwood, r, (float)(i * 37 % 256), (float)(i * 91 % 256));
            if (g_theme.lastMove && (i == 10 || i == 13)) {
                SolidBrush hl(Color(95, 236, 170, 40));
                gr.FillRectangle(&hl, r);
            }
            if (demo[i]) gr.DrawImage(pieceBitmap(demo[i], (int)sq, g_pieceFill, true), r);
            if (g_theme.dots && (i == 5 + 4 || i == 3)) {
                SolidBrush dot(Color(105, 40, 22, 12));
                gr.FillEllipse(&dot, r.X + sq * 0.39f, r.Y + sq * 0.39f, sq * 0.22f, sq * 0.22f);
            }
        }
        // text samples beside the board
        float tx = ix + frame.Width + S(22), tw = iw - frame.Width - S(22);
        if (tw > S(120)) {
            drawText(gr, L"Printed headings", RectF(tx, yy, tw, S(30)), F_SERIF, S(18), C_PRINT, FontStyleBold);
            drawText(gr, L"Handwritten moves: Nf3, O-O", RectF(tx, yy + S(34), tw, S(30)), F_HAND, S(15), C_INK);
            drawText(gr, L"TYPED LABELS 1200", RectF(tx, yy + S(68), tw, S(26)), F_MONO, S(13), C_FAINT);
            drawText(gr, L"accent", RectF(tx, yy + S(100), tw, S(26)), F_HAND, S(15), C_RED);
            drawTick(gr, tx + S(70), yy + S(104), S(16), C_RED);
            inkButton(gr, B_NONE, RectF(tx, yy + S(136), min(tw, S(150)), S(34)), L"A BUTTON");
        }
        yy += frame.Height + S(22);
    }

    drawText(gr, L"ACCENT", RectF(ix, yy, iw, S(22)), F_MONO, S(12), C_FAINT);
    yy += S(26);
    float sw = min(S(34), (iw - S(8) * 7) / 8);
    for (int i = 0; i < COUNT_OF(ACCENTS); i++) {
        RectF r(ix + i * (sw + S(8)), yy, sw, sw);
        SolidBrush b(Color(255, ACCENTS[i].r, ACCENTS[i].g, ACCENTS[i].b));
        gr.FillEllipse(&b, r);
        Pen rim(C_PRINT, 1.f);
        gr.DrawEllipse(&rim, r);
        if (i == g_theme.accent) inkCircle(gr, r, C_PRINT);
        g_buttons.push_back({r, B_SWATCH_ACCENT + i});
    }
    yy += sw + S(18);
    drawText(gr, L"BOARD", RectF(ix, yy, iw, S(22)), F_MONO, S(12), C_FAINT);
    yy += S(26);
    for (int i = 0; i < COUNT_OF(BOARDS); i++) {
        RectF r(ix + i * (sw + S(8)), yy, sw, sw);
        SolidBrush l(Color(255, BOARDS[i].l[0], BOARDS[i].l[1], BOARDS[i].l[2])), d(Color(255, BOARDS[i].d[0], BOARDS[i].d[1], BOARDS[i].d[2]));
        gr.FillRectangle(&l, RectF(r.X, r.Y, sw / 2, sw / 2));
        gr.FillRectangle(&d, RectF(r.X + sw / 2, r.Y, sw / 2, sw / 2));
        gr.FillRectangle(&d, RectF(r.X, r.Y + sw / 2, sw / 2, sw / 2));
        gr.FillRectangle(&l, RectF(r.X + sw / 2, r.Y + sw / 2, sw / 2, sw / 2));
        Pen rim(C_PRINT, 1.f);
        gr.DrawRectangle(&rim, r);
        if (i == g_theme.board) inkCircle(gr, r, C_RED);
        g_buttons.push_back({r, B_SWATCH_BOARD + i});
    }
    yy += sw + S(18);
    if (yy + S(40) < card.Y + card.Height)
        drawText(gr, L"Changes apply straight away and are saved for next time. Board art placed in assets/ shows "
                     L"when the texture is set to \x201Cyour image\x201D.",
                 RectF(ix, yy, iw, card.Y + card.Height - yy - S(10)), F_SERIF, S(12), C_FAINT, FontStyleItalic, 0, 0, true);
}
