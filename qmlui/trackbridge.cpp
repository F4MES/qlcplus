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
    , m_cdjCount(0)
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &TrackBridge::slotLaunch);
}

TrackBridge::~TrackBridge()
{
    stop();
}

void TrackBridge::setPlaces(const QString &logDir, const QString &runtimeDir)
{
    m_logDir = logDir;
    m_runtimeDir = runtimeDir;
}

void TrackBridge::setJarOverride(const QString &jar)
{
    m_jarOverride = jar;
}

void TrackBridge::restart()
{
    // R557_READER: another Beat Link Trigger jar - the helper closes the network the proper way
    // (its pipe) and starts again at once on the new one
    stop();
    m_wanted = true;
    m_failures = 0;
    m_logStart = true;               // the new jar in the log
    m_lastWhy.clear();
    slotLaunch();
}

int TrackBridge::deckMaster() const { return m_deckMaster; }
int TrackBridge::deckFollowed() const { return m_deckFollowed; }
QList<int> TrackBridge::decksHeard() const { return m_decksHeard; }
QList<int> TrackBridge::decksPlaying() const { return m_decksPlaying; }
int TrackBridge::nextDeck() const { return m_nextDeck; }
QString TrackBridge::nextState() const { return m_nextState; }
QString TrackBridge::nextTrack() const { return m_nextTrack; }

int TrackBridge::cdjCount() const
{
    return m_cdjCount;
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
    m_bltFeeds = false;                  // TRACK_APP_BLTFEEDS_R575 stop
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
    if (m_cdjCount != 0)
    {
        m_cdjCount = 0;
        emit cdjCountChanged(0);
    }
    if (m_deckMaster != 0 || m_deckFollowed != 0 || m_decksHeard.isEmpty() == false || m_decksPlaying.isEmpty() == false)
    {
        m_deckMaster = 0;                // R560_DECKS: nothing known without the helper
        m_deckFollowed = 0;
        m_decksHeard.clear();
        m_decksPlaying.clear();
        emit decksChanged();
    }
    if (m_nextDeck != 0 || m_nextState.isEmpty() == false || m_nextTrack.isEmpty() == false)
    {
        m_nextDeck = 0;                  // R561_NEXT: nor the next track
        m_nextState.clear();
        m_nextTrack.clear();
        emit decksChanged();
    }
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

    // R548_RUNTIME (Tobias 11-04: "den skal kunne deployes paa en ren pc og saa bare virke fra start"):
    // the Java and beat-link TRACK ships with (runtime\<id>: java\bin + beat-link-trigger.jar) first
    if (m_runtimeDir.isEmpty() == false)
    {
        QDir rt(m_runtimeDir);
        const QStringList javas = QStringList()
            << QStringLiteral("java/bin/javaw.exe") << QStringLiteral("java/bin/java.exe") << QStringLiteral("java/bin/java");
        QString exe;
        for (const QString &rel : javas)
        {
            if (QFileInfo::exists(rt.filePath(rel)))
            {
                exe = rt.filePath(rel);
                break;
            }
        }
        const QString jar = rt.filePath(QStringLiteral("beat-link-trigger.jar"));
        if (exe.isEmpty() == false && QFileInfo::exists(jar))
        {
            if (java.isEmpty())
                java = exe;
            if (bltJar.isEmpty())
                bltJar = jar;
        }
        // R557_READER: a newer Beat Link Trigger fetched in SETUP (cdjreader\<version>), on the runtime's Java
        if (exe.isEmpty() == false && m_jarOverride.isEmpty() == false && QFileInfo::exists(m_jarOverride)
            && qEnvironmentVariable("QLC_BRIDGE_BLT_JAR").isEmpty())
            bltJar = m_jarOverride;
    }

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
        why = QStringLiteral("no Java + beat-link: TRACK's runtime folder is missing and Beat Link Trigger is not installed");
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

    // the JVM's own output (crashes, a wrong Java) goes to bridge-console.log beside the helper's
    // bridge.log; its "STATUS cdjs=N" lines are read on the way (R548_CDJS)
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &TrackBridge::slotOutput);
    m_outBuf.clear();

    // TRACK_APP_FIX_R595: out of memory a JVM often loses one thread and keeps the rest - the socket stays up, TRACK
    // says "connected" and nothing arrives. With this it exits instead, and the restart with its back-off takes over
    QStringList args = QStringList() << QStringLiteral("-Dfile.encoding=UTF-8")
                                     << QStringLiteral("-XX:+ExitOnOutOfMemoryError");
    if (m_logDir.isEmpty() == false)
        args << QStringLiteral("-Dqlc.bridge.logdir=") + QDir::toNativeSeparators(m_logDir);
    args
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

void TrackBridge::slotOutput()
{
    if (m_process == nullptr)
        return;
    m_outBuf.append(m_process->readAllStandardOutput());

    // R548_CDJS: "STATUS cdjs=N" is read here; every other line goes to bridge-console.log
    QByteArray keep;
    int nl;
    while ((nl = m_outBuf.indexOf('\n')) >= 0)
    {
        const QByteArray raw = m_outBuf.left(nl + 1);
        m_outBuf.remove(0, nl + 1);
        const QByteArray line = raw.trimmed();
        if (line.startsWith("STATUS cdjs="))
        {
            if (m_bltFeeds)
            {
                m_bltFeeds = false;      // TRACK_APP_BLTFEEDS_R575: the helper reads the CDJs itself again
                emit cdjCountChanged(m_cdjCount);
            }
            bool ok = false;
            const int n = line.mid(12).trimmed().toInt(&ok);
            if (ok && n != m_cdjCount)
            {
                m_cdjCount = n;
                emit cdjCountChanged(n);
            }
            if (ok)
                emit statusSeen();       // R557_READER
        }
        else if (line.startsWith("DECKS "))
        {
            // R560_DECKS: "DECKS m=2 f=2 h=2,3 p=2,3" - the master, the deck the lights follow, the
            // decks the room hears and those playing (0 / empty: none)
            QHash<QString, QString> kv;
            foreach (const QByteArray &part, line.mid(6).split(' '))
            {
                const int eq = part.indexOf('=');
                if (eq > 0)
                    kv.insert(QString::fromLatin1(part.left(eq)), QString::fromLatin1(part.mid(eq + 1)));
            }
            auto list = [](const QString &s) {
                QList<int> out;
                foreach (const QString &v, s.split(QLatin1Char(','), Qt::SkipEmptyParts))
                {
                    bool ok = false;
                    const int n = v.toInt(&ok);
                    if (ok && n >= 1 && n <= 6)
                        out.append(n);
                }
                return out;
            };
            // R566_FIX: a deck is 1-6 or none, as h= and p= are read
            int m = kv.value(QStringLiteral("m")).toInt();
            int f = kv.value(QStringLiteral("f")).toInt();
            if (m < 0 || m > 6)
                m = 0;
            if (f < 0 || f > 6)
                f = 0;
            const QList<int> h = list(kv.value(QStringLiteral("h")));
            const QList<int> pl = list(kv.value(QStringLiteral("p")));
            if (m != m_deckMaster || f != m_deckFollowed || h != m_decksHeard || pl != m_decksPlaying)
            {
                m_deckMaster = m;
                m_deckFollowed = f;
                m_decksHeard = h;
                m_decksPlaying = pl;
                emit decksChanged();
            }
        }
        else if (line.startsWith("NEXT "))
        {
            // R561_NEXT: "NEXT d=1 s=analysing t=<title>" - the deck the next track is probably on (0 none),
            // whether the lights have its sections yet, and its title (the rest of the line, UTF-8)
            const int ti = line.indexOf(" t=");
            const QByteArray head = ti >= 0 ? line.mid(5, ti - 5) : line.mid(5);
            int d = 0;
            QString st;
            foreach (const QByteArray &part, head.split(' '))
            {
                if (part.startsWith("d="))
                    d = part.mid(2).toInt();
                else if (part.startsWith("s="))
                    st = QString::fromLatin1(part.mid(2));
            }
            if (d < 0 || d > 6 || (st != QLatin1String("ready") && st != QLatin1String("analysing") && st != QLatin1String("failed")))
            {
                d = 0;
                st.clear();
            }
            const QString t = d > 0 && ti >= 0 ? QString::fromUtf8(line.mid(ti + 3)).trimmed() : QString();
            if (d != m_nextDeck || st != m_nextState || t != m_nextTrack)
            {
                m_nextDeck = d;
                m_nextState = st;
                m_nextTrack = t;
                emit decksChanged();
            }
        }
        else
            keep.append(raw);
    }
    if (m_outBuf.size() > 65536)
    {
        keep.append(m_outBuf);      // no line end in 64 kB: not ours, keep it in the log
        m_outBuf.clear();
    }
    if (keep.isEmpty())
        return;

    QString dir = m_logDir;
    if (dir.isEmpty())
    {
        dir = qEnvironmentVariable("LOCALAPPDATA");
        dir = dir.isEmpty() ? QDir::homePath() + QStringLiteral("/.qlc-rig-update")
                            : QDir(dir).filePath(QStringLiteral("qlc-rig-update"));
    }
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("bridge-console.log")));
    if (f.size() > 2000000)
        f.remove();                 // the JVM's own lines only: a fresh file past 2 MB
    if (f.open(QIODevice::Append))
        f.write(keep);
}

void TrackBridge::slotFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_cdjCount != 0)
    {
        m_cdjCount = 0;
        emit cdjCountChanged(0);
    }
    // R565_FIX: the helper is gone - what it said about the decks and the next track goes with it (a crash
    // left the page on the last DECKS / NEXT until a new helper said otherwise)
    if (m_deckMaster != 0 || m_deckFollowed != 0 || m_decksHeard.isEmpty() == false || m_decksPlaying.isEmpty() == false
        || m_nextDeck != 0 || m_nextState.isEmpty() == false || m_nextTrack.isEmpty() == false)
    {
        m_deckMaster = 0;
        m_deckFollowed = 0;
        m_decksHeard.clear();
        m_decksPlaying.clear();
        m_nextDeck = 0;
        m_nextState.clear();
        m_nextTrack.clear();
        emit decksChanged();
    }
    emit helperExited(status == QProcess::NormalExit ? exitCode : -1);   // R557_READER
    if (m_wanted == false)
        return;
    if (status == QProcess::NormalExit && exitCode == kBltRunning)
    {
        m_failures = 0;
        m_logStart = false;
        m_waitedForBlt = true;
        if (m_bltFeeds == false)
        {
            m_bltFeeds = true;           // TRACK_APP_BLTFEEDS_R575: BLT feeds TRACK - no "NO CDJs" for that
            emit cdjCountChanged(m_cdjCount);
        }
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
    writeLog(m_logDir, what);
}

void TrackBridge::writeLog(const QString &logDir, const QString &what)
{
    // R548_PLACES: TRACK's own logs folder (installed TRACK), else beside QLC+'s and the BLT guard's lines
    QString dir = logDir;
    QString name = QStringLiteral("track.log");
    if (dir.isEmpty())
    {
        dir = qEnvironmentVariable("LOCALAPPDATA");
        dir = dir.isEmpty() ? QDir::homePath() + QStringLiteral("/.qlc-rig-update")
                            : QDir(dir).filePath(QStringLiteral("qlc-rig-update"));
        name = QStringLiteral("startup.log");
    }
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(name));
    // R566_FIX (bug round 566): track.log grew for as long as TRACK was installed (bridge.log and
    // bridge-console.log stop at 2 MB) - past 2 MB it becomes track.log.old (one kept) and starts again
    if (f.size() > 2000000)
    {
        QFile::remove(f.fileName() + QStringLiteral(".old"));
        QFile::rename(f.fileName(), f.fileName() + QStringLiteral(".old"));
    }
    if (f.open(QIODevice::Append | QIODevice::Text) == false)
        return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        << QStringLiteral("  TRACK  ") << what << '\n';
}
