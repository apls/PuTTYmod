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
/* Not declared in the VC2003 SDK headers. */
#ifdef __cplusplus
extern "C"
#endif
BOOL WINAPI AllowSetForegroundWindow(DWORD dwProcessId);
#include <windowsx.h>
#include <commctrl.h>

/*
 * This build does not define UNICODE, so the generic TabCtrl_*
 * convenience macros in commctrl.h resolve to their *A (ANSI)
 * message variants (TCM_INSERTITEMA / TCM_GETITEMA). Feeding them
 * our UTF-16 TCITEMW text makes the control reinterpret the
 * wide string as ANSI bytes - every tab label collapses to one
 * garbage character. The *W message ids are unconditionally
 * defined in the header, so always talk to the tab control through
 * them.
 */
#define tabctrl_insert_item(hwnd, iItem, pitem) \
    (int)SendMessage((hwnd), TCM_INSERTITEMW, (WPARAM)(int)(iItem), \
                     (LPARAM)(pitem))
#define tabctrl_get_item(hwnd, iItem, pitem) \
    (BOOL)SendMessage((hwnd), TCM_GETITEMW, (WPARAM)(int)(iItem), \
                      (LPARAM)(pitem))


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
static void frame_focus_active(void);
static void frame_ensure_foreground(void);
static HMENU frame_view_menu = NULL;
static WinGuiSeat *active_wgs = NULL;
/* True between WM_ENTERMENULOOP and WM_EXITMENULOOP: keyboard
 * messages addressed to the frame belong to menu navigation and must
 * reach DefWindowProc, never the terminal. */
static bool frame_menumode = false;

/*
 * Focus guard (frame timer 1), the successor to the old budgeted
 * watchdog. The shell's post-destruction focus re-resolution on
 * Windows 7 can arrive arbitrarily late, and can land on a window
 * that already holds the focus - which fires no WM_SETFOCUS, so no
 * event would ever re-arm a budgeted retry and the keyboard stayed
 * stranded on the tab control for good. The guard has no budget:
 * while our frame is the foreground window and the keyboard sits on
 * one of our own non-terminal windows (the frame itself, the tab
 * control) or on nothing at all, it keeps pushing focus back onto
 * the active terminal. It never fights for the foreground: when
 * another application owns it, the user switched away deliberately
 * (or a certificate / token dialog from another process is up), and
 * WM_ACTIVATE re-arms the guard when they come back.
 */
static bool focus_guard_running = false;

/*
 * True when our frame is the foreground window but the keyboard is
 * stranded on one of our own non-terminal windows or on nothing at
 * all. GetFocus() returns NULL when another process owns the
 * keyboard, and menu tracking puts the focus on the system menu
 * window, so this test is inherently process-local: it never fires
 * while the user works in another application or navigates the
 * menu bar.
 */
static bool focus_guard_stranded(HWND frame)
{
    if (!active_wgs || !active_wgs->term_hwnd ||
        !IsWindow(active_wgs->term_hwnd))
        return false;
    if (IsIconic(frame) || GetForegroundWindow() != frame)
        return false;
    HWND f = GetFocus();
    return f == tab_ctrl || f == frame || f == NULL;
}

static void focus_guard_arm(void)
{
    if (frame_hwnd && !focus_guard_running) {
        focus_guard_running = true;
        SetTimer(frame_hwnd, 1, 100, NULL);
    }
}

/* Tab drag-reorder state. */
static bool tab_drag_active = false;
static int tab_drag_index = -1;
/*
 * The dragged session's window handle. Used instead of a raw tab
 * index because reordering rebuilds the tab control under the cursor;
 * a handle-based lookup stays valid across every such rebuild.
 */
static HWND tab_drag_hwnd = NULL;
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

/*
 * True while any session has a blocking dialog running in its nested
 * message loop. Destroying a session (and therefore the frame) while
 * that loop is live would tear the session down underneath the dialog
 * and return into freed memory, so frame-close must refuse in that
 * state; per-session WM_CLOSE refuses on its own counter instead.
 */
static bool any_session_blocking(void)
{
    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *wgs = container_of(node, WinGuiSeat, wgslistnode);
        if (wgs->blocking_dialogs > 0)
            return true;
    }
    return false;
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

static int session_index_from_hwnd(HWND hwnd)
{
    struct WinGuiSeatListNode *node;
    int i = 0;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *wgs = container_of(node, WinGuiSeat, wgslistnode);
        if (wgs->term_hwnd == hwnd)
            return i;
        i++;
    }
    return -1;
}

/*
 * Insert wgs (already unlinked) so that it ends up at position `to`.
 * `to` refers to the final list including wgs: items from `to` on are
 * shifted right to make room, so no index adjustment is needed for
 * the preceding unlink. `to` >= current count appends at the end.
 */
static void wgs_insert_at(WinGuiSeat *wgs, int to)
{
    struct WinGuiSeatListNode *ref = &wgslisthead; /* == append at end */
    struct WinGuiSeatListNode *node;
    int i;

    if (to < session_count()) {
        for (node = wgslisthead.next, i = 0;
             node != &wgslisthead && i < to; node = node->next, i++)
            ;
        ref = node;
    }
    wgs->wgslistnode.prev = ref->prev;
    wgs->wgslistnode.next = ref;
    ref->prev->next = &wgs->wgslistnode;
    ref->prev = &wgs->wgslistnode;
}

static void session_move(int from, int to)
{
    WinGuiSeat *wgs = session_at(from);
    if (!wgs || from == to || from < 0 || to < 0)
        return;
    wgs_unlink(wgs);
    wgs_insert_at(wgs, to);
}

/* ----------------------------------------------------------------------
 * Tab labels, derived from the session window titles, with
 * duplicates disambiguated by a bracketed number.
 */

static wchar_t *dup_wstr(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *r = snewn(n, wchar_t);
    memcpy(r, s, n * sizeof(wchar_t));
    return r;
}

static wchar_t *tab_label_for(WinGuiSeat *wgs, WinGuiSeat *exclude)
{
    if (!wgs)
        return dup_wstr(L"?");

    const wchar_t *base = wgs->window_name ? wgs->window_name : L"PuTTY";

    /*
     * Count sessions with the same title among the display set
     * (all sessions except `exclude`, which is the session being
     * removed and is still linked in the list until wgs_cleanup),
     * and find this one's position among them. The first of a set of
     * same-named sessions keeps the plain name; the rest get the
     * name with a bracketed ordinal, e.g. "host - PuTTY (2)".
     */
    int same = 0, mynum = 0;
    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *other = container_of(node, WinGuiSeat, wgslistnode);
        if (other == exclude)
            continue;
        const wchar_t *othername = other->window_name ?
            other->window_name : L"PuTTY";
        if (!wcscmp(othername, base)) {
            same++;
            if (other == wgs)
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

/*
 * Set while tabframe_rebuild_tabs is running: TabCtrl_SetCurSel fires
 * TCN_SELCHANGE synchronously, and FrameWndProc would otherwise call
 * tabframe_activate_session into the middle of the rebuild.
 */
static bool tabframe_rebuilding = false;

/*
 * Rebuild the entire tab control from the session list. The list is
 * the single source of truth: every TCITEM is disposable, and its
 * lParam carries the session window handle so that item order can be
 * mapped back onto sessions without trusting the control's own state.
 *
 * `exclude` is the session being removed (or NULL): it is still
 * linked in the list at rebuild time (wgs_cleanup unlinks it only in
 * WM_NCDESTROY, after WM_DESTROY), so the rebuild must skip it or the
 * just-closing tab would reappear.
 *
 * `select_index` is the item to select afterwards, counted within the
 * display set (i.e. excluding `exclude`); -1 clears the selection.
 */
void tabframe_rebuild_tabs(int select_index, WinGuiSeat *exclude)
{
    if (!tab_ctrl || tabframe_rebuilding)
        return;

    tabframe_rebuilding = true;
    TabCtrl_DeleteAllItems(tab_ctrl);
    int i = 0;
    struct WinGuiSeatListNode *node;
    for (node = wgslisthead.next; node != &wgslisthead; node = node->next) {
        WinGuiSeat *wgs = container_of(node, WinGuiSeat, wgslistnode);
        if (wgs == exclude)
            continue;
        TCITEMW ti;
        wchar_t *label = tab_label_for(wgs, exclude);
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT | TCIF_PARAM;
        ti.pszText = label;
        ti.lParam = (LPARAM)wgs->term_hwnd;
        tabctrl_insert_item(tab_ctrl, i, &ti);
        sfree(label);
        i++;
    }
    if (select_index >= 0 && select_index < i)
        TabCtrl_SetCurSel(tab_ctrl, select_index);
    else if (select_index == -1)
        TabCtrl_SetCurSel(tab_ctrl, -1);
    tabframe_rebuilding = false;
}

/*
 * Title changed (e.g. remote MOTD or a title escape sequence): the
 * duplicate-disambiguation numbers can shift on other tabs too, so
 * refresh all of them.
 */
void tabframe_update_tab_label(WinGuiSeat *wgs)
{
    if (!frame_hwnd)
        return;
    if (session_index(wgs) < 0)
        return;
    tabframe_rebuild_tabs(TabCtrl_GetCurSel(tab_ctrl), NULL);
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
        wchar_t *wlabel = tab_label_for(session_at(i), NULL);
        /*
         * Build the item text in wide characters and use AppendMenuW:
         * this build is not UNICODE, so AppendMenuA would interpret
         * the label through the ANSI code page and mangle any
         * non-ASCII session title. (The old code converted the label
         * to UTF-8 bytes and fed them to the ACP - mojibake - and
         * truncated into a possibly non-NUL-terminated buffer.)
         */
        char *numtxt = dupprintf("%d: ", i + 1);
        wchar_t *numw = dup_mb_to_wc(CP_ACP, numtxt);
        wchar_t *item = dupwcscat_fn(numw, wlabel, NULL);
        sfree(numtxt);
        sfree(numw);
        sfree(wlabel);
        AppendMenuW(menu, MF_STRING | MF_ENABLED,
                    IDM_VIEWSESSION + IDM_VIEW_STEP * i, item);
        /* Check only after the item exists; before, it is a no-op. */
        CheckMenuItem(menu, IDM_VIEWSESSION + IDM_VIEW_STEP * i,
                      (i == TabCtrl_GetCurSel(tab_ctrl) ? MF_CHECKED
                       : MF_UNCHECKED) | MF_BYCOMMAND);
        sfree(item);
    }
    if (n == 0)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_VIEWSESSION,
                    L"(no sessions)");
}

static bool multiple_instances_setting(void)
{
    HKEY key = open_regkey_fn(false, false, HKEY_CURRENT_USER,
                              "Software", "SimonTatham", "PuTTY", NULL);
    bool value = false;
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
    /*
     * While the frame is minimised (or its client rect is otherwise
     * degenerate), GetClientRect returns a near-zero rect. Moving the
     * session children to that size shrinks their terminals to 1x1,
     * and any output arriving while minimised is then written into a
     * single column - the vertical-text-down-the-left-edge artefact
     * seen after restoring. Skip layout entirely in that state; the
     * restore WM_SIZE lays everything out again with the real size.
     */
    if (!frame_hwnd || IsIconic(frame_hwnd))
        return;
    GetClientRect(frame_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= TAB_STRIP_HEIGHT)
        return;
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
    if (!frame_hwnd || tabframe_rebuilding)
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
    /*
     * WSAAsyncSelect registrations are bound to whatever winsel_hwnd
     * was at (re-)registration time, and stay there until the next
     * WSAAsyncSelect call. Now that the network-event sink has moved
     * to this tab's window, every socket must be re-registered
     * immediately: a socket still registered to some other tab's
     * window would keep posting FD_READ/FD_CLOSE there, and when
     * that window is later destroyed those messages - and with them
     * all server output - are lost for good, making the surviving
     * terminal look deaf to the keyboard (keystrokes still reach the
     * server, but no reply is ever displayed). The seat dialogs call
     * socket_reselect_all() with their own session active, which is
     * exactly how that pinning happened in the bug reports.
     */
    socket_reselect_all();
    SetFocus(wgs->term_hwnd);
    frame_focus_active();
    /* A late shell re-resolution from an earlier destroy can still
     * strand focus; let the guard watch over this activation too. */
    focus_guard_arm();
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

    /*
     * The session list already contains wgs (linked in
     * session_create), so a plain rebuild shows it and selects it.
     */
    tabframe_rebuild_tabs(idx, NULL);
    tabframe_activate_session(wgs);
}

static void close_frame_if_empty_cb(void *vctx)
{
    /*
     * The last session tab has closed; a destroyed child does not
     * tear down its parent, so shut the whole frame down here.
     *
     * This runs as a toplevel callback, i.e. on the next
     * run_toplevel_callbacks() after the message that queued it has
     * fully unwound. By then the closing session's DestroyWindow has
     * returned and its WM_NCDESTROY has already run wgs_cleanup(),
     * which unlinked the node from the session list. The emptiness
     * test below is therefore race-free: the "still linked while
     * remove_session runs" state the queueing happens in has already
     * resolved. The !frame_closing_all guard skips us on the
     * frame-driven shutdown, where the frame destroys itself anyway.
     */
    if (frame_hwnd && IsWindow(frame_hwnd) && !frame_closing_all &&
        wgslisthead.next == &wgslisthead)
        DestroyWindow(frame_hwnd);
}

/*
 * Deferred focus re-assertion, queued from tabframe_remove_session.
 * It runs as a toplevel callback, i.e. after the destroyed tab's
 * window has finished its WM_DESTROY/WM_NCDESTROY and the shell has
 * re-resolved keyboard focus. On Windows 7 that re-resolution can hand
 * focus to the frame (the parent), leaving the remaining tabs deaf to
 * the keyboard unless we re-claim the foreground and push focus back
 * onto the newly active terminal out of line with the destruction.
 * The synchronous activation inside remove_session does the same work
 * earlier, before the shell has re-resolved; this is the safety net
 * for the later resolution. Kept deliberately defensive because it
 * cannot be exercised without a live Windows 7 foreground-lock
 * environment.
 */
static void reassert_focus_cb(void *vctx)
{
    (void)vctx;
    if (frame_hwnd && IsWindow(frame_hwnd) && active_wgs) {
        frame_focus_active();
        /* Catch any re-resolution that lands even later than this. */
        focus_guard_arm();
    }
}

void tabframe_remove_session(WinGuiSeat *wgs)
{
    int idx = session_index(wgs);
    if (idx < 0)
        return;

    bool was_active = (wgs == active_wgs);

    int final_sel = -1;
    if (was_active) {
        active_wgs = NULL;
        /*
         * Pick the neighbour to activate, counted among the sessions
         * *excluding* wgs, which is still linked until wgs_cleanup()
         * runs.
         */
        int n = session_count() - 1;
        if (n > 0) {
            final_sel = idx < n ? idx : n - 1;
            int pos = 0;
            struct WinGuiSeatListNode *node;
            for (node = wgslisthead.next; node != &wgslisthead;
                 node = node->next) {
                if (node == &wgs->wgslistnode)
                    continue;
                if (pos++ == final_sel) {
                    /*
                     * Activate before the final rebuild: the shell has
                     * not yet re-resolved keyboard focus for the
                     * closing window (its NCDESTROY is still to come
                     * in this same synchronous dispatch), so the
                     * SetFocus here is legitimate. The rebuild
                     * afterwards corrects the tab selection, which
                     * activate() cannot know (the list still holds
                     * wgs, so its index is off by one for items
                     * after the closed tab).
                     */
                    tabframe_activate_session(
                        container_of(node, WinGuiSeat, wgslistnode));
                    break;
                }
            }
            /*
             * Win7 focus safety net: after the shell re-resolves
             * keyboard focus for the destroyed window it can land on
             * the frame or the tab control, at an arbitrarily later
             * time. The guard keeps repairing focus for as long as
             * our frame is foreground and the keyboard is stranded on
             * one of our own non-terminal windows; a re-resolution
             * that lands on an already-focused window fires no
             * WM_SETFOCUS to re-arm anything, which is exactly why
             * the guard is unbudgeted.
             */
            if (active_wgs) {
                queue_toplevel_callback(reassert_focus_cb, NULL);
                focus_guard_arm();
            }
        } else {
            tabframe_refresh_menu();
        }
    } else {
        final_sel = TabCtrl_GetCurSel(tab_ctrl);
        if (final_sel > idx)
            final_sel--;  /* selection shifts left past the removed tab */
    }

    /*
     * Rebuild excluding wgs: it is still linked in the session list
     * (wgs_cleanup unlinks it in WM_NCDESTROY, after this WM_DESTROY),
     * so without the exclude the closing tab's item would be re-added.
     */
    tabframe_rebuild_tabs(final_sel, wgs);
    tabframe_layout();

    if (!frame_closing_all)
        queue_toplevel_callback(close_frame_if_empty_cb, NULL);
}

/* ----------------------------------------------------------------------
 * Tab control subclass: drag to reorder, double-click to close.
 */

static LRESULT CALLBACK TabCtrlSubclass(HWND hwnd, UINT message,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_SETFOCUS:
        /*
         * The tab control must never hold the keyboard. TCS_FOCUSNEVER
         * only blocks *click* focus; the shell's focus re-resolution
         * after a session window is destroyed can still land here
         * (its sibling session windows are hidden, so it is a
         * candidate). Run the full re-claim (show, z-order, activate,
         * focus - a bare SetFocus can fail or land on the frame) and
         * arm the guard: if the shell re-resolves again later and
         * lands back here, no further WM_SETFOCUS will fire, and the
         * guard is what catches that.
         */
        if (active_wgs && active_wgs->term_hwnd &&
            IsWindow(active_wgs->term_hwnd)) {
            frame_focus_active();
            focus_guard_arm();
            return 0;
        }
        break;
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
                /*
                 * Track the session by its window handle, which
                 * survives the rebuilds the reordering triggers
                 * (index-based tracking would go stale the moment
                 * a rebuild renumbers the items under the cursor).
                 */
                {
                    TCITEMW ti;
                    memset(&ti, 0, sizeof(ti));
                    ti.mask = TCIF_PARAM;
                    if (tabctrl_get_item(hwnd, hit, &ti))
                        tab_drag_hwnd = (HWND)ti.lParam;
                }
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
         *
         * Each swap is list-move + full control rebuild: the
         * session list is the single source of truth, so the
         * control can never drift out of step with it.
         */
        if (tab_drag_active) {
            RECT rc;
            int n = TabCtrl_GetItemCount(hwnd);
            int x = GET_X_LPARAM(lParam);
            int drag_idx = session_index_from_hwnd(tab_drag_hwnd);
            if (drag_idx >= 0 &&
                TabCtrl_GetItemRect(hwnd, drag_idx, &rc)) {
                int dir = 0;
                if (x > rc.right && drag_idx + 1 < n)
                    dir = +1;
                else if (x < rc.left && drag_idx > 0)
                    dir = -1;
                if (dir != 0 &&
                    !(tab_drag_settled && dir != tab_drag_dir)) {
                    session_move(drag_idx, drag_idx + dir);
                    tab_drag_index = drag_idx + dir;
                    tabframe_rebuild_tabs(tab_drag_index, NULL);
                    tab_drag_dir = dir;
                    tab_drag_settled = true;
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
                tab_drag_hwnd = NULL;
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
            tab_drag_hwnd = NULL;
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
            tab_drag_hwnd = NULL;
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
    /*
     * Clear the active-session pointer before any window is
     * destroyed: a destruction can re-entrantly trigger the
     * shell's focus re-resolution, and the focus-reclaim handlers
     * (frame_focus_active, the tab control's WM_SETFOCUS redirect)
     * read active_wgs, which would dangle once the active
     * session's node has been freed by wgs_cleanup.
     */
    active_wgs = NULL;
    while (wgslisthead.next != &wgslisthead) {
        WinGuiSeat *wgs = container_of(
            wgslisthead.next, WinGuiSeat, wgslistnode);
        /*
         * DestroyWindow (not SendMessage(WM_CLOSE)) so no per-session
         * confirmation dialog can stall the shutdown: the session
         * window's WM_CLOSE handler passes straight through while
         * frame_closing_all() is set, and DestroyWindow dispatches
         * WM_DESTROY and WM_NCDESTROY *synchronously* before
         * returning. Each session's wgs_cleanup() therefore unlinks
         * its list node before the loop retests, which the assert
         * below turns from an assumption into a guarantee: if the
         * list ever failed to shrink, the loop would hang instead.
         */
        assert(!wgs->torn_down);
        int before = session_count();
        DestroyWindow(wgs->term_hwnd);
        /* The node must be gone from the list; if it is not, the
         * loop above would spin forever. */
        assert(session_count() == before - 1);
    }
}

/*
 * Bring the frame to the front even where the OS foreground lock
 * rejects a plain SetForegroundWindow: Windows 7 and later grant the
 * foreground only to the process that received the last input event.
 * Restoring a minimised window goes through the shell and earns the
 * right; otherwise grant it to ourselves and inject an ALT keypress,
 * which the input system counts as a user input event.
 */
static void frame_ensure_foreground(void)
{
    if (!frame_hwnd || !IsWindow(frame_hwnd))
        return;
    if (IsIconic(frame_hwnd)) {
        /* Restoring goes through the shell and earns the right. */
        ShowWindow(frame_hwnd, SW_RESTORE);
        SetForegroundWindow(frame_hwnd);
        return;
    }
    if (GetForegroundWindow() != frame_hwnd) {
        /*
         * Another application owns the foreground: break the Windows
         * 7+ foreground lock by granting the right to ourselves and
         * injecting a synthetic ALT keypress, which the input system
         * counts as a user input event.
         *
         * Do NOT inject the ALT when we already are the foreground
         * window: a lone ALT press activates our own menu bar, and
         * the user's next keystrokes would be eaten by menu-mnemonic
         * handling instead of reaching the terminal.
         */
        AllowSetForegroundWindow(GetCurrentProcessId());
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        /*
         * A lone ALT press arms our own menu bar once the frame
         * becomes foreground, which would eat the user's next
         * keystrokes as menu mnemonics. Inject a second press to
         * disarm it: the pair toggles menu mode on and straight back
         * off, and both events sit in the input queue ahead of any
         * real keystroke, so the net effect is foreground without
         * menu mode.
         */
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    }
    SetForegroundWindow(frame_hwnd);
}

/*
 * Make the active session's terminal window the keyboard focus.
 * Windows 7 can leave focus on the frame or the tab control after
 * the shell re-resolves focus following a child window's destruction,
 * and a bare SetFocus on a window that was just re-shown and
 * re-ordered can land on the frame. Force the window visible and on
 * top of the child z-order, make it the active window, and only then
 * take the focus.
 *
 * This routine deliberately never tries to win the foreground: focus
 * repair only applies inside a window that is already the foreground
 * (the guard checks that first). Fighting another process for the
 * foreground from here - with synthetic ALT presses and the like -
 * is what used to arm the menu bar and eat the user's keystrokes,
 * e.g. when a certificate / token dialog from another process held
 * the foreground around a tab close.
 */
static void frame_focus_active(void)
{
    if (!active_wgs || !active_wgs->term_hwnd ||
        !IsWindow(active_wgs->term_hwnd))
        return;
    ShowWindow(active_wgs->term_hwnd, SW_SHOW);
    SetWindowPos(active_wgs->term_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (GetFocus() != active_wgs->term_hwnd) {
        SetActiveWindow(active_wgs->term_hwnd);
        SetFocus(active_wgs->term_hwnd);
    }
}

static void session_open_cb(void *vctx)
{
    char *cmdline = (char *)vctx;
    session_open_from_cmdline(cmdline);
    sfree(cmdline);
    /* Re-assert foreground now that the new tab exists, so the window
     * ends up in front even if the earlier attempt raced the tab. */
    frame_ensure_foreground();
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
        frame_focus_active();
        /*
         * The shell's async focus re-resolution (triggered when a
         * child window is destroyed) can land focus on the frame or
         * the tab control AFTER the synchronous re-claims have run;
         * the guard retries until focus settles on the terminal.
         */
        focus_guard_arm();
        return 0;

      case WM_ACTIVATE:
        /*
         * The frame just became active again: re-arm the guard so a
         * focus that went stranded while we were in the background
         * gets repaired as soon as the user comes back.
         */
        if (LOWORD(wParam) != WA_INACTIVE)
            focus_guard_arm();
        break;

      case WM_ENTERMENULOOP:
        frame_menumode = true;
        break;
      case WM_EXITMENULOOP:
        frame_menumode = false;
        break;

      /*
       * The frame must never be the dead end of the keyboard path.
       * If focus is stranded on the frame - the shell's
       * post-destruction focus re-resolution does exactly this on
       * Windows 7 - forward the original keyboard/IME message to the
       * active session window instead of letting DefWindowProc turn
       * it into menu noise. Menu-mode keys are excluded by the
       * frame_menumode flag (GetFocus() alone cannot tell an armed
       * menu bar from stranded focus: in menu mode the focus _is_
       * the frame).
       */
      case WM_KEYDOWN: case WM_KEYUP:
      case WM_SYSKEYDOWN: case WM_SYSKEYUP:
      case WM_CHAR: case WM_SYSCHAR:
      case WM_DEADCHAR: case WM_SYSDEADCHAR:
      case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION:
      case WM_IME_COMPOSITION: case WM_IME_CHAR:
        if (!frame_menumode && active_wgs && active_wgs->term_hwnd &&
            IsWindow(active_wgs->term_hwnd) && GetFocus() == hwnd) {
            return SendMessage(active_wgs->term_hwnd, message,
                               wParam, lParam);
        }
        break;

      case WM_TIMER:
        if (wParam == 1) {
            /*
             * Foreground-scoped focus repair (see focus_guard_arm):
             * keep pushing focus onto the active terminal for as
             * long as our frame is foreground and the keyboard is
             * stranded on one of our own non-terminal windows. No
             * budget: the re-resolution can arrive arbitrarily late,
             * and landing on an already-focused window fires no
             * WM_SETFOCUS to re-arm anything else.
             */
            HWND fg = GetForegroundWindow();
            DWORD fgpid = 0;
            GetWindowThreadProcessId(fg, &fgpid);
            if (focus_guard_stranded(hwnd)) {
                frame_focus_active();
                if (!focus_guard_stranded(hwnd)) {
                    KillTimer(hwnd, 1);
                    focus_guard_running = false;
                }
            } else if (fg && fg != hwnd &&
                       fgpid == GetCurrentProcessId()) {
                /*
                 * One of our own dialogs (seat warning, host key,
                 * confirmation box) owns the foreground: the
                 * keyboard is legitimately elsewhere, but the
                 * shell's re-resolution at the dialog's teardown
                 * happens while we are not foreground-checked, and
                 * can land on an already-focused window without
                 * firing any event. Keep watching through the whole
                 * dialog instead of stopping and relying on
                 * WM_ACTIVATE to re-arm.
                 */
            } else {
                KillTimer(hwnd, 1);
                focus_guard_running = false;
            }
        }
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
                        /*
                         * Ignore selection changes fired by our own
                         * TabCtrl_SetCurSel calls inside rebuilds and
                         * activation (SetCurSel notifies
                         * synchronously); tabframe_rebuilding covers
                         * the rebuild path, the active_wgs comparison
                         * below covers the rest.
                         */
                        int sel = TabCtrl_GetCurSel(tab_ctrl);
                        WinGuiSeat *wgs = session_at(sel);
                        if (wgs && wgs != active_wgs &&
                            !tabframe_rebuilding)
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
            /*
             * Route through WM_CLOSE so the blocking-dialog guard
             * (any_session_blocking) applies to the menu Exit exactly
             * as it does to the X button: a bare DestroyWindow here
             * would tear sessions down underneath any nested dialog
             * loop they are running (use-after-free on return).
             */
            SendMessage(hwnd, WM_CLOSE, 0, 0);
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
            return 0;
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
          default:
            /*
             * View-menu session picks. IDM_VIEW_STEP is 10, which is
             * not a multiple of 16, so from the third item on the
             * IDs do not survive the (wParam & ~0xF) switch masking
             * and land here; match the full ID range explicitly. The
             * modulo and bounds checks keep real session commands
             * (IDM_SPECIAL_MIN etc., which can also land in this
             * numeric range) out.
             */
            if (wParam >= IDM_VIEWSESSION &&
                (wParam - IDM_VIEWSESSION) % IDM_VIEW_STEP == 0) {
                int idx = (int)((wParam - IDM_VIEWSESSION) / IDM_VIEW_STEP);
                if (idx < IDM_VIEW_MAX && idx < TabCtrl_GetItemCount(tab_ctrl)) {
                    TabCtrl_SetCurSel(tab_ctrl, idx);
                    WinGuiSeat *wgs = session_at(idx);
                    if (wgs)
                        tabframe_activate_session(wgs);
                    return 0;
                }
            }
            /*
             * Menu commands from the frame's own menu bar reach the
             * active session window, whose WndProc owns the session
             * IDM_* handlers.
             */
            if (message == WM_COMMAND && active_wgs &&
                active_wgs->term_hwnd &&
                IsWindow(active_wgs->term_hwnd))
                SendMessage(active_wgs->term_hwnd, WM_COMMAND, wParam,
                            lParam);
            return 0;
        }

      case WM_COPYDATA:
        {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lParam;
            if (cds->dwData == 1 && cds->lpData && cds->cbData > 0) {
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
                frame_ensure_foreground();
                return TRUE;
            }
            /*
             * Not our handoff message: must NOT fall through to
             * WM_CLOSE below, or any local process sending us a
             * foreign WM_COPYDATA would close every session.
             */
            break;
        }

      case WM_CLOSE: {
        /*
         * If a session has a blocking dialog up (host-key prompt,
         * fatal error box, ...), refuse the frame close: the frame
         * parents that dialog's nested message loop, and
         * frame_close_all_sessions() would DestroyWindow the session
         * directly - bypassing the per-session WM_CLOSE guard - and
         * tear it down underneath the dialog. The user dismisses the
         * dialog and closes again.
         */
        if (!any_session_blocking()) {
            frame_close_all_sessions();
            DestroyWindow(hwnd);
        } else {
            /* Tell the user why the close was refused. */
            MessageBoxW(hwnd,
                        L"A session dialog is still open. "
                        L"Close it before closing PuTTY.",
                        L"PuTTY", MB_ICONWARNING | MB_OK);
        }
        return 0;
      }

      case WM_DESTROY:
        if (hwnd == frame_hwnd) {
            KillTimer(hwnd, 1);
            focus_guard_running = false;
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

    /*
     * WS_CLIPCHILDREN: the session windows and the tab strip are all
     * child windows covering most of the client area. Without this,
     * the frame's background brush paints over the terminal whenever
     * the frame repaints (resize, activation, minimize/restore),
     * causing white flashes under the session display.
     */
    frame_hwnd = CreateWindowExW(
        0, FRAME_CLASS_NAMEW, L"PuTTY",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
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
