// The in-game HUD: a bar for each player above and below the board (picture, name, rating, what
// they have captured, their clock) and one panel beside it (moves, status, actions). Included by
// main.cpp in place of the old clock + tray + scoresheet stack.

// A square action button: icon on top, small caption underneath.
static void iconButton(Graphics& gr, int id, RectF r, int icon, const wchar_t* caption, bool enabled = true) {
    bool hover = enabled && r.Contains(g_mouseX, g_mouseY);
    if (hover) {
        SolidBrush fill(C_INK);
        gr.FillRectangle(&fill, r);
    }
    Pen p(enabled ? C_INK : C_RULE, S(1.2f));
    gr.DrawRectangle(&p, r);
    Color c = hover ? C_PAPER : enabled ? C_INK : C_FAINT;
    float is = min(r.Height * 0.62f, r.Width * 0.5f);
    drawIcon(gr, icon, RectF(r.X + (r.Width - is) / 2, r.Y + S(2), is, is), c);
    drawText(gr, caption, RectF(r.X, r.Y + r.Height - S(20), r.Width, S(18)), F_SERIF, S(10), c, FontStyleBold, 1, 1);
    if (enabled) g_buttons.push_back({r, id});
}

static void renderPlayerBar(Graphics& gr, RectF r, int color) {
    bool me = color == g.human, turn = !g.over && g.pos.side == color;
    paperSheet(gr, r, true);
    if (turn) {   // whose move it is: an accent edge
        SolidBrush edge(C_RED);
        gr.FillRectangle(&edge, RectF(r.X, r.Y, S(5), r.Height));
    }
    float av = r.Height - S(14);
    RectF face(r.X + S(16), r.Y + S(7), av, av);
    if (me) drawFace(gr, g_profile.photo, g_profile.avatar, face);
    else if (g.online) {
        SolidBrush bg(Color(255, 222, 214, 196));
        gr.FillEllipse(&bg, face);
        gr.DrawImage(pieceBitmap(mkpiece(color, KING), (int)av, 0.72f, false), face);
        Pen rim(C_PRINT, 1.f);
        gr.DrawEllipse(&rim, face);
    } else drawPortrait(gr, oppPortrait(), face, false);

    float tx = face.X + av + S(12), clockW = S(128), tw = r.X + r.Width - clockW - S(20) - tx;
    float nameW = min(tw, max(S(120), tw * 0.5f));
    std::wstring name = me ? (g_myName.empty() ? std::wstring(L"You") : widen(g_myName)) : oppName();
    drawText(gr, name, RectF(tx, r.Y + S(5), nameW, r.Height * 0.5f), F_SERIF, S(17), C_PRINT, FontStyleBold, 0, 2);
    std::wstring sub;
    Color subC = C_FAINT;
    if (!me && g_thinking) { sub = L"thinking\x2026"; subC = C_RED; }
    else if (!me && g.online) sub = L"rated " + std::to_wstring(oppRating());
    else if (!me) sub = std::to_wstring(oppRating()) + L"  \x00B7  " + widen(curLevel().title);
    else if (g.online) sub = L"rated " + std::to_wstring(g_profile.rating);
    else sub = std::wstring(L"playing ") + (color == WHITE ? L"white" : L"black");
    drawText(gr, sub, RectF(tx, r.Y + r.Height * 0.52f, nameW, r.Height * 0.4f), F_MONO, S(12), subC, FontStyleRegular, 0, 0);

    // what this player has taken, and the material lead
    static const int START[7] = {0, 8, 2, 2, 2, 1, 1}, VAL[7] = {0, 1, 3, 3, 5, 9, 0}, ORDER[5] = {QUEEN, ROOK, BISHOP, KNIGHT, PAWN};
    int cnt[2][7] = {}, mat[2] = {0, 0};
    for (int s = 0; s < 64; s++)
        if (int p = g.pos.sq[s]) {
            cnt[pcolor(p)][ptype(p)]++;
            mat[pcolor(p)] += VAL[ptype(p)];
        }
    int enemy = color ^ 1;
    float icon = min(S(22), r.Height * 0.4f), cx = tx + nameW + S(8), cmax = r.X + r.Width - clockW - S(60);
    GraphicsState st = gr.Save();
    gr.SetClip(RectF(cx, r.Y, max(0.f, cmax - cx), r.Height), CombineModeIntersect);
    float x = cx;
    for (int t : ORDER) {
        int lost = max(0, START[t] - cnt[enemy][t]);
        for (int i = 0; i < lost; i++) {
            gr.DrawImage(pieceBitmap(mkpiece(enemy, t), (int)icon, 0.95f, false), RectF(x, r.Y + (r.Height - icon) / 2, icon, icon));
            x += icon * 0.58f;
        }
        if (lost) x += icon * 0.4f;
    }
    gr.Restore(st);
    int diff = mat[color] - mat[enemy];
    if (diff > 0)
        drawText(gr, L"+" + std::to_wstring(diff), RectF(cmax, r.Y, S(44), r.Height), F_MONO, S(15), C_RED, FontStyleBold, 1, 1);

    // the clock: lit while it runs
    RectF ck(r.X + r.Width - clockW - S(10), r.Y + S(8), clockW, r.Height - S(16));
    long long t = clockFor(color);
    SolidBrush face2(turn ? Color(255, 26, 24, 20) : C_CARD);
    gr.FillRectangle(&face2, ck);
    Pen bez(turn ? C_RED : C_RULE, S(1.5f));
    gr.DrawRectangle(&bez, ck);
    Color digits = g.timed && t < 10000 ? Color(255, 255, 96, 72) : turn ? C_LCD : C_FAINT;
    drawText(gr, fmtClock(t, g.timed), ck, F_MONO, ck.Height * 0.62f, digits, FontStyleBold, 1, 1);
}

static void renderGamePanel(Graphics& gr, RectF sh) {
    paperSheet(gr, sh);
    float pad = S(16), ix = sh.X + pad, iw = sh.Width - 2 * pad, y = sh.Y + S(12);
    std::wstring event = g.online ? (g.doc.event.empty() ? L"Online game" : L"Tournament game")
                       : g_customOn ? g_customEvent : L"Ladder board " + std::to_wstring(curLevel().number);
    drawText(gr, event, RectF(ix, y, iw - S(70), S(24)), F_SERIF, S(15), C_PRINT, FontStyleBold);
    drawText(gr, tcLabel(g.baseMs, g.incMs), RectF(ix, y, iw, S(24)), F_MONO, S(12), C_FAINT, FontStyleRegular, 2);
    y += S(28);
    Pen print(C_PRINT, 1.f), rule(C_RULE, 1.f);
    gr.DrawLine(&print, ix, y, ix + iw, y);
    y += S(10);

    // moves
    float actions = S(64), statusH = S(54), brainH = S(22);
    RectF tbl(ix, y, iw, sh.Y + sh.Height - pad - actions - statusH - brainH - y);
    float hh = S(22), lh = S(25), c0 = S(36), cw = (tbl.Width - c0) / 2;
    {
        SolidBrush head(Color(36, C_PRINT.GetR(), C_PRINT.GetG(), C_PRINT.GetB()));
        gr.FillRectangle(&head, RectF(tbl.X, tbl.Y, tbl.Width, hh));
        drawText(gr, L"WHITE", RectF(tbl.X + c0, tbl.Y, cw, hh), F_SERIF, S(10), C_PRINT, FontStyleBold, 1, 1);
        drawText(gr, L"BLACK", RectF(tbl.X + c0 + cw, tbl.Y, cw, hh), F_SERIF, S(10), C_PRINT, FontStyleBold, 1, 1);
    }
    wchar_t buf[64];
    int rows = max(1, (int)((tbl.Height - hh) / lh)), total = ((int)g.sans.size() + 1) / 2;
    int firstRow = max(0, total - rows);
    for (int r = 0; r < rows; r++) {
        float ry = tbl.Y + hh + r * lh;
        gr.DrawLine(&rule, tbl.X, ry + lh, tbl.X + tbl.Width, ry + lh);
        int n = firstRow + r;
        if (n >= total && n > 0 && r > 0 && n > total) continue;
        swprintf(buf, 64, L"%d", n + 1);
        drawText(gr, buf, RectF(tbl.X, ry, c0 - S(8), lh), F_MONO, S(11), C_FAINT, FontStyleRegular, 2, 1);
        for (int k = 0; k < 2; k++) {
            size_t i = (size_t)n * 2 + k;
            if (i >= g.sans.size()) break;
            bool latest = i + 1 == g.sans.size();
            if (latest) {
                SolidBrush hl(C_HILITE);
                gr.FillRectangle(&hl, RectF(tbl.X + c0 + k * cw + S(4), ry + S(3), cw - S(8), lh - S(5)));
            }
            drawText(gr, g.sans[i], RectF(tbl.X + c0 + k * cw + S(12), ry - S(3), cw - S(14), lh + S(2)), F_HAND, S(14), C_INK);
        }
    }
    gr.DrawRectangle(&print, tbl);
    gr.DrawLine(&print, tbl.X + c0, tbl.Y, tbl.X + c0, tbl.Y + tbl.Height);
    gr.DrawLine(&print, tbl.X + c0 + cw, tbl.Y, tbl.X + c0 + cw, tbl.Y + tbl.Height);
    y = tbl.Y + tbl.Height + S(4);

    // how well the brain is reading you, in small print
    if (g.predTotal > 0)
        drawText(gr, L"the brain saw " + std::to_wstring(g.predHits) + L" of your " + std::to_wstring(g.predTotal) + L" moves coming",
                 RectF(ix, y, iw, brainH), F_SERIF, S(11), C_FAINT, FontStyleItalic, 2, 1);
    y += brainH;

    // status / result (the overlay writes the bot's live thoughts into g_thinkLine)
    g_thinkLine = RectF(ix, y, iw, statusH - S(8));
    if (g_confirm && !g.over) {
        drawText(gr, g_confirm == 2 ? L"Leaving concedes the game. Are you sure you want to resign?" : L"Are you sure you want to resign?",
                 g_thinkLine, F_SERIF, S(15), C_RED, FontStyleBold, 1, 1, true);
    } else if (g.over) {
        std::wstring score = g.resultWhite > 0 ? L"1\x2013" L"0" : g.resultWhite < 0 ? L"0\x2013" L"1" : L"\x00BD\x2013\x00BD";
        drawText(gr, score, RectF(ix, y, S(70), statusH - S(8)), F_SERIF, S(22), C_RED, FontStyleBold, 0, 1);
        drawText(gr, g.resultText + L" by " + g.reasonText + (g.ratingText.empty() ? L"" : L"\n" + g.ratingText),
                 RectF(ix + S(74), y, iw - S(74), statusH - S(8)), F_SERIF, S(13), C_PRINT, FontStyleRegular, 0, 1, true);
    } else if (g.preFrom >= 0) {
        drawText(gr, g.preTo >= 0 ? L"Move queued: it plays as soon as " + oppName() + L" has moved."
                                  : L"Queueing a move\x2026 pick a square (right-click to cancel).",
                 g_thinkLine, F_SERIF, S(13), C_INK, FontStyleItalic, 0, 1, true);
    } else if (g_thinking) {
        if (!g_showThoughts) drawText(gr, oppName() + L" is thinking\x2026", g_thinkLine, F_SERIF, S(13), C_FAINT, FontStyleItalic);
    } else if (g.online && g.pos.side != g.human) {
        drawText(gr, L"Waiting for " + oppName() + L"\x2026", g_thinkLine, F_SERIF, S(14), C_FAINT, FontStyleItalic);
    } else {
        drawText(gr, g.pos.inCheck() ? L"You are in check." : L"Your move.", g_thinkLine, F_SERIF, S(14), g.pos.inCheck() ? C_RED : C_PRINT,
                 FontStyleBold);
    }
    y = sh.Y + sh.Height - pad - actions;

    // actions
    float gap = S(8), bw = (iw - 3 * gap) / 4, bh = actions - S(4);
    auto cell = [&](int k) { return RectF(ix + k * (bw + gap), y, bw, bh); };
    if (g_confirm && !g.over) {
        float hw = (iw - gap) / 2;
        inkButton(gr, B_CONFIRM_YES, RectF(ix, y + S(8), hw, bh - S(16)), L"YES, RESIGN");
        inkButton(gr, B_CONFIRM_NO, RectF(ix + hw + gap, y + S(8), hw, bh - S(16)), L"KEEP PLAYING");
    } else if (!g.over) {
        iconButton(gr, B_UNDO, cell(0), IC_UNDO, L"TAKE BACK", !g.online && g.pos.hist.size() >= 1 && !(g.pos.side == g.human && g.pos.hist.size() < 2));
        iconButton(gr, B_FLIP, cell(1), IC_FLIP, L"TURN");
        iconButton(gr, B_RESIGN, cell(2), IC_FLAG, L"RESIGN");
        iconButton(gr, B_MENU, cell(3), IC_EXIT, L"LEAVE");
    } else if (g.online) {
        iconButton(gr, B_ONLINE, cell(0), IC_ONLINE, L"ROOM");
        iconButton(gr, B_FLIP, cell(1), IC_FLIP, L"TURN");
        if (g_guest) iconButton(gr, B_JOINCLUB, cell(3), IC_JOIN, L"JOIN");
        else iconButton(gr, B_MENU, cell(3), IC_LADDER, L"LADDER");
    } else {
        iconButton(gr, B_REMATCH, cell(0), IC_AGAIN, L"REMATCH");
        iconButton(gr, B_NEXT, cell(1), IC_NEXT, L"NEXT", g.level < 149 && !g_customOn);
        iconButton(gr, B_FLIP, cell(2), IC_FLIP, L"TURN");
        iconButton(gr, B_MENU, cell(3), IC_EXIT, L"LEAVE");
    }
    std::wstring footer = g_brain.busy() ? L"the club brain is studying this game\x2026" : g.online ? g_lobbyMsg : L"";
    if (!footer.empty())
        drawText(gr, footer, RectF(sh.X, sh.Y + sh.Height + S(2), sh.Width, S(18)), F_SERIF, S(11), C_CREAM, FontStyleItalic, 2, 0);
}

static void renderGameSide(Graphics& gr) {
    if (g.puzzle) { renderPuzzleSide(gr); return; }
    Geom b = boardGeom();
    float barH = S(60), gapB = S(10);
    float left = b.x - b.frame, width = b.size + 2 * b.frame;
    RectF top(left, b.y - b.frame - gapB - barH, width, barH), bottom(left, b.y + b.size + b.frame + gapB, width, barH);
    int bottomColor = g.flipped ? BLACK : WHITE;
    renderPlayerBar(gr, top, bottomColor ^ 1);
    renderPlayerBar(gr, bottom, bottomColor);
    float x0 = left + width + S(22);
    renderGamePanel(gr, RectF(x0, top.Y, g_W - x0 - S(18), bottom.Y + bottom.Height - top.Y));
}
