#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlobj.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <chrono>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static const bool ENABLE_LEGACY_INJECTION = true;
static const DWORD MONITOR_SLEEP_MS = 1000;
static const wchar_t* APP_FOLDER_NAME = L"FModFN";

/// <summary>
/// Main bootstrapper class responsible for launching the game, optionally injecting DLLs,
/// monitoring loaded modules, and handling credentials.
/// </summary>
class Bootstrapper {
public:
    /// <summary>
    /// Initializes default DLLs to inject and executable names.
    /// </summary>
    Bootstrapper() {
        gameDLLs = { L"Starfall.dll", L"Vivox_sdk64.dll", L"Horizion.ClientPatches.dll"};
        gameExeName = L"FortniteClient-Win64-Shipping.exe";
        launcherExeName = L"FortniteLauncher.exe";
        beExeName = L"FortniteClient-Win64-Shipping_BE.exe";
    }

    /// <summary>
    /// Runs the bootstrapper: launches helper processes, starts the game suspended,
    /// optionally injects DLLs, resumes the game, and starts the monitoring thread.
    /// </summary>
    /// <returns>True if bootstrapper ran successfully, false otherwise.</returns>
    bool Run() {
        std::filesystem::path exeFolder = std::filesystem::current_path();

        std::wstring localApp;
        if (!GetLocalAppData(localApp)) {
            std::wcerr << L"[error] Could not resolve LOCALAPPDATA\n";
            return false;
        }

        std::filesystem::path appdir = std::filesystem::path(localApp) / APP_FOLDER_NAME;

        std::string email = ReadTextFileUtf8(appdir / L"email.txt");
        std::string password = ReadTextFileUtf8(appdir / L"password.txt");
        if (email.empty() || password.empty()) {
            std::wcerr << L"[error] Missing email.txt or password.txt\n";
            MessageBoxW(NULL, (L"Missing email.txt or password.txt in " + appdir.wstring()).c_str(), L"Bootstrapper", MB_OK | MB_ICONERROR);
            return false;
        }

        LaunchSimpleProcess(appdir / launcherExeName, L"");
        LaunchSimpleProcess(appdir / beExeName, L"");
        std::wstring args(
            L"-log -epicapp=Fortnite -epicenv=Prod -epiclocale=en-us "
            L"-epicportal -skippatchcheck -nobe -fromfl=eac "
            L"-fltoken=3db3ba5dcbd2e16703f3978d -nosplash "
            L"-caldera=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9..."
            L" -AUTH_LOGIN=host@fmod.dev"
            L" -AUTH_PASSWORD=host"
            L" -AUTH_TYPE=epic"
            L" -nullrhi -nosound -unattended -log"
        );


        std::filesystem::path gamePath = exeFolder / gameExeName;
        if (!std::filesystem::exists(gamePath)) {
            std::wcerr << L"[error] Game exe not found: " << gamePath.wstring() << L"\n";
            return false;
        }

        // Prepare STARTUPINFO (no redirected stdout/stderr anymore)
        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);

        PROCESS_INFORMATION pi{};

        // Compose command line
        std::wstring cmdLine = L"\"" + gamePath.wstring() + L"\" " + args;

        // Create process suspended (no inherited handles, no stdout redirection)
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(0);
        if (!CreateProcessW(
            NULL,
            cmdBuf.data(),
            NULL,
            NULL,
            FALSE, // do not inherit handles
            CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
            NULL,
            gamePath.parent_path().c_str(),
            &si,
            &pi)) {
            std::wcerr << L"[-] CreateProcess failed\n";
            return false;
        }

        hProcess = pi.hProcess;
        targetPid = pi.dwProcessId;
        std::wcout << L"[info] Game process created (PID: " << targetPid << L")\n";

        // Inject your initial DLLs as usual (legacy injection)
        if (ENABLE_LEGACY_INJECTION) {
            for (auto& dll : gameDLLs) {
                std::filesystem::path dllP = exeFolder / dll;
                if (!std::filesystem::exists(dllP)) continue;
                LegacyInjectDLL(targetPid, dllP.wstring());
            }
        }

        // Resume the main thread
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        // Start monitoring thread for loaded modules (keeps original monitoring logic)
        HANDLE mon = CreateThread(NULL, 0, MonitorThreadProc, this, 0, NULL);
        if (mon) CloseHandle(mon);

        std::wcout << L"[<] Waiting for game initialization...\n";

        // Your existing wait or exit logic here
        std::wstring dummy;
        std::getline(std::wcin, dummy);

        CloseHandle(hProcess);
        return true;
    }

private:
    std::vector<std::wstring> gameDLLs;
    std::wstring gameExeName;
    std::wstring launcherExeName;
    std::wstring beExeName;
    HANDLE hProcess = nullptr;
    DWORD targetPid = 0;
    HANDLE monitoringThread = nullptr;
    std::unordered_set<std::wstring> checkedModules;
    std::mutex checkedModulesMutex;

    /// <summary>
    /// Get the LOCALAPPDATA path.
    /// </summary>
    static bool GetLocalAppData(std::wstring& out) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path))) {
            out = path;
            CoTaskMemFree(path);
            return true;
        }
        return false;
    }

    /// <summary>
    /// Reads a UTF-8 text file into a string.
    /// </summary>
    static std::string ReadTextFileUtf8(const std::filesystem::path& p) {
        std::ifstream f(p); if (!f) return {};
        std::ostringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t start = 0; while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
        return s.substr(start);
    }

    /// <summary>
    /// Launch a simple process without waiting.
    /// </summary>
    static bool LaunchSimpleProcess(const std::wstring& exePath, const std::wstring& args) {
        STARTUPINFOW si{}; PROCESS_INFORMATION pi{}; si.cb = sizeof(si);
        std::wstring cmd = L"\"" + exePath + L"\""; if (!args.empty()) cmd += L" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true; }
        return false;
    }

    /// <summary>
    /// Launch a process suspended (for injection/checking).
    /// </summary>
    static bool CreateProcessSuspended(const std::wstring& exePath, const std::wstring& args, PROCESS_INFORMATION& outPi) {
        STARTUPINFOW si{}; si.cb = sizeof(si);
        std::wstring cmd = L"\"" + exePath + L"\""; if (!args.empty()) cmd += L" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        return CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &outPi) == TRUE;
    }

    /// <summary>
    /// Inject a DLL into the target process using LoadLibraryW.
    /// </summary>
    static bool LegacyInjectDLL(DWORD pid, const std::wstring& dllPath) {
        HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!proc) return false;
        size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { CloseHandle(proc); return false; }
        if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, NULL)) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false; }
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadAddr = GetProcAddress(k32, "LoadLibraryW");
        if (!loadAddr) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false; }
        HANDLE th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)loadAddr, remote, 0, NULL);
        if (!th) { VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc); return false; }
        WaitForSingleObject(th, INFINITE); CloseHandle(th);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE); CloseHandle(proc);
        return true;
    }

    /// <summary>
    /// Monitors loaded modules in the target process and logs suspicious ones.
    /// </summary>
    void MonitorProc() {
        std::vector<HMODULE> mods(1024);
        DWORD needed = 0;
        while (hProcess) {
            if (!EnumProcessModules(hProcess, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) { Sleep(MONITOR_SLEEP_MS); continue; }
            DWORD count = needed / sizeof(HMODULE);
            if (count > mods.size()) { mods.resize(count + 16); Sleep(MONITOR_SLEEP_MS); continue; }

            for (DWORD i = 0; i < count; ++i) {
                wchar_t name[MAX_PATH]; if (!GetModuleFileNameExW(hProcess, mods[i], name, MAX_PATH)) continue;
                std::wstring path(name);
                std::scoped_lock lk(checkedModulesMutex);
                if (checkedModules.find(path) != checkedModules.end()) continue;
                checkedModules.insert(path);
                if (IsSystemDLL(path) || IsGameDLL(path)) continue;
                if (!IsFileSignedOrKnownGood(path)) std::wcout << L"[AC] Unsigned/unexpected module: " << path << L"\n";
            }
            Sleep(MONITOR_SLEEP_MS);
        }
    }

    static DWORD WINAPI MonitorThreadProc(LPVOID param) {
        reinterpret_cast<Bootstrapper*>(param)->MonitorProc();
        return 0;
    }

    bool IsGameDLL(const std::wstring& path) {
        for (auto& g : gameDLLs) if (path.find(g) != std::wstring::npos) return true;
        return false;
    }

    static bool IsSystemDLL(const std::wstring& path) {
        std::wstring lower = path; std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        wchar_t sysDir[MAX_PATH]; GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring sys = sysDir; std::transform(sys.begin(), sys.end(), sys.begin(), ::towlower);
        return lower.rfind(sys, 0) == 0 || lower.find(L"\\windows\\winsxs\\") != std::wstring::npos || lower.find(L"\\windows\\system32\\drivers\\") != std::wstring::npos;
    }

    static bool IsFileSignedOrKnownGood(const std::wstring&) { return true; }
};

int wmain(int, wchar_t* []) {
    Bootstrapper b;
    if (!b.Run()) { std::wcerr << L"[fatal] bootstrapper failed\n"; return 1; }
    return 0;
}
