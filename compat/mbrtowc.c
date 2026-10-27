/*
 * VC2003's CRT declares mbrtowc/wcrtomb in <wchar.h> but does not
 * provide them in the library. Implement them on top of the Win32
 * conversion functions using the ANSI code page.
 */
#include <wchar.h>
#include <string.h>
#include <windows.h>

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    if (!s) {
        if (ps) memset(ps, 0, sizeof(*ps));
        return 0;
    }
    if (!*s) {
        if (pwc) *pwc = 0;
        return 0;
    }
    wchar_t tmp;
    if (!pwc) pwc = &tmp;
    int r = MultiByteToWideChar(CP_ACP, 0, s, (int)n, pwc, 1);
    if (r == 0) return (size_t)-1;
    return (size_t)r;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    if (!s) {
        if (ps) memset(ps, 0, sizeof(*ps));
        return 0;
    }
    int r = WideCharToMultiByte(CP_ACP, 0, &wc, 1, s, 8, NULL, NULL);
    if (r == 0) return (size_t)-1;
    return (size_t)r;
}
