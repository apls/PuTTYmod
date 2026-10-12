/*
 * Tabbed MDI frame window for the Windows PuTTY front end.
 *
 * A single top-level frame window owns a tab strip (SysTabControl32)
 * and a program-level menu bar. Each PuTTY session is a child window
 * of the frame; exactly one is visible at a time, selected by its
 * tab.
 */

#include <assert.h>
#include <stdio.h>

#include "putty.h"                 /* includes platform.h, hence winsock2.h */
#include "terminal.h"
#include "storage.h"
#include "putty-rc.h"
#include "win-gui-seat.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

/*
 * VC2003's commctrl.h predates some of the tab control macros. The
 * tab control we create is Unicode, so wide messages are correct.
 */
#ifndef TCM_SETITEMW
#define TCM_SETITEMW (WM_USER + 52)
#endif

/*
 * Menu identifiers handled by the frame itself. Session-level
 * identifiers (IDM_COPY etc.) are defined in window.c and are
 * forwarded to the active session window.
 */
#define IDM_CLOSESESS   0x0300
#define IDM_EXIT        0x0310
#define IDM_MULTIINST   0x0320
#define IDM_TABNEXT     0x0330
#define IDM_TABPREV     0x0340
#define IDM_VIEWSESSION 0x0350
#define IDM_VIEW_MAX    20
#define IDM_VIEW_STEP   10

static void build_view_menu(HMENU menu);

#define FRAME_CLASS_NAMEA "PuTTYFrame"
#define FRAME_CLASS_NAMEW L"PuTTYFrame"

#define TAB_STRIP_HEIGHT 26


static HWND frame_hwnd = NULL;
static HWND tab_ctrl = NULL;
static WNDPROC tab_ctrl_oldproc = NULL;
static HMENU frame_savedsess_menu = NULL;
static bool frame_closing_all = false;

/* Fullscreen state for the frame window. */
static bool frame_fullscreen = false;
static RECT frame_pre_fs_rect;
static void tabframe_layout(void);
static HMENU frame_view_menu = NULL;
static WinGuiSeat *active_wgs = NULL;

/* Tab drag-reorder state. */
static bool tab_drag_active = false;
static int tab_drag_index = -1;
/*
 * Direction of the most recent swap, and whether the cursor has been
 * back inside the dragged tab since then (see WM_MOUSEMOVE).
 */
static int tab_drag_dir = 0;
static bool tab_drag_settled = false;

extern HINSTANCE hinst;                /* defined in window.c */

/* ----------------------------------------------------------------------
 * Session list helpers. The WinGuiSeat linked list (wgslisthead) is
 * kept in tab order.
 */

static int session_count(void)
{
    int n = 0;
    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next)
        n++;
    return n;
}

static WinGuiSeat *session_at(int index)
{
    struct WinGuiSeatListNode *node;
    int i = 0;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        if (i++ == index)
            return container_of(node, WinGuiSeat, wgslistnode);
    }
    return NULL;
}

static int session_index(WinGuiSeat *wgs)
{
    struct WinGuiSeatListNode *node;
    int i = 0;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        if (node == &wgs->wgslistnode)
            return i;
        i++;
    }
    return -1;
}

static void session_move(int from, int to)
{
    if (from == to || from < 0 || to < 0)
        return;
    WinGuiSeat *wgs = session_at(from);
    if (!wgs)
        return;
    wgs_unlink(wgs);
    /*
     * Re-insert so that the node ends up at position `to`. Walk to
     * the node currently at position `to` (after the unlink, indices
     * >= from have shifted down by one), and insert before it.
     */
    struct WinGuiSeatListNode *ref = &wgslisthead; /* == append at end */
    if (to < session_count()) {
        struct WinGuiSeatListNode *node;
        int i = 0;
        for (node = wgslisthead.next; node != &wgslisthead;
             node = node->next, i++) {
            if (i == to) {
                ref = node;
                break;
            }
        }
    }
    /* Insert wgs before ref. */
    wgs->wgslistnode.prev = ref->prev;
    wgs->wgslistnode.next = ref;
    ref->prev->next = &wgs->wgslistnode;
    ref->prev = &wgs->wgslistnode;
}

static void tabframe_rebuild_all_tab_labels(void);

/*
 * Move tab `from` to index `to`, keeping the session list and the tab
 * control in step. TCM_MOVEITEM(W) refuses to work on this control,
 * so the tab item itself is moved by deleting it and reinserting it
 * at the destination; the labels are rebuilt from the session list
 * afterwards, so nothing else has to survive the move.
 */
static void tabframe_move_tab(int from, int to)
{
    TCITEMW ti;
    session_move(from, to);
    TabCtrl_DeleteItem(tab_ctrl, from);
    ti.mask = TCIF_TEXT;
    ti.pszText = (wchar_t *)L" ";
    SendMessageW(tab_ctrl, TCM_INSERTITEMW, to, (LPARAM)&ti);
    tabframe_rebuild_all_tab_labels();
}

/* ----------------------------------------------------------------------
 * Tab labels, derived from the session window titles, with
 * duplicates disambiguated by a bracketed number.
 */

static wchar_t *dup_wstr(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *r = snewn(n, wchar_t);
    memcpy(r, s, n);
    return r;
}

static wchar_t *tab_label(int index)
{
    WinGuiSeat *wgs = session_at(index);
    if (!wgs)
        return dup_wstr(L"?");

    const wchar_t *base = wgs->window_name ? wgs->window_name : L"PuTTY";

    /*
     * Count sessions with the same title, and find this one's
     * position among them. The first of a set of same-named sessions
     * keeps the plain name; the rest get the name with a bracketed
     * ordinal, e.g. "host - PuTTY (2)".
     */
    int same = 0, mynum = 0;
    int i;
    for (i = 0; i < session_count(); i++) {
        WinGuiSeat *other = session_at(i);
        const wchar_t *othername = other->window_name ?
            other->window_name : L"PuTTY";
        if (!wcscmp(othername, base)) {
            same++;
            if (i == index)
                mynum = same;
        }
    }

    if (same <= 1 || mynum <= 1)
        return dup_wstr(base);
    wchar_t suffix[32];
    wchar_t *out;
    size_t blen = wcslen(base);
    swprintf(suffix, L" (%d)", mynum);
    out = snewn(blen + wcslen(suffix) + 1, wchar_t);
    memcpy(out, base, blen * sizeof(wchar_t));
    wcscpy(out + blen, suffix);
    return out;
}

void tabframe_update_tab_label(WinGuiSeat *wgs)
{
    if (!frame_hwnd || !tab_ctrl)
        return;
    int i = session_index(wgs);
    if (i < 0)
        return;
    wchar_t *label = tab_label(i);
    TCITEMW ti;
    ti.mask = TCIF_TEXT;
    ti.pszText = label;
    SendMessageW(tab_ctrl, TCM_SETITEMW, i, (LPARAM)&ti);
    sfree(label);
    /*
     * Renumbering: changing one title can change the bracket numbers
     * on other tabs too, so refresh them all.
     */
    int n = TabCtrl_GetItemCount(tab_ctrl);
    int j;
    for (j = 0; j < n; j++) {
        if (j == i)
            continue;
        wchar_t *other = tab_label(j);
        TCITEMW ti2;
        ti2.mask = TCIF_TEXT;
        ti2.pszText = other;
        SendMessageW(tab_ctrl, TCM_SETITEMW, j, (LPARAM)&ti2);
        sfree(other);
    }
}

static void tabframe_rebuild_all_tab_labels(void)
{
    if (!tab_ctrl)
        return;
    int n = TabCtrl_GetItemCount(tab_ctrl);
    int i;
    for (i = 0; i < n; i++) {
        wchar_t *label = tab_label(i);
        TCITEMW ti;
        ti.mask = TCIF_TEXT;
        ti.pszText = label;
        SendMessageW(tab_ctrl, TCM_SETITEMW, i, (LPARAM)&ti);
        sfree(label);
    }
}

/* ----------------------------------------------------------------------
 * Menu bar.
 */

static HMENU build_specials_submenu(WinGuiSeat *wgs)
{
    if (!wgs || !wgs->specials)
        return NULL;

    /* Same lame manual nesting stack as win_seat_update_specials_menu. */
    HMENU saved_menu = NULL;
    int nesting = 1;
    HMENU new_menu = CreatePopupMenu();
    int i;
    for (i = 0; nesting > 0; i++) {
        switch (wgs->specials[i].code) {
          case SS_SEP:
            AppendMenu(new_menu, MF_SEPARATOR, 0, 0);
            break;
          case SS_SUBMENU:
            assert(nesting < 2);
            nesting++;
            saved_menu = new_menu;
            new_menu = CreatePopupMenu();
            AppendMenu(saved_menu, MF_POPUP | MF_ENABLED,
                       (UINT_PTR) new_menu, wgs->specials[i].name);
            break;
          case SS_EXITMENU:
            nesting--;
            if (nesting) {
                new_menu = saved_menu;
                saved_menu = NULL;
            }
            break;
          default:
            AppendMenu(new_menu, MF_ENABLED, IDM_SPECIAL_MIN + 0x10 * i,
                       wgs->specials[i].name);
            break;
        }
    }
    return new_menu;
}

static void rebuild_savedsess_menu(HMENU menu)
{
    int i;
    while (DeleteMenu(menu, 0, MF_BYPOSITION))
        /* do nothing */;
    /* skip sesslist.sessions[0] == Default Settings */
    for (i = 1;
         i < ((sesslist.nsessions <= MENU_SAVED_MAX+1) ? sesslist.nsessions
                                                       : MENU_SAVED_MAX+1);
         i++)
        AppendMenu(menu, MF_ENABLED,
                   IDM_SAVED_MIN + (i-1)*MENU_SAVED_STEP,
                   sesslist.sessions[i]);
    if (sesslist.nsessions <= 1)
        AppendMenu(menu, MF_GRAYED, IDM_SAVED_MIN, "(No sessions)");
}

static void build_view_menu(HMENU menu)
{
    if (!menu)
        return;

    int n = TabCtrl_GetItemCount(tab_ctrl);
    int i;
    for (i = 0; i < n && i < IDM_VIEW_MAX; i++) {
        wchar_t *wlabel = tab_label(i);
        char label[128];
        WideCharToMultiByte(CP_UTF8, 0, wlabel, -1, label, 128, NULL, NULL);
        sfree(wlabel);
        char *item = dupprintf("%d: %s", i+1, label);
        if (i == TabCtrl_GetCurSel(tab_ctrl))
            CheckMenuItem(menu, IDM_VIEWSESSION + IDM_VIEW_STEP * i,
                          MF_CHECKED | MF_BYCOMMAND);
        else
            CheckMenuItem(menu, IDM_VIEWSESSION + IDM_VIEW_STEP * i,
                          MF_UNCHECKED | MF_BYCOMMAND);
        AppendMenu(menu, MF_STRING | MF_ENABLED,
                   IDM_VIEWSESSION + IDM_VIEW_STEP * i, item);
        sfree(item);
    }
    if (n == 0)
        AppendMenu(menu, MF_STRING | MF_GRAYED, IDM_VIEWSESSION, "(no sessions)");
}

static bool multiple_instances_setting(void)
{
    HKEY key = open_regkey_fn(false, false, HKEY_CURRENT_USER,
                              "Software", "SimonTatham", "PuTTY", NULL);
    bool value = true;
    if (key) {
        DWORD d;
        if (get_reg_dword(key, "MultipleInstances", &d))
            value = d != 0;
        close_regkey(key);
    }
    return value;
}

static void set_multiple_instances_setting(bool value)
{
    HKEY key = open_regkey_fn(true, true, HKEY_CURRENT_USER,
                              "Software", "SimonTatham", "PuTTY", NULL);
    if (key) {
        put_reg_dword(key, "MultipleInstances", value ? 1 : 0);
        close_regkey(key);
    }
}

/* Frame-level fullscreen: expand frame window to fill monitor,
 * remove title bar/borders. */
static void frame_toggle_fullscreen(void)
{
    if (!frame_hwnd)
        return;

    if (frame_fullscreen) {
        /* Exit fullscreen: restore previous state. */
        DWORD style = GetWindowLongPtr(frame_hwnd, GWL_STYLE);
        style &= ~(WS_BORDER | WS_DLGFRAME);
        style |= WS_CAPTION | WS_THICKFRAME;
        SetWindowLongPtr(frame_hwnd, GWL_STYLE, style);
        SetWindowPos(frame_hwnd, HWND_TOP,
                     frame_pre_fs_rect.left, frame_pre_fs_rect.top,
                     frame_pre_fs_rect.right - frame_pre_fs_rect.left,
                     frame_pre_fs_rect.bottom - frame_pre_fs_rect.top,
                     SWP_FRAMECHANGED);
        frame_fullscreen = false;
    } else {
        /* Enter fullscreen: save state, expand to monitor. */
        GetWindowRect(frame_hwnd, &frame_pre_fs_rect);

        /* Resize to fill primary monitor. */
        HMONITOR mon = MonitorFromWindow(frame_hwnd,
                                         MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            SetWindowPos(frame_hwnd, HWND_TOP, mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
        }

        /* Remove title bar and all borders. */
        DWORD style = GetWindowLongPtr(frame_hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
        SetWindowLongPtr(frame_hwnd, GWL_STYLE, style);
        SetWindowPos(frame_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        frame_fullscreen = true;
    }

    /* Layout children to fill new frame size. */
    tabframe_layout();
}

void tabframe_refresh_menu(void)
{
    if (!frame_hwnd)
        return;

    HMENU bar = CreateMenu();

    /* --- Session --- */
    HMENU session_menu = CreatePopupMenu();
    AppendMenu(session_menu, MF_ENABLED, IDM_NEWSESS, "&New Session...");
    AppendMenu(session_menu, active_wgs ? MF_ENABLED : MF_GRAYED,
               IDM_DUPSESS, "&Duplicate session");
    if (active_wgs && active_wgs->session_closed)
        AppendMenu(session_menu, MF_ENABLED, IDM_RESTART, "&Restart Session");
    AppendMenu(session_menu, active_wgs ? MF_ENABLED : MF_GRAYED,
               IDM_CLOSESESS, "Close &Session");
    AppendMenu(session_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(session_menu, active_wgs ? MF_ENABLED : MF_GRAYED,
               IDM_FULLSCREEN, "&Full Screen");
    AppendMenu(session_menu, active_wgs ? MF_ENABLED : MF_GRAYED,
               IDM_RECONF, "Chan&ge Settings...");
    AppendMenu(session_menu,
               MF_STRING | (multiple_instances_setting() ? MF_CHECKED :
                            MF_UNCHECKED),
               IDM_MULTIINST, "&Multiple instances");
    AppendMenu(session_menu, active_wgs ? MF_ENABLED : MF_GRAYED,
               IDM_SHOWLOG, "&Event Log");
    AppendMenu(session_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(session_menu, MF_ENABLED, IDM_EXIT, "E&xit");
    AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) session_menu,
               "&Session");


    /* --- Command --- */
    HMENU cmd_menu = build_specials_submenu(active_wgs);
    if (cmd_menu)
        AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) cmd_menu,
                   "&Command");

    /* --- Saved --- */
    /*
     * Fresh popup every time: the old menu bar (and every submenu in
     * it) is destroyed below once the new one is installed, so no
     * menu object may persist between refreshes.
     */
    HMENU savedsess_menu = CreatePopupMenu();
    rebuild_savedsess_menu(savedsess_menu);
    AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) savedsess_menu,
               "Si&tes");
    /* Remembered only so WM_INITMENUPOPUP can recognise this menu; it
     * is destroyed along with the old bar a few lines below, and
     * replaced by a new one on the next refresh. */
    frame_savedsess_menu = savedsess_menu;

    /* --- Edit --- */
    HMENU edit_menu = CreatePopupMenu();
    unsigned enabled = active_wgs ? MF_ENABLED : MF_GRAYED;
    AppendMenu(edit_menu, enabled, IDM_COPY, "&Copy");
    AppendMenu(edit_menu, enabled, IDM_COPYALL, "Copy &All");
    AppendMenu(edit_menu, enabled, IDM_PASTE, "&Paste");
    AppendMenu(edit_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(edit_menu, enabled, IDM_CLRSB, "C&lear Scrollback");
    AppendMenu(edit_menu, enabled, IDM_RESET, "Rese&t Terminal");
    AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) edit_menu, "&Edit");

    /* --- View --- */
    HMENU view_menu = CreatePopupMenu();
    build_view_menu(view_menu);
    AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) view_menu, "&View");
    frame_view_menu = view_menu;

    /* --- Help --- */
    HMENU help_menu = CreatePopupMenu();
    if (has_help())
        AppendMenu(help_menu, MF_ENABLED, IDM_HELP, "&Help");
    char *about = dupprintf("&About %s", appname);
    AppendMenu(help_menu, MF_ENABLED, IDM_ABOUT, about);
    sfree(about);
    AppendMenu(bar, MF_POPUP | MF_ENABLED, (UINT_PTR) help_menu, "&Help");

    HMENU oldbar = GetMenu(frame_hwnd);
    SetMenu(frame_hwnd, bar);
    if (oldbar)
        DestroyMenu(oldbar);
    DrawMenuBar(frame_hwnd);
}

/* ----------------------------------------------------------------------
 * Layout: tab strip on top (hidden when there's only one session),
 * session child windows filling the rest.
 */

static void tabframe_layout(void)
{
    RECT rc;
    GetClientRect(frame_hwnd, &rc);
    int n = TabCtrl_GetItemCount(tab_ctrl);
    int tabh = frame_fullscreen ? 0 : (n > 1 ? TAB_STRIP_HEIGHT : 0);
    ShowWindow(tab_ctrl, tabh ? SW_SHOW : SW_HIDE);
    MoveWindow(tab_ctrl, 0, 0, rc.right, TAB_STRIP_HEIGHT, TRUE);

    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *wgs = container_of(node, WinGuiSeat, wgslistnode);
        if (wgs->term_hwnd && IsWindow(wgs->term_hwnd))
            MoveWindow(wgs->term_hwnd, 0, tabh, rc.right,
                       rc.bottom - tabh, TRUE);
    }
}

/* ----------------------------------------------------------------------
 * Session activation.
 */

void tabframe_activate_session(WinGuiSeat *wgs)
{
    if (!frame_hwnd)
        return;
    int idx = session_index(wgs);
    if (idx < 0)
        return;

    active_wgs = wgs;

    /* Hide every other session, show this one, and bring it to the
     * top of the child z-order so it's actually visible. */
    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *other = container_of(node, WinGuiSeat, wgslistnode);
        if (other != wgs && other->term_hwnd && IsWindow(other->term_hwnd))
            ShowWindow(other->term_hwnd, SW_HIDE);
    }
    if (wgs->term_hwnd && IsWindow(wgs->term_hwnd)) {
        ShowWindow(wgs->term_hwnd, SW_SHOW);
        SetWindowPos(wgs->term_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    TabCtrl_SetCurSel(tab_ctrl, idx);
    winselgui_set_hwnd(wgs->term_hwnd);
    SetFocus(wgs->term_hwnd);
    term_set_focus(wgs->term, true);

    tabframe_refresh_menu();
    tabframe_layout();
}

WinGuiSeat *tabframe_active(void)
{
    return active_wgs;
}

bool tabframe_closing_all(void)
{
    return frame_closing_all;
}

HWND tabframe_hwnd(void)
{
    return frame_hwnd;
}

void tabframe_add_session(WinGuiSeat *wgs)
{
    int idx = session_index(wgs);
    assert(idx >= 0);

    wchar_t *label = tab_label(idx);
    TCITEMW ti;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TCIF_TEXT;
    ti.pszText = label;
    TabCtrl_InsertItem(tab_ctrl, idx, &ti);
    sfree(label);

    tabframe_activate_session(wgs);
    tabframe_update_tab_label(wgs);
}

static void close_frame_if_empty_cb(void *vctx)
{
    /*
     * The last tab has closed: shut the whole program down. This is
     * deferred out of the window-destruction path because wgs is
     * still linked (and its window still being destroyed) when
     * tabframe_remove_session runs.
     */
    if (frame_hwnd && !frame_closing_all &&
        wgslisthead.next == &wgslisthead)
        DestroyWindow(frame_hwnd);
}

void tabframe_remove_session(WinGuiSeat *wgs)
{
    int idx = session_index(wgs);
    if (idx < 0)
        return;

    bool was_active = (wgs == active_wgs);

    TabCtrl_DeleteItem(tab_ctrl, idx);

    if (was_active) {
        active_wgs = NULL;
        /*
         * Pick the neighbour to activate from the list *excluding*
         * wgs, which is still linked until wgs_cleanup() runs.
         */
        int n = session_count() - 1;
        if (n > 0) {
            int target = idx < n ? idx : n - 1;
            WinGuiSeat *next = NULL;
            int pos = 0;
            struct WinGuiSeatListNode *node;
            for (node = wgslisthead.next; node != &wgslisthead;
                 node = node->next) {
                if (node == &wgs->wgslistnode)
                    continue;
                if (pos++ == target) {
                    next = container_of(node, WinGuiSeat, wgslistnode);
                    break;
                }
            }
            if (next)
                tabframe_activate_session(next);
        } else {
            tabframe_refresh_menu();
        }
    } else {
        /* Renumber bracketed duplicate labels in the remaining tabs. */
        tabframe_rebuild_all_tab_labels();
    }
    tabframe_layout();

    if (!frame_closing_all) {
        /*
         * If that was the last tab, exit the program. wgs is still
         * linked at this point, so the emptiness test happens in the
         * callback, once cleanup has finished.
         */
        queue_toplevel_callback(close_frame_if_empty_cb, NULL);
    }
}

/* ----------------------------------------------------------------------
 * Tab control subclass: drag to reorder, double-click to close.
 */

static LRESULT CALLBACK TabCtrlSubclass(HWND hwnd, UINT message,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_LBUTTONDOWN:
        {
            TC_HITTESTINFO ht;
            ht.pt.x = GET_X_LPARAM(lParam);
            ht.pt.y = GET_Y_LPARAM(lParam);
            int hit = TabCtrl_HitTest(hwnd, &ht);
            if (hit >= 0) {
                tab_drag_active = true;
                tab_drag_index = hit;
                tab_drag_dir = 0;
                tab_drag_settled = false;
                SetCapture(hwnd);
            }
        }
        break;
      case WM_MOUSEMOVE:
        /*
         * Reorder by dragging: swap with a neighbour as soon as the
         * cursor leaves the dragged tab's own rectangle, so a short
         * drag onto the neighbouring tab is enough. A swap puts the
         * dragged tab on the far side of the neighbour, which leaves
         * the cursor outside it again; without care the two tabs then
         * swap back and forth. tab_drag_settled blocks a swap in the
         * opposite direction until the cursor has come back inside
         * the dragged tab, while swaps in the same direction stay
         * allowed so a fast drag can cross several tabs.
         */
        if (tab_drag_active) {
            RECT rc;
            int n = TabCtrl_GetItemCount(hwnd);
            int x = GET_X_LPARAM(lParam);
            if (TabCtrl_GetItemRect(hwnd, tab_drag_index, &rc)) {
                int dir = 0;
                if (x > rc.right && tab_drag_index + 1 < n)
                    dir = +1;
                else if (x < rc.left && tab_drag_index > 0)
                    dir = -1;
                if (dir != 0 &&
                    !(tab_drag_settled && dir != tab_drag_dir)) {
                    tabframe_move_tab(tab_drag_index,
                                      tab_drag_index + dir);
                    tab_drag_index += dir;
                    tab_drag_dir = dir;
                    tab_drag_settled = true;
                    TabCtrl_SetCurSel(tab_ctrl, tab_drag_index);
                }
                if (x >= rc.left && x <= rc.right)
                    tab_drag_settled = false;
            }
        }
        break;
      case WM_LBUTTONDBLCLK:
        /*
         * Double-click on a tab closes that session. SysTabControl32
         * never sends NM_DBLCLK itself, so we detect the second click
         * here.
         */
        {
            TC_HITTESTINFO ht;
            ht.pt.x = GET_X_LPARAM(lParam);
            ht.pt.y = GET_Y_LPARAM(lParam);
            int hit = TabCtrl_HitTest(hwnd, &ht);
            if (tab_drag_active) {
                tab_drag_active = false;
                tab_drag_index = -1;
                ReleaseCapture();
            }
            if (hit >= 0) {
                WinGuiSeat *wgs = session_at(hit);
                if (wgs && wgs->term_hwnd && IsWindow(wgs->term_hwnd))
                    SendMessage(wgs->term_hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
      case WM_LBUTTONUP:
        if (tab_drag_active) {
            int idx = tab_drag_index;
            tab_drag_active = false;
            tab_drag_index = -1;
            ReleaseCapture();
            /*
             * Activate the session that was dragged (which is also
             * what a plain click selected).
             */
            WinGuiSeat *wgs = session_at(idx);
            if (wgs && wgs != active_wgs)
                tabframe_activate_session(wgs);
            return 0;   /* consumed: don't let a click-select fire too */
        }
        break;
      case WM_CAPTURECHANGED:
        if (tab_drag_active && (HWND)lParam != hwnd) {
            tab_drag_active = false;
            tab_drag_index = -1;
        }
        break;
    }
    return CallWindowProc(tab_ctrl_oldproc, hwnd, message, wParam, lParam);
}

/* ----------------------------------------------------------------------
 * Frame window procedure.
 */

static void frame_close_all_sessions(void)
{
    frame_closing_all = true;
    while (wgslisthead.next != &wgslisthead) {
        WinGuiSeat *wgs = container_of(
            wgslisthead.next, WinGuiSeat, wgslistnode);
        /*
         * DestroyWindow (not WM_CLOSE) so no per-session confirmation
         * dialogs can slow the shutdown down.
         */
        DestroyWindow(wgs->term_hwnd);
    }
    active_wgs = NULL;
}

static void session_open_cb(void *vctx)
{
    char *cmdline = (char *)vctx;
    session_open_from_cmdline(cmdline);
    sfree(cmdline);
}

static LRESULT CALLBACK FrameWndProc(HWND hwnd, UINT message,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_CREATE:
        tab_ctrl = CreateWindowExW(
            0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_SINGLELINE,
            0, 0, 0, 0, hwnd, NULL, hinst, NULL);
        tab_ctrl_oldproc = (WNDPROC)SetWindowLongPtr(
            tab_ctrl, GWLP_WNDPROC, (LONG_PTR)TabCtrlSubclass);
        return 0;

      case WM_SETFOCUS:
        if (active_wgs && active_wgs->term_hwnd &&
            IsWindow(active_wgs->term_hwnd))
            SetFocus(active_wgs->term_hwnd);
        return 0;

      case WM_SIZE:
        if (tab_ctrl)
            tabframe_layout();
        return 0;

      case WM_GETMINMAXINFO:
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            if (mmi->ptMinTrackSize.x < 200)
                mmi->ptMinTrackSize.x = 200;
            if (mmi->ptMinTrackSize.y < 120)
                mmi->ptMinTrackSize.y = 120;
        }
        return 0;

      case WM_NOTIFY:
        {
            NMHDR *nm = (NMHDR *)lParam;
            if (nm->hwndFrom == tab_ctrl) {
                switch (nm->code) {
                  case TCN_SELCHANGE:
                    {
                        int sel = TabCtrl_GetCurSel(tab_ctrl);
                        WinGuiSeat *wgs = session_at(sel);
                        if (wgs && wgs != active_wgs)
                            tabframe_activate_session(wgs);
                        return 0;
                    }
                  case NM_DBLCLK:
                    {
                        NMITEMACTIVATE *nia = (NMITEMACTIVATE *)nm;
                        WinGuiSeat *wgs = session_at(nia->iItem);
                        if (wgs && wgs->term_hwnd &&
                            IsWindow(wgs->term_hwnd))
                            SendMessage(wgs->term_hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    }
                }
            }
        }
        break;

      case WM_INITMENUPOPUP:
        if ((HMENU)wParam == frame_savedsess_menu) {
            get_sesslist(&sesslist, false); /* free */
            get_sesslist(&sesslist, true);
            rebuild_savedsess_menu(frame_savedsess_menu);
            return 0;
        }
        break;

      case WM_COMMAND:
        switch (wParam & ~0xF) {
          case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
          case IDM_CLOSESESS:
            if (active_wgs && active_wgs->term_hwnd &&
                IsWindow(active_wgs->term_hwnd))
                SendMessage(active_wgs->term_hwnd, WM_CLOSE, 0, 0);
            return 0;
          case IDM_NEWSESS:
            new_session_config_dialog();
            return 0;
          case IDM_SAVEDSESS:
            {
                unsigned int sessno = ((lParam - IDM_SAVED_MIN)
                                       / MENU_SAVED_STEP) + 1;
                if (sessno < (unsigned)sesslist.nsessions)
                    open_saved_session(sesslist.sessions[sessno]);
            }
            return 0;
          case IDM_MULTIINST:
            set_multiple_instances_setting(!multiple_instances_setting());
            tabframe_refresh_menu();
          case IDM_FULLSCREEN:
            frame_toggle_fullscreen();
            return 0;
          case IDM_TABNEXT:
          case IDM_TABPREV:
            {
                int n = TabCtrl_GetItemCount(tab_ctrl);
                if (n > 1) {
                    int sel = TabCtrl_GetCurSel(tab_ctrl);
                    int next = (wParam & ~0xF) == IDM_TABNEXT ?
                        (sel + 1) % n : (sel + n - 1) % n;
                    TabCtrl_SetCurSel(tab_ctrl, next);
                    WinGuiSeat *wgs = session_at(next);
                    if (wgs)
                        tabframe_activate_session(wgs);
                }
            }
            return 0;
          case IDM_VIEWSESSION:
            {
                int idx = (wParam - IDM_VIEWSESSION) / IDM_VIEW_STEP;
                if (idx >= 0) {
                    TabCtrl_SetCurSel(tab_ctrl, idx);
                    WinGuiSeat *wgs = session_at(idx);
                    if (wgs)
                        tabframe_activate_session(wgs);
                }
            }
            return 0;
          default:
            /* Forward everything else to the active session window. */
            if (active_wgs && active_wgs->term_hwnd &&
                IsWindow(active_wgs->term_hwnd))
                SendMessage(active_wgs->term_hwnd, WM_COMMAND, wParam,
                            lParam);
            return 0;
        }

      case WM_COPYDATA:
        {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lParam;
            if (cds->dwData == 1 && cds->lpData) {
                /*
                 * Don't open the new session synchronously inside
                 * WM_COPYDATA: the sending process is still blocked
                 * in SendMessageTimeout until we return, and opening
                 * a session can detour through a modal config box.
                 * Defer to a toplevel callback instead.
                 */
                char *cmdline = snewn(cds->cbData, char);
                memcpy(cmdline, cds->lpData, cds->cbData);
                cmdline[cds->cbData - 1] = '\0';
                queue_toplevel_callback(session_open_cb, cmdline);
            }
            SetForegroundWindow(hwnd);
            return TRUE;
        }

      case WM_CLOSE:
        frame_close_all_sessions();
        DestroyWindow(hwnd);
        return 0;

      case WM_DESTROY:
        if (hwnd == frame_hwnd) {
            frame_hwnd = NULL;
            tab_ctrl = NULL;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* ----------------------------------------------------------------------
 * Frame creation.
 */

void tabframe_create(HINSTANCE inst, int show)
{
    WNDCLASSW wndclassw;
    wndclassw.style = 0;
    wndclassw.lpfnWndProc = FrameWndProc;
    wndclassw.cbClsExtra = 0;
    wndclassw.cbWndExtra = 0;
    wndclassw.hInstance = inst;
    wndclassw.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
    wndclassw.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclassw.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wndclassw.lpszMenuName = NULL;
    wndclassw.lpszClassName = FRAME_CLASS_NAMEW;
    RegisterClassW(&wndclassw);

    frame_hwnd = CreateWindowExW(
        0, FRAME_CLASS_NAMEW, L"PuTTY",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600, NULL, NULL, inst, NULL);

    tabframe_refresh_menu();
    ShowWindow(frame_hwnd, show);
}

/* ----------------------------------------------------------------------
 * Multiple-instances handoff: called from WinMain before creating our
 * own frame. Returns true if the command line was handed off to an
 * already-running instance, which should then exit.
 */

bool tabframe_handoff_if_single_instance(const char *cmdline)
{
    if (multiple_instances_setting())
        return false;

    HWND other = FindWindowA(FRAME_CLASS_NAMEA, NULL);
    if (!other)
        return false;

    COPYDATASTRUCT cds;
    cds.dwData = 1;
    cds.cbData = (DWORD)(strlen(cmdline) + 1);
    cds.lpData = (void *)cmdline;
    DWORD_PTR res;
    if (!SendMessageTimeout(other, WM_COPYDATA, 0, (LPARAM)&cds,
                            SMTO_ABORTIFHUNG, 5000, &res))
        return false;       /* other instance wedged: start our own */
    return true;
}
