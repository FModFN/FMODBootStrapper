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
#include <atomic>
#include <thread>
#include <algorithm>
#include <cwctype>
#include <iomanip>  // For timestamps

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static const bool ENABLE_LEGACY_INJECTION = true;
static const DWORD MONITOR_SLEEP_MS = 1000;
static const wchar_t* APP_FOLDER_NAME = L"FModFN";
static const bool RELAUNCH_HELPERS_ON_RESTART = false;  // Set to true if helpers need relaunching each time

// Ctrl+C handler for graceful exit
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT) {
        std::wcout << L"\n[info] Ctrl+C detected. Cleaning up and exiting...\n";
        // TODO: Add cleanup (e.g., kill game if running)
        exit(0);
    }
    return FALSE;
}

class Bootstrapper {
public:
    Bootstrapper() {
        gameDLLs = { L"Starfall.dll", L"Vivox_sdk64.dll" };
        gameExeName = L"FortniteClient-Win64-Shipping.exe";
        launcherExeName = L"FortniteLauncher.exe";
        beExeName = L"FortniteClient-Win64-Shipping_BE.exe";
    }

    bool Run() {
        // Set up Ctrl+C handler
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

        std::filesystem::path exeFolder = std::filesystem::current_path();
        std::wcout << L"[info] Bootstrapper started in: " << exeFolder.wstring() << L" (PID: " << GetCurrentProcessId() << L")\n";

        std::wstring localApp;
        if (!GetLocalAppData(localApp)) {
            std::wcerr << L"[error] Could not resolve LOCALAPPDATA\n";
            return false;
        }

        std::filesystem::path appdir = std::filesystem::path(localApp) / APP_FOLDER_NAME;

        std::string email = ReadTextFileUtf8(appdir / L"email.txt");
        std::string password = ReadTextFileUtf8(appdir / L"password.txt");
        if (email.empty() || password.empty()) {
            std::wcerr << L"[error] Missing email.txt or password.txt in " << appdir.wstring() << L"\n";
            MessageBoxW(NULL, (L"Missing email.txt or password.txt in " + appdir.wstring()).c_str(), L"Bootstrapper", MB_OK | MB_ICONERROR);
            return false;
        }
        std::wcout << L"[info] Credentials loaded.\n";

        // Launch helpers once (or on restarts if enabled)
        LaunchHelpers(appdir);

        std::wstring args(
            L"-log -epicapp=Fortnite -epicenv=Prod -epiclocale=en-us "
            L"-epicportal -skippatchcheck -nobe -fromfl=eac "
            L"-fltoken=3db3ba5dcbd2e16703f3978d -nosplash "
            L"-caldera=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9..."
            L" -AUTH_LOGIN=host@fmod.dev"
            L" -AUTH_PASSWORD=host"
            L" -AUTH_TYPE=epic"
            L" -nullrhi -nosound -unattended"
        );

        std::filesystem::path gamePath = exeFolder / gameExeName;
        if (!std::filesystem::exists(gamePath)) {
            std::wcerr << L"[error] Game exe not found: " << gamePath.wstring() << L"\n";
            return false;
        }
        std::wcout << L"[info] Game path: " << gamePath.wstring() << L"\n";

        // Infinite supervisor loop: Relaunch on every exit
        int launchCount = 0;
        while (true) {
            launchCount++;
            std::wcout << L"\n[info] === Launch #" << launchCount << " ===" << std::endl;

            // Reset state
            restartNeeded = false;
            restartCount = 0;  // Reset per full launch (no inner loop needed now)

            // Create pipe, process, etc. (same as before)
            SECURITY_ATTRIBUTES saAttr{};
            saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
            saAttr.bInheritHandle = TRUE;
            saAttr.lpSecurityDescriptor = NULL;

            HANDLE hChildStdoutRd = NULL;
            HANDLE hChildStdoutWr = NULL;

            if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
                std::wcerr << L"[fatal] CreatePipe failed. Exiting.\n";
                return false;
            }

            if (!SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0)) {
                std::wcerr << L"[fatal] SetHandleInformation failed. Exiting.\n";
                CloseHandle(hChildStdoutRd);
                CloseHandle(hChildStdoutWr);
                return false;
            }

            STARTUPINFOW si{};
            si.cb = sizeof(STARTUPINFOW);
            si.hStdOutput = hChildStdoutWr;
            si.hStdError = hChildStdoutWr;
            si.dwFlags |= STARTF_USESTDHANDLES;

            PROCESS_INFORMATION pi{};

            std::wstring cmdLine = L"\"" + gamePath.wstring() + L"\" " + args;

            if (!CreateProcessW(NULL, cmdLine.data(), NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NEW_CONSOLE, NULL, gamePath.parent_path().c_str(), &si, &pi)) {
                std::wcerr << L"[fatal] CreateProcess failed (error: " << GetLastError() << L"). Exiting.\n";
                CloseHandle(hChildStdoutRd);
                CloseHandle(hChildStdoutWr);
                return false;
            }

            CloseHandle(hChildStdoutWr);

            hProcess = pi.hProcess;
            targetPid = pi.dwProcessId;
            std::wcout << L"[info] Game launched (PID: " << targetPid << L")\n";

            // Inject DLLs with logging
            if (ENABLE_LEGACY_INJECTION) {
                for (auto& dll : gameDLLs) {
                    std::filesystem::path dllP = exeFolder / dll;
                    if (!std::filesystem::exists(dllP)) {
                        std::wcerr << L"[warn] DLL missing: " << dll << L"\n";
                        continue;
                    }
                    if (LegacyInjectDLL(targetPid, dllP.wstring())) {
                        std::wcout << L"[+] Injected: " << dll << L"\n";
                    }
                    else {
                        std::wcerr << L"[-] Injection failed: " << dll << L"\n";
                    }
                }
            }

            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);

            // Monitor thread
            std::thread monitorThread([this, hChildStdoutRd, pi, exeFolder]() {
                constexpr DWORD bufferSize = 4096;
                char buffer[bufferSize];
                DWORD bytesRead;
                std::string output;

                while (true) {
                    BOOL success = ReadFile(hChildStdoutRd, buffer, bufferSize - 1, &bytesRead, NULL);
                    if (!success || bytesRead == 0) break;
                    buffer[bytesRead] = '\0';
                    output += buffer;

                    if (output.find("current=WaitingPostMatch") != std::string::npos) {
                        std::wcout << L"[trigger] Detected restart signal. Terminating game...\n";
                        if (TerminateProcess(this->hProcess, 0)) {
                            this->restartNeeded = true;
                            this->restartReason = L"Trigger detected";
                        }
                        else {
                            std::wcerr << L"[-] TerminateProcess failed: " << GetLastError() << L"\n";
                        }
                        break;
                    }
                }
                CloseHandle(hChildStdoutRd);
                });

            std::wcout << L"[info] Monitoring game output...\n";

            // Wait for exit
            DWORD waitResult = WaitForSingleObject(hProcess, INFINITE);
            monitorThread.join();

            DWORD exitCode = 0;
            if (waitResult == WAIT_OBJECT_0) {
                GetExitCodeProcess(hProcess, &exitCode);
            }

            CloseHandle(hProcess);
            hProcess = nullptr;
            targetPid = 0;

            std::wstring reason = L"Unknown";
            bool shouldRestart = true;

            if (restartNeeded) {
                reason = restartReason;
                std::wcout << L"[restart] Game terminated early (" << reason << "). Restarting in 2s...\n";
                Sleep(2000);
            }
            else {
                // Natural exit: Always restart (customize here if needed, e.g., if (exitCode == 0) shouldRestart = false;)
                reason = L"Natural exit (code " + std::to_wstring(exitCode) + L")";
                std::wcout << L"[info] Game exited (" << reason << "). Restarting in 5s...\n";
                Sleep(5000);  // Longer delay for natural exits
                if (RELAUNCH_HELPERS_ON_RESTART) LaunchHelpers(appdir);  // Optional
            }

            if (!shouldRestart) {
                std::wcout << L"[info] Manual stop requested. Exiting bootstrapper.\n";
                return true;
            }

            // Optional: Rapid restart limit (e.g., if crashing in loop)
            if (exitCode != 0 && launchCount % 3 == 0) {  // Every 3rd non-zero exit
                std::wcout << L"[warn] Multiple crashes detected. Pausing 30s...\n";
                Sleep(30000);
            }
        }

        return true;  // Unreachable in infinite loop
    }

private:
    std::vector<std::wstring> gameDLLs;
    std::wstring gameExeName;
    std::wstring launcherExeName;
    std::wstring beExeName;
    HANDLE hProcess = nullptr;
    DWORD targetPid = 0;
    std::atomic<bool> restartNeeded{ false };
    std::wstring restartReason{ L"" };  // New: Track why restarting
    int restartCount{ 0 };
    const int maxRestarts{ 5 };  // Not used in infinite mode, but kept for future

    void LaunchHelpers(const std::filesystem::path& appdir) {
        std::wcout << L"[info] Launching helpers...\n";
        LaunchSimpleProcess(appdir / launcherExeName, L"");
        LaunchSimpleProcess(appdir / beExeName, L"");
    }

    // ... (All other private static methods unchanged: GetLocalAppData, ReadTextFileUtf8, LaunchSimpleProcess, CreateProcessSuspended, LegacyInjectDLL, MonitorProc, etc.)
    static bool GetLocalAppData(std::wstring& out) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path))) {
            out = path;
            CoTaskMemFree(path);
            return true;
        }
        return false;
    }

    static std::string ReadTextFileUtf8(const std::filesystem::path& p) {
        std::ifstream f(p);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
        return s.substr(start);
    }

    static bool LaunchSimpleProcess(const std::wstring& exePath, const std::wstring& args) {
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::wstring cmd = L"\"" + exePath + L"\"";
        if (!args.empty()) cmd += L" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        return false;
    }

    static bool LegacyInjectDLL(DWORD pid, const std::wstring& dllPath) {
        HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!proc) return false;
        size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) {
            CloseHandle(proc);
            return false;
        }
        if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, NULL)) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            CloseHandle(proc);
            return false;
        }
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadAddr = GetProcAddress(k32, "LoadLibraryW");
        if (!loadAddr) {
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
