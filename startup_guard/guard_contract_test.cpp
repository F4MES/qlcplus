#include "qlcsingleinstance.h"

int main()
{
#ifdef _WIN32
    // A conflicting kernel object must cause a closed gate, not an unlocked start.
    HANDLE collision = CreateEventW(nullptr, FALSE, FALSE,
                                   L"Global\\QLCPlus.F4MES.Rig.Application");
    if (!collision)
        return 1;
    {
        QlcSingleInstance denied;
        if (denied.allowed() || denied.exitCode() != 1)
            return 2;
    }
    CloseHandle(collision);
    {
        QlcSingleInstance first;
        if (!first.allowed())
            return 3;
        QlcSingleInstance duplicate;
        if (duplicate.allowed() || duplicate.exitCode() != 0)
            return 4;
    }
#endif
    QlcSingleInstance afterExit;
    return afterExit.allowed() ? 0 : 5;
}
