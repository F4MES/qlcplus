/* Rig startup guard. Shared by the classic and QML applications on Windows. */
#ifndef QLCSINGLEINSTANCE_H
#define QLCSINGLEINSTANCE_H

#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cwchar>
#endif

class QlcSingleInstance final
{
public:
    QlcSingleInstance()
    {
#ifdef _WIN32
        // One identity across installations, UI variants and Windows sessions.
        // Keep a handle, not mutex ownership: the OS closes it even after a crash.
        m_handle = CreateMutexW(nullptr, FALSE, L"Global\\QLCPlus.F4MES.Rig.Application");
        const DWORD error = GetLastError();
        if (!m_handle)
        {
            m_exitCode = 1; // Fail closed, including a different user's denied handle.
            std::fprintf(stderr, "QLC+: cannot acquire startup guard (Windows error %lu).\n",
                         static_cast<unsigned long>(error));
            writeLog("refused - startup guard error", error);
            notify(false, error);
        }
        else if (error == ERROR_ALREADY_EXISTS)
        {
            m_exitCode = 0;
            std::fputs("QLC+ is already running; extra start ignored.\n", stderr);
            // R492_GUARD_NOTICE: the handle goes BEFORE the notice - a notice left open
            // must never keep the guard alive and block the next real start
            CloseHandle(m_handle);
            m_handle = nullptr;
            writeLog("ignored - QLC+ is already running", 0);
            notify(true, 0);
        }
        else
        {
            m_allowed = true;
            writeLog("started", 0);
        }
#else
        m_allowed = true; // Only the Windows lighting PC is restricted.
#endif
    }

    ~QlcSingleInstance()
    {
#ifdef _WIN32
        if (m_handle)
            CloseHandle(m_handle);
#endif
    }

    QlcSingleInstance(const QlcSingleInstance &) = delete;
    QlcSingleInstance &operator=(const QlcSingleInstance &) = delete;
    bool allowed() const { return m_allowed; }
    int exitCode() const { return m_exitCode; }

private:
#ifdef _WIN32
    // QLC_GUARD_QUIET=1 (the guard tests, CI): no window, ever.
    static bool quiet()
    {
        wchar_t value[8] = { 0 };
        const DWORD n = GetEnvironmentVariableW(L"QLC_GUARD_QUIET", value, 8);
        return n > 0 && n < 8 && value[0] != L'0';
    }

    static void utf8(const wchar_t *text, char *out, int size)
    {
        out[0] = 0;
        if (text != nullptr && WideCharToMultiByte(CP_UTF8, 0, text, -1, out, size, nullptr, nullptr) <= 0)
            out[0] = 0;
    }

    // R492_GUARD_LOG (Tobias 10-22: QLC+ set to start with Windows did not seem to start):
    // one line per start in %LOCALAPPDATA%\qlc-rig-update\startup.log - when, how long
    // after Windows started, what the guard did, and the command line that started it
    static void writeLog(const char *what, DWORD error)
    {
        wchar_t dir[MAX_PATH] = { 0 };
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return;
        wchar_t path[MAX_PATH + 64] = { 0 };
        std::swprintf(path, MAX_PATH + 64, L"%ls\\qlc-rig-update", dir);
        CreateDirectoryW(path, nullptr);
        std::swprintf(path, MAX_PATH + 64, L"%ls\\qlc-rig-update\\startup.log", dir);
        HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        SYSTEMTIME now;
        GetLocalTime(&now);
        static char command[1536];
        utf8(GetCommandLineW(), command, int(sizeof(command)));
        char line[2048];
        const int length = std::snprintf(line, sizeof(line),
            "%04u-%02u-%02u %02u:%02u:%02u  QLC+  %s (error %lu)  pid %lu  %lu s after Windows started  %s\r\n",
            unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
            unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond), what,
            static_cast<unsigned long>(error), static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long>(GetTickCount() / 1000), command);
        if (length > 0)
        {
            DWORD written = 0;
            WriteFile(file, line, DWORD(length < int(sizeof(line)) ? length : int(sizeof(line)) - 1), &written, nullptr);
        }
        CloseHandle(file);
    }

    // the running QLC+'s main window to the front: what a second double-click meant
    static BOOL CALLBACK front(HWND window, LPARAM self)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == DWORD(self) || IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr)
            return TRUE;
        wchar_t title[256] = { 0 };
        GetWindowTextW(window, title, 256);
        if (std::wcsstr(title, L"Q Light Controller Plus") == nullptr)
            return TRUE;
        if (IsIconic(window))
            ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return FALSE;
    }

    // R492_GUARD_NOTICE (Tobias 10-22: "ja"): a start the guard stops is no longer silent
    static void notify(bool running, DWORD error)
    {
        if (quiet())
            return;
        if (running)
        {
            EnumWindows(front, LPARAM(GetCurrentProcessId()));
            MessageBoxW(nullptr, L"QLC+ is already running.\n\nThis start was ignored - the open QLC+ window is in front.",
                        L"QLC+", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
            return;
        }
        wchar_t text[256] = { 0 };
        std::swprintf(text, 256, L"QLC+ could not start: its startup guard failed (Windows error %lu).\n\n"
                                 L"Restart the PC, then start QLC+ again.", static_cast<unsigned long>(error));
        MessageBoxW(nullptr, text, L"QLC+", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }
#endif

    bool m_allowed = false;
    int m_exitCode = 1;
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#endif
};
#endif
