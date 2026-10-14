/*
 * The log, file paths and byte helpers that every part of VinceFix uses.
 * Log: VinceFix.log next to this DLL, or in %TEMP% if that folder is not writable. Each line is opened, appended and
 * closed again, so the log is complete even if the game crashes.
 */
#include "vincefix.h"

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
IMPORT void WINAPI GetLocalTime(SYSTEMTIME *time);
IMPORT DWORD WINAPI GetCurrentProcessId(void);
IMPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, WCHAR *buffer, DWORD size);
IMPORT DWORD WINAPI GetTempPathW(DWORD size, WCHAR *buffer);
IMPORT BOOL WINAPI WriteFile(HANDLE file, const void *data, DWORD size, DWORD *written, void *overlapped);

#define FILE_APPEND_DATA 0x4
#define OPEN_ALWAYS 4

static WCHAR g_log_path[520];

/* ---- building a line ---------------------------------------------------------------------------------------------- */

/* The put functions are noinline: inlined, clang unrolls put() into about 33 bytes of code per character of each
   constant string and copies put_dec's 64-bit division loop into every caller (that was most of the DLL's size). */
COLD void put(Line *line, const char *s) { while (*s && line->length < sizeof line->text - 2) line->text[line->length++] = *s++; }
COLD void put_dec(Line *line, unsigned long long v, int width) {
    char digits[24]; int n = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) digits[n++] = '0';
    while (n && line->length < sizeof line->text - 2) line->text[line->length++] = digits[--n];
}
COLD void put_hex(Line *line, unsigned long long v) {
    char digits[20]; int n = 0;
    do { digits[n++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
    put(line, "0x");
    while (n && line->length < sizeof line->text - 2) line->text[line->length++] = digits[--n];
}
COLD void begin(Line *line) {
    SYSTEMTIME t; GetLocalTime(&t);
    line->length = 0;
    put_dec(line, t.wYear, 4); put(line, "-"); put_dec(line, t.wMonth, 2); put(line, "-"); put_dec(line, t.wDay, 2); put(line, " ");
    put_dec(line, t.wHour, 2); put(line, ":"); put_dec(line, t.wMinute, 2); put(line, ":"); put_dec(line, t.wSecond, 2); put(line, ".");
    put_dec(line, t.wMilliseconds, 3); put(line, "  pid "); put_dec(line, GetCurrentProcessId(), 0); put(line, "  ");
}
COLD void put_wide(Line *line, const WCHAR *s) {
    char c[2] = {0, 0};
    for (; *s && line->length < sizeof line->text - 2; s++) { c[0] = *s >= 0x20 && *s < 0x7F ? (char)*s : '?'; put(line, c); }
}
COLD void put_ms(Line *line, DWORD us) { put_dec(line, us / 1000, 0); put(line, "."); put_dec(line, us % 1000 / 10, 2); }

/* ---- writing it --------------------------------------------------------------------------------------------------- */

/* The log, opened (and created if needed) for appending; others may read and write it meanwhile. */
static HANDLE open_log(const WCHAR *path) {
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
}

COLD void emit(Line *line) {
    line->text[line->length++] = '\r'; line->text[line->length++] = '\n';
    if (!g_log_path[0]) return;
    HANDLE f = open_log(g_log_path);
    if (f == INVALID_HANDLE) return;
    DWORD written; WriteFile(f, line->text, line->length, &written, 0); CloseHandle(f);
}
COLD void log_text(const char *text) { Line line; begin(&line); put(&line, text); emit(&line); }

void init_log_path(void) {
    self_dir(g_log_path, 520);
    if (g_log_path[0] && append_name(g_log_path, 520, L"VinceFix.log")) {
        HANDLE f = open_log(g_log_path);
        if (f != INVALID_HANDLE) { CloseHandle(f); return; }
    }
    g_log_path[0] = 0;
    if (GetTempPathW(520, g_log_path) && append_name(g_log_path, 520, L"VinceFix.log")) return;
    g_log_path[0] = 0;
}

/* ---- paths and bytes ---------------------------------------------------------------------------------------------- */

static unsigned wlen(const WCHAR *s) { unsigned n = 0; while (s[n]) n++; return n; }
int append_name(WCHAR *dir, unsigned capacity, const WCHAR *name) {
    unsigned n = wlen(dir), m = wlen(name);
    if (n + m + 2 > capacity) return 0;
    if (n && dir[n - 1] != L'\\') dir[n++] = L'\\';
    for (unsigned i = 0; i <= m; i++) dir[n + i] = name[i];
    return 1;
}

void self_dir(WCHAR *out, unsigned capacity) {
    DWORD n = GetModuleFileNameW(g_self, out, capacity);
    if (n >= capacity) n = 0;
    while (n && out[n - 1] != L'\\') n--;
    out[n] = 0;
}

void zero(void *p, unsigned n) { for (BYTE *b = p; n; n--) *b++ = 0; }
void copy_bytes(void *to, const void *from, unsigned n) {
    BYTE *t = to;
    const BYTE *f = from;
    while (n--) *t++ = *f++;
}
int same_bytes(const void *a, const void *b, unsigned n) {
    for (const BYTE *x = a, *y = b; n; n--) if (*x++ != *y++) return 0;
    return 1;
}
