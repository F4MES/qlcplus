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
        }
        else if (error == ERROR_ALREADY_EXISTS)
        {
            m_exitCode = 0;
            std::fputs("QLC+ is already running; extra start ignored.\n", stderr);
        }
        else
            m_allowed = true;
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
    bool m_allowed = false;
    int m_exitCode = 1;
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#endif
};
#endif
