// Fuzz harness (libFuzzer + ASan), build with -DHADAL_FUZZ=ON
// Input: [flags] chunk ("@@@" chunk)*, flags: bit0 pinned, bit1 paused, bits2-3 tray reply
// Chunk: [peer byte: even = phone] request, "@T" = token
#define HADAL_FUZZ
#include "svc.cpp"
#include <afunix.h>
#include <regex>

static std::string TOKEN; // Random per run
static const char* PHONE = "100.100.1.2";
static const char* OTHER = "100.100.9.9";

static std::mutex g_seenMu;
static std::vector<std::string> g_seen;
static std::atomic<int> g_reply{0};
static SOCKET g_lsn;
static sockaddr_un g_addr; // AF_UNIX, no TIME_WAIT

#define BUG(...) do { fprintf(stderr, "\n*** ORACLE: " __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)

static void fakeTray(HANDLE h) {
    std::vector<char> b(1 << 20);
    for (;;) {
        std::string m;
        DWORD got;
        for (;;) {
            BOOL ok = ReadFile(h, b.data(), (DWORD)b.size(), &got, nullptr);
            m.append(b.data(), got);
            if (ok) break;
            if (GetLastError() != ERROR_MORE_DATA) { CloseHandle(h); return; }
        }
        { std::lock_guard<std::mutex> l(g_seenMu); g_seen.push_back(m); }
        std::string r = g_reply == 1 ? "-nope" : g_reply == 2 && m == "info" ? "+{\"fg\":\"x\",\"vol\":5}" : "+ok";
        WriteFile(h, r.data(), (DWORD)r.size(), &got, nullptr);
    }
}

static void connectTray() {
    static int n = 0;
    std::wstring name = L"\\\\.\\pipe\\hadal-fuzz-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(n++);
    HANDLE s = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                1, 65536, 65536, 0, nullptr);
    HANDLE c = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (s == INVALID_HANDLE_VALUE || c == INVALID_HANDLE_VALUE || !SetNamedPipeHandleState(c, &mode, nullptr, nullptr)) BUG("fake tray setup failed");
    std::thread(fakeTray, c).detach();
    std::lock_guard<std::mutex> l(g_agentMu);
    g_agent = s;
}

static void init() {
    // No log file, it serializes workers
    g_dir = L"\\\\?\\hadal-fuzz-no-files\\";
    TOKEN = g_token = newToken();
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    g_addr.sun_family = AF_UNIX;
    snprintf(g_addr.sun_path, sizeof g_addr.sun_path, "%shadal-fuzz-%lu.sock", tmp, GetCurrentProcessId());
    DeleteFileA(g_addr.sun_path);
    g_lsn = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(g_lsn, (sockaddr*)&g_addr, sizeof g_addr) || listen(g_lsn, 8)) BUG("listen failed (%d)", WSAGetLastError());
}

// Allowed tray commands
static bool allowedTray(const std::string& m, bool authed) {
    static const std::regex notify(R"(notify (security|power) [^\x00-\x1f]*)");
    if (std::regex_match(m, notify)) return authed || m.rfind("notify security ", 0) == 0;
    if (!authed) return false;
    static const std::regex fixed(R"(info|lock|monitors_off|vol_up|vol_down|mute|prev|playpause|next|screenshot|clip_get)"
                                  R"(|vol_set (100|[1-9]?[0-9])|display (internal|clone|extend|external))"
                                  R"(|in k (enter|backspace|tab|esc|space|left|right|up|down|home|end|delete|win|pgup|pgdn|f5|f11)|in [cdu] [lrm])"
                                  R"(|in k (alt\+tab|alt\+f4|win\+d|win\+tab|win\+shift\+s|ctrl\+[cvxzyastw]|ctrl\+tab|ctrl\+shift\+t|ctrl\+shift\+esc))"
                                  R"(|files|apps|audio|app ([0-9]|[1-4][0-9])|audio_out [0-9a-fA-F{}.\-]{1,100})"
                                  R"(|audio_app (0|[1-9][0-9]{0,9}) (100|[1-9]?[0-9])|audio_mute (0|[1-9][0-9]{0,9}))");
    if (std::regex_match(m, fixed)) return true;
    if (m.rfind("clip_set ", 0) == 0) return true;
    if (m.rfind("in t ", 0) == 0) {
        if (m.size() < 6 || m.size() > 1005) return false;
        for (unsigned char ch : m) if (ch < 0x20) return false;
        return true;
    }
    if (m.rfind("open http://", 0) == 0 || m.rfind("open https://", 0) == 0) {
        if (m.size() > 5 + 2048) return false;
        for (size_t i = 5; i < m.size(); i++) {
            unsigned char ch = m[i];
            if (ch <= 0x20 || ch >= 0x7f || strchr("\"<>\\^`{|}", ch)) return false;
        }
        return true;
    }
    static const std::regex num(R"(in (m|a|s) (-?(?:0|[1-9][0-9]*))(?: (-?(?:0|[1-9][0-9]*)))?)");
    std::smatch g;
    if (!std::regex_match(m, g, num) || g[2].length() > 6 || g[3].length() > 6) return false;
    int a = std::stoi(g[2]), b = g[3].matched ? std::stoi(g[3]) : 0;
    if (g[1] == "s") return !g[3].matched && abs(a) <= 500 && g[2] != "-0";
    if (!g[3].matched || g[2] == "-0" || g[3] == "-0") return false;
    if (g[1] == "m") return abs(a) <= 2000 && abs(b) <= 2000;
    return a >= 0 && b >= 0 && a <= 65535 && b <= 65535;
}

// Plain file name
static bool okName(const std::string& n) {
    if (n.empty() || n.size() > 200 || n[0] == ' ' || n[0] == '.' || n.back() == ' ' || n.back() == '.') return false;
    for (unsigned char ch : n) if (ch < 0x20 || ch == 0x7f || strchr("\\/:*?\"<>|", ch)) return false;
    static const std::regex dev(R"((con|prn|aux|nul|conin\$|conout\$|com[0-9]|lpt[0-9]) *(\..*)?)", std::regex::icase);
    return !std::regex_match(n, dev);
}

static void oneConnection(const std::string& peer, const std::string& raw) {
    SOCKET cl = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(cl, (sockaddr*)&g_addr, sizeof g_addr)) BUG("connect failed (%d)", WSAGetLastError());
    SOCKET sv = accept(g_lsn, nullptr, nullptr);
    DWORD rt = 5000;
    setsockopt(sv, SOL_SOCKET, SO_RCVTIMEO, (char*)&rt, sizeof rt);
    // One sender thread (ASan leaks per-thread memory)
    static std::mutex sm;
    static std::condition_variable scv;
    static const std::string* job = nullptr;
    static SOCKET jobSock;
    static bool busy = false;
    static bool started = (std::thread([] {
        for (;;) {
            std::unique_lock<std::mutex> l(sm);
            scv.wait(l, [] { return job != nullptr; });
            const std::string* j = job;
            l.unlock();
            sendAll(jobSock, *j);
            shutdown(jobSock, SD_SEND);
            l.lock();
            job = nullptr;
            busy = false;
            scv.notify_all();
        }
    }).detach(), true);
    (void)started;
    { std::lock_guard<std::mutex> l(sm); jobSock = cl; job = &raw; busy = true; }
    scv.notify_all();

    std::string pinnedBefore;
    { std::lock_guard<std::mutex> l(g_tokMu); pinnedBefore = g_peer; }
    { std::lock_guard<std::mutex> l(g_seenMu); g_seen.clear(); }
    Handoff ho;
    int after = handle(sv, peer, ho);
    if (after == AFTER_STREAM) {
        static const std::regex stream(R"(stream (480|720|1080|1440|2160) (30|60|120) ([1-9]|[1-7][0-9]|80) ([0-9]|1[0-5]) [01])");
        if (!std::regex_match(ho.req, stream)) BUG("stream request outside the grammar: %s", printable(ho.req).c_str());
    }
    if (after == AFTER_FILE) {
        static const std::regex file(R"((recv (0|[1-9][0-9]{0,8})|send_file) (.*))");
        std::smatch g;
        if (!std::regex_match(ho.req, g, file) || !okName(g[3]) || raw.find(TOKEN) == std::string::npos)
            BUG("file request outside the grammar: %s", printable(ho.req).c_str());
    }
    if (after == AFTER_INPUT) inputLoop(sv, ho.extra, peer);
    else {
        shutdown(sv, SD_SEND);
        char b[4096];
        while (recv(sv, b, sizeof b, 0) > 0) {} // Drain first
        closesocket(sv);
    }
    { std::unique_lock<std::mutex> l(sm); scv.wait(l, [] { return !busy; }); }
    std::string resp;
    char b[4096];
    for (int n; (n = recv(cl, b, sizeof b, 0)) > 0;) resp.append(b, n);
    closesocket(cl);

    int code = resp.size() >= 12 && resp.rfind("HTTP/1.1 ", 0) == 0 ? atoi(resp.c_str() + 9) : 0;
    if (after == AFTER_FILE) code = 200;
    if (!code) BUG("no HTTP response");
    bool hasToken = raw.find(TOKEN) != std::string::npos;
    bool stranger = !pinnedBefore.empty() && peer != pinnedBefore;
    if (stranger && code != 400 && code != 403 && code != 429) BUG("unpinned device got %d", code);
    if (code == 200 && !hasToken) BUG("200 without the token");
    std::lock_guard<std::mutex> l(g_seenMu);
    for (auto& m : g_seen) {
        if (stranger) BUG("unpinned device reached the tray: %s", printable(m).c_str());
        if (!allowedTray(m, hasToken)) BUG("tray command outside the grammar: %s", printable(m).c_str());
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool ready = (init(), true);
    (void)ready;
    if (size < 1) return 0;
    uint8_t flags = data[0];
    g_fails = 0;
    g_lockUntil = 0;
    g_paused = (flags & 2) != 0;
    setTimer("", 0);
    { std::lock_guard<std::mutex> l(g_tokMu); g_peer = flags & 1 ? PHONE : ""; }
    g_reply = (flags >> 2) & 3;
    bool have;
    { std::lock_guard<std::mutex> l(g_agentMu); have = g_agent != nullptr; }
    if (g_reply == 3 && have) { std::lock_guard<std::mutex> l(g_agentMu); CloseHandle(g_agent); g_agent = nullptr; }
    if (g_reply != 3 && !have) connectTray();

    std::string in((const char*)data + 1, size - 1);
    int chunks = 0;
    for (size_t p = 0; p <= in.size() && chunks < 24; chunks++) {
        size_t e = in.find("@@@", p);
        if (e == std::string::npos) e = in.size();
        std::string c = in.substr(p, e - p);
        p = e + 3;
        if (c.empty()) continue;
        std::string raw = c.substr(1);
        for (size_t t; (t = raw.find("@T")) != std::string::npos;) raw.replace(t, 2, TOKEN);
        oneConnection((c[0] & 1) ? OTHER : PHONE, raw);
    }
    return 0;
}
