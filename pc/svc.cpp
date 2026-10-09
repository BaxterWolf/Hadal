// Hadal service
// Usage: hadal-svc.exe <user SID> [--console] | --selftest
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <sddl.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const int PORT = 47810;
static const wchar_t* PIPE = L"\\\\.\\pipe\\hadal";

static std::wstring g_dir;
static std::wstring g_pipeSddl;
static std::mutex g_tokMu;
static std::string g_token;
static std::mutex g_agentMu;
static HANDLE g_agent = nullptr;
static std::atomic<uint32_t> g_ip{0};
static std::atomic<bool> g_paused{false};
static HANDLE g_stopEv;
static SERVICE_STATUS_HANDLE g_ss;

static void logf(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    FILE* f = _wfopen((g_dir + L"hadal.log").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    bool full = _ftelli64(f) > (1 << 20);
    fclose(f);
    if (full) MoveFileExW((g_dir + L"hadal.log").c_str(), (g_dir + L"hadal.log.old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

static std::string utf8(const wchar_t* w) {
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

// Token

static std::string newToken() {
    unsigned char b[32];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        logf("BCryptGenRandom failed");
        ExitProcess(1);
    }
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) { s += hx[c >> 4]; s += hx[c & 15]; }
    SecureZeroMemory(b, sizeof b);
    return s;
}

static void saveToken(const std::string& t) {
    FILE* f = _wfopen((g_dir + L"token").c_str(), L"wb");
    if (!f) { logf("cannot write token file"); return; }
    fwrite(t.data(), 1, t.size(), f);
    fclose(f);
}

// Paired phone IP
static std::string g_peer;

static void savePeer(const std::string& ip) {
    std::wstring f = g_dir + L"peer";
    if (ip.empty()) { DeleteFileW(f.c_str()); return; }
    if (FILE* p = _wfopen(f.c_str(), L"wb")) { fwrite(ip.data(), 1, ip.size(), p); fclose(p); }
}

static void loadPeer() {
    char b[16] = {};
    if (FILE* f = _wfopen((g_dir + L"peer").c_str(), L"rb")) { fread(b, 1, 15, f); fclose(f); }
    in_addr a;
    g_peer = inet_pton(AF_INET, b, &a) == 1 && (ntohl(a.s_addr) & 0xFFC00000) == 0x64400000 ? b : "";
}

static void loadToken() {
    char b[65] = {};
    if (FILE* f = _wfopen((g_dir + L"token").c_str(), L"rb")) { fread(b, 1, 64, f); fclose(f); }
    g_token = b;
    if (g_token.size() != 64 || g_token.find_first_not_of("0123456789abcdef") != std::string::npos) {
        g_token = newToken();
        saveToken(g_token);
        logf("generated new token");
    }
}

// Constant time compare
static bool ctEq(const std::string& a, const std::string& secret) {
    unsigned char d = a.size() != secret.size();
    for (size_t i = 0; i < secret.size(); i++) d |= (unsigned char)((i < a.size() ? a[i] : 0) ^ secret[i]);
    return d == 0;
}

// Pipe

static DWORD pio(HANDLE h, bool wr, void* b, DWORD n, DWORD* got, DWORD ms) {
    OVERLAPPED o{};
    o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    *got = 0;
    BOOL ok = wr ? WriteFile(h, b, n, nullptr, &o) : ReadFile(h, b, n, nullptr, &o);
    DWORD e = ok ? 0 : GetLastError();
    if (e == ERROR_IO_PENDING) {
        if (WaitForSingleObject(o.hEvent, ms) != WAIT_OBJECT_0) CancelIoEx(h, &o);
        e = 0;
    }
    if (e == 0 || e == ERROR_MORE_DATA) e = GetOverlappedResult(h, &o, got, TRUE) ? 0 : GetLastError();
    CloseHandle(o.hEvent);
    return e;
}

static bool pread(HANDLE h, std::string& out, DWORD ms, size_t cap) {
    out.clear();
    std::vector<char> buf(65536);
    for (;;) {
        DWORD got, e = pio(h, false, buf.data(), (DWORD)buf.size(), &got, ms);
        out.append(buf.data(), got);
        if (e == 0) return true;
        if (e != ERROR_MORE_DATA || out.size() > cap) return false;
    }
}

static bool pwrite(HANDLE h, const std::string& s, DWORD ms) {
    DWORD got;
    return pio(h, true, (void*)s.data(), (DWORD)s.size(), &got, ms) == 0 && got == s.size();
}

// Ask the tray
static std::string agentCall(const std::string& req, DWORD ms) {
    std::lock_guard<std::mutex> l(g_agentMu);
    std::string r;
    if (g_agent && pwrite(g_agent, req, ms) && pread(g_agent, r, ms, 64u << 20) && !r.empty()) return r;
    if (g_agent) { CloseHandle(g_agent); g_agent = nullptr; logf("tray disconnected"); }
    return "";
}

// Feed pipes from the tray
static std::mutex g_streamMu;
static std::condition_variable g_streamCv;
static HANDLE g_streamPipe = nullptr;
static HANDLE g_filePipe = nullptr;

static HANDLE takeFeed(HANDLE& slot) {
    std::unique_lock<std::mutex> l(g_streamMu);
    g_streamCv.wait_for(l, std::chrono::seconds(5), [&] { return slot != nullptr; });
    HANDLE p = nullptr;
    std::swap(p, slot);
    return p;
}

static void dropFeed(HANDLE& slot) {
    std::lock_guard<std::mutex> l(g_streamMu);
    if (slot) { CloseHandle(slot); slot = nullptr; }
}

static void setPaused(bool p) {
    g_paused = p;
    std::wstring f = g_dir + L"paused";
    if (!p) DeleteFileW(f.c_str());
    else if (FILE* x = _wfopen(f.c_str(), L"w")) fclose(x);
}

static bool validUrl(const std::string& u) {
    if (u.size() < 9 || u.size() > 2048 || (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0)) return false;
    for (unsigned char ch : u)
        if (ch <= 0x20 || ch >= 0x7f || strchr("\"<>\\^`{|}", ch)) return false;
    return true;
}

// Plain file names only
static bool validName(const std::string& n) {
    if (n.empty() || n.size() > 200 || n.front() == ' ' || n.front() == '.' || n.back() == ' ' || n.back() == '.') return false;
    for (unsigned char ch : n)
        if (ch < 0x20 || ch == 0x7f || strchr("\\/:*?\"<>|", ch)) return false;
    std::string stem = n.substr(0, n.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& ch : stem) ch = (char)toupper((unsigned char)ch);
    static const char* dev[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (auto d : dev) if (stem == d) return false;
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '0' && stem[3] <= '9') return false;
    return true;
}

static bool urlDecode(const std::string& s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') { out += s[i]; continue; }
        if (i + 2 >= s.size() || !isxdigit((unsigned char)s[i + 1]) || !isxdigit((unsigned char)s[i + 2])) return false;
        out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
        i += 2;
    }
    return true;
}

static bool uintArg(const std::string& s, unsigned long lim, unsigned long& v) {
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    v = std::stoul(s);
    return v <= lim;
}

// Live sessions
static std::atomic<SOCKET> g_inputSock{INVALID_SOCKET};
static std::atomic<SOCKET> g_streamSock{INVALID_SOCKET};

static std::string timerJson();
static void setTimer(const std::string& act, int minutes);

static std::string command(const std::string& m) {
    if (m == "timer") return "+" + timerJson();
    if (m == "timer_cancel") { setTimer("", 0); logf("power timer cancelled from tray"); return "+"; }
    if (m == "pause") {
        setPaused(true);
        bool hadTimer = timerJson() != "null";
        setTimer("", 0);
        logf(hadTimer ? "paused from tray; power timer cancelled" : "paused from tray");
    }
    else if (m == "resume") { setPaused(false); logf("resumed from tray"); }
    else if (m == "regen") {
        std::lock_guard<std::mutex> l(g_tokMu);
        g_token = newToken();
        saveToken(g_token);
        g_peer.clear();
        savePeer("");
        for (SOCKET s : {g_inputSock.load(), g_streamSock.load()}) if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
        logf("token regenerated; paired phone cleared");
    }
    else if (m != "state") return "-unknown command";
    std::string tok;
    { std::lock_guard<std::mutex> l(g_tokMu); tok = g_token; }
    char ip[16] = "";
    in_addr a;
    a.s_addr = g_ip;
    if (a.s_addr) inet_ntop(AF_INET, &a, ip, sizeof ip);
    return std::string("+") + (g_paused ? "1" : "0") + " " + std::to_string(PORT) + " " + tok + " " + ip;
}

static HANDLE makePipe(bool first) {
    SECURITY_ATTRIBUTES sa{sizeof sa};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(g_pipeSddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
        return INVALID_HANDLE_VALUE;
    HANDLE p = CreateNamedPipeW(PIPE, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    return p;
}

static void pipeLoop() {
    HANDLE p = makePipe(true); // No squatters
    if (p == INVALID_HANDLE_VALUE) { logf("cannot create pipe (%lu); is another process using it?", GetLastError()); return; }
    for (;;) {
        OVERLAPPED o{};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD e = ConnectNamedPipe(p, &o) ? 0 : GetLastError(), got;
        if (e == ERROR_IO_PENDING) e = GetOverlappedResult(p, &o, &got, TRUE) ? 0 : GetLastError();
        CloseHandle(o.hEvent);
        HANDLE next;
        while ((next = makePipe(false)) == INVALID_HANDLE_VALUE) { logf("pipe create failed (%lu)", GetLastError()); Sleep(5000); }
        std::string msg;
        if ((e == 0 || e == ERROR_PIPE_CONNECTED) && pread(p, msg, 5000, 4096)) {
            if (msg == "agent") {
                std::lock_guard<std::mutex> l(g_agentMu);
                if (g_agent) CloseHandle(g_agent);
                g_agent = p;
                p = nullptr;
                logf("tray connected");
            } else if (msg == "stream" || msg == "file") {
                std::lock_guard<std::mutex> l(g_streamMu);
                HANDLE& slot = msg == "stream" ? g_streamPipe : g_filePipe;
                if (slot) CloseHandle(slot);
                slot = p;
                p = nullptr;
                g_streamCv.notify_all();
            } else if (pwrite(p, command(msg), 5000)) {
                pread(p, msg, 5000, 16);
            }
        }
        if (p) { DisconnectNamedPipe(p); CloseHandle(p); }
        p = next;
    }
}

// System state

static uint32_t tailscaleIp() {
    ULONG sz = 32768;
    std::vector<char> buf;
    DWORD r;
    do {
        buf.resize(sz);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    } while (r == ERROR_BUFFER_OVERFLOW);
    if (r != NO_ERROR) return 0;
    for (auto a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || (!wcsstr(a->Description, L"Tailscale") && !wcsstr(a->FriendlyName, L"Tailscale")))
            continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            auto s = (sockaddr_in*)u->Address.lpSockaddr;
            if ((ntohl(s->sin_addr.s_addr) & 0xFFC00000) == 0x64400000) return s->sin_addr.s_addr;
        }
    }
    return 0;
}

static bool sessionInfo(std::string& user, bool& locked) {
    user.clear();
    locked = false;
    DWORD sid = WTSGetActiveConsoleSessionId(), n;
    WTSINFOEXW* ix = nullptr;
    if (sid == 0xFFFFFFFF) return true;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sid, WTSSessionInfoEx, (LPWSTR*)&ix, &n)) {
        static bool once = (logf("WTSQuerySessionInformation failed: %lu", GetLastError()), true);
        (void)once;
        return false;
    }
    if (ix->Level == 1) {
        user = utf8(ix->Data.WTSInfoExLevel1.UserName);
        locked = ix->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    }
    WTSFreeMemory(ix);
    return true;
}

// Perf sampler

static std::mutex g_perfMu;
static std::string g_perf = "{}";
static std::atomic<int> g_cpu{0};

struct Gpu { std::string name, luid; uint64_t vram; };

static std::vector<Gpu> listGpus() {
    std::vector<Gpu> out;
    IDXGIFactory1* f;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return out;
    IDXGIAdapter1* a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        a->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        char l[40];
        sprintf_s(l, "luid_0x%08lx_0x%08lx", (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        out.push_back({utf8(d.Description), l, d.DedicatedVideoMemory});
    }
    f->Release();
    return out;
}

static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// NVML (NVIDIA only)
struct Nvml {
    using Dev = void*;
    int (*init)() = nullptr;
    int (*count)(unsigned*) = nullptr;
    int (*handle)(unsigned, Dev*) = nullptr;
    int (*name)(Dev, char*, unsigned) = nullptr;
    int (*temp)(Dev, int, unsigned*) = nullptr;
    int (*fan)(Dev, unsigned*) = nullptr;
    int (*power)(Dev, unsigned*) = nullptr;
    bool ok = false;
    Nvml() {
        HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return;
        auto get = [&](auto& f, const char* n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, n)); return f != nullptr; };
        ok = get(init, "nvmlInit_v2") && get(count, "nvmlDeviceGetCount_v2") && get(handle, "nvmlDeviceGetHandleByIndex_v2") &&
             get(name, "nvmlDeviceGetName") && get(temp, "nvmlDeviceGetTemperature") && get(fan, "nvmlDeviceGetFanSpeed") &&
             get(power, "nvmlDeviceGetPowerUsage") && init() == 0;
    }
    std::string json(const std::string& gpuName) {
        long long v[3] = {-1, -1, -1};
        if (!ok) return "";
        if (!read(*this, gpuName.c_str(), v)) {
            ok = false;
            logf("NVIDIA sensors stopped responding (driver update?); GPU temperature off until restart");
            return "";
        }
        std::string s;
        if (v[0] >= 0) s += ",\"tempC\":" + std::to_string(v[0]);
        if (v[1] >= 0) s += ",\"fanPct\":" + std::to_string(v[1]);
        if (v[2] >= 0) s += ",\"powerW\":" + std::to_string(v[2] / 1000);
        return s;
    }
    // A driver update can pull nvml.dll out from under us
    static bool read(const Nvml& n, const char* gpuName, long long v[3]) {
        __try {
            unsigned c = 0, x;
            if (n.count(&c) != 0) return true;
            for (unsigned i = 0; i < c; i++) {
                Dev d;
                char nm[96] = "";
                if (n.handle(i, &d) != 0 || n.name(d, nm, sizeof nm) != 0 || strcmp(gpuName, nm) != 0) continue;
                if (n.temp(d, 0 /* NVML_TEMPERATURE_GPU */, &x) == 0) v[0] = x;
                if (n.fan(d, &x) == 0) v[1] = x;
                if (n.power(d, &x) == 0) v[2] = x;
                break;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
};

static std::string batteryJson() {
    SYSTEM_POWER_STATUS p;
    if (!GetSystemPowerStatus(&p) || (p.BatteryFlag & 128) || p.BatteryLifePercent > 100) return "null";
    return "{\"pct\":" + std::to_string(p.BatteryLifePercent) + ",\"plugged\":" + (p.ACLineStatus == 1 ? "true" : "false") +
           ",\"charging\":" + ((p.BatteryFlag & 8) ? "true" : "false") + ",\"secsLeft\":" + std::to_string(p.BatteryLifeTime == (DWORD)-1 ? -1 : (long)p.BatteryLifeTime) + "}";
}

// Bluetooth batteries
static std::string deviceBatteries() {
    static const DEVPROPKEY battery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return "[]";
    std::vector<std::string> seen;
    std::string out;
    SP_DEVINFO_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        DEVPROPTYPE t;
        BYTE pct = 0;
        wchar_t nm[128] = L"";
        if (!SetupDiGetDevicePropertyW(set, &d, &battery, &t, &pct, 1, nullptr, 0) || t != DEVPROP_TYPE_BYTE || pct > 100) continue;
        if (!SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_FriendlyName, &t, (BYTE*)nm, sizeof nm - 2, nullptr, 0) || t != DEVPROP_TYPE_STRING) continue;
        std::string n = utf8(nm);
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        out += (out.empty() ? "" : ",") + std::string("{\"name\":") + jstr(n) + ",\"pct\":" + std::to_string(pct) + "}";
    }
    SetupDiDestroyDeviceInfoList(set);
    return "[" + out + "]";
}

static std::vector<std::pair<std::string, double>> pdhArray(PDH_HCOUNTER c) {
    DWORD sz = 0, n = 0;
    std::vector<std::pair<std::string, double>> out;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &sz, &n, nullptr) != PDH_MORE_DATA) return out;
    std::vector<BYTE> b(sz);
    auto items = (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data();
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &sz, &n, items) != ERROR_SUCCESS) return out;
    for (DWORD i = 0; i < n; i++)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            out.push_back({lower(utf8(items[i].szName)), items[i].FmtValue.doubleValue});
    return out;
}

static void perfLoop() {
    PDH_HQUERY q;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) { logf("PdhOpenQuery failed"); return; }
    const wchar_t* paths[] = {L"\\Processor(*)\\% Processor Time", L"\\GPU Engine(*)\\Utilization Percentage",
                              L"\\GPU Adapter Memory(*)\\Dedicated Usage", L"\\PhysicalDisk(*)\\% Idle Time",
                              L"\\Network Interface(*)\\Bytes Received/sec", L"\\Network Interface(*)\\Bytes Sent/sec"};
    PDH_HCOUNTER c[6] = {};
    for (int i = 0; i < 6; i++)
        if (PdhAddEnglishCounterW(q, paths[i], 0, &c[i]) != ERROR_SUCCESS) logf("perf counter unavailable: %s", utf8(paths[i]).c_str());
    std::string cpuName;
    wchar_t nm[128];
    DWORD nb = sizeof nm;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, nm, &nb) == ERROR_SUCCESS)
        cpuName = utf8(nm);
    std::vector<Gpu> gpus = listGpus();
    Nvml nvml;
    std::string devices = "[]";
    int tick = 0;
    std::vector<int> cpuHist;
    std::vector<std::pair<std::string, std::vector<int>>> gpuHist;
    auto push = [](std::vector<int>& h, int v) { h.push_back(v); if (h.size() > 60) h.erase(h.begin()); };
    auto arr = [](const std::vector<int>& h) {
        std::string s;
        for (int v : h) s += (s.empty() ? "" : ",") + std::to_string(v);
        return "[" + s + "]";
    };
    PdhCollectQueryData(q);
    while (WaitForSingleObject(g_stopEv, 2000) == WAIT_TIMEOUT) {
        PdhCollectQueryData(q);
        std::string cores;
        double total = 0;
        auto cpu = pdhArray(c[0]);
        std::sort(cpu.begin(), cpu.end(), [](auto& a, auto& b) { return atoi(a.first.c_str()) < atoi(b.first.c_str()); });
        for (auto& [n, v] : cpu) {
            if (n == "_total") total = v;
            else cores += (cores.empty() ? "" : ",") + std::to_string(std::lround(std::min(v, 100.0)));
        }
        g_cpu = (int)std::lround(std::min(total, 100.0));
        push(cpuHist, g_cpu);

        // Task Manager style GPU %
        std::vector<std::pair<std::string, double>> engSum;
        for (auto& [n, v] : pdhArray(c[1])) {
            size_t l = n.find("luid_"), e = n.find("engtype_");
            if (l == std::string::npos || e == std::string::npos) continue;
            std::string k = n.substr(l, 26) + "|" + n.substr(e + 8);
            auto it = std::find_if(engSum.begin(), engSum.end(), [&](auto& p) { return p.first == k; });
            if (it == engSum.end()) engSum.push_back({k, v}); else it->second += v;
        }
        // Merge hybrid dGPU LUIDs
        struct Row { std::string name; double util, used; uint64_t vram; };
        std::vector<Row> rows;
        auto mem = pdhArray(c[2]);
        for (auto& g : gpus) {
            double util = 0, used = 0;
            for (auto& [k, v] : engSum) if (k.rfind(g.luid, 0) == 0) util = std::max(util, v);
            for (auto& [n, v] : mem) if (n.find(g.luid) != std::string::npos) used += v;
            auto it = std::find_if(rows.begin(), rows.end(), [&](auto& r) { return r.name == g.name; });
            if (it == rows.end()) rows.push_back({g.name, util, used, g.vram});
            else { it->util = std::max(it->util, util); it->used = std::max(it->used, used); }
        }
        std::string gpuJson;
        for (auto& r : rows) {
            int util = (int)std::lround(std::min(r.util, 100.0));
            auto h = std::find_if(gpuHist.begin(), gpuHist.end(), [&](auto& p) { return p.first == r.name; });
            if (h == gpuHist.end()) h = gpuHist.insert(gpuHist.end(), {r.name, {}});
            push(h->second, util);
            gpuJson += (gpuJson.empty() ? "" : ",") + std::string("{\"name\":") + jstr(r.name) + ",\"util\":" + std::to_string(util) +
                       ",\"hist\":" + arr(h->second) + nvml.json(r.name) +
                       ",\"vramUsedMb\":" + std::to_string((uint64_t)r.used >> 20) + ",\"vramTotalMb\":" + std::to_string(r.vram >> 20) + "}";
        }

        auto idle = pdhArray(c[3]);
        std::string disks;
        DWORD drives = GetLogicalDrives();
        for (char d = 'A'; d <= 'Z'; d++) {
            wchar_t root[] = {(wchar_t)d, L':', L'\\', 0};
            ULARGE_INTEGER freeB, size;
            if (!(drives & (1u << (d - 'A'))) || GetDriveTypeW(root) != DRIVE_FIXED || !GetDiskFreeSpaceExW(root, nullptr, &size, &freeB)) continue;
            int active = -1;
            std::string letter = std::string(1, (char)tolower(d)) + ":";
            for (auto& [n, v] : idle) if (n != "_total" && n.find(letter) != std::string::npos) active = (int)std::lround(100 - std::min(v, 100.0));
            disks += (disks.empty() ? "" : ",") + std::string("{\"name\":\"") + d + ":\",\"usedGb\":" + std::to_string((size.QuadPart - freeB.QuadPart) >> 30) +
                     ",\"totalGb\":" + std::to_string(size.QuadPart >> 30) + ",\"active\":" + std::to_string(active) + "}";
        }

        double rx = 0, tx = 0;
        for (auto& [n, v] : pdhArray(c[4])) rx += v;
        for (auto& [n, v] : pdhArray(c[5])) tx += v;
        MEMORYSTATUSEX m{sizeof m};
        GlobalMemoryStatusEx(&m);
        std::string j = "{\"cpu\":{\"name\":" + jstr(cpuName) + ",\"total\":" + std::to_string(g_cpu) + ",\"hist\":" + arr(cpuHist) + ",\"cores\":[" + cores + "]}" +
                        ",\"ram\":{\"usedMb\":" + std::to_string((m.ullTotalPhys - m.ullAvailPhys) >> 20) + ",\"totalMb\":" + std::to_string(m.ullTotalPhys >> 20) + "}" +
                        ",\"gpus\":[" + gpuJson + "],\"disks\":[" + disks + "]" +
                        ",\"net\":{\"rxBps\":" + std::to_string((uint64_t)rx) + ",\"txBps\":" + std::to_string((uint64_t)tx) + "}" +
                        ",\"battery\":" + batteryJson() + ",\"devices\":" + (tick++ % 30 == 0 ? devices = deviceBatteries() : devices) + "}";
        std::lock_guard<std::mutex> l(g_perfMu);
        g_perf = j;
    }
}

// Power + timer

static std::string power(const std::string& what) {
#ifdef HADAL_FUZZ
    return ""; // Fuzz build
#endif
    if (what == "sleep") return SetSuspendState(FALSE, FALSE, FALSE) ? "" : "sleep failed (error " + std::to_string(GetLastError()) + ")";
    if (InitiateSystemShutdownExW(nullptr, (LPWSTR)L"Hadal: requested from phone", 5, FALSE, what == "restart",
                                  SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
        return "";
    DWORD e = GetLastError();
    logf("InitiateSystemShutdownEx failed: %lu", e);
    return what + " failed (error " + std::to_string(e) + ")";
}

static std::mutex g_timerMu;
static std::string g_timerAct;
static ULONGLONG g_timerAt = 0;
static bool g_timerWarned = false;

static std::string timerJson() {
    std::lock_guard<std::mutex> l(g_timerMu);
    if (!g_timerAt) return "null";
    ULONGLONG now = GetTickCount64();
    return "{\"action\":\"" + g_timerAct + "\",\"remaining\":" + std::to_string(g_timerAt > now ? (g_timerAt - now) / 1000 : 0) + "}";
}

static void setTimer(const std::string& act, int minutes) {
    std::lock_guard<std::mutex> l(g_timerMu);
    g_timerAct = act;
    g_timerAt = minutes ? GetTickCount64() + minutes * 60000ull : 0;
    g_timerWarned = false;
}

static void tickTimer() {
    std::string act, warn;
    {
        std::lock_guard<std::mutex> l(g_timerMu);
        ULONGLONG now = GetTickCount64();
        if (g_timerAt && now >= g_timerAt) { act = g_timerAct; g_timerAt = 0; }
        else if (g_timerAt && !g_timerWarned && g_timerAt - now <= 60000) { g_timerWarned = true; warn = g_timerAct; }
    }
    if (!warn.empty()) agentCall("notify power Scheduled " + warn + " in 1 minute. Cancel it from the tray menu or the phone.", 3000);
    if (!act.empty()) { logf("timer: %s", act.c_str()); power(act); }
}

static std::string statusJson() {
    MEMORYSTATUSEX m{sizeof m};
    GlobalMemoryStatusEx(&m);
    std::string user, session = "null";
    bool locked, known = sessionInfo(user, locked), tray;
    { std::lock_guard<std::mutex> l(g_agentMu); tray = g_agent != nullptr; }
    if (known && !user.empty()) {
        std::string r = agentCall("info", 3000);
        tray = !r.empty();
        if (r.size() > 2 && r[0] == '+' && r[1] == '{' && r.back() == '}') session = r.substr(1);
    }
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD hn = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(host, &hn);
    return "{\"host\":" + jstr(utf8(host)) +
           ",\"uptime\":" + std::to_string(GetTickCount64() / 1000) +
           ",\"cpu\":" + std::to_string(g_cpu) +
           ",\"timer\":" + timerJson() +
           ",\"ramUsedMb\":" + std::to_string((m.ullTotalPhys - m.ullAvailPhys) >> 20) +
           ",\"ramTotalMb\":" + std::to_string(m.ullTotalPhys >> 20) +
           ",\"user\":" + (known ? jstr(user) : "null") +
           ",\"locked\":" + (locked ? "true" : "false") +
           ",\"tray\":" + (tray ? "true" : "false") +
           ",\"paused\":" + (g_paused ? "true" : "false") +
           ",\"session\":" + session + "}";
}

static bool enableShutdownPriv() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &t)) return false;
    TOKEN_PRIVILEGES p{1};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &p.Privileges[0].Luid);
    p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(t, FALSE, &p, 0, nullptr, nullptr);
    DWORD e = GetLastError();
    CloseHandle(t);
    return e == ERROR_SUCCESS;
}

// HTTP

struct Req { std::string method, path, token; size_t length = 0; };

static bool parseReq(const std::string& raw, Req& r) {
    size_t eol = raw.find("\r\n"), a, b;
    if (eol == std::string::npos) return false;
    std::string line = raw.substr(0, eol);
    if ((a = line.find(' ')) == std::string::npos || (b = line.find(' ', a + 1)) == std::string::npos) return false;
    if (line.compare(b + 1, 5, "HTTP/") != 0) return false;
    r.method = line.substr(0, a);
    r.path = line.substr(a + 1, b - a - 1);
    if (r.path.empty() || r.path[0] != '/') return false;
    r.token.clear();
    r.length = 0;
    for (size_t p = eol + 2, e; (e = raw.find("\r\n", p)) != std::string::npos && e != p; p = e + 2) {
        std::string h = raw.substr(p, e - p);
        if (h.size() > 14 && _strnicmp(h.c_str(), "authorization:", 14) == 0) {
            size_t v = h.find_first_not_of(' ', 14);
            if (v != std::string::npos && _strnicmp(h.c_str() + v, "Bearer ", 7) == 0) r.token = h.substr(v + 7);
        } else if (h.size() > 15 && _strnicmp(h.c_str(), "content-length:", 15) == 0) {
            std::string v = h.substr(h.find_first_not_of(' ', 15) == std::string::npos ? h.size() : h.find_first_not_of(' ', 15));
            if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return false;
            r.length = std::stoul(v);
        }
    }
    return true;
}

static std::string printable(std::string s) {
    if (s.size() > 80) s.resize(80);
    for (char& c : s) if (c < 0x20 || c > 0x7e) c = '?';
    return s;
}

static void reply(SOCKET c, int code, const char* type, const std::string& body) {
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                       : code == 405 ? "Method Not Allowed" : code == 409 ? "Conflict" : code == 429 ? "Too Many Requests"
                       : code == 503 ? "Service Unavailable" : "Error";
    std::string s = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(body.size()) + (code == 401 ? "\r\nWWW-Authenticate: Bearer" : "") +
                    "\r\nConnection: close\r\n\r\n" + body;
    for (size_t off = 0; off < s.size();) {
        int n = send(c, s.data() + off, (int)std::min<size_t>(s.size() - off, 1 << 20), 0);
        if (n <= 0) return;
        off += n;
    }
}
static void fail(SOCKET c, int code, const std::string& msg) { reply(c, code, "application/json", "{\"error\":" + jstr(msg) + "}"); }
static void done(SOCKET c, const std::string& msg) { reply(c, 200, "application/json", "{\"msg\":" + jstr(msg) + "}"); }

// Touchpad

static bool smallInt(const std::string& s, int lim, int& v) {
    size_t i = !s.empty() && s[0] == '-';
    if (s.size() <= i || s.size() - i > 5 || s.find_first_not_of("0123456789", i) != std::string::npos) return false;
    v = std::stoi(s);
    return abs(v) <= lim;
}

// Validate input line
// m dx dy | a x y | s dy | c/d/u l/r/m | t text | k key
static std::string inputReq(const std::string& l) {
    static const char* keys[] = {"enter", "backspace", "tab", "esc", "space", "left", "right", "up", "down", "home", "end", "delete", "win",
                                 "pgup", "pgdn", "f5", "f11", "alt+tab", "alt+f4", "win+d", "win+tab", "win+shift+s", "ctrl+c", "ctrl+v",
                                 "ctrl+x", "ctrl+z", "ctrl+y", "ctrl+a", "ctrl+s", "ctrl+w", "ctrl+t", "ctrl+tab", "ctrl+shift+t", "ctrl+shift+esc"};
    if (l.rfind("t ", 0) == 0 && l.size() > 2 && l.size() <= 1002) {
        for (unsigned char ch : l) if (ch < 0x20) return "";
        return "in " + l;
    }
    if (l.rfind("k ", 0) == 0) {
        for (auto k : keys) if (l.compare(2, std::string::npos, k) == 0) return "in " + l;
        return "";
    }
    if (l.size() == 3 && (l[0] == 'c' || l[0] == 'd' || l[0] == 'u') && l[1] == ' ' && (l[2] == 'l' || l[2] == 'r' || l[2] == 'm')) return "in " + l;
    int a = 0, b = 0;
    size_t sp = l.find(' ', 2);
    if (l.rfind("m ", 0) == 0 && sp != std::string::npos && smallInt(l.substr(2, sp - 2), 2000, a) && smallInt(l.substr(sp + 1), 2000, b))
        return "in m " + std::to_string(a) + " " + std::to_string(b);
    if (l.rfind("a ", 0) == 0 && sp != std::string::npos && smallInt(l.substr(2, sp - 2), 65535, a) && smallInt(l.substr(sp + 1), 65535, b) && a >= 0 && b >= 0)
        return "in a " + std::to_string(a) + " " + std::to_string(b);
    if (l.rfind("s ", 0) == 0 && smallInt(l.substr(2), 500, a)) return "in s " + std::to_string(a);
    return "";
}

static void inputLoop(SOCKET c, std::string buf, std::string peer) {
    SOCKET old = g_inputSock.exchange(c);
    if (old != INVALID_SOCKET) shutdown(old, SD_BOTH);
    DWORD idle = 10 * 60 * 1000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&idle, sizeof idle);
    logf("%s: touchpad/keyboard session started", peer.c_str());
    agentCall("notify security Phone connected as touchpad/keyboard", 3000);
    char b[4096];
    bool warned = false;
    std::string held;
    for (bool ok = true; ok;) {
        for (size_t nl; ok && (nl = buf.find('\n')) != std::string::npos; buf.erase(0, nl + 1)) {
            std::string req = inputReq(buf.substr(0, nl));
            if (req.size() == 6 && req[3] == 'd' && held.find(req[5]) == std::string::npos) held += req[5];
            if (req.size() == 6 && req[3] == 'u') held.erase(std::remove(held.begin(), held.end(), req[5]), held.end());
            std::string r = req.empty() || g_paused ? "" : agentCall(req, 2000);
            ok = !r.empty();
            if (!ok) logf("%s: input session closed (%s)", peer.c_str(), g_paused ? "paused" : req.empty() ? "invalid input" : "tray not responding");
            else if (r[0] == '-' && !warned) {
                warned = true;
                logf("%s: input %s", peer.c_str(), r.c_str() + 1);
                agentCall("notify security Phone touchpad/keyboard input " + r.substr(1), 3000);
            }
        }
        int n = ok && buf.size() < 8192 ? recv(c, b, sizeof b, 0) : 0;
        if (n <= 0) break;
        buf.append(b, n);
    }
    for (char h : held) agentCall(std::string("in u ") + h, 2000);
    SOCKET me = c; // compare_exchange writes to it
    g_inputSock.compare_exchange_strong(me, INVALID_SOCKET);
    closesocket(c);
    logf("%s: touchpad/keyboard session ended", peer.c_str());
    agentCall("notify security Phone touchpad/keyboard disconnected", 3000);
}

// Streaming

static bool sendAll(SOCKET c, const std::string& s) {
    for (size_t off = 0; off < s.size();) {
        int n = send(c, s.data() + off, (int)std::min<size_t>(s.size() - off, 1 << 20), 0);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

static void streamLoop(SOCKET c, std::string req, std::string peer) {
    SOCKET old = g_streamSock.exchange(c);
    if (old != INVALID_SOCKET) shutdown(old, SD_BOTH);
    DWORD st = 10000;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char*)&st, sizeof st);
    dropFeed(g_streamPipe);
    HANDLE p = agentCall(req, 5000) == "+" ? takeFeed(g_streamPipe) : nullptr;
    if (!p) {
        std::string e = "tray app could not start the stream";
        uint32_t n = (uint32_t)e.size();
        sendAll(c, std::string("E") + std::string((char*)&n, 4) + e);
    } else {
        logf("%s: screen stream started (%s)", peer.c_str(), req.c_str());
        agentCall("notify security Phone is streaming your screen", 3000);
        // Echo pings
        std::mutex sendMu;
        std::thread echo([&] {
            char b[9];
            for (int got = 0;;) {
                int n = recv(c, b + got, 9 - got, 0);
                if (n <= 0) { if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) continue; return; }
                if ((got += n) < 9) continue;
                if (b[0] != 'p') return;
                std::lock_guard<std::mutex> l(sendMu);
                sendAll(c, std::string("P\x08\0\0\0", 5) + std::string(b + 1, 8));
                got = 0;
            }
        });
        for (std::string m; !g_paused && pread(p, m, 10000, 16u << 20);) {
            std::lock_guard<std::mutex> l(sendMu);
            if (!sendAll(c, m)) break;
        }
        CloseHandle(p);
        shutdown(c, SD_BOTH);
        echo.join();
        logf("%s: screen stream ended", peer.c_str());
        agentCall("notify security Phone stopped streaming your screen", 3000);
    }
    SOCKET me = c;
    g_streamSock.compare_exchange_strong(me, INVALID_SOCKET);
    shutdown(c, SD_SEND);
    closesocket(c);
}

// File transfer

struct Handoff { std::string extra, req; size_t len = 0; };

static std::mutex g_fileMu;

// Upload is recv, download is send_file
static void fileLoop(SOCKET c, Handoff h, std::string peer) {
    DWORD t = 30000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&t, sizeof t);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char*)&t, sizeof t);
    bool up = h.req.rfind("recv ", 0) == 0;
    std::unique_lock<std::mutex> one(g_fileMu, std::try_to_lock);
    if (!one) { fail(c, 409, "another file transfer is running"); closesocket(c); return; }
    dropFeed(g_filePipe);
    std::string r = agentCall(h.req, 5000);
    HANDLE p = !r.empty() && r[0] == '+' ? takeFeed(g_filePipe) : nullptr;
    if (!p) fail(c, r.size() > 1 && r[0] == '-' ? 404 : 409, r.size() > 1 && r[0] == '-' ? r.substr(1) : "tray app not running");
    else if (up) {
        size_t sent = 0;
        bool ok = true;
        auto chunk = [&](const std::string& s) { ok = ok && pwrite(p, s, 30000); sent += s.size(); };
        if (!h.extra.empty()) chunk(h.extra.substr(0, h.len));
        std::vector<char> b(1 << 16);
        while (ok && sent < h.len) {
            int n = recv(c, b.data(), (int)std::min<size_t>(b.size(), h.len - sent), 0);
            if (n <= 0) { ok = false; break; }
            chunk(std::string(b.data(), n));
        }
        std::string res;
        if (ok) ok = pwrite(p, "", 30000) && pread(p, res, 60000, 4096) && !res.empty() && res[0] == '+';
        CloseHandle(p);
        if (ok) { logf("%s: uploaded %zu bytes", peer.c_str(), h.len); done(c, res.substr(1)); }
        else fail(c, 500, res.size() > 1 ? res.substr(1) : "upload interrupted");
    } else if (r.size() < 2 || r.size() > 13 || r.find_first_not_of("0123456789", 1) != std::string::npos) {
        CloseHandle(p);
        fail(c, 500, "bad reply from the tray app");
    } else {
        size_t size = std::stoull(r.substr(1)), sent = 0;
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(size) + "\r\nConnection: close\r\n\r\n";
        bool ok = sendAll(c, head);
        for (std::string m; ok && sent < size && pread(p, m, 30000, 1 << 20) && !m.empty(); sent += m.size()) ok = sendAll(c, m);
        CloseHandle(p);
        logf("%s: downloaded %zu of %zu bytes", peer.c_str(), sent, size);
    }
    shutdown(c, SD_SEND);
    closesocket(c);
}

// Requests

enum { AFTER_NONE, AFTER_SLEEP, AFTER_INPUT, AFTER_STREAM, AFTER_FILE };

// Per device, so one can't lock out another
struct Fails { int n = 0; ULONGLONG until = 0; };
static std::map<std::string, Fails> g_fails;

static int handle(SOCKET c, const std::string& peer, Handoff& ho) {
    // Refuse early
    std::string pinned;
    { std::lock_guard<std::mutex> l(g_tokMu); pinned = g_peer; }
    if (!pinned.empty() && peer != pinned) {
        static ULONGLONG lastLog = 0;
        if (GetTickCount64() - lastLog > 60000) { lastLog = GetTickCount64(); logf("%s: refused (not the paired phone %s)", peer.c_str(), pinned.c_str()); }
        fail(c, 403, "this PC is paired with another device");
        return AFTER_NONE;
    }
    auto fl = g_fails.find(peer);
    if (fl != g_fails.end() && GetTickCount64() < fl->second.until) {
        static ULONGLONG lastLog = 0;
        if (GetTickCount64() - lastLog > 60000) { lastLog = GetTickCount64(); logf("%s: rejected (locked out)", peer.c_str()); }
        fail(c, 429, "too many failed attempts; try again in a few minutes");
        return AFTER_NONE;
    }
    std::string& extra = ho.extra;
    std::string raw;
    char buf[2048];
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() <= 8192 && GetTickCount64() < deadline) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        raw.append(buf, n);
    }
    Req r;
    size_t hend = raw.find("\r\n\r\n");
    if (hend == std::string::npos || !parseReq(raw, r)) {
        logf("%s: malformed request", peer.c_str());
        fail(c, 400, "bad request");
        return AFTER_NONE;
    }
    extra = raw.substr(hend + 4);
    std::string what = printable(r.method + " " + r.path);
    bool good, pinnedNow = false;
    {
        std::lock_guard<std::mutex> l(g_tokMu);
        good = ctEq(r.token, g_token);
        if (good && g_peer.empty()) { g_peer = peer; savePeer(peer); pinnedNow = true; }
    }
    if (pinnedNow) {
        logf("%s: paired phone pinned; other devices are now refused", peer.c_str());
        agentCall("notify security Paired with the phone at " + peer + ". Other devices are now refused.", 3000);
    }
    if (!good) {
        logf("%s: %s -> bad token", peer.c_str(), what.c_str());
        Fails& f = g_fails[peer];
        if (++f.n >= 10) {
            f.n = 0;
            f.until = GetTickCount64() + 5 * 60 * 1000;
            logf("%s: 10 failed attempts: locked out for 5 minutes", peer.c_str());
            agentCall("notify security 10 failed login attempts from " + peer + ". That device is locked out for 5 minutes.", 3000);
        }
        fail(c, 401, "bad token");
        return AFTER_NONE;
    }
    g_fails.erase(peer);
    bool poll = r.method == "GET" && (r.path == "/status" || r.path == "/perf");
    if (!poll) logf("%s: %s", peer.c_str(), what.c_str()); // Skip polls

    if (poll) {
        std::string body;
        if (r.path == "/status") body = statusJson();
        else { std::lock_guard<std::mutex> l(g_perfMu); body = g_perf; }
        reply(c, 200, "application/json", body);
        return AFTER_NONE;
    }
    static const char* powers[] = {"/sleep", "/restart", "/shutdown"};
    static const char* session[] = {"/lock", "/monitors_off", "/vol_up", "/vol_down", "/mute", "/prev", "/playpause", "/next",
                                    "/screenshot", "/clipboard", "/input", "/open"};
    static const char* displays[] = {"internal", "clone", "extend", "external"};
    bool isPower = false, isSession = false, isTimer = r.path == "/timer/cancel", isFile = false;
    const char* want = "POST";
    for (auto p : powers) isPower |= r.path == p;
    for (auto p : session) isSession |= r.path == p;
    std::string req = r.path.substr(1), timerAct;
    int n = -1;
    unsigned long u = 0, u2 = 0;
    auto parts = [&](size_t from) {
        std::vector<std::string> p;
        for (size_t s = from, e; s <= r.path.size(); s = e + 1) {
            e = r.path.find('/', s);
            if (e == std::string::npos) e = r.path.size();
            p.push_back(r.path.substr(s, e - s));
        }
        return p;
    };
    if (r.path.rfind("/volume/", 0) == 0 && smallInt(r.path.substr(8), 100, n) && n >= 0) { isSession = true; req = "vol_set " + std::to_string(n); }
    for (auto d : displays) if (r.path == std::string("/display/") + d) { isSession = true; req = std::string("display ") + d; }
    if (r.path.rfind("/stream/", 0) == 0) {
        auto p = parts(8);
        int h = 0, f = 0, m = 0, mon = 0, ll = 0;
        if (p.size() >= 3 && p.size() <= 5 && smallInt(p[0], 2160, h) && smallInt(p[1], 120, f) && smallInt(p[2], 80, m) &&
            (p.size() < 4 || (smallInt(p[3], 15, mon) && mon >= 0)) && (p.size() < 5 || (smallInt(p[4], 1, ll) && ll >= 0)) &&
            (h == 480 || h == 720 || h == 1080 || h == 1440 || h == 2160) && (f == 30 || f == 60 || f == 120) && m >= 1) {
            isSession = true;
            req = "stream " + std::to_string(h) + " " + std::to_string(f) + " " + std::to_string(m) + " " + std::to_string(mon) + " " + std::to_string(ll);
        }
    }
    std::string name;
    if (r.path == "/files" || r.path == "/apps" || r.path == "/audio") { isSession = true; want = "GET"; }
    else if ((r.path.rfind("/files/", 0) == 0 || r.path.rfind("/upload/", 0) == 0) && urlDecode(r.path.substr(r.path.find('/', 1) + 1), name) && validName(name)) {
        bool upload = r.path[1] == 'u';
        isSession = isFile = true;
        want = upload ? "POST" : "GET";
        req = upload ? "recv " + std::to_string(r.length) + " " + name : "send_file " + name;
    }
    else if (r.path.rfind("/app/", 0) == 0 && uintArg(r.path.substr(5), 49, u)) { isSession = true; req = "app " + std::to_string(u); }
    else if (r.path.rfind("/audio/out/", 0) == 0) {
        std::string id = r.path.substr(11);
        if (!id.empty() && id.size() <= 100 && id.find_first_not_of("0123456789abcdefABCDEF{}.-") == std::string::npos) { isSession = true; req = "audio_out " + id; }
    }
    else if (r.path.rfind("/audio/app/", 0) == 0) {
        auto p = parts(11);
        if (p.size() == 2 && uintArg(p[0], 0xFFFFFFFF, u)) {
            if (p[1] == "mute") { isSession = true; req = "audio_mute " + std::to_string(u); }
            else if (uintArg(p[1], 100, u2)) { isSession = true; req = "audio_app " + std::to_string(u) + " " + std::to_string(u2); }
        }
    }
    for (auto p : powers) {
        std::string pre = std::string("/timer") + p + "/";
        if (r.path.rfind(pre, 0) == 0 && smallInt(r.path.substr(pre.size()), 1440, n) && n >= 1) { isTimer = true; timerAct = p + 1; }
    }
    if (!isPower && !isSession && !isTimer) { fail(c, 404, "unknown action"); return AFTER_NONE; }
    if (r.path == "/clipboard" ? r.method != "GET" && r.method != "POST" : r.method != want) { fail(c, 405, "method not allowed"); return AFTER_NONE; }
    if (g_paused) { fail(c, 503, "remote control is paused on the PC"); return AFTER_NONE; }
    if (!isFile && r.length > 256 * 1024) { fail(c, 400, "body too large (max 256 KB)"); return AFTER_NONE; }
    for (ULONGLONG end = GetTickCount64() + 10000; !isFile && extra.size() < r.length && GetTickCount64() < end;) {
        int got = recv(c, buf, sizeof buf, 0);
        if (got <= 0) break;
        extra.append(buf, got);
    }
    if (!isFile && extra.size() < r.length) { fail(c, 400, "incomplete body"); return AFTER_NONE; }
    std::string body = extra.substr(0, std::min(extra.size(), r.length));

    if (isTimer) {
        setTimer(timerAct, timerAct.empty() ? 0 : n);
        std::string msg = timerAct.empty() ? "Power timer cancelled" : "Scheduled " + timerAct + " in " + std::to_string(n) + " min";
        logf("%s", msg.c_str());
        agentCall("notify power " + msg + " (from phone)", 3000);
        done(c, msg);
        return AFTER_NONE;
    }
    if (isPower) {
        agentCall(std::string("notify power ") + (req == "sleep" ? "Sleep" : req == "restart" ? "Restart (in 5 s)" : "Shutdown (in 5 s)") + " requested from phone", 3000);
        if (req == "sleep") { done(c, "Going to sleep"); return AFTER_SLEEP; }
        std::string err = power(req);
        if (!err.empty()) { fail(c, 500, err); return AFTER_NONE; }
        done(c, req == "restart" ? "Restarting in 5 s" : "Shutting down in 5 s");
        return AFTER_NONE;
    }

    std::string user;
    bool locked;
    bool image = r.path == "/screenshot", stream = req.rfind("stream ", 0) == 0;
    if (!sessionInfo(user, locked)) { fail(c, 500, "cannot query the Windows session"); return AFTER_NONE; }
    if (user.empty()) { fail(c, 409, "no user logged in"); return AFTER_NONE; }
    if (locked && (image || stream || r.path == "/input")) { fail(c, 409, "PC is locked"); return AFTER_NONE; }

    if (r.path == "/input" || stream) {
        bool tray;
        { std::lock_guard<std::mutex> l(g_agentMu); tray = g_agent != nullptr; }
        if (!tray) { fail(c, 409, "tray app not running"); return AFTER_NONE; }
        std::string ok = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
        send(c, ok.data(), (int)ok.size(), 0);
        if (stream) { ho.req = req; return AFTER_STREAM; }
        return AFTER_INPUT;
    }
    if (isFile) { ho.req = req; ho.len = r.length; return AFTER_FILE; }
    if (r.path == "/clipboard") req = r.method == "GET" ? "clip_get" : "clip_set " + body;
    if (r.path == "/open") {
        if (!validUrl(body)) { fail(c, 400, "only http:// and https:// links can be opened"); return AFTER_NONE; }
        req = "open " + body;
    }
    std::string res = agentCall(req, image ? 20000 : 5000);
    if (res.empty()) { fail(c, 409, "tray app not running"); return AFTER_NONE; }
    if (res[0] != '+') { fail(c, 500, res.substr(1)); return AFTER_NONE; }
    if (image) reply(c, 200, "image/jpeg", res.substr(1));
    else if (req == "clip_get") reply(c, 200, "text/plain; charset=utf-8", res.substr(1));
    else if (req == "files" || req == "apps" || req == "audio") reply(c, 200, "application/json", res.substr(1));
    else done(c, res.substr(1));
    return AFTER_NONE;
}

static bool stopping(DWORD ms = 0) { return WaitForSingleObject(g_stopEv, ms) == WAIT_OBJECT_0; }

static void serve() {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    std::thread(pipeLoop).detach();
    std::thread(perfLoop).detach();
    bool waitingLogged = false;
    while (!stopping()) {
        uint32_t ip = tailscaleIp();
        if (!ip) {
            if (!waitingLogged) { logf("waiting for a Tailscale IP"); waitingLogged = true; }
            tickTimer();
            stopping(3000);
            continue;
        }
        waitingLogged = false;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&excl, sizeof excl);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(PORT);
        a.sin_addr.s_addr = ip;
        char ipText[16];
        inet_ntop(AF_INET, &a.sin_addr, ipText, sizeof ipText);
        if (bind(s, (sockaddr*)&a, sizeof a) || listen(s, 8)) {
            logf("bind %s:%d failed: %d", ipText, PORT, WSAGetLastError());
            closesocket(s);
            stopping(5000);
            continue;
        }
        g_ip = ip;
        logf("listening on %s:%d", ipText, PORT);
        ULONGLONG lastCheck = GetTickCount64();
        while (!stopping()) {
            fd_set f;
            FD_ZERO(&f);
            FD_SET(s, &f);
            timeval tv{1, 0};
            tickTimer();
            int n = select(0, &f, nullptr, nullptr, &tv);
            if (n < 0) { logf("select failed: %d", WSAGetLastError()); stopping(5000); break; }
            if (n == 1) {
                sockaddr_in pa{};
                int pl = sizeof pa;
                SOCKET c = accept(s, (sockaddr*)&pa, &pl);
                if (c != INVALID_SOCKET) {
                    char peer[16] = "?";
                    inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof peer);
                    DWORD rt = 5000, st = 30000;
                    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&rt, sizeof rt);
                    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char*)&st, sizeof st);
                    int after = AFTER_NONE;
                    Handoff ho;
                    if ((ntohl(pa.sin_addr.s_addr) & 0xFFC00000) == 0x64400000) after = handle(c, peer, ho);
                    else logf("%s: rejected (not a Tailscale address)", peer);
                    if (after == AFTER_INPUT) { std::thread(inputLoop, c, ho.extra, std::string(peer)).detach(); continue; }
                    if (after == AFTER_STREAM) { std::thread(streamLoop, c, ho.req, std::string(peer)).detach(); continue; }
                    if (after == AFTER_FILE) { std::thread(fileLoop, c, ho, std::string(peer)).detach(); continue; }
                    shutdown(c, SD_SEND);
                    closesocket(c);
                    if (after == AFTER_SLEEP && !SetSuspendState(FALSE, FALSE, FALSE)) logf("SetSuspendState failed: %lu", GetLastError());
                }
            }
            if (GetTickCount64() - lastCheck > 15000) { // Rebind if the Tailscale IP changed
                lastCheck = GetTickCount64();
                if (tailscaleIp() != ip) { logf("Tailscale IP changed or went away"); break; }
            }
        }
        g_ip = 0;
        closesocket(s);
    }
}

// Service

static void setState(DWORD s) {
    SERVICE_STATUS st{SERVICE_WIN32_OWN_PROCESS, s, s == SERVICE_RUNNING ? (DWORD)SERVICE_ACCEPT_STOP : 0};
    SetServiceStatus(g_ss, &st);
}

static DWORD WINAPI ctrl(DWORD c, DWORD, void*, void*) {
    if (c == SERVICE_CONTROL_STOP) { setState(SERVICE_STOP_PENDING); SetEvent(g_stopEv); }
    return NO_ERROR;
}

static void WINAPI svcMain(DWORD, wchar_t**) {
    g_ss = RegisterServiceCtrlHandlerExW(L"hadal", ctrl, nullptr);
    setState(SERVICE_RUNNING);
    serve();
    logf("service stopped");
    setState(SERVICE_STOPPED);
}

#define CHECK(x) if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; }
static int selftest() {
    Req r;
    CHECK(parseReq("POST /lock HTTP/1.1\r\nHost: x\r\nauthorization:  bearer abc\r\n\r\n", r));
    CHECK(r.method == "POST" && r.path == "/lock" && r.token == "abc");
    CHECK(parseReq("GET /status HTTP/1.1\r\n\r\n", r) && r.token.empty());
    CHECK(!parseReq("garbage\r\n\r\n", r));
    CHECK(!parseReq("GET /x\r\n\r\n", r));
    CHECK(!parseReq("GET /x FTP/1\r\n\r\n", r));
    CHECK(!parseReq("POST  HTTP/1.1\r\n\r\n", r) && !parseReq("POST x HTTP/1.1\r\n\r\n", r));
    CHECK(parseReq("GET /status HTTP/1.1\r\nAuthorization: Basic abc\r\n\r\n", r) && r.token.empty());
    CHECK(parseReq("POST /clipboard HTTP/1.1\r\ncontent-length: 12\r\n\r\n", r) && r.length == 12);
    CHECK(!parseReq("POST /clipboard HTTP/1.1\r\nContent-Length: -1\r\n\r\n", r));
    CHECK(!parseReq("POST /clipboard HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n", r));
    CHECK(inputReq("m 5 -3") == "in m 5 -3");
    CHECK(inputReq("m 5") == "" && inputReq("m 99999 1") == "" && inputReq("m 1 2 3") == "" && inputReq("m +1 2") == "" && inputReq("m - 1") == "");
    CHECK(inputReq("s -40") == "in s -40" && inputReq("s 501") == "" && inputReq("s ") == "");
    CHECK(inputReq("c l") == "in c l" && inputReq("d r") == "in d r" && inputReq("c x") == "" && inputReq("c ll") == "");
    CHECK(inputReq("t héllo wörld") == "in t héllo wörld" && inputReq("t a\tb") == "" && inputReq("t ") == "");
    CHECK(inputReq("k enter") == "in k enter" && inputReq("k enterx") == "" && inputReq("k ") == "");
    CHECK(inputReq("x") == "" && inputReq("") == "" && inputReq("notify hi") == "");
    CHECK(inputReq("a 0 65535") == "in a 0 65535" && inputReq("a 100 200") == "in a 100 200");
    CHECK(inputReq("a -1 5") == "" && inputReq("a 65536 5") == "" && inputReq("a 5") == "" && inputReq("a 1 2 3") == "");
    CHECK(validUrl("https://example.com/a?b=c#d") && validUrl("http://x.io"));
    CHECK(!validUrl("file:///C:/x") && !validUrl("javascript:alert(1)") && !validUrl("https://a b") && !validUrl("https://a\"b") && !validUrl("https://ä.de") && !validUrl("https://"));
    CHECK(inputReq("k ctrl+c") == "in k ctrl+c" && inputReq("k win+shift+s") == "in k win+shift+s" && inputReq("k ctrl+alt+del") == "");
    CHECK(validName("IMG_2026.jpg") && validName("résumé (1).pdf") && validName(".x") == false && validName("a.") == false);
    CHECK(!validName("") && !validName("a/b") && !validName("a\\b") && !validName("..") && !validName("c:x") && !validName("a\tb"));
    CHECK(!validName("CON") && !validName("con.txt") && !validName("CON .txt") && !validName("com1.log") && validName("com10.log") && validName("console.txt"));
    CHECK(!validName(std::string(201, 'a')) && validName(std::string(200, 'a')) && !validName("x:stream") && !validName(" a"));
    std::string d;
    CHECK(urlDecode("a%20b%2Fc", d) && d == "a b/c" && !urlDecode("a%2", d) && !urlDecode("a%zz", d) && urlDecode("%C3%A9", d) && d == "\xC3\xA9");
    CHECK(ctEq("abc", "abc") && !ctEq("abd", "abc") && !ctEq("ab", "abc") && !ctEq("abcd", "abc") && !ctEq("", "abc"));
    CHECK(jstr("a\"b\\\n") == "\"a\\\"b\\\\\\u000a\"");
    CHECK(printable("GET /\x01\xff") == "GET /??");
    std::string t = newToken();
    CHECK(t.size() == 64 && t.find_first_not_of("0123456789abcdef") == std::string::npos && t != newToken());
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = tmp;
    savePeer("100.100.1.2"); loadPeer(); CHECK(g_peer == "100.100.1.2");
    savePeer("192.168.1.5"); loadPeer(); CHECK(g_peer.empty());
    savePeer(""); loadPeer(); CHECK(g_peer.empty());
    puts("selftest ok");
    return 0;
}

#ifndef HADAL_FUZZ // Fuzz has its own main
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--selftest")) return selftest();
    PSID sid;
    if (argc < 2 || !ConvertStringSidToSidW(argv[1], &sid)) {
        fwprintf(stderr, L"usage: hadal-svc <user SID> [--console]  |  hadal-svc --selftest\n");
        return 2;
    }
    LocalFree(sid);
    wchar_t d[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\Hadal\\", d, MAX_PATH);
    g_dir = d;
    CreateDirectoryW(d, nullptr);
    logf("starting");
    loadToken();
    loadPeer();
    g_paused = GetFileAttributesW((g_dir + L"paused").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_paused) logf("remote control is paused (from before restart)");
    if (!enableShutdownPriv()) logf("SeShutdownPrivilege not available: sleep/restart/shutdown will fail");

    // Pipe DACL
    g_pipeSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(argv[1]) + L")";
    BYTE svcSid[SECURITY_MAX_SID_SIZE];
    DWORD cb = sizeof svcSid, dcb = 64;
    wchar_t dom[64], *ss;
    SID_NAME_USE use;
    if (LookupAccountNameW(nullptr, L"NT SERVICE\\hadal", svcSid, &cb, dom, &dcb, &use) && ConvertSidToStringSidW(svcSid, &ss)) {
        g_pipeSddl += L"(A;;GA;;;" + std::wstring(ss) + L")";
        LocalFree(ss);
    }

    g_stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (argc > 2 && !wcscmp(argv[2], L"--console")) { serve(); return 0; }
    SERVICE_TABLE_ENTRYW t[] = {{(LPWSTR)L"hadal", svcMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(t) ? 0 : 1;
}
#endif
