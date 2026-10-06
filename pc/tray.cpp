// Hadal tray app
#define NOMINMAX
#include <windows.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <shellapi.h>
#include <shlwapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <dbghelp.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <shlobj.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace winrt::Windows::Media::Control;

static const wchar_t* PIPE = L"\\\\.\\pipe\\hadal";
enum { WM_TRAY = WM_APP + 1, WM_MONOFF, WM_CLIPGET, WM_CLIPSET, WM_OPENURL, WM_LAUNCH };
enum { ID_QR = 1, ID_REGEN, ID_PAUSE, ID_LOG, ID_EXIT, ID_TIMER, ID_SENDFILE, ID_OPENDL, ID_ADDAPP, ID_TOAST0 /* + category */, ID_APP0 = 100 /* + app slot */ };
enum { T_ROUTINE, T_SCREEN, T_POWER, T_SECURITY };
enum { ST_OFF, ST_ON, ST_PAUSED };
static HWND g_wnd, g_qrWnd;
static UINT g_taskbarMsg;
static std::wstring g_mySid;
static std::atomic<int> g_state{ST_OFF};
static HICON g_icons[3];
static std::atomic<int> g_streamGen{0};
void streamRun(HANDLE pipe, int maxH, int fps, int mbps, int monitor, bool lowLatency, std::function<bool()> stopped); // stream.cpp

static std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// Tray icon

// Icons: 2 on, 3 paused, 4 off
static HICON trayIcon(int id) {
    int s = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON, s, s, LR_DEFAULTCOLOR);
}

static void icon(DWORD op) {
    static const wchar_t* tips[] = {L"Hadal - service not running", L"Hadal - ready", L"Hadal - remote control paused"};
    NOTIFYICONDATAW d{sizeof d};
    d.hWnd = g_wnd;
    d.uID = 1;
    d.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    d.uCallbackMessage = WM_TRAY;
    d.hIcon = g_icons[g_state];
    wcscpy_s(d.szTip, tips[g_state]);
    Shell_NotifyIconW(op, &d);
}
static void setState(int s) { g_state = s; icon(NIM_MODIFY); }
static int stateFrom(const std::string& svcReply) { return svcReply.size() < 2 ? ST_OFF : svcReply[1] == '1' ? ST_PAUSED : ST_ON; }

// Toast settings
static const wchar_t* g_toastKeys[] = {L"toast_routine", L"toast_screen", L"toast_power"};
static bool toastOn(int cat) {
    DWORD v = 1, sz = sizeof v;
    if (cat != T_SECURITY) RegGetValueW(HKEY_CURRENT_USER, L"Software\\Hadal", g_toastKeys[cat], RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}
static void setToastOn(int cat, bool on) {
    DWORD v = on;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Hadal", g_toastKeys[cat], REG_DWORD, &v, sizeof v);
}

// Collapse repeats
static void toast(int cat, const std::wstring& text) {
    static ULONGLONG last[4];
    static const ULONGLONG window[] = {8000, 60000, 0, 0};
    ULONGLONG now = GetTickCount64();
    if (!toastOn(cat) || (last[cat] && now - last[cat] < window[cat])) return;
    last[cat] = now;
    NOTIFYICONDATAW d{sizeof d};
    d.hWnd = g_wnd;
    d.uID = 1;
    d.uFlags = NIF_INFO;
    d.dwInfoFlags = NIIF_INFO;
    wcscpy_s(d.szInfoTitle, L"Hadal");
    wcsncpy_s(d.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &d);
}

// Pipe

// Only trust system/admin/our own pipe
static bool trustedServer(HANDLE h) {
    PSID owner;
    PSECURITY_DESCRIPTOR sd;
    if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd)) return false;
    wchar_t* s = nullptr;
    std::wstring o = ConvertSidToStringSidW(owner, &s) ? s : L"";
    LocalFree(s);
    LocalFree(sd);
    return o == L"S-1-5-18" || o == L"S-1-5-19" || o == L"S-1-5-32-544" || o.rfind(L"S-1-5-80-", 0) == 0 || o == g_mySid;
}

static HANDLE openPipe() {
    // Identify only, no impersonation
    HANDLE h = CreateFileW(PIPE, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr) || !trustedServer(h)) { CloseHandle(h); return nullptr; }
    return h;
}

static bool readMsg(HANDLE h, size_t cap, std::string& s) {
    s.clear();
    std::vector<char> b(65536);
    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(h, b.data(), (DWORD)b.size(), &n, nullptr);
        s.append(b.data(), n);
        if (ok) return true;
        if (GetLastError() != ERROR_MORE_DATA || s.size() > cap) return false;
    }
}

static std::string readMsg(HANDLE h, size_t cap) {
    std::string s;
    return readMsg(h, cap, s) ? s : "";
}

// Service command
static std::string svc(const char* cmd) {
    HANDLE h = openPipe();
    if (!h) return "";
    DWORD n;
    std::string r = WriteFile(h, cmd, (DWORD)strlen(cmd), &n, nullptr) ? readMsg(h, 4096) : "";
    CloseHandle(h);
    return r;
}

// Session actions

static std::string fgApp() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "";
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string r = QueryFullProcessImageNameW(p, 0, path, &n) ? narrow(PathFindFileNameW(path)) : "";
    CloseHandle(p);
    return r;
}

static Microsoft::WRL::ComPtr<IAudioEndpointVolume> endpoint() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> en;
    Microsoft::WRL::ComPtr<IMMDevice> dev;
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> v;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)v.GetAddressOf())))
        return nullptr;
    return v;
}

static std::string volume(const std::string& a) {
    auto v = endpoint();
    if (!v) return "";
    BOOL muted = FALSE;
    float lvl = 0;
    if (a == "mute") {
        v->GetMute(&muted);
        v->SetMute(!muted, nullptr);
        return muted ? "Unmuted" : "Muted";
    }
    v->GetMasterVolumeLevelScalar(&lvl);
    if (a.rfind("vol_set ", 0) == 0) { lvl = atoi(a.c_str() + 8) / 100.f; v->SetMute(FALSE, nullptr); }
    else lvl += a == "vol_up" ? 0.05f : -0.05f;
    lvl = std::clamp(lvl, 0.f, 1.f);
    v->SetMasterVolumeLevelScalar(lvl, nullptr);
    return "Volume " + std::to_string(std::lround(lvl * 100)) + "%";
}

static std::string volJson() {
    auto v = endpoint();
    BOOL muted = FALSE;
    float lvl = 0;
    if (!v || FAILED(v->GetMasterVolumeLevelScalar(&lvl)) || FAILED(v->GetMute(&muted))) return "\"vol\":null,\"muted\":false";
    return "\"vol\":" + std::to_string(std::lround(lvl * 100)) + ",\"muted\":" + (muted ? "true" : "false");
}

static bool key(WORD vk, bool ext = false) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[0].ki.dwFlags = ext ? KEYEVENTF_EXTENDEDKEY : 0;
    in[1].ki.dwFlags = in[0].ki.dwFlags | KEYEVENTF_KEYUP;
    return SendInput(2, in, sizeof(INPUT)) == 2;
}

// Key combos
static bool combo(const std::string& s) {
    static const struct { const char* name; WORD vk; bool ext; } names[] = {
        {"ctrl", VK_CONTROL, false}, {"alt", VK_MENU, false}, {"shift", VK_SHIFT, false}, {"win", VK_LWIN, true}, {"tab", VK_TAB, false},
        {"esc", VK_ESCAPE, false}, {"f4", VK_F4, false}, {"f5", VK_F5, false}, {"f11", VK_F11, false}};
    std::vector<std::pair<WORD, bool>> keys;
    for (size_t p = 0, e; p <= s.size(); p = e + 1) {
        e = s.find('+', p);
        if (e == std::string::npos) e = s.size();
        std::string k = s.substr(p, e - p);
        if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') { keys.push_back({(WORD)toupper(k[0]), false}); continue; }
        auto it = std::find_if(std::begin(names), std::end(names), [&](auto& n) { return k == n.name; });
        if (it == std::end(names)) return false;
        keys.push_back({it->vk, it->ext});
    }
    std::vector<INPUT> v;
    for (int up = 0; up < 2; up++)
        for (size_t i = 0; i < keys.size(); i++) {
            auto [vk, ext] = keys[up ? keys.size() - 1 - i : i];
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
            v.push_back(in);
        }
    return SendInput((UINT)v.size(), v.data(), sizeof(INPUT)) == v.size();
}

static const struct { const char* name; UINT32 flag; } g_topo[] = {
    {"internal", SDC_TOPOLOGY_INTERNAL}, {"clone", SDC_TOPOLOGY_CLONE}, {"extend", SDC_TOPOLOGY_EXTEND}, {"external", SDC_TOPOLOGY_EXTERNAL}};

static std::string displayMode() {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &np, &nm) != ERROR_SUCCESS) return "";
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    DISPLAYCONFIG_TOPOLOGY_ID id{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &np, p.data(), &nm, m.data(), &id) != ERROR_SUCCESS) return "";
    for (auto& t : g_topo) if ((UINT32)id == t.flag) return t.name;
    return "";
}

static bool setDisplay(const std::string& mode) {
    for (auto& t : g_topo) if (mode == t.name) return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | t.flag) == ERROR_SUCCESS;
    return false;
}

// Media sessions
template <class Op> static auto wait(Op op) {
    if (op.wait_for(std::chrono::seconds(2)) != winrt::Windows::Foundation::AsyncStatus::Completed) throw std::runtime_error("timeout");
    return op.GetResults();
}

static GlobalSystemMediaTransportControlsSession mediaSession() {
    static std::mutex mu;
    static GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    std::lock_guard<std::mutex> l(mu);
    try {
        if (!mgr) mgr = wait(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
        return mgr.GetCurrentSession();
    } catch (...) { return nullptr; }
}

static std::string mediaJson() {
    try {
        auto s = mediaSession();
        if (!s) return "null";
        auto p = wait(s.TryGetMediaPropertiesAsync());
        bool playing = s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        return "{\"app\":" + jstr(winrt::to_string(s.SourceAppUserModelId())) + ",\"title\":" + jstr(winrt::to_string(p.Title())) +
               ",\"artist\":" + jstr(winrt::to_string(p.Artist())) + ",\"playing\":" + (playing ? "true" : "false") + "}";
    } catch (...) { return "null"; }
}

// Cached so status never waits on media apps
static std::mutex g_mediaMu;
static std::string g_media = "null";
static std::atomic<ULONGLONG> g_mediaAsked{0};

// Only while the phone is asking
static void mediaLoop() {
    winrt::init_apartment();
    for (;; Sleep(1500)) {
        if (GetTickCount64() - g_mediaAsked > 15000) continue;
        std::string j = mediaJson();
        std::lock_guard<std::mutex> l(g_mediaMu);
        g_media = j;
    }
}

static bool media(const std::string& a) {
    try {
        if (auto s = mediaSession())
            if (wait(a == "prev" ? s.TrySkipPreviousAsync() : a == "next" ? s.TrySkipNextAsync() : s.TryTogglePlayPauseAsync())) return true;
    } catch (...) {}
    return key(a == "prev" ? VK_MEDIA_PREV_TRACK : a == "next" ? VK_MEDIA_NEXT_TRACK : VK_MEDIA_PLAY_PAUSE, true);
}

// Input
static bool input(const std::string& l) {
    if (l.size() < 3) return false;
    char t = l[0];
    std::string a = l.substr(2);
    if (t == 't') {
        std::vector<INPUT> v;
        for (wchar_t ch : wide(a))
            for (DWORD up : {0ul, (DWORD)KEYEVENTF_KEYUP}) {
                INPUT k{};
                k.type = INPUT_KEYBOARD;
                k.ki.wScan = ch;
                k.ki.dwFlags = KEYEVENTF_UNICODE | up;
                v.push_back(k);
            }
        return SendInput((UINT)v.size(), v.data(), sizeof(INPUT)) == v.size();
    }
    if (t == 'k') {
        static const struct { const char* name; WORD vk; bool ext; } keys[] = {
            {"enter", VK_RETURN, false}, {"backspace", VK_BACK, false}, {"tab", VK_TAB, false}, {"esc", VK_ESCAPE, false},
            {"space", VK_SPACE, false}, {"left", VK_LEFT, true}, {"right", VK_RIGHT, true}, {"up", VK_UP, true}, {"down", VK_DOWN, true},
            {"home", VK_HOME, true}, {"end", VK_END, true}, {"delete", VK_DELETE, true}, {"win", VK_LWIN, true},
            {"pgup", VK_PRIOR, true}, {"pgdn", VK_NEXT, true}, {"f5", VK_F5, false}, {"f11", VK_F11, false}};
        for (auto& k : keys) if (a == k.name) return key(k.vk, k.ext);
        return a.find('+') != std::string::npos && combo(a);
    }
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    UINT n = 1;
    if (t == 'm' || t == 'a') {
        if (sscanf_s(a.c_str(), "%ld %ld", &in[0].mi.dx, &in[0].mi.dy) != 2) return false;
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | (t == 'a' ? MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK : 0);
    } else if (t == 's') {
        in[0].mi.mouseData = (DWORD)(atoi(a.c_str()) * 4);
        in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
    } else if (t == 'c' || t == 'd' || t == 'u') {
        DWORD down = a == "l" ? MOUSEEVENTF_LEFTDOWN : a == "r" ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_MIDDLEDOWN, up = down << 1;
        in[0].mi.dwFlags = t == 'u' ? up : down;
        if (t == 'c') { in[1].mi.dwFlags = up; n = 2; }
    } else return false;
    return SendInput(n, in, sizeof(INPUT)) == n;
}

// Screenshot
static std::string screenshot() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC s = GetDC(nullptr), m = CreateCompatibleDC(s);
    HBITMAP b = CreateCompatibleBitmap(s, w, h);
    HGDIOBJ old = SelectObject(m, b);
    BOOL ok = BitBlt(m, 0, 0, w, h, s, x, y, SRCCOPY | CAPTUREBLT);
    CURSORINFO ci{sizeof ci};
    ICONINFO ii{};
    if (ok && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && GetIconInfo(ci.hCursor, &ii)) {
        DrawIconEx(m, ci.ptScreenPos.x - x - (int)ii.xHotspot, ci.ptScreenPos.y - y - (int)ii.yHotspot, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    SelectObject(m, old);
    DeleteDC(m);
    ReleaseDC(nullptr, s);
    std::string out;
    if (ok) {
        static const CLSID jpeg = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        ULONG quality = 85;
        Gdiplus::Bitmap bmp(b, nullptr);
        if (IStream* st = SHCreateMemStream(nullptr, 0)) {
            ULARGE_INTEGER sz;
            Gdiplus::EncoderParameters ep{1, {{Gdiplus::EncoderQuality, 1, Gdiplus::EncoderParameterValueTypeLong, &quality}}};
            if (bmp.Save(st, &jpeg, &ep) == Gdiplus::Ok && SUCCEEDED(IStream_Size(st, &sz)) && SUCCEEDED(IStream_Reset(st))) {
                out.resize((size_t)sz.QuadPart);
                if (FAILED(IStream_Read(st, out.data(), (ULONG)out.size()))) out.clear();
            }
            st->Release();
        }
    }
    DeleteObject(b);
    return out;
}

// Files

static std::wstring hadalDir(bool outbox) {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) d = std::wstring(L"\\\\?\\") + p + L"\\Hadal";
    CoTaskMemFree(p);
    if (d.empty()) return d;
    CreateDirectoryW(d.c_str(), nullptr);
    if (outbox) { d += L"\\To phone"; CreateDirectoryW(d.c_str(), nullptr); }
    return d;
}

static bool plainName(const std::wstring& n) {
    if (n.empty() || n.size() > 200 || n == L"." || n == L".." || n.front() == L' ' || n.back() == L' ' || n.back() == L'.') return false;
    for (wchar_t c : n) if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) return false;
    return true;
}

static std::string sizeText(unsigned long long b) {
    char s[32];
    if (b >= 1ull << 30) sprintf_s(s, "%.1f GB", b / 1073741824.0);
    else if (b >= 1ull << 20) sprintf_s(s, "%.1f MB", b / 1048576.0);
    else sprintf_s(s, "%llu KB", (b + 1023) / 1024);
    return s;
}

// Never overwrite
static HANDLE createUnique(const std::wstring& dir, const std::wstring& name, std::wstring& made) {
    size_t dot = name.rfind(L'.');
    std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot), ext = stem.size() == name.size() ? L"" : name.substr(dot);
    for (int i = 0; i < 1000; i++) {
        made = i ? stem + L" (" + std::to_wstring(i) + L")" + ext : name;
        HANDLE f = CreateFileW((dir + L"\\" + made).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return f;
    }
    return INVALID_HANDLE_VALUE;
}

static std::string listOutbox() {
    std::wstring dir = hadalDir(true);
    std::string out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\*").c_str(), &fd);
    for (int n = 0; h != INVALID_HANDLE_VALUE && n < 200;) {
        if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && plainName(fd.cFileName)) {
            ULARGE_INTEGER sz{fd.nFileSizeLow, fd.nFileSizeHigh};
            out += (out.empty() ? "" : ",") + std::string("{\"name\":") + jstr(narrow(fd.cFileName)) + ",\"size\":" + std::to_string(sz.QuadPart) + "}";
            n++;
        }
        if (!FindNextFileW(h, &fd)) break;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    return "[" + out + "]";
}

// Phone to PC
static std::string receiveFile(unsigned long long len, const std::wstring& name) {
    std::wstring dir = hadalDir(false), made;
    if (dir.empty() || !plainName(name)) return "-bad file name";
    HANDLE f = createUnique(dir, name, made);
    if (f == INVALID_HANDLE_VALUE) return "-couldn't create the file in Downloads\\Hadal";
    std::thread([=] {
        HANDLE p = openPipe();
        DWORD n;
        unsigned long long got = 0;
        bool ok = p && WriteFile(p, "file", 4, &n, nullptr);
        for (std::string m; ok && readMsg(p, 1 << 20, m) && !m.empty(); got += m.size())
            ok = got + m.size() <= len && WriteFile(f, m.data(), (DWORD)m.size(), &n, nullptr) && n == m.size();
        ok = ok && got == len && FlushFileBuffers(f);
        CloseHandle(f);
        std::string r = ok ? "+Saved to Downloads\\Hadal as " + narrow(made.c_str()) : "-the upload was interrupted";
        if (!ok) DeleteFileW((dir + L"\\" + made).c_str());
        if (p) { WriteFile(p, r.data(), (DWORD)r.size(), &n, nullptr); CloseHandle(p); }
        if (ok) toast(T_SCREEN, L"Received " + made + L" (" + wide(sizeText(len)) + L") from phone");
    }).detach();
    return "+";
}

// PC to phone
static std::string sendFile(const std::wstring& name) {
    std::wstring dir = hadalDir(true);
    if (dir.empty() || !plainName(name)) return "-bad file name";
    HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION fi;
    if (f == INVALID_HANDLE_VALUE) return "-no such file in Downloads\\Hadal\\To phone";
    if (!GetFileInformationByHandle(f, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) { CloseHandle(f); return "-not a regular file"; }
    unsigned long long size = ((unsigned long long)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    std::thread([=] {
        HANDLE p = openPipe();
        DWORD n;
        bool ok = p && WriteFile(p, "file", 4, &n, nullptr);
        std::vector<char> b(1 << 16);
        unsigned long long sent = 0;
        while (ok && sent < size && ReadFile(f, b.data(), (DWORD)b.size(), &n, nullptr) && n) { ok = WriteFile(p, b.data(), n, &n, nullptr); sent += n; }
        CloseHandle(f);
        if (p) CloseHandle(p);
        if (ok && sent == size) toast(T_SCREEN, L"Phone downloaded " + name);
    }).detach();
    return "+" + std::to_string(size);
}

static bool pickFile(std::wstring& path, const wchar_t* title, const wchar_t* filter) {
    wchar_t buf[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = g_wnd;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH * 2;
    o.lpstrTitle = title;
    o.lpstrFilter = filter;
    o.Flags = OFN_FILEMUSTEXIST | OFN_NODEREFERENCELINKS | OFN_EXPLORER;
    if (!GetOpenFileNameW(&o)) return false;
    path = buf;
    return true;
}

// App shortcuts

static const int MAX_APPS = 50;

static std::wstring appPath(int i) {
    wchar_t v[MAX_PATH * 2];
    DWORD n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Hadal\\Apps", std::to_wstring(i).c_str(), RRF_RT_REG_SZ, nullptr, v, &n) == ERROR_SUCCESS ? v : L"";
}

static std::wstring appName(const std::wstring& path) {
    std::wstring n = PathFindFileNameW(path.c_str());
    for (auto ext : {L".lnk", L".exe", L".url"})
        if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, ext) == 0) return n.substr(0, n.size() - 4);
    return n;
}

static std::string appsJson() {
    std::string out;
    for (int i = 0; i < MAX_APPS; i++) {
        std::wstring p = appPath(i);
        if (!p.empty()) out += (out.empty() ? "" : ",") + std::string("{\"id\":") + std::to_string(i) + ",\"name\":" + jstr(narrow(appName(p).c_str())) + "}";
    }
    return "[" + out + "]";
}

// Audio

// Undocumented
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID CLSID_PolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14}; // PKEY_Device_FriendlyName

using Microsoft::WRL::ComPtr;

static std::string procName(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "";
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string r = QueryFullProcessImageNameW(p, 0, path, &n) ? narrow(appName(path).c_str()) : "";
    CloseHandle(p);
    return r;
}

static bool forSessions(const std::function<void(DWORD, const std::string&, ISimpleAudioVolume*)>& f) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioSessionManager2> mgr;
    ComPtr<IAudioSessionEnumerator> list;
    int n = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)mgr.GetAddressOf())) ||
        FAILED(mgr->GetSessionEnumerator(&list)) || FAILED(list->GetCount(&n)))
        return false;
    for (int i = 0; i < n; i++) {
        ComPtr<IAudioSessionControl> c;
        ComPtr<IAudioSessionControl2> c2;
        ComPtr<ISimpleAudioVolume> v;
        AudioSessionState st;
        DWORD pid = 0;
        if (FAILED(list->GetSession(i, &c)) || FAILED(c.As(&c2)) || FAILED(c.As(&v)) || FAILED(c->GetState(&st)) || st == AudioSessionStateExpired) continue;
        c2->GetProcessId(&pid);
        std::string name = c2->IsSystemSoundsSession() == S_OK ? "System sounds" : procName(pid);
        if (!name.empty()) f(pid, name, v.Get());
    }
    return true;
}

static std::string audioJson() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> all;
    ComPtr<IMMDevice> def;
    LPWSTR defId = nullptr;
    UINT n = 0;
    std::string outs;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && SUCCEEDED(all->GetCount(&n))) {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def))) def->GetId(&defId);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            ComPtr<IPropertyStore> ps;
            LPWSTR id = nullptr;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (FAILED(all->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
            std::string name = SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR ? narrow(v.pwszVal) : "Output " + std::to_string(i + 1);
            PropVariantClear(&v);
            outs += (outs.empty() ? "" : ",") + std::string("{\"id\":") + jstr(narrow(id)) + ",\"name\":" + jstr(name) + ",\"default\":" + (defId && !wcscmp(id, defId) ? "true" : "false") + "}";
            CoTaskMemFree(id);
        }
        CoTaskMemFree(defId);
    }
    std::vector<std::tuple<DWORD, std::string, float, BOOL>> apps;
    forSessions([&](DWORD pid, const std::string& name, ISimpleAudioVolume* v) {
        float lvl = 0;
        BOOL mute = FALSE;
        v->GetMasterVolume(&lvl);
        v->GetMute(&mute);
        if (std::none_of(apps.begin(), apps.end(), [&](auto& a) { return std::get<0>(a) == pid; })) apps.push_back({pid, name, lvl, mute});
    });
    std::string a;
    for (auto& [pid, name, lvl, mute] : apps)
        a += (a.empty() ? "" : ",") + std::string("{\"pid\":") + std::to_string(pid) + ",\"name\":" + jstr(name) + ",\"vol\":" + std::to_string(std::lround(lvl * 100)) + ",\"muted\":" + (mute ? "true" : "false") + "}";
    return "{\"outputs\":[" + outs + "],\"apps\":[" + a + "]}";
}

static std::string setOutput(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> d;
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) || FAILED(en->GetDevice(id.c_str(), &d))) return "-no such output";
    if (FAILED(CoCreateInstance(CLSID_PolicyConfig, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)pc.GetAddressOf()))) return "-can't change the output on this PC";
    for (ERole r : {eConsole, eMultimedia, eCommunications})
        if (FAILED(pc->SetDefaultEndpoint(id.c_str(), r))) return "-couldn't switch the output";
    ComPtr<IPropertyStore> ps;
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name = SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR ? narrow(v.pwszVal) : "new output";
    PropVariantClear(&v);
    toast(T_ROUTINE, L"Sound output: " + wide(name) + L" (from phone)");
    return "+Sound now plays on " + name;
}

static std::string run(const std::string& req) {
    // Anti-cheat blocks this
    if (req.rfind("in ", 0) == 0)
        return input(req.substr(3)) ? "+" : "-was blocked by Windows. Usually anti-cheat, or a program running as administrator.";
    if (req.rfind("notify ", 0) == 0) {
        std::string rest = req.substr(7), cat = rest.substr(0, rest.find(' '));
        toast(cat == "screen" ? T_SCREEN : cat == "power" ? T_POWER : cat == "routine" ? T_ROUTINE : T_SECURITY,
              wide(rest.find(' ') == std::string::npos ? rest : rest.substr(rest.find(' ') + 1)));
        return "+";
    }
    if (req == "info") {
        std::string m;
        g_mediaAsked = GetTickCount64();
        { std::lock_guard<std::mutex> l(g_mediaMu); m = g_media; }
        return "+{\"fg\":" + jstr(fgApp()) + "," + volJson() + ",\"media\":" + m + ",\"display\":" + jstr(displayMode()) + "}";
    }
    if (req == "files") return "+" + listOutbox();
    if (req.rfind("recv ", 0) == 0) {
        size_t sp = req.find(' ', 5);
        if (sp == std::string::npos) return "-bad request";
        return receiveFile(std::stoull(req.substr(5, sp - 5)), wide(req.substr(sp + 1)));
    }
    if (req.rfind("send_file ", 0) == 0) return sendFile(wide(req.substr(10)));
    if (req == "apps") return "+" + appsJson();
    if (req.rfind("app ", 0) == 0) {
        std::wstring p = appPath(atoi(req.c_str() + 4));
        if (p.empty()) return "-that app shortcut was removed on the PC";
        if (!SendMessageW(g_wnd, WM_LAUNCH, 0, (LPARAM)&p)) return "-couldn't start " + narrow(appName(p).c_str());
        toast(T_ROUTINE, L"Started " + appName(p) + L" (from phone)");
        return "+Started " + narrow(appName(p).c_str());
    }
    if (req == "audio") return "+" + audioJson();
    if (req.rfind("audio_out ", 0) == 0) return setOutput(wide(req.substr(10)));
    if (req.rfind("audio_app ", 0) == 0 || req.rfind("audio_mute ", 0) == 0) {
        bool mute = req[6] == 'm';
        DWORD pid = strtoul(req.c_str() + (mute ? 11 : 10), nullptr, 10);
        int vol = mute ? -1 : atoi(req.c_str() + req.rfind(' ') + 1);
        std::string name;
        BOOL muted = FALSE;
        bool found = false;
        forSessions([&](DWORD p, const std::string& n, ISimpleAudioVolume* v) {
            if (p != pid) return;
            if (!found) { v->GetMute(&muted); muted = !muted; }
            found = true;
            name = n;
            if (mute) v->SetMute(muted, nullptr);
            else v->SetMasterVolume(vol / 100.f, nullptr);
        });
        if (!found) return "-that app isn't playing sound any more";
        return "+" + name + (mute ? (muted ? " muted" : " unmuted") : " at " + std::to_string(vol) + "%");
    }
    if (req.rfind("stream ", 0) == 0) {
        int h, fps, mbps, mon, ll;
        if (sscanf_s(req.c_str() + 7, "%d %d %d %d %d", &h, &fps, &mbps, &mon, &ll) != 5) return "-bad stream request";
        int gen = ++g_streamGen;
        std::thread([=] {
            static std::mutex one;
            std::lock_guard<std::mutex> l(one);
            if (g_streamGen != gen) return;
            HANDLE p = openPipe();
            DWORD n;
            if (p && WriteFile(p, "stream", 6, &n, nullptr)) streamRun(p, h, fps, mbps, mon, ll != 0, [gen] { return g_streamGen != gen; });
            if (p) CloseHandle(p);
        }).detach();
        return "+";
    }
    if (req.rfind("open ", 0) == 0) {
        std::string u = req.substr(5);
        if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return "-not a web link";
        if (!SendMessageW(g_wnd, WM_OPENURL, 0, (LPARAM)&u)) return "-couldn't open the link";
        std::string host = u.substr(u.find("://") + 3);
        toast(T_ROUTINE, L"Opened a link from phone: " + wide(host.substr(0, host.find('/'))));
        return "+Opened on PC";
    }
    if (req == "screenshot") {
        std::string jpg = screenshot();
        if (jpg.empty()) return "-screenshot failed";
        toast(T_SCREEN, L"Screenshot taken from phone");
        return "+" + jpg;
    }
    if (req == "clip_get") {
        std::string t;
        SendMessageW(g_wnd, WM_CLIPGET, 0, (LPARAM)&t);
        toast(T_SCREEN, L"PC clipboard read by phone");
        return "+" + t;
    }
    if (req.rfind("clip_set ", 0) == 0) {
        std::string t = req.substr(9);
        if (!SendMessageW(g_wnd, WM_CLIPSET, 0, (LPARAM)&t)) return "-couldn't set the PC clipboard";
        toast(T_SCREEN, L"Clipboard set from phone");
        return "+Copied to PC clipboard";
    }
    std::string msg;
    bool ok = true;
    if (req == "lock") { ok = LockWorkStation(); msg = "PC locked"; }
    else if (req == "monitors_off") { PostMessageW(g_wnd, WM_MONOFF, 0, 0); msg = "Monitors off"; }
    else if (req == "vol_up" || req == "vol_down" || req == "mute" || req.rfind("vol_set ", 0) == 0) { msg = volume(req); ok = !msg.empty(); }
    else if (req == "prev" || req == "playpause" || req == "next") {
        ok = media(req);
        msg = req == "prev" ? "Previous track" : req == "next" ? "Next track" : "Play/pause";
    }
    else if (req.rfind("display ", 0) == 0) {
        static const char* names[] = {"PC screen only", "Duplicate", "Extend", "Second screen only"};
        for (int i = 0; i < 4; i++) if (req.substr(8) == g_topo[i].name) msg = std::string("Display: ") + names[i];
        ok = setDisplay(req.substr(8));
    }
    else return "-unknown action";
    if (!ok) return "-" + req + " failed";
    toast(T_ROUTINE, wide(msg) + L" (from phone)");
    return "+" + msg;
}

static void agentLoop() {
    winrt::init_apartment();
    for (;;) {
        HANDLE h = openPipe();
        DWORD n;
        if (h && WriteFile(h, "agent", 5, &n, nullptr)) {
            setState(stateFrom(svc("state")));
            for (std::string req; !(req = readMsg(h, 1 << 20)).empty();) {
                std::string r = run(req);
                if (!WriteFile(h, r.data(), (DWORD)r.size(), &n, nullptr)) break;
            }
        }
        if (h) CloseHandle(h);
        setState(ST_OFF);
        Sleep(3000);
    }
}

// QR code (v5-L)

static const int QN = 37;
static bool g_qr[QN][QN];

static int gfMul(int x, int y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z;
}

static bool makeQr(const std::string& s) {
    if (s.size() > 106) return false;
    bool fn[QN][QN] = {};
    auto set = [&](int r, int c, bool v) { g_qr[r][c] = v; fn[r][c] = true; };
    for (int i = 0; i < QN; i++) { set(6, i, i % 2 == 0); set(i, 6, i % 2 == 0); } // Timing
    for (auto [cr, cc] : {std::pair{3, 3}, {3, QN - 4}, {QN - 4, 3}}) // Finders
        for (int dr = -4; dr <= 4; dr++)
            for (int dc = -4; dc <= 4; dc++) {
                int r = cr + dr, c = cc + dc, d = (std::max)(abs(dr), abs(dc));
                if (r >= 0 && r < QN && c >= 0 && c < QN) set(r, c, d != 2 && d != 4);
            }
    for (int dr = -2; dr <= 2; dr++) // Alignment
        for (int dc = -2; dc <= 2; dc++) set(30 + dr, 30 + dc, (std::max)(abs(dr), abs(dc)) != 1);
    int rem = 1 << 3 | 0; // Format
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int fmt = ((1 << 3) << 10 | rem) ^ 0x5412;
    auto bit = [](int v, int i) { return ((v >> i) & 1) != 0; };
    for (int i = 0; i <= 5; i++) set(i, 8, bit(fmt, i));
    set(7, 8, bit(fmt, 6));
    set(8, 8, bit(fmt, 7));
    set(8, 7, bit(fmt, 8));
    for (int i = 9; i < 15; i++) set(8, 14 - i, bit(fmt, i));
    for (int i = 0; i < 8; i++) set(8, QN - 1 - i, bit(fmt, i));
    for (int i = 8; i < 15; i++) set(QN - 15 + i, 8, bit(fmt, i));
    set(QN - 8, 8, true); // Dark module

    std::vector<bool> bits;
    auto put = [&](int v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((v >> i) & 1); };
    put(4, 4);
    put((int)s.size(), 8);
    for (unsigned char c : s) put(c, 8);
    put(0, (std::min)(4, 864 - (int)bits.size()));
    while (bits.size() % 8) bits.push_back(false);
    std::vector<int> cw;
    for (size_t i = 0; i < bits.size(); i += 8) { int v = 0; for (int j = 0; j < 8; j++) v = v << 1 | (int)bits[i + j]; cw.push_back(v); }
    for (int pad = 0xEC; cw.size() < 108; pad ^= 0xEC ^ 0x11) cw.push_back(pad);
    std::vector<int> div(26), ecc(26); // Reed-Solomon
    div[25] = 1;
    for (int i = 0, root = 1; i < 26; i++, root = gfMul(root, 2))
        for (int j = 0; j < 26; j++) { div[j] = gfMul(div[j], root); if (j + 1 < 26) div[j] ^= div[j + 1]; }
    for (int b : cw) {
        int f = b ^ ecc[0];
        ecc.erase(ecc.begin());
        ecc.push_back(0);
        for (int j = 0; j < 26; j++) ecc[j] ^= gfMul(div[j], f);
    }
    cw.insert(cw.end(), ecc.begin(), ecc.end());

    size_t i = 0; // Placement
    for (int right = QN - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < QN; vert++)
            for (int j = 0; j < 2; j++) {
                int c = right - j, r = ((right + 1) & 2) == 0 ? QN - 1 - vert : vert;
                if (fn[r][c]) continue;
                bool v = i < cw.size() * 8 && ((cw[i >> 3] >> (7 - (i & 7))) & 1);
                i++;
                g_qr[r][c] = v ^ ((r + c) % 2 == 0);
            }
    }
    return true;
}

// UI

static std::wstring g_qrCaption;

static void showQr(const std::string& st) {
    int paused, port;
    char tok[65] = "", ip[16] = "";
    if (st.size() < 2 || sscanf_s(st.c_str() + 1, "%d %d %64s %15s", &paused, &port, tok, 65u, ip, 16u) < 4) {
        MessageBoxW(g_wnd, st.empty() ? L"The Hadal service isn't running." : L"Tailscale isn't connected on this PC yet.", L"Hadal", MB_ICONWARNING);
        return;
    }
    makeQr("hadal://" + std::string(ip) + ":" + std::to_string(port) + "/" + tok);
    SecureZeroMemory(tok, sizeof tok);
    g_qrCaption = wide(ip) + L":" + std::to_wstring(port) + L"\nScan with the Hadal app, then close this window";
    if (!g_qrWnd) {
        int px = 8 * GetDpiForSystem() / 96;
        RECT rc{0, 0, (QN + 8) * px, (QN + 8) * px + 6 * px};
        AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        g_qrWnd = CreateWindowW(L"hadal", L"Hadal pairing", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    InvalidateRect(g_qrWnd, nullptr, TRUE);
    ShowWindow(g_qrWnd, SW_SHOW);
    SetForegroundWindow(g_qrWnd);
}

static void menu() {
    std::string st = svc("state");
    bool up = !st.empty(), paused = st.size() > 1 && st[1] == '1';
    UINT dis = up ? 0 : MF_GRAYED;
    HMENU m = CreatePopupMenu();
    if (!up) AppendMenuW(m, MF_GRAYED, 0, L"Service not running");
    AppendMenuW(m, dis, ID_QR, L"Show pairing QR");
    AppendMenuW(m, dis, ID_REGEN, L"Regenerate token");
    AppendMenuW(m, dis | (paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause remote control");
    std::string tj = up ? svc("timer") : "";
    char act[16] = "";
    int rem = 0;
    if (tj.size() > 2 && sscanf_s(tj.c_str() + 1, "{\"action\":\"%15[a-z]\",\"remaining\":%d", act, 16u, &rem) == 2)
        AppendMenuW(m, 0, ID_TIMER, (L"Cancel scheduled " + wide(act) + L" (in " + std::to_wstring((rem + 59) / 60) + L" min)").c_str());
    HMENU tm = CreatePopupMenu();
    static const wchar_t* cats[] = {L"Routine actions (lock, volume, media, display, links, apps)", L"Screen, clipboard and file access", L"Power actions and timers"};
    for (int i = 0; i < 3; i++) AppendMenuW(tm, toastOn(i) ? MF_CHECKED : 0, ID_TOAST0 + i, cats[i]);
    AppendMenuW(tm, MF_CHECKED | MF_GRAYED, 0, L"Security alerts (always on)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Notifications");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, 0, ID_SENDFILE, L"Send a file to the phone…");
    AppendMenuW(m, 0, ID_OPENDL, L"Open files from the phone");
    HMENU am = CreatePopupMenu();
    int apps = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        std::wstring p = appPath(i);
        if (!p.empty()) { AppendMenuW(am, 0, ID_APP0 + i, (L"Remove " + appName(p)).c_str()); apps++; }
    }
    if (apps) AppendMenuW(am, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(am, apps < MAX_APPS ? 0 : MF_GRAYED, ID_ADDAPP, L"Add an app…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)am, L"Apps the phone can start");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, 0, ID_LOG, L"Open log");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, 0, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_TOAST0 && cmd < ID_TOAST0 + 3) setToastOn(cmd - ID_TOAST0, !toastOn(cmd - ID_TOAST0));
    else if (cmd >= ID_APP0 && cmd < ID_APP0 + MAX_APPS) {
        std::wstring p = appPath(cmd - ID_APP0);
        if (MessageBoxW(g_wnd, (L"Remove " + appName(p) + L" from the apps the phone can start?").c_str(), L"Hadal", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Hadal\\Apps", std::to_wstring(cmd - ID_APP0).c_str());
    } else if (cmd == ID_ADDAPP) {
        std::wstring p;
        int slot = 0;
        while (slot < MAX_APPS && !appPath(slot).empty()) slot++;
        if (slot < MAX_APPS && pickFile(p, L"Pick an app the phone may start", L"Programs and shortcuts\0*.exe;*.lnk;*.url\0All files\0*.*\0")) {
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Hadal\\Apps", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(k, std::to_wstring(slot).c_str(), 0, REG_SZ, (const BYTE*)p.c_str(), (DWORD)((p.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(k);
                toast(T_ROUTINE, appName(p) + L" can now be started from the phone");
            }
        }
    } else if (cmd == ID_SENDFILE) {
        std::wstring src, made, dir = hadalDir(true);
        if (!dir.empty() && pickFile(src, L"Send a file to the phone", L"All files\0*.*\0")) {
            std::wstring name = PathFindFileNameW(src.c_str());
            HANDLE f = plainName(name) ? createUnique(dir, name, made) : INVALID_HANDLE_VALUE;
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            if (f != INVALID_HANDLE_VALUE && CopyFileW(src.c_str(), (dir + L"\\" + made).c_str(), FALSE))
                toast(T_SCREEN, made + L" is ready: open Files on the phone to download it");
            else MessageBoxW(g_wnd, L"Couldn't copy that file to Downloads\\Hadal\\To phone.", L"Hadal", MB_ICONWARNING);
        }
    } else if (cmd == ID_OPENDL) {
        std::wstring d = hadalDir(false);
        if (!d.empty()) ShellExecuteW(nullptr, L"open", d.substr(4).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == ID_TIMER && svc("timer_cancel") == "+") toast(T_POWER, L"Power timer cancelled");
    else if (cmd == ID_QR) showQr(st);
    else if (cmd == ID_REGEN) {
        if (MessageBoxW(g_wnd, L"Generate a new token? The phone will need to scan the new QR code.", L"Hadal", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            showQr(svc("regen"));
    } else if (cmd == ID_PAUSE) {
        setState(stateFrom(svc(paused ? "resume" : "pause")));
    } else if (cmd == ID_LOG) {
        wchar_t log[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramData%\\Hadal\\hadal.log", log, MAX_PATH);
        ShellExecuteW(nullptr, L"open", L"notepad.exe", log, nullptr, SW_SHOWNORMAL);
    } else if (cmd == ID_EXIT) {
        icon(NIM_DELETE);
        ExitProcess(0);
    }
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_taskbarMsg) { icon(NIM_ADD); return 0; }
    switch (msg) {
    case WM_TRAY:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP) menu();
        return 0;
    case WM_MONOFF:
        return DefWindowProcW(h, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
    case WM_OPENURL:
        return (INT_PTR)ShellExecuteW(nullptr, L"open", wide(*(const std::string*)l).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    case WM_LAUNCH:
        return (INT_PTR)ShellExecuteW(nullptr, L"open", ((const std::wstring*)l)->c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    case WM_CLIPGET: {
        auto out = (std::string*)l;
        if (OpenClipboard(h)) {
            if (HANDLE d = GetClipboardData(CF_UNICODETEXT))
                if (auto p = (const wchar_t*)GlobalLock(d)) { *out = narrow(p); GlobalUnlock(d); }
            CloseClipboard();
        }
        if (out->size() > (1u << 20)) out->resize(1u << 20);
        return 0;
    }
    case WM_CLIPSET: {
        std::wstring t = wide(*(const std::string*)l);
        BOOL ok = FALSE;
        if (OpenClipboard(h)) {
            EmptyClipboard();
            size_t bytes = (t.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                memcpy(GlobalLock(g), t.c_str(), bytes);
                GlobalUnlock(g);
                ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
                if (!ok) GlobalFree(g);
            }
            CloseClipboard();
        }
        return ok;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        int px = rc.right / (QN + 8);
        for (int r = 0; r < QN; r++)
            for (int c = 0; c < QN; c++)
                if (g_qr[r][c]) { RECT m{(c + 4) * px, (r + 4) * px, (c + 5) * px, (r + 5) * px}; FillRect(dc, &m, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        static HFONT font = CreateFontW(-MulDiv(10, GetDpiForWindow(h), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        RECT t{0, (QN + 7) * px, rc.right, rc.bottom};
        DrawTextW(dc, g_qrCaption.c_str(), -1, &t, DT_CENTER);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (h == g_qrWnd) { DestroyWindow(h); g_qrWnd = nullptr; memset(g_qr, 0, sizeof g_qr); }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static ULONGLONG g_startTick;

// Crash log + relaunch
static LONG WINAPI crashLog(EXCEPTION_POINTERS* ep) {
    if (GetTickCount64() - g_startTick > 30000) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 40];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf_s(cmd, L"\"%s\" --after %lu", exe, GetCurrentProcessId());
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Hadal", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\crash.txt", dir);
    FILE* f = _wfopen(path, L"a");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d exception 0x%08lx at %p, thread %lu\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); i++) {
        DWORD64 a = sf.AddrPC.Offset, disp = 0;
        HMODULE m = nullptr;
        wchar_t mod[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m)) GetModuleFileNameW(m, mod, MAX_PATH);
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        IMAGEHLP_LINE64 line{sizeof line};
        DWORD ldisp = 0;
        fprintf(f, "  %ls+0x%llx %s", PathFindFileNameW(mod), a - (DWORD64)m, SymFromAddr(proc, a, &disp, si) ? si->Name : "");
        if (SymGetLineFromAddr64(proc, a, &ldisp, &line)) fprintf(f, " (%s:%lu)", PathFindFileNameA(line.FileName), line.LineNumber);
        fputc('\n', f);
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdLine, int) {
    unsigned long crashed = 0;
    if (swscanf_s(cmdLine, L"--after %lu", &crashed) == 1)
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, crashed)) { WaitForSingleObject(old, 15000); CloseHandle(old); }
    CreateMutexW(nullptr, TRUE, L"Local\\hadal-tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_startTick = GetTickCount64();
    SetUnhandledExceptionFilter(crashLog);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR gt;
    Gdiplus::GdiplusStartup(&gt, &gi, nullptr);

    HANDLE tok;
    BYTE buf[256];
    DWORD n;
    wchar_t* s;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n) && ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) {
            g_mySid = s;
            LocalFree(s);
        }
        CloseHandle(tok);
    }

    g_icons[ST_OFF] = trayIcon(4);
    g_icons[ST_ON] = trayIcon(2);
    g_icons[ST_PAUSED] = trayIcon(3);
    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"hadal";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"hadal", L"hadal", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    icon(NIM_ADD);
    std::thread(agentLoop).detach();
    std::thread(mediaLoop).detach();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
