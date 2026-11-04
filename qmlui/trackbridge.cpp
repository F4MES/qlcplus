/*
  Q Light Controller Plus
  trackbridge.cpp

  TRACK app (track-app branch, Tobias 11-04): QLC+ starts and watches its own
  beat-link helper instead of Beat Link Trigger. See trackbridge.h and
  trackbridge/README.md.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTextStream>

#include "trackbridge.h"

TrackBridge::TrackBridge(QObject *parent)
    : QObject(parent)
    , m_process(nullptr)
    , m_wanted(false)
    , m_logStart(true)
    , m_waitedForBlt(false)
    , m_failures(0)
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &TrackBridge::slotLaunch);
}

TrackBridge::~TrackBridge()
{
    stop();
}

void TrackBridge::start()
{
    if (m_wanted)
        return;
    m_wanted = true;
    m_failures = 0;
    log(QStringLiteral("TRACK app: QLC+ runs its own beat-link helper (no Beat Link Trigger needed)"));
    slotLaunch();
}

void TrackBridge::stop()
{
    m_wanted = false;
    m_retry.stop();
    if (m_process == nullptr)
        return;
    if (m_process->state() != QProcess::NotRunning)
    {
        // the helper stops itself when its stdin closes (--parent-pipe): beat-link
        // leaves the network the proper way. Killed only if it does not.
        m_process->closeWriteChannel();
        if (m_process->waitForFinished(3000) == false)
        {
            m_process->kill();
            m_process->waitForFinished(2000);
        }
        log(QStringLiteral("beat-link helper stopped with QLC+"));
    }
    m_process->deleteLater();
    m_process = nullptr;
}

/*********************************************************************
 * Finding Beat Link Trigger's Java and jar
 *********************************************************************/

QStringList TrackBridge::bltRoots() const
{
    QStringList roots;
#if defined(Q_OS_WIN)
    // the same places tools/install-blt-guard.ps1 looks
    const QStringList bases = QStringList()
        << qEnvironmentVariable("ProgramFiles")
        << qEnvironmentVariable("ProgramFiles(x86)")
        << qEnvironmentVariable("LOCALAPPDATA");
    const QStringList relatives = QStringList()
        << QStringLiteral("Beat Link Trigger")
        << QStringLiteral("Programs/Beat Link Trigger")
        << QStringLiteral("Deep Symmetry/Beat Link Trigger");
    for (const QString &base : bases)
    {
        if (base.isEmpty())
            continue;
        for (const QString &rel : relatives)
            roots << QDir(base).filePath(rel);
    }
    const QStringList uninstallKeys = QStringList()
        << QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall")
        << QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall")
        << QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    for (const QString &key : uninstallKeys)
    {
        QSettings reg(key, QSettings::NativeFormat);
        const QStringList entries = reg.childGroups();
        for (const QString &entry : entries)
        {
            reg.beginGroup(entry);
            const QString name = reg.value(QStringLiteral("DisplayName")).toString();
            const QString place = reg.value(QStringLiteral("InstallLocation")).toString();
            reg.endGroup();
            if (name.contains(QStringLiteral("Beat Link Trigger"), Qt::CaseInsensitive) && place.isEmpty() == false)
                roots << QDir::fromNativeSeparators(place);
        }
    }
#elif defined(Q_OS_MACOS)
    roots << QStringLiteral("/Applications/Beat Link Trigger.app/Contents")
          << QDir::homePath() + QStringLiteral("/Applications/Beat Link Trigger.app/Contents");
#else
    roots << QStringLiteral("/opt/beat-link-trigger")
          << QStringLiteral("/opt/beatlinktrigger");
#endif
    roots.removeDuplicates();
    return roots;
}

bool TrackBridge::locate(QString &java, QStringList &classpath, QString &why) const
{
    java.clear();
    classpath.clear();

    // the helper itself, beside QLC+ (or in the bundle's Resources)
    const QString appDir = QCoreApplication::applicationDirPath();
    QString helper = QDir(appDir).filePath(QStringLiteral("trackbridge.jar"));
    if (QFileInfo::exists(helper) == false)
        helper = QDir(appDir).filePath(QStringLiteral("../Resources/trackbridge.jar"));
    if (QFileInfo::exists(helper) == false)
    {
        why = QStringLiteral("trackbridge.jar is not beside QLC+ (%1)").arg(QDir::toNativeSeparators(appDir));
        return false;
    }

    QString bltJar = qEnvironmentVariable("QLC_BRIDGE_BLT_JAR");
    java = qEnvironmentVariable("QLC_BRIDGE_JAVA");

    if (bltJar.isEmpty() || java.isEmpty())
    {
        const QStringList roots = bltRoots();
        for (const QString &root : roots)
        {
            if (QFileInfo(root).isDir() == false)
                continue;
            QString jar;
            const QStringList appDirs = QStringList()
                << QDir(root).filePath(QStringLiteral("app"))
                << QDir(root).filePath(QStringLiteral("lib/app"));
            for (const QString &dirName : appDirs)
            {
                QDir dir(dirName);
                if (dir.exists() == false)
                    continue;
                // jpackage's .cfg names the jar: app.mainjar= or (with the startup guard) app.classpath=
                const QStringList cfgs = dir.entryList(QStringList() << QStringLiteral("*.cfg"), QDir::Files, QDir::Name);
                for (const QString &cfgName : cfgs)
                {
                    QFile cfg(dir.filePath(cfgName));
                    if (cfg.open(QIODevice::ReadOnly | QIODevice::Text) == false)
                        continue;
                    QTextStream in(&cfg);
                    while (in.atEnd() == false && jar.isEmpty())
                    {
                        const QString line = in.readLine().trimmed();
                        QString value;
                        if (line.startsWith(QStringLiteral("app.mainjar=")))
                            value = line.mid(12);
                        else if (line.startsWith(QStringLiteral("app.classpath=")))
                            value = line.mid(14);
                        if (value.isEmpty())
                            continue;
                        value.replace(QStringLiteral("$APPDIR"), dir.absolutePath());
                        value = QDir::fromNativeSeparators(value);
                        const QString base = QFileInfo(value).fileName();
                        if (base.startsWith(QStringLiteral("qlc-blt-guard"), Qt::CaseInsensitive) == false
                            && base.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                            && QFileInfo::exists(value))
                            jar = value;
                    }
                }
                if (jar.isEmpty())
                {
                    const QStringList jars = dir.entryList(QStringList() << QStringLiteral("beat-link-trigger*.jar"),
                                                           QDir::Files, QDir::Name);
                    if (jars.isEmpty() == false)
                        jar = dir.filePath(jars.last());
                }
                if (jar.isEmpty() == false)
                    break;
            }
            if (jar.isEmpty())
                continue;

            QString exe;
            const QStringList javas = QStringList()
                << QStringLiteral("runtime/bin/javaw.exe")
                << QStringLiteral("runtime/bin/java.exe")
                << QStringLiteral("runtime/Contents/Home/bin/java")
                << QStringLiteral("runtime/bin/java")
                << QStringLiteral("lib/runtime/bin/java");
            for (const QString &rel : javas)
            {
                if (QFileInfo::exists(QDir(root).filePath(rel)))
                {
                    exe = QDir(root).filePath(rel);
                    break;
                }
            }
            if (exe.isEmpty())
                continue;

            if (bltJar.isEmpty())
                bltJar = jar;
            if (java.isEmpty())
                java = exe;
            break;
        }
    }

    if (bltJar.isEmpty() || java.isEmpty())
    {
        why = QStringLiteral("Beat Link Trigger is not installed where TRACK looks (its Java and beat-link are needed)");
        return false;
    }

    // a newer beat-link (e.g. for the CDJ-3000X) goes in trackbridge-lib and wins over BLT's own
    QDir lib(QDir(appDir).filePath(QStringLiteral("trackbridge-lib")));
    if (lib.exists())
    {
        const QStringList jars = lib.entryList(QStringList() << QStringLiteral("*.jar"), QDir::Files, QDir::Name);
        for (const QString &j : jars)
            classpath << lib.filePath(j);
    }
    classpath << bltJar << helper;
    return true;
}

/*********************************************************************
 * Running it
 *********************************************************************/

void TrackBridge::slotLaunch()
{
    if (m_wanted == false)
        return;
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning)
        return;

    QString java, why;
    QStringList classpath;
    if (locate(java, classpath, why) == false)
    {
        retryIn(60000, why);
        return;
    }

    if (m_process != nullptr)
        m_process->deleteLater();
    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &TrackBridge::slotFinished);
    connect(m_process, &QProcess::errorOccurred, this, &TrackBridge::slotErrorOccurred);

    // the JVM's own output (crashes, a wrong Java) beside the helper's bridge.log
    QString logDir = qEnvironmentVariable("LOCALAPPDATA");
    logDir = logDir.isEmpty() ? QDir::homePath() + QStringLiteral("/.qlc-rig-update")
                              : QDir(logDir).filePath(QStringLiteral("qlc-rig-update"));
    QDir().mkpath(logDir);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setStandardOutputFile(QDir(logDir).filePath(QStringLiteral("bridge-console.log")), QIODevice::Append);

    const QStringList args = QStringList()
        << QStringLiteral("-Dfile.encoding=UTF-8")
        << QStringLiteral("-cp") << classpath.join(QDir::listSeparator())
        << QStringLiteral("clojure.main") << QStringLiteral("-m") << QStringLiteral("qlc.bridge")
        << QStringLiteral("--parent-pipe");
    m_process->start(java, args);
    m_started.start();
    // logged once, and again after a stop - not on every try while BLT holds the lock
    if (m_logStart)
        log(QStringLiteral("beat-link helper starting: %1 (%2)").arg(QDir::toNativeSeparators(java),
                                                                     QDir::toNativeSeparators(classpath.join(QStringLiteral(" + ")))));
    m_logStart = false;
    // after a wait for Beat Link Trigger: said once the helper has held on for 10 s
    if (m_waitedForBlt)
    {
        QProcess *started = m_process;
        QTimer::singleShot(10000, this, [this, started]() {
            if (m_process == started && started->state() == QProcess::Running && m_waitedForBlt)
            {
                m_waitedForBlt = false;
                m_lastWhy.clear();
                log(QStringLiteral("Beat Link Trigger is closed - TRACK's beat-link helper took over"));
            }
        });
    }
}

void TrackBridge::slotFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_wanted == false)
        return;
    if (status == QProcess::NormalExit && exitCode == kBltRunning)
    {
        m_failures = 0;
        m_logStart = false;
        m_waitedForBlt = true;
        retryIn(15000, QStringLiteral("Beat Link Trigger is running - it feeds TRACK; the helper waits until it is closed"));
        return;
    }
    // a helper that ran a while is a fresh start, not a crash loop
    if (m_started.isValid() && m_started.elapsed() > 60000)
    {
        m_failures = 0;
        m_lastWhy.clear();
    }
    m_failures++;
    m_logStart = true;
    const int delay = qMin(30000, 2000 << qMin(m_failures - 1, 4));
    retryIn(delay, QStringLiteral("beat-link helper stopped (%1 %2) - starting it again")
                       .arg(status == QProcess::NormalExit ? QStringLiteral("code") : QStringLiteral("crash"))
                       .arg(exitCode));
}

void TrackBridge::slotErrorOccurred(QProcess::ProcessError error)
{
    if (m_wanted == false || error != QProcess::FailedToStart)
        return;     // a crash also ends in finished()
    retryIn(30000, QStringLiteral("beat-link helper could not start: %1").arg(m_process ? m_process->errorString() : QString()));
}

void TrackBridge::retryIn(int ms, const QString &why)
{
    if (why != m_lastWhy)
    {
        log(QStringLiteral("%1 (again in %2 s)").arg(why).arg(ms / 1000));
        m_lastWhy = why;
    }
    m_retry.start(ms);
}

void TrackBridge::log(const QString &what) const
{
    // one line per event beside QLC+'s and the BLT guard's lines: never a reason to stop
    QString dir = qEnvironmentVariable("LOCALAPPDATA");
    dir = dir.isEmpty() ? QDir::homePath() + QStringLiteral("/.qlc-rig-update")
                        : QDir(dir).filePath(QStringLiteral("qlc-rig-update"));
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("startup.log")));
    if (f.open(QIODevice::Append | QIODevice::Text) == false)
        return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        << QStringLiteral("  TRACK  ") << what << '\n';
}
