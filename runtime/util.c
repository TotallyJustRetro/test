#include <errno.h>
#include "util.h"
#undef fopen
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <shlobj.h>
#include <commdlg.h>

static int to_w(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
static void from_w(const wchar_t *w, char *s, size_t n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, s, (int)n, NULL, NULL); }

FILE *fopen_u(const char *path, const char *mode)
{
    wchar_t wp[2048], wm[16];
    if (!to_w(path, wp, 2048) || !to_w(mode, wm, 16)) return fopen(path, mode);
    return _wfopen(wp, wm);
}
int mkdir_u(const char *path)
{
    wchar_t wp[2048];
    if (!to_w(path, wp, 2048)) return -1;
    return (_wmkdir(wp) == 0 || errno == EEXIST) ? 0 : -1;
}
int file_exists(const char *path)
{
    wchar_t wp[2048];
    if (!to_w(path, wp, 2048)) return 0;
    DWORD a = GetFileAttributesW(wp);
    return a != INVALID_FILE_ATTRIBUTES;
}
#else
FILE *fopen_u(const char *path, const char *mode) { return fopen(path, mode); }
int mkdir_u(const char *path) { return mkdir(path, 0755) == 0 || file_exists(path) ? 0 : -1; }
int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
#endif

const char *path_base(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

#ifdef _WIN32
/* Dialog args/result passed to worker thread */
typedef struct {
    int      kind;
    wchar_t  title[256];
    wchar_t  buf[2048];
    int      result;
    HANDLE   done;          /* signalled when thread finishes */
} DlgArgs;

static DWORD WINAPI dlg_thread(void *p)
{
    DlgArgs *a = (DlgArgs *)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (a->kind == DLG_FOLDER) {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.lpszTitle = a->title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST id = SHBrowseForFolderW(&bi);
        if (id) { a->result = SHGetPathFromIDListW(id, a->buf); CoTaskMemFree(id); }
    } else {
        static const wchar_t *filters[] = {
            L"Game Boy ROM (*.gb;*.gbc)\0*.gb;*.gbc\0All files\0*.*\0",
            L"Romhack or patch (*.ips;*.bps;*.ups;*.gb;*.gbc)\0*.ips;*.bps;*.ups;*.gb;*.gbc\0All files\0*.*\0",
            L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga\0All files\0*.*\0",
            L"Audio (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0All files\0*.*\0"};
        OPENFILENAMEW o;
        memset(&o, 0, sizeof o);
        o.lStructSize  = sizeof o;
        o.lpstrFilter  = filters[a->kind];
        o.lpstrFile    = a->buf;
        o.nMaxFile     = 2048;
        o.lpstrTitle   = a->title;
        o.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        a->result = GetOpenFileNameW(&o);
    }
    CoUninitialize();
    SetEvent(a->done);
    return 0;
}

int dlg_pick(int kind, const char *title, char *out, size_t n)
{
    out[0] = 0;
    DlgArgs a;
    memset(&a, 0, sizeof a);
    a.kind = kind;
    to_w(title, a.title, 256);
    a.done = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!a.done) return 0;
    HANDLE t = CreateThread(NULL, 0, dlg_thread, &a, 0, NULL);
    if (!t) { CloseHandle(a.done); return 0; }
    /* Pump SDL events while waiting so the window stays responsive */
    while (WaitForSingleObject(a.done, 16) == WAIT_TIMEOUT) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CloseHandle(t);
    CloseHandle(a.done);
    if (!a.result) return 0;
    from_w(a.buf, out, n);
    return 1;
}
void open_folder(const char *path)
{
    wchar_t wp[2048];
    if (to_w(path, wp, 2048)) ShellExecuteW(NULL, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
}
#else
int dlg_pick(int kind, const char *title, char *out, size_t n)
{
    out[0] = 0;
    char cmd[1024];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (choose %s with prompt \"%s\")' 2>/dev/null", kind == DLG_FOLDER ? "folder" : "file", title);
#else
    snprintf(cmd, sizeof cmd, "zenity --file-selection %s --title=\"%s\" 2>/dev/null || kdialog %s . 2>/dev/null",
             kind == DLG_FOLDER ? "--directory" : "", title, kind == DLG_FOLDER ? "--getexistingdirectory" : "--getopenfilename");
#endif
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    if (!fgets(out, (int)n, p)) { pclose(p); out[0] = 0; return 0; }
    pclose(p);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}
void open_folder(const char *path)
{
    char cmd[1200];
#ifdef __APPLE__
    snprintf(cmd, sizeof cmd, "open \"%s\" >/dev/null 2>&1 &", path);
#else
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
#endif
    if (system(cmd)) {}
}
#endif
