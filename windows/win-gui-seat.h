/*
 * Main state structure for an instance of the Windows PuTTY front
 * end, containing all the PuTTY objects and all the Windows API
 * resources like the window handle.
 */

typedef struct WinGuiSeat WinGuiSeat;

struct PopupMenu {
    HMENU menu;
};
enum { SYSMENU, CTXMENU };             /* indices into popup_menus field */

#define FONT_NORMAL 0
#define FONT_BOLD 1
#define FONT_UNDERLINE 2
#define FONT_BOLDUND 3
#define FONT_WIDE       0x04
#define FONT_HIGH       0x08
#define FONT_NARROW     0x10

#define FONT_OEM        0x20
#define FONT_OEMBOLD    0x21
#define FONT_OEMUND     0x22
#define FONT_OEMBOLDUND 0x23

#define FONT_MAXNO      0x40
#define FONT_SHIFT      5

enum BoldMode {
    BOLD_NONE, BOLD_SHADOW, BOLD_FONT
};
enum UnderlineMode {
    UND_LINE, UND_FONT
};

struct _dpi_info {
    POINT cur_dpi;
    RECT new_wnd_rect;
};

/*
 * Against the future possibility of having more than one of these
 * in a process (and the current possibility of having zero), we
 * keep a linked list of all live WinGuiSeats, so that cleanups
 * can be done to any that exist.
 */
struct WinGuiSeatListNode {
    struct WinGuiSeatListNode *next, *prev;
};
extern struct WinGuiSeatListNode wgslisthead; /* static end pointer */

struct WinGuiSeat {
    struct WinGuiSeatListNode wgslistnode;

    /* Set once the session's terminal and backend have been torn
     * down, while the window still exists pending WM_NCDESTROY. */
    bool torn_down;

    Seat seat;
    TermWin termwin;
    LogPolicy logpolicy;

    HWND term_hwnd;

    int extra_width, extra_height;
    int font_width, font_height;
    bool font_dualwidth, font_varpitch;
    int offset_width, offset_height;
    bool was_zoomed;
    int prev_rows, prev_cols; // FIXME I don't think these are even used

    HBITMAP caretbm;
    int caret_x, caret_y;

    int kbd_codepage;

    Ldisc *ldisc;
    Backend *backend;

    cmdline_get_passwd_input_state cmdline_get_passwd_state;

    struct unicode_data ucsdata;
    bool session_closed;
    bool reconfiguring;

    const SessionSpecial *specials;
    HMENU specials_menu;
    int n_specials;

    struct PopupMenu popup_menus[2];
    HMENU savedsess_menu;

    Conf *conf;
    LogContext *logctx;
    Terminal *term;

    int cursor_type;
    int vtmode;

    HFONT fonts[FONT_MAXNO];
    LOGFONT lfont;
    bool fontflag[FONT_MAXNO];
    enum BoldMode bold_font_mode;

    bool bold_colours;
    enum UnderlineMode und_mode;
    int descent, font_strikethrough_y;

    COLORREF colours[OSC4_NCOLOURS];
    HPALETTE pal;
    LPLOGPALETTE logpal;
    bool tried_pal;
    COLORREF colorref_modifier;

    struct _dpi_info dpi_info;

    int dbltime, lasttime, lastact;
    Mouse_Button lastbtn;

    bool send_raw_mouse;
    int wheel_accumulator;

    bool pointer_indicates_raw_mouse;

    BusyStatus busy_status;

    wchar_t *window_name, *icon_name;

    int alt_numberpad_accumulator;
    int compose_state;
    int compose_char;
    WPARAM compose_keycode;

    HDC wintw_hdc;

    bool resizing;
    bool need_backend_resize;

    long next_flash;
    bool flashing;

    long last_beep_time;

    bool ignore_clip;
    bool fullscr_on_max;
    bool processed_resize;
    bool in_scrollbar_loop;
    UINT last_mousemove;
    WPARAM last_wm_mousemove_wParam, last_wm_ncmousemove_wParam;
    LPARAM last_wm_mousemove_lParam, last_wm_ncmousemove_lParam;
    wchar_t pending_surrogate;
};

extern const LogPolicyVtable win_gui_logpolicy_vt; /* in dialog.c */

static inline void wgs_link(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev = wgslisthead.prev;
    wgs->wgslistnode.next = &wgslisthead;
    wgs->wgslistnode.prev->next = &wgs->wgslistnode;
    wgs->wgslistnode.next->prev = &wgs->wgslistnode;
}

static inline void wgs_unlink(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev->next = wgs->wgslistnode.next;
    wgs->wgslistnode.next->prev = wgs->wgslistnode.prev;
}

/*
 * Menu identifiers shared between window.c (session windows) and
 * tabframe.c (the frame's menu bar).
 *
 * From MSDN: in WM_SYSCOMMAND the four low-order bits of wParam are
 * reserved to Windows, so all these identifiers have the low 4 bits
 * clear. Also, identifiers should be < 0xF000.
 */
#define IDM_SHOWLOG   0x0010
#define IDM_NEWSESS   0x0020
#define IDM_DUPSESS   0x0030
#define IDM_RESTART   0x0040
#define IDM_RECONF    0x0050
#define IDM_CLRSB     0x0060
#define IDM_RESET     0x0070
#define IDM_HELP      0x0140
#define IDM_ABOUT     0x0150
#define IDM_SAVEDSESS 0x0160
#define IDM_COPYALL   0x0170
#define IDM_FULLSCREEN  0x0180
#define IDM_COPY      0x0190
#define IDM_PASTE     0x01A0
#define IDM_SPECIALSEP 0x0200

#define IDM_SPECIAL_MIN 0x0400
#define IDM_SPECIAL_MAX 0x0800

#define IDM_SAVED_MIN 0x1000
#define IDM_SAVED_MAX 0x5000
#define MENU_SAVED_STEP 16
/* Maximum number of sessions on saved-session submenu */
#define MENU_SAVED_MAX ((IDM_SAVED_MAX-IDM_SAVED_MIN) / MENU_SAVED_STEP)

/*
 * Interface between window.c and tabframe.c.
 */
HWND tabframe_hwnd(void);
void tabframe_create(HINSTANCE inst, int show);
void tabframe_add_session(WinGuiSeat *wgs);
void tabframe_remove_session(WinGuiSeat *wgs);
void tabframe_update_tab_label(WinGuiSeat *wgs);
void tabframe_refresh_menu(void);
void tabframe_activate_session(WinGuiSeat *wgs);
WinGuiSeat *tabframe_active(void);
bool tabframe_closing_all(void);
bool tabframe_handoff_if_single_instance(const char *cmdline);

/* In window.c: create a new session (tab) with the given Conf. */
WinGuiSeat *session_create(Conf *conf);
/* In window.c: parse a command-line string and open it as a new tab. */
void session_open_from_cmdline(const char *cmdline);
/* In window.c: pop up the configuration dialog with default settings,
 * and open a new tab if the user accepts. */
void new_session_config_dialog(void);
/* In window.c: open a new tab for the saved session whose name is
 * given. */
void open_saved_session(const char *name);
