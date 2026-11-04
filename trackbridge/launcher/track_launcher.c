/*
  TRACK.exe - the TRACK app's launcher (track-app branch, runde 548).

  Sits in %LOCALAPPDATA%\TRACK (the desktop shortcut points here) and starts the build
  current.txt names:  app\<build>\TRACK-engine.exe --track-app <its own arguments>.
  An update only writes current.txt (TRACK itself - qmlui/trackapp.cpp), so this file
  never changes under a running TRACK.

  A build started for the first time (pending.txt = current.txt) that stops with an
  error within 20 s is not kept: current.txt goes back to previous.txt and that build
  starts instead. Exit code 0 (QLC+'s "already running" guard, or a quick quit) is not
  an error. After 20 s alive the build is no longer pending.

  Licensed under the Apache License, Version 2.0.

      x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows -o TRACK-launcher.exe track_launcher.c
*/
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>

#define PATHLEN 1024
#define REVLEN 80

static wchar_t g_root[PATHLEN];

static void join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, PATHLEN - 1, L"%s\\%s", a, b);
    out[PATHLEN - 1] = 0;
}

static BOOL exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* the first line of root\name, only [A-Za-z0-9._-] (a build id) */
static void readRev(const wchar_t *name, wchar_t *out)
{
    wchar_t path[PATHLEN];
    char buf[REVLEN];
    DWORD got = 0;
    int i, n = 0;
    out[0] = 0;
    join(path, g_root, name);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!ReadFile(h, buf, REVLEN - 1, &got, NULL))
        got = 0;
    CloseHandle(h);
    for (i = 0; i < (int)got; i++)
    {
        char c = buf[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            out[n++] = (wchar_t)c;
        else if (c == '\r' || c == '\n')
            break;
        else
        {
            n = 0;          /* not a build id */
            break;
        }
    }
    out[n] = 0;
}

static void writeRev(const wchar_t *name, const wchar_t *rev)
{
    wchar_t path[PATHLEN];
    char buf[REVLEN + 2];
    DWORD put = 0;
    int i, n = 0;
    join(path, g_root, name);
    for (i = 0; rev[i] && n < REVLEN; i++)
        buf[n++] = (char)rev[i];
    buf[n++] = '\n';
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, buf, (DWORD)n, &put, NULL);
    CloseHandle(h);
}

static void logLine(const wchar_t *what)
{
    wchar_t path[PATHLEN], dir[PATHLEN], line[PATHLEN];
    char utf8[PATHLEN * 3];
    SYSTEMTIME t;
    DWORD put = 0;
    int n;
    join(dir, g_root, L"logs");
    CreateDirectoryW(dir, NULL);
    join(path, dir, L"track.log");
    GetLocalTime(&t);
    _snwprintf(line, PATHLEN - 1, L"%04d-%02d-%02d %02d:%02d:%02d  TRACK  launcher: %s\r\n",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what);
    line[PATHLEN - 1] = 0;
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 1)
        return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, utf8, (DWORD)(n - 1), &put, NULL);
    CloseHandle(h);
}

static void engineOf(const wchar_t *rev, wchar_t *exe, wchar_t *dir)
{
    wchar_t apps[PATHLEN];
    join(apps, g_root, L"app");
    join(dir, apps, rev);
    join(exe, dir, L"TRACK-engine.exe");
}

/* everything after our own program name on the command line */
static const wchar_t *tailArgs(void)
{
    const wchar_t *c = GetCommandLineW();
    if (*c == L'"')
    {
        c++;
        while (*c && *c != L'"')
            c++;
        if (*c == L'"')
            c++;
    }
    else
    {
        while (*c && *c != L' ' && *c != L'\t')
            c++;
    }
    while (*c == L' ' || *c == L'\t')
        c++;
    return c;
}

static BOOL startEngine(const wchar_t *rev, PROCESS_INFORMATION *pi)
{
    static wchar_t cmd[32768];
    wchar_t exe[PATHLEN], dir[PATHLEN];
    STARTUPINFOW si;
    engineOf(rev, exe, dir);
    if (!exists(exe))
        return FALSE;
    _snwprintf(cmd, 32767, L"\"%s\" --track-app %s", exe, tailArgs());
    cmd[32767] = 0;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(pi, sizeof(*pi));
    return CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, pi);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    wchar_t current[REVLEN], previous[REVLEN], pending[REVLEN], msg[PATHLEN];
    wchar_t exe[PATHLEN], dir[PATHLEN];
    PROCESS_INFORMATION pi;
    wchar_t *slash;
    (void)inst; (void)prev; (void)cmdLine; (void)show;

    GetModuleFileNameW(NULL, g_root, PATHLEN);
    slash = wcsrchr(g_root, L'\\');
    if (slash)
        *slash = 0;

    readRev(L"current.txt", current);
    readRev(L"previous.txt", previous);
    readRev(L"pending.txt", pending);

    engineOf(current, exe, dir);
    if (current[0] == 0 || !exists(exe))
    {
        engineOf(previous, exe, dir);
        if (previous[0] == 0 || !exists(exe))
        {
            MessageBoxW(NULL, L"TRACK is not installed in this folder.\n\nRun get-track-app.cmd again.",
                        L"TRACK", MB_OK | MB_ICONERROR);
            return 1;
        }
        _snwprintf(msg, PATHLEN - 1, L"build %s is missing - starting %s", current, previous);
        msg[PATHLEN - 1] = 0;
        logLine(msg);
        wcscpy(current, previous);
        writeRev(L"current.txt", current);
    }

    if (!startEngine(current, &pi))
    {
        MessageBoxW(NULL, L"TRACK could not start.\n\nSee logs\\track.log in the TRACK folder.", L"TRACK",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    if (pending[0] != 0 && wcscmp(pending, current) == 0)
    {
        DWORD code = 0;
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code)
            && code != 0 && previous[0] != 0 && wcscmp(previous, current) != 0)
        {
            engineOf(previous, exe, dir);
            if (exists(exe))
            {
                _snwprintf(msg, PATHLEN - 1, L"build %s stopped at its first start (code 0x%lX) - back to %s",
                           current, (unsigned long)code, previous);
                msg[PATHLEN - 1] = 0;
                logLine(msg);
                writeRev(L"current.txt", previous);
                {
                    wchar_t p[PATHLEN];
                    join(p, g_root, L"pending.txt");
                    DeleteFileW(p);
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                if (!startEngine(previous, &pi))
                    return 1;
            }
        }
        else
        {
            wchar_t p[PATHLEN];
            join(p, g_root, L"pending.txt");
            DeleteFileW(p);
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
