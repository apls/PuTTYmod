/*
 * Implementation of do_select() for network.c to use, that uses
 * WSAAsyncSelect to convert network activity into window messages,
 * for integration into a GUI event loop.
 */

#include "putty.h"
#include "tree234.h"

static void wm_netevent_callback(void *vctx);

static HWND winsel_hwnd = NULL;
static tree234 *moribund_sockets = NULL;

/*
 * WM_DONE_WITH_SOCKET must reach winselgui_response even when the
 * session window that happened to own winsel_hwnd is the one being
 * torn down (session_teardown runs while the dying window is still
 * winsel_hwnd, and messages queued to a window mid-destruction are
 * discarded). If the removal message is lost, the socket value stays
 * in moribund_sockets forever; on 32-bit Windows the value is handed
 * straight back to a new socket, and every network event for that
 * new socket would then be silently dropped. So the removal message
 * goes to a dedicated message-only window that outlives every
 * session window.
 */
#ifndef HWND_MESSAGE
#define HWND_MESSAGE ((HWND)-3)
#endif
static HWND winsel_msg_hwnd = NULL;

static LRESULT CALLBACK winselmsg_wndproc(HWND hwnd, UINT message,
                                          WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DONE_WITH_SOCKET) {
        winselgui_response(message, wParam, lParam);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

static HWND winsel_message_window(void)
{
    if (!winsel_msg_hwnd) {
        static const char cls[] = "PuTTYWinsockMsgOnly";
        WNDCLASSA wc;
        wc.style = 0;
        wc.lpfnWndProc = winselmsg_wndproc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = NULL;
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = cls;
        if (!RegisterClassA(&wc) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return NULL;
        winsel_msg_hwnd = CreateWindowExA(0, cls, NULL, 0, 0, 0, 0, 0,
                                          HWND_MESSAGE, NULL, NULL, NULL);
    }
    return winsel_msg_hwnd;
}

static int moribund_socket_cmp(void *av, void *bv)
{
    uintptr_t a = (uintptr_t)av, b = (uintptr_t)bv;
    return a < b ? -1 : a > b ? +1 : 0;
}

void winselgui_set_hwnd(HWND hwnd)
{
    winsel_hwnd = hwnd;
}

void winselgui_clear_hwnd(void)
{
    winsel_hwnd = NULL;
}

const char *do_select(SOCKET skt, bool enable)
{
    int msg, events;
    if (enable) {
        msg = WM_NETEVENT;
        events = (FD_CONNECT | FD_READ | FD_WRITE |
                  FD_OOB | FD_CLOSE | FD_ACCEPT);
    } else {
        msg = events = 0;
    }

    assert(winsel_hwnd);

    if (p_WSAAsyncSelect(skt, winsel_hwnd, msg, events) == SOCKET_ERROR)
        return winsock_error_string(p_WSAGetLastError());

    return NULL;
}

struct wm_netevent_params {
    /* Used to pass data to wm_netevent_callback */
    WPARAM wParam;
    LPARAM lParam;
};

static bool callback_is_for_socket(
    void *predicate_ctx, toplevel_callback_fn_t fn, void *callback_ctx)
{
    if (fn != wm_netevent_callback)
        return false;
    struct wm_netevent_params *params =
        (struct wm_netevent_params *)callback_ctx;
    if (params->wParam != (WPARAM)(uintptr_t)predicate_ctx)
        return false;

    /* The 'struct wm_netevent_params' would have been freed by the
     * callback function wm_netevent_callback(). Now that isn't going
     * to run, so we must free it ourself. */
    sfree(callback_ctx);
    return true;
}

void done_with_socket(SOCKET skt)
{
    HWND sink = winsel_message_window();
    if (!moribund_sockets)
        moribund_sockets = newtree234(moribund_socket_cmp);
    if (!sink)
        sink = winsel_hwnd;   /* last-ditch: old behaviour */
    PostMessage(sink, WM_DONE_WITH_SOCKET, (WPARAM)skt, 0);
    add234(moribund_sockets, (void *)skt);
    delete_callbacks(callback_is_for_socket, (void *)(uintptr_t)skt);
}

static void wm_netevent_callback(void *vctx)
{
    struct wm_netevent_params *params = (struct wm_netevent_params *)vctx;
    select_result(params->wParam, params->lParam);
    sfree(params);
}

void winselgui_response(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DONE_WITH_SOCKET) {
        del234(moribund_sockets, (void *)wParam);
        return;
    } else if (moribund_sockets &&
               find234(moribund_sockets, (void *)wParam, NULL)) {
        return;
    }

    /*
     * To protect against re-entrancy when Windows's recv()
     * immediately triggers a new WSAAsyncSelect window message, we
     * don't call select_result directly from this handler but instead
     * wait until we're back out at the top level of the message loop.
     */
    struct wm_netevent_params *params = snew(struct wm_netevent_params);
    params->wParam = wParam;
    params->lParam = lParam;
    queue_toplevel_callback(wm_netevent_callback, params);
}
