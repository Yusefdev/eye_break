// eye_break_win.c - Windows overlay + GTK3 settings + Snooze penalty
#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdbool.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>
#include <glib.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

// =================== Defaults (اگر INI نبود) ===================
#define DEF_WORK_INTERVAL_SEC   (20*60)
#define DEF_BREAK_DEFAULT_SEC   20
#define DEF_PENALTY_SEC         (5*60)
#define DEF_IDLE_PAUSE_MS       (5*60*1000)
#define DEF_BLUR_RADIUS         10
#define DEF_GRACE_SEC           60
#define DEF_WAV_BREAK_SHOWN     L"break_shown.wav"
#define DEF_WAV_BREAK_LOCK      L"break_lock.wav"
#define DEF_WAV_PENALTY         L"penalty.wav"

// ==============================================
#define APP_CLASS   L"EyeBreakWinClass"
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY     1001
#define IDT_WORK    2001
#define IDT_BREAK   2002
#define IDT_GRACE   2004
#define IDT_TOAST   2005
#define IDT_SNOOZE  2006

typedef enum { BS_NONE=0, BS_GRACE, BS_ACTIVE, BS_SNOOZE } BreakState;

typedef struct {
    int  work_interval_sec;
    int  break_default_sec;
    int  penalty_sec;
    int  idle_pause_ms;
    int  blur_radius;
    int  grace_sec;
    wchar_t wav_break_shown[MAX_PATH];
    wchar_t wav_break_lock[MAX_PATH];
    wchar_t wav_penalty[MAX_PATH];
} Config;

static Config     g_cfg = {
    DEF_WORK_INTERVAL_SEC, DEF_BREAK_DEFAULT_SEC, DEF_PENALTY_SEC,
    DEF_IDLE_PAUSE_MS, DEF_BLUR_RADIUS, DEF_GRACE_SEC,
    L"", L"", L""
};

static HINSTANCE  g_hInst;
static HWND       g_hwndMain = NULL;
static HWND       g_hwndOverlay = NULL;
static HBITMAP    g_hShotBmp = NULL;
static HHOOK      g_hKeyHook = NULL, g_hMouseHook = NULL;

static BreakState g_state   = BS_NONE;
static BOOL       g_inBreak = FALSE;          // فقط وقتی قفل فعال است TRUE
static int        g_breakRemain = 0;
static int        g_graceRemain = 0;
static int        g_workElapsed = 0;
static NOTIFYICONDATA nid = {0};
static BOOL       g_settingsOpen = FALSE;

// UX: وضعیت دکمه‌ها و تُست
static BOOL       g_hoverPenalty = FALSE, g_pressPenalty = FALSE;
static BOOL       g_hoverPower   = FALSE, g_pressPower   = FALSE;
static wchar_t    g_toast[128] = L"";
static int        g_toastSec = 0;

// ===== اعلان‌ها =====
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK OverlayProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

// ---------------- ابزار عمومی ----------------
static ULONGLONG GetIdleMs(void) {
    LASTINPUTINFO li = {0}; li.cbSize = sizeof(li);
    if (GetLastInputInfo(&li)) {
        return (ULONGLONG)GetTickCount64() - (ULONGLONG)li.dwTime;
    }
    return 0;
}
static void EnableBlur(HWND hwnd) {
    DWM_BLURBEHIND bb = {0};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    DwmEnableBlurBehindWindow(hwnd, &bb);
}
static void FreeShot(void) {
    if (g_hShotBmp) { DeleteObject(g_hShotBmp); g_hShotBmp = NULL; }
}
static HBITMAP CaptureVirtualScreenBitmap(void) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return hbm;
}

// -------- بلور نرم (سه پاس BoxBlur) --------
static void BoxBlur32(BYTE* bits, int w, int h, int stride, int radius) {
    if (radius <= 0) return;
    int dia = radius * 2 + 1;

    BYTE* temp = (BYTE*)malloc(h * stride);
    if (!temp) return;

    for (int y = 0; y < h; ++y) {
      BYTE* src = bits + y * stride;
      BYTE* dst = temp + y * stride;
      int sumB=0,sumG=0,sumR=0,sumA=0;
      for (int i = -radius; i <= radius; ++i) {
        int xi = i; if (xi < 0) xi = 0; if (xi >= w) xi = w-1;
        BYTE* p = src + xi*4;
        sumB += p[0]; sumG += p[1]; sumR += p[2]; sumA += p[3];
      }
      for (int x = 0; x < w; ++x) {
        dst[x*4+0] = (BYTE)(sumB / dia);
        dst[x*4+1] = (BYTE)(sumG / dia);
        dst[x*4+2] = (BYTE)(sumR / dia);
        dst[x*4+3] = (BYTE)(sumA / dia);
        int xout = x - radius; if (xout < 0) xout = 0;
        int xin  = x + radius + 1; if (xin >= w) xin = w-1;
        BYTE* pout = src + xout*4;
        BYTE* pin  = src + xin*4;
        sumB += pin[0] - pout[0];
        sumG += pin[1] - pout[1];
        sumR += pin[2] - pout[2];
        sumA += pin[3] - pout[3];
      }
    }
    for (int x = 0; x < w; ++x) {
      int sumB=0,sumG=0,sumR=0,sumA=0;
      for (int i = -radius; i <= radius; ++i) {
        int yi = i; if (yi < 0) yi = 0; if (yi >= h) yi = h-1;
        BYTE* p = temp + yi*stride + x*4;
        sumB += p[0]; sumG += p[1]; sumR += p[2]; sumA += p[3];
      }
      for (int y = 0; y < h; ++y) {
        BYTE* dst = bits + y*stride + x*4;
        dst[0] = (BYTE)(sumB / dia);
        dst[1] = (BYTE)(sumG / dia);
        dst[2] = (BYTE)(sumR / dia);
        dst[3] = (BYTE)(sumA / dia);
        int yout = y - radius; if (yout < 0) yout = 0;
        int yin  = y + radius + 1; if (yin >= h) yin = h-1;
        BYTE* pout = temp + yout*stride + x*4;
        BYTE* pin  = temp + yin*stride + x*4;
        sumB += pin[0] - pout[0];
        sumG += pin[1] - pout[1];
        sumR += pin[2] - pout[2];
        sumA += pin[3] - pout[3];
      }
    }
    free(temp);
}
static HBITMAP BlurBitmap(HBITMAP src, int radius) {
    if (!src) return NULL;
    BITMAP bm; GetObject(src, sizeof(bm), &bm);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(NULL);
    BYTE* bits = NULL;
    HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return NULL; }

    HDC hdcs = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdcs, src);
    HDC hdcd = CreateCompatibleDC(NULL);
    HGDIOBJ old2 = SelectObject(hdcd, dib);
    BitBlt(hdcd, 0, 0, bm.bmWidth, bm.bmHeight, hdcs, 0, 0, SRCCOPY);
    SelectObject(hdcs, old);
    SelectObject(hdcd, old2);
    DeleteDC(hdcs); DeleteDC(hdcd);

    int stride = bm.bmWidth * 4;
    BoxBlur32(bits, bm.bmWidth, bm.bmHeight, stride, radius);
    BoxBlur32(bits, bm.bmWidth, bm.bmHeight, stride, radius);
    BoxBlur32(bits, bm.bmWidth, bm.bmHeight, stride, radius);
    return dib;
}

// -------- قفل/هوک ورودی‌ها --------
static void StartHooks(void) {
    if (!g_hKeyHook)
        g_hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyProc, g_hInst, 0);
    if (!g_hMouseHook)
        g_hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, g_hInst, 0);
}
static void StopHooks(void) {
    if (g_hKeyHook) { UnhookWindowsHookEx(g_hKeyHook); g_hKeyHook = NULL; }
    if (g_hMouseHook) { UnhookWindowsHookEx(g_hMouseHook); g_hMouseHook = NULL; }
}
LRESULT CALLBACK LowLevelKeyProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_inBreak) return 1;
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_inBreak) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT*)lParam;
        POINT pt = ms->pt;
        HWND h = WindowFromPoint(pt);
        if (h == g_hwndOverlay || IsChild(g_hwndOverlay, h)) {
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }
        return 1;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// -------- Tray --------
static void TrayAdd(HWND h) {
    nid.cbSize = sizeof(nid);
    nid.hWnd = h;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_INFORMATION);
    lstrcpy(nid.szTip, L"Eye Break (Running)");
    Shell_NotifyIcon(NIM_ADD, &nid);
}
static void TrayRemove(void) { if (nid.hWnd) Shell_NotifyIcon(NIM_DELETE, &nid); }

// -------- INI مسیر --------
static void GetIniPath(wchar_t* outPath, size_t cap) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash+1) = 0;
    wcsncpy(outPath, exe, cap-1);
    outPath[cap-1]=0;
    wcsncat(outPath, L"eye_break.ini", cap-1-wcslen(outPath));
}

// -------- INI لود/ذخیره ساده --------
static void LoadConfig(void) {
    wchar_t path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    FILE* f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) {
        if (!g_cfg.wav_break_shown[0]) wcsncpy(g_cfg.wav_break_shown, DEF_WAV_BREAK_SHOWN, MAX_PATH-1);
        if (!g_cfg.wav_break_lock[0])  wcsncpy(g_cfg.wav_break_lock,  DEF_WAV_BREAK_LOCK,  MAX_PATH-1);
        if (!g_cfg.wav_penalty[0])     wcsncpy(g_cfg.wav_penalty,     DEF_WAV_PENALTY,     MAX_PATH-1);
        return;
    }
    wchar_t key[128], val[512];
    while (!feof(f)) {
        if (fwscanf(f, L" %127ls = %511ls ", key, val) == 2) {
            if (!wcscmp(key, L"work_interval_sec")) g_cfg.work_interval_sec = _wtoi(val);
            else if (!wcscmp(key, L"break_default_sec")) g_cfg.break_default_sec = _wtoi(val);
            else if (!wcscmp(key, L"penalty_sec")) g_cfg.penalty_sec = _wtoi(val);
            else if (!wcscmp(key, L"idle_pause_ms")) g_cfg.idle_pause_ms = _wtoi(val);
            else if (!wcscmp(key, L"blur_radius")) g_cfg.blur_radius = _wtoi(val);
            else if (!wcscmp(key, L"grace_sec")) g_cfg.grace_sec = _wtoi(val);
            else if (!wcscmp(key, L"wav_break_shown")) wcsncpy(g_cfg.wav_break_shown, val, MAX_PATH-1);
            else if (!wcscmp(key, L"wav_break_lock"))  wcsncpy(g_cfg.wav_break_lock,  val, MAX_PATH-1);
            else if (!wcscmp(key, L"wav_penalty"))     wcsncpy(g_cfg.wav_penalty,     val, MAX_PATH-1);
        }
        wint_t ch; while ((ch = fgetwc(f)) != WEOF && ch != L'\n');
    }
    fclose(f);
    if (!g_cfg.wav_break_shown[0]) wcsncpy(g_cfg.wav_break_shown, DEF_WAV_BREAK_SHOWN, MAX_PATH-1);
    if (!g_cfg.wav_break_lock[0])  wcsncpy(g_cfg.wav_break_lock,  DEF_WAV_BREAK_LOCK,  MAX_PATH-1);
    if (!g_cfg.wav_penalty[0])     wcsncpy(g_cfg.wav_penalty,     DEF_WAV_PENALTY,     MAX_PATH-1);
}
static void SaveConfig(void) {
    wchar_t path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    FILE* f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"work_interval_sec = %d\n", g_cfg.work_interval_sec);
    fwprintf(f, L"break_default_sec = %d\n", g_cfg.break_default_sec);
    fwprintf(f, L"penalty_sec       = %d\n", g_cfg.penalty_sec);
    fwprintf(f, L"idle_pause_ms     = %d\n", g_cfg.idle_pause_ms);
    fwprintf(f, L"blur_radius       = %d\n", g_cfg.blur_radius);
    fwprintf(f, L"grace_sec         = %d\n", g_cfg.grace_sec);
    fwprintf(f, L"wav_break_shown   = %ls\n", g_cfg.wav_break_shown[0]?g_cfg.wav_break_shown:DEF_WAV_BREAK_SHOWN);
    fwprintf(f, L"wav_break_lock    = %ls\n", g_cfg.wav_break_lock[0]?g_cfg.wav_break_lock:DEF_WAV_BREAK_LOCK);
    fwprintf(f, L"wav_penalty       = %ls\n", g_cfg.wav_penalty[0]?g_cfg.wav_penalty:DEF_WAV_PENALTY);
    fclose(f);
}

// -------- Settings (GTK3) --------
typedef struct {
    GtkWidget *w;
    GtkSpinButton *sp_work, *sp_break, *sp_penalty, *sp_idle, *sp_blur, *sp_grace;
    GtkEntry *e_wav_shown, *e_wav_lock, *e_wav_penalty;
} SettingsUI;

static void settings_on_save(GtkButton *btn, gpointer user_data) {
    SettingsUI* ui = (SettingsUI*)user_data;
    g_cfg.work_interval_sec = gtk_spin_button_get_value_as_int(ui->sp_work);
    g_cfg.break_default_sec = gtk_spin_button_get_value_as_int(ui->sp_break);
    g_cfg.penalty_sec       = gtk_spin_button_get_value_as_int(ui->sp_penalty);
    g_cfg.idle_pause_ms     = gtk_spin_button_get_value_as_int(ui->sp_idle);
    g_cfg.blur_radius       = gtk_spin_button_get_value_as_int(ui->sp_blur);
    g_cfg.grace_sec         = gtk_spin_button_get_value_as_int(ui->sp_grace);

    wchar_t wbuf[MAX_PATH];
    const char* s1 = gtk_entry_get_text(ui->e_wav_shown);
    const char* s2 = gtk_entry_get_text(ui->e_wav_lock);
    const char* s3 = gtk_entry_get_text(ui->e_wav_penalty);
    MultiByteToWideChar(CP_UTF8, 0, s1, -1, wbuf, MAX_PATH); wcsncpy(g_cfg.wav_break_shown, wbuf, MAX_PATH-1);
    MultiByteToWideChar(CP_UTF8, 0, s2, -1, wbuf, MAX_PATH); wcsncpy(g_cfg.wav_break_lock,  wbuf, MAX_PATH-1);
    MultiByteToWideChar(CP_UTF8, 0, s3, -1, wbuf, MAX_PATH); wcsncpy(g_cfg.wav_penalty,     wbuf, MAX_PATH-1);

    SaveConfig();
    gtk_window_close(GTK_WINDOW(ui->w));
}
static void settings_on_cancel(GtkButton *btn, gpointer user_data) {
    SettingsUI* ui = (SettingsUI*)user_data;
    gtk_window_close(GTK_WINDOW(ui->w));
}
static void settings_destroy(GtkWidget* w, gpointer data) {
    g_settingsOpen = FALSE;
    gtk_main_quit();
}
static void settings_add_row(GtkWidget* grid, int r, const char* label, GtkWidget* w) {
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w,   1, r, 1, 1);
}
static gpointer SettingsThread(gpointer arg) {
    if (!gtk_init_check(0, NULL)) return NULL;

    SettingsUI ui; memset(&ui, 0, sizeof(ui));

    ui.w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.w), "Eye Break - Settings");
    gtk_window_set_default_size(GTK_WINDOW(ui.w), 520, 420);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_add(GTK_CONTAINER(ui.w), grid);

    ui.sp_work    = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(60, 8*3600, 60));
    ui.sp_break   = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5,  30*60, 5));
    ui.sp_penalty = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(60, 30*60, 60));
    ui.sp_idle    = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(60*1000, 60*60*1000, 60*1000));
    ui.sp_blur    = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 40, 1));
    ui.sp_grace   = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 10*60, 5));
    ui.e_wav_shown   = GTK_ENTRY(gtk_entry_new());
    ui.e_wav_lock    = GTK_ENTRY(gtk_entry_new());
    ui.e_wav_penalty = GTK_ENTRY(gtk_entry_new());

    gtk_spin_button_set_value(ui.sp_work,    g_cfg.work_interval_sec);
    gtk_spin_button_set_value(ui.sp_break,   g_cfg.break_default_sec);
    gtk_spin_button_set_value(ui.sp_penalty, g_cfg.penalty_sec);
    gtk_spin_button_set_value(ui.sp_idle,    g_cfg.idle_pause_ms);
    gtk_spin_button_set_value(ui.sp_blur,    g_cfg.blur_radius);
    gtk_spin_button_set_value(ui.sp_grace,   g_cfg.grace_sec);

    char buf[MAX_PATH*3];
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.wav_break_shown[0]?g_cfg.wav_break_shown:DEF_WAV_BREAK_SHOWN, -1, buf, sizeof(buf), NULL, NULL);
    gtk_entry_set_text(ui.e_wav_shown, buf);
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.wav_break_lock[0]?g_cfg.wav_break_lock:DEF_WAV_BREAK_LOCK, -1, buf, sizeof(buf), NULL, NULL);
    gtk_entry_set_text(ui.e_wav_lock, buf);
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.wav_penalty[0]?g_cfg.wav_penalty:DEF_WAV_PENALTY, -1, buf, sizeof(buf), NULL, NULL);
    gtk_entry_set_text(ui.e_wav_penalty, buf);

    settings_add_row(grid, 0, "Work interval (sec):",   GTK_WIDGET(ui.sp_work));
    settings_add_row(grid, 1, "Break time (sec):",      GTK_WIDGET(ui.sp_break));
    settings_add_row(grid, 2, "Penalty (sec):",         GTK_WIDGET(ui.sp_penalty));
    settings_add_row(grid, 3, "Idle pause (ms):",       GTK_WIDGET(ui.sp_idle));
    settings_add_row(grid, 4, "Blur radius:",           GTK_WIDGET(ui.sp_blur));
    settings_add_row(grid, 5, "Grace before lock (s):", GTK_WIDGET(ui.sp_grace));
    settings_add_row(grid, 6, "WAV break shown:",       GTK_WIDGET(ui.e_wav_shown));
    settings_add_row(grid, 7, "WAV break lock:",        GTK_WIDGET(ui.e_wav_lock));
    settings_add_row(grid, 8, "WAV penalty:",           GTK_WIDGET(ui.e_wav_penalty));

    GtkWidget *hb = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(hb), GTK_BUTTONBOX_END);
    GtkWidget *btn_save   = gtk_button_new_with_label("Save");
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    gtk_container_add(GTK_CONTAINER(hb), btn_save);
    gtk_container_add(GTK_CONTAINER(hb), btn_cancel);
    gtk_grid_attach(GTK_GRID(grid), hb, 0, 9, 2, 1);

    g_signal_connect(btn_save,   "clicked", G_CALLBACK(settings_on_save),   &ui);
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(settings_on_cancel), &ui);
    g_signal_connect(ui.w, "destroy", G_CALLBACK(settings_destroy), NULL);

    gtk_widget_show_all(ui.w);
    gtk_main();
    return NULL;
}
static void OpenSettingsWindow(void) {
    if (g_settingsOpen) return;
    g_settingsOpen = TRUE;
    g_thread_new("settings", SettingsThread, NULL);
}

// --------- Helper UI ---------
static void DrawCenteredText(HDC hdc, RECT rc, LPCWSTR text, int size, COLORREF color) {
    HFONT hf = CreateFont(size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, hf);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawText(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(hf);
}
static BOOL PtInRectI(RECT r, int x, int y) { return (x>=r.left && x<=r.right && y>=r.top && y<=r.bottom); }
static RECT BtnPenaltyRect(RECT rc) { int w=rc.right-rc.left,h=rc.bottom-rc.top; RECT r={w/2-240,h-160,w/2-20,h-100}; return r; }
static RECT BtnPowerRect(RECT rc)   { int w=rc.right-rc.left,h=rc.bottom-rc.top; RECT r={w/2+20,h-160,w/2+240,h-100}; return r; }

static void ShowToast(const wchar_t* msg, int seconds) {
    wcsncpy(g_toast, msg, 127); g_toast[127]=0;
    g_toastSec = seconds;
    SetTimer(g_hwndOverlay, IDT_TOAST, 1000, NULL);
}

// ---------- چرخهٔ استراحت / وقفه ----------
static void ShowBreakOverlay(int breakSeconds) {
    g_state = BS_GRACE;
    g_inBreak = FALSE;
    g_breakRemain = breakSeconds;
    g_graceRemain = g_cfg.grace_sec;

    FreeShot();
    HBITMAP shot = CaptureVirtualScreenBitmap();
    g_hShotBmp = BlurBitmap(shot, g_cfg.blur_radius);
    if (shot) DeleteObject(shot);

    if (!g_hwndOverlay) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc   = OverlayProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"EyeBreakOverlay";
        RegisterClass(&wc);

        int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        g_hwndOverlay = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"", WS_POPUP,
            x, y, w, h, NULL, NULL, g_hInst, NULL);
        EnableBlur(g_hwndOverlay);
    }

    // صوت: نمایش صفحهٔ استراحت
    PlaySound(g_cfg.wav_break_shown[0]?g_cfg.wav_break_shown:DEF_WAV_BREAK_SHOWN, NULL, SND_FILENAME | SND_ASYNC);

    ShowWindow(g_hwndOverlay, SW_SHOW);
    SetTimer(g_hwndOverlay, IDT_GRACE, 1000, NULL);
    InvalidateRect(g_hwndOverlay, NULL, TRUE);
}

static void HideBreakOverlayResetWork(void) {
    // بستن هر حالت استراحت/وقفه (برای پایان کلی چرخه)
    KillTimer(g_hwndOverlay, IDT_GRACE);
    KillTimer(g_hwndOverlay, IDT_BREAK);
    KillTimer(g_hwndOverlay, IDT_TOAST);
    g_toast[0]=0; g_toastSec=0;

    if (g_inBreak) StopHooks();
    g_inBreak = FALSE;
    g_state   = BS_NONE;

    ShowWindow(g_hwndOverlay, SW_HIDE);
    FreeShot();
    g_workElapsed = 0;
}

static void HideOverlayStartSnooze(void) {
    // بستن صفحه برای پنالتی (Snooze) بدون ریست کل چرخه
    KillTimer(g_hwndOverlay, IDT_GRACE);
    KillTimer(g_hwndOverlay, IDT_BREAK);
    KillTimer(g_hwndOverlay, IDT_TOAST);
    g_toast[0]=0; g_toastSec=0;

    if (g_inBreak) StopHooks();
    g_inBreak = FALSE;

    ShowWindow(g_hwndOverlay, SW_HIDE);
    FreeShot();

    g_state = BS_SNOOZE;
    // بعد از پایان پنالتی، دوباره صفحه استراحت می‌آید
    SetTimer(g_hwndMain, IDT_SNOOZE, g_cfg.penalty_sec * 1000, NULL);
}

// ---------- OverlayProc ----------
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == IDT_GRACE && g_state == BS_GRACE) {
            g_graceRemain--;
            if (g_graceRemain <= 0) {
                KillTimer(hwnd, IDT_GRACE);
                StartHooks();
                g_inBreak = TRUE;
                g_state   = BS_ACTIVE;
                // صوت: ورود به حالت قفل/استراحت
                PlaySound(g_cfg.wav_break_lock[0]?g_cfg.wav_break_lock:DEF_WAV_BREAK_LOCK, NULL, SND_FILENAME | SND_ASYNC);
                SetTimer(hwnd, IDT_BREAK, 1000, NULL);
                ShowToast(L"قفل فعال شد", 2);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wParam == IDT_BREAK && g_state == BS_ACTIVE) {
            g_breakRemain--;
            if (g_breakRemain <= 0) {
                HideBreakOverlayResetWork();
            } else {
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (wParam == IDT_TOAST) {
            if (g_toastSec > 0) {
                g_toastSec--;
                if (g_toastSec == 0) {
                    KillTimer(hwnd, IDT_TOAST);
                    g_toast[0]=0;
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        RECT rc; GetClientRect(hwnd, &rc);
        RECT pb = BtnPenaltyRect(rc), pw = BtnPowerRect(rc);
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        BOOL hovP = PtInRectI(pb, x, y), hovW = PtInRectI(pw, x, y);
        if (hovP != g_hoverPenalty || hovW != g_hoverPower) {
            g_hoverPenalty = hovP; g_hoverPower = hovW;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        RECT pb = BtnPenaltyRect(rc), pw = BtnPowerRect(rc);
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        g_pressPenalty = PtInRectI(pb, x, y);
        g_pressPower   = PtInRectI(pw, x, y);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        RECT rc; GetClientRect(hwnd, &rc);
        RECT pb = BtnPenaltyRect(rc), pw = BtnPowerRect(rc);
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        BOOL insideP = PtInRectI(pb, x, y);
        BOOL insideW = PtInRectI(pw, x, y);

        if (g_pressPenalty && insideP) {
            // صوت پنالتی
            PlaySound(g_cfg.wav_penalty[0]?g_cfg.wav_penalty:DEF_WAV_PENALTY, NULL, SND_FILENAME | SND_ASYNC);
            ShowToast(L"پنالتی: ۵ دقیقه فرصت کار", 2);
            // بستن صفحه و شروع SNOOZE
            HideOverlayStartSnooze();
        } else if (g_pressPower && insideW) {
            ShellExecuteW(NULL, L"open", L"C:\\Windows\\System32\\shutdown.exe", L"/s /t 0", NULL, SW_HIDE);
        }
        g_pressPenalty = g_pressPower = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        if (g_hShotBmp) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, g_hShotBmp);
            BITMAP bm; GetObject(g_hShotBmp, sizeof(bm), &bm);
            StretchBlt(hdc, 0, 0, w, h, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old); DeleteDC(mem);
        } else {
            HBRUSH hb = CreateSolidBrush(RGB(20,20,20));
            FillRect(hdc, &rc, hb); DeleteObject(hb);
        }

        wchar_t buf[128];
        if (g_state == BS_GRACE) {
            DrawCenteredText(hdc, (RECT){0, h/2 - 100, w, h/2 - 40}, L"تا شروع استراحت (صفحه فعلاً آزاد است)", 38, RGB(255,255,255));
            swprintf(buf, 128, L"%02d:%02d", g_graceRemain/60, g_graceRemain%60);
            DrawCenteredText(hdc, (RECT){0, h/2 - 30, w, h/2 + 30}, buf, 46, RGB(200,230,255));
        } else if (g_state == BS_ACTIVE) {
            DrawCenteredText(hdc, (RECT){0, h/2 - 100, w, h/2 - 40}, L"استراحت چشم", 46, RGB(255,255,255));
            swprintf(buf, 128, L"%02d:%02d", g_breakRemain/60, g_breakRemain%60);
            DrawCenteredText(hdc, (RECT){0, h/2 - 30, w, h/2 + 30}, buf, 46, RGB(255,255,255));
        }

        RECT pb = BtnPenaltyRect(rc), pw = BtnPowerRect(rc);
        HBRUSH brBgPenalty = CreateSolidBrush(g_pressPenalty?RGB(60,60,60):(g_hoverPenalty?RGB(45,45,45):RGB(32,32,32)));
        HBRUSH brBgPower   = CreateSolidBrush(g_pressPower?RGB(80,30,30):(g_hoverPower?RGB(70,20,20):RGB(50,10,10)));
        FrameRect(hdc, &pb, (HBRUSH)GetStockObject(WHITE_BRUSH));
        FrameRect(hdc, &pw, (HBRUSH)GetStockObject(WHITE_BRUSH));
        FillRect(hdc, &pb, brBgPenalty);
        FillRect(hdc, &pw, brBgPower);
        DeleteObject(brBgPenalty); DeleteObject(brBgPower);

        DrawCenteredText(hdc, pb, L"پنالتی +۵ دقیقه", 24, RGB(255,255,255));
        DrawCenteredText(hdc, pw, L"خاموش کردن",      24, RGB(255,200,200));

        if (g_toast[0] && g_toastSec>0) {
            RECT tr = { 0, 40, w, 80 };
            DrawCenteredText(hdc, tr, g_toast, 28, RGB(255,255,200));
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------- Tray Menu ----------
static void ToggleStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t path[MAX_PATH]; GetModuleFileName(NULL, path, MAX_PATH);
            RegSetValueEx(hKey, L"EyeBreakWin", 0, REG_SZ, (BYTE*)path, (DWORD)((lstrlen(path)+1)*sizeof(wchar_t)));
        } else {
            RegDeleteValue(hKey, L"EyeBreakWin");
        }
        RegCloseKey(hKey);
    }
}
static void ShowTrayMenu(HWND h) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenu(m, MF_STRING, 1, L"Start break now");
    AppendMenu(m, MF_STRING, 5, L"Settings…");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 2, L"Add to Startup");
    AppendMenu(m, MF_STRING, 3, L"Remove from Startup");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 4, L"Exit");
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
    DestroyMenu(m);

    switch (cmd) {
        case 1: // شروع فوری چرخه استراحت (GRACE → ACTIVE)
            if (g_state == BS_NONE) {
                g_workElapsed = g_cfg.work_interval_sec;
                PostMessage(h, WM_TIMER, IDT_WORK, 0);
            } else if (g_state == BS_SNOOZE) {
                // اگر در Snooze هستیم، بلافاصله نمایش بده
                KillTimer(h, IDT_SNOOZE);
                g_state = BS_NONE;
                ShowBreakOverlay(g_cfg.break_default_sec);
            }
            break;
        case 5: OpenSettingsWindow(); break;
        case 2: ToggleStartup(true);  break;
        case 3: ToggleStartup(false); break;
        case 4: PostQuitMessage(0);   break;
        default: break;
    }
}

// ---------- Main Wnd ----------
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        LoadConfig();
        TrayAdd(h);
        SetTimer(h, IDT_WORK, 1000, NULL);
        return 0;

    case WM_TIMER:
        if (w == IDT_WORK) {
            // فقط وقتی در حالت عادی هستیم (نه GRACE/ACTIVE/SNOOZE)
            if (g_state == BS_NONE) {
                if (GetIdleMs() < (ULONGLONG)g_cfg.idle_pause_ms) {
                    g_workElapsed++;
                    if (g_workElapsed >= g_cfg.work_interval_sec) {
                        ShowBreakOverlay(g_cfg.break_default_sec);
                    }
                }
            }
        } else if (w == IDT_SNOOZE) {
            // پایان پنالتی → دوباره نمایش صفحهٔ استراحت
            KillTimer(h, IDT_SNOOZE);
            g_state = BS_NONE;
            ShowBreakOverlay(g_cfg.break_default_sec);
        }
        return 0;

    case WM_TRAYICON:
        if (w == ID_TRAY && (l == WM_RBUTTONUP || l == WM_LBUTTONUP)) {
            ShowTrayMenu(h);
        }
        return 0;

    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ---------- Entry ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInst;
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    RegisterClass(&wc);

    g_hwndMain = CreateWindowEx(0, APP_CLASS, L"EyeBreakWin",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
        NULL, NULL, hInst, NULL);
    ShowWindow(g_hwndMain, SW_HIDE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}