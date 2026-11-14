/*
  Q Light Controller Plus
  trackreader.cpp

  The TRACK app's CDJ reader updates (runde 557) - see trackreader.h.

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

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "trackbridge.h"
#include "trackreader.h"

namespace
{
const int kFirstCheckMs = 2 * 60 * 1000;
const int kCheckEveryMs = 6 * 60 * 60 * 1000;
const int kWatchMs = 60 * 1000;         // the new jar has this long to show the CDJ count
const int kSettleMs = 15 * 1000;        // ... after running at least this long
const qint64 kHashChunk = 4 * 1024 * 1024;
const char *kBundled = "bundled";
}

TrackReader::TrackReader(TrackBridge *bridge, const QString &root, const QString &logDir,
                         const QString &bundledJar, const QString &bundledVersion, QObject *parent)
    : QObject(parent)
    , m_bridge(bridge)
    , m_available(root.isEmpty() == false && bridge != nullptr)
    , m_root(root)
    , m_logDir(logDir)
    , m_bundledJar(bundledJar)
    , m_bundledVersion(bundledVersion)
    , m_candReady(false)
    , m_step(0)
    , m_proc(nullptr)
    , m_hash(QCryptographicHash::Sha256)
    , m_watching(false)
    , m_rollingBack(false)
{
    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, &TrackReader::slotCheck);
    m_watch.setSingleShot(true);
    connect(&m_watch, &QTimer::timeout, this, &TrackReader::slotWatchTimeout);
    if (m_bridge != nullptr)
    {
        connect(m_bridge, &TrackBridge::statusSeen, this, &TrackReader::slotStatus);
        connect(m_bridge, &TrackBridge::helperExited, this, &TrackReader::slotHelperExit);
    }
    if (m_available == false)
        return;
    m_current = readLine(file(QStringLiteral("current.txt")));
    m_previous = readLine(file(QStringLiteral("previous.txt")));
    // a jar that is gone, or a runtime that now brings the same or a newer one: the runtime's own
    if (m_current.isEmpty() == false && QFileInfo::exists(jarOf(m_current)) == false)
    {
        log(QStringLiteral("CDJ reader: %1 is gone - the runtime's own %2").arg(m_current, m_bundledVersion));
        m_current.clear();
        writeLine(file(QStringLiteral("current.txt")), QString());
    }
    if (m_current.isEmpty() == false && m_bundledVersion.isEmpty() == false
        && compareVersions(m_bundledVersion, m_current) >= 0)
    {
        log(QStringLiteral("CDJ reader: TRACK's runtime brings %1 (was %2) - back on the runtime's own")
                .arg(m_bundledVersion, m_current));
        m_previous = m_current;
        m_current.clear();
        writeLine(file(QStringLiteral("current.txt")), QString());
        writeLine(file(QStringLiteral("previous.txt")), m_previous);
    }
    if (m_previous != QLatin1String(kBundled) && m_previous.isEmpty() == false
        && QFileInfo::exists(jarOf(m_previous)) == false)
        m_previous.clear();
}

TrackReader::~TrackReader()
{
    m_checkTimer.stop();
    m_watch.stop();
    if (m_proc != nullptr && m_proc->state() != QProcess::NotRunning)
    {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

/*********************************************************************
 * Small things
 *********************************************************************/

QString TrackReader::file(const QString &rel) const
{
    return QDir(m_root).filePath(QStringLiteral("cdjreader/") + rel);
}

QString TrackReader::jarOf(const QString &version) const
{
    return file(version + QStringLiteral("/beat-link-trigger.jar"));
}

QString TrackReader::readLine(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text) == false)
        return QString();
    return QString::fromUtf8(f.readLine(256)).trimmed();
}

bool TrackReader::writeLine(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text) == false)
        return false;
    f.write(text.toUtf8() + "\n");
    return f.commit();
}

QString TrackReader::curl() const
{
#if defined(Q_OS_WIN)
    return QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/curl.exe"));
#else
    return QStringLiteral("curl");
#endif
}

QString TrackReader::apiUrl() const
{
    // QLC_TRACK_BLT_API: another list (tests)
    const QString env = qEnvironmentVariable("QLC_TRACK_BLT_API");
    return env.isEmpty() ? QStringLiteral("https://api.github.com/repos/Deep-Symmetry/beat-link-trigger/releases?per_page=10")
                         : env;
}

void TrackReader::log(const QString &what) const
{
    TrackBridge::writeLog(m_logDir, what);
}

void TrackReader::setStep(int step, const QString &note)
{
    m_step = step;
    m_note = note;
    emit changed();
}

int TrackReader::compareVersions(const QString &a, const QString &b)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)(?:\\.(\\d+))?(?:\\.(\\d+))?"));
    auto key = [](const QString &v) {
        QList<int> k;
        const QRegularExpressionMatch m = re.match(v);
        for (int i = 1; i <= 3; i++)
            k << (m.hasMatch() && m.captured(i).isEmpty() == false ? m.captured(i).toInt() : 0);
        const QString low = v.toLower();
        // a preview / snapshot / rc comes before its own release
        k << ((low.contains(QStringLiteral("preview")) || low.contains(QStringLiteral("snapshot"))
               || low.contains(QStringLiteral("rc")) || low.contains(QStringLiteral("beta"))) ? 0 : 1);
        return k;
    };
    const QList<int> ka = key(a), kb = key(b);
    for (int i = 0; i < ka.size(); i++)
        if (ka.at(i) != kb.at(i))
            return ka.at(i) < kb.at(i) ? -1 : 1;
    return 0;
}

QString TrackReader::versionOfRuntime(const QString &runtimeId)
{
    const int at = runtimeId.lastIndexOf(QStringLiteral("-blt"));
    return at >= 0 ? runtimeId.mid(at + 4) : QString();
}

/*********************************************************************
 * Properties
 *********************************************************************/

bool TrackReader::available() const { return m_available; }
QString TrackReader::version() const { return m_current.isEmpty() ? m_bundledVersion : m_current; }
bool TrackReader::bundled() const { return m_current.isEmpty(); }
QString TrackReader::bundledVersion() const { return m_bundledVersion; }
QString TrackReader::latestStable() const { return m_stable.version; }
QString TrackReader::latestPreview() const { return m_preview.version; }
QString TrackReader::candidate() const { return m_cand.version; }
bool TrackReader::candidatePreview() const { return m_cand.preview; }
bool TrackReader::candidateReady() const { return m_candReady; }
QString TrackReader::candidateDate() const { return m_cand.date; }
int TrackReader::step() const { return m_step; }
QString TrackReader::note() const { return m_note; }
QString TrackReader::checkedAt() const
{
    return m_checked.isValid() ? m_checked.toString(QStringLiteral("HH:mm")) : QString();
}

QString TrackReader::previous() const
{
    if (m_previous == QLatin1String(kBundled))
        return m_current.isEmpty() ? QString() : m_bundledVersion;
    return m_previous == m_current ? QString() : m_previous;
}

/*********************************************************************
 * Start: the jar in use, then the checks
 *********************************************************************/

void TrackReader::apply()
{
    if (m_bridge == nullptr)
        return;
    m_bridge->setJarOverride(m_current.isEmpty() ? QString() : jarOf(m_current));
}

void TrackReader::start()
{
    if (m_available == false)
        return;
    apply();
    log(QStringLiteral("CDJ reader: beat-link from Beat Link Trigger %1%2")
            .arg(version(), m_current.isEmpty() ? QStringLiteral(" (TRACK's runtime)") : QStringLiteral(" (fetched)")));
    bool isInt = false;
    const int first = qEnvironmentVariableIntValue("QLC_TRACK_BLT_FIRST_MS", &isInt);
    m_checkTimer.start(isInt && first > 0 ? first : kFirstCheckMs);
    // TRACK_APP_READERKEEP_R604 (the log of 11-14): the tidy ran 30 s after the start - before the first check
    // (2 min) knew which jar is offered - so it removed the 8.1.0-Preview it had fetched, and the check fetched it
    // again a minute later, at every start. It runs once the check has answered (releasesRead)
}

void TrackReader::checkNow()
{
    if (m_available == false || m_proc != nullptr || m_step == 1)
        return;
    m_checkTimer.stop();
    slotCheck();
}

/*********************************************************************
 * The check: Deep Symmetry's releases
 *********************************************************************/

void TrackReader::slotCheck()
{
    m_checkTimer.start(kCheckEveryMs);
    if (m_available == false || m_proc != nullptr || m_step == 1)
        return;
    setStep(4, tr("checking ..."));
    QProcess *p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus status) {
        const QByteArray out = p->readAllStandardOutput();
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
        {
            setStep(6, tr("no answer from GitHub"));
            if (m_lastWhy != QStringLiteral("check"))
                log(QStringLiteral("CDJ reader: no answer from GitHub (curl %1) - tries again later").arg(code));
            m_lastWhy = QStringLiteral("check");
            return;
        }
        releasesRead(out);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        setStep(6, tr("curl would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--max-time") << QStringLiteral("45")
             << QStringLiteral("-A") << QStringLiteral("track-app-cdj-reader")
             << QStringLiteral("-H") << QStringLiteral("Accept: application/vnd.github+json")
             << apiUrl());
}

void TrackReader::releasesRead(const QByteArray &json)
{
    const QJsonArray list = QJsonDocument::fromJson(json).array();
    // R569_FIX: the version names a folder (cdjreader\<version>) - it starts with a digit and has no "..", so a
    // release's name can never point the jar anywhere else
    static const QRegularExpression jarName(QStringLiteral("^beat-link-trigger-([0-9][A-Za-z0-9._-]{0,47})\\.jar$"));
    Release stable, preview;
    for (const QJsonValue &v : list)
    {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("draft")).toBool())
            continue;
        const bool pre = o.value(QStringLiteral("prerelease")).toBool();
        if ((pre && preview.version.isEmpty() == false) || (pre == false && stable.version.isEmpty() == false))
            continue;
        for (const QJsonValue &av : o.value(QStringLiteral("assets")).toArray())
        {
            const QJsonObject a = av.toObject();
            const QRegularExpressionMatch m = jarName.match(a.value(QStringLiteral("name")).toString());
            const QString digest = a.value(QStringLiteral("digest")).toString();
            if (m.hasMatch() == false || m.captured(1).contains(QStringLiteral("..")) || digest.startsWith(QStringLiteral("sha256:")) == false)
                continue;              // no jar, or no SHA-256 to check it against: not offered
            Release r;
            r.version = m.captured(1);
            r.url = a.value(QStringLiteral("browser_download_url")).toString();
            r.size = qint64(a.value(QStringLiteral("size")).toDouble());
            r.sha256 = digest.mid(7).toLatin1().toLower();
            r.preview = pre;
            const QDateTime when = QDateTime::fromString(a.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);
            r.date = when.isValid() ? when.toLocalTime().toString(QStringLiteral("d.M")) : QString();
            // https only - a test list (QLC_TRACK_BLT_API) may point at a local http server
            const bool web = r.url.startsWith(QStringLiteral("https://"))
                             || (qEnvironmentVariable("QLC_TRACK_BLT_API").isEmpty() == false && r.url.startsWith(QStringLiteral("http://")));
            if (r.sha256.size() != 64 || r.size <= 0 || web == false)
                continue;
            (pre ? preview : stable) = r;
            break;
        }
    }
    m_checked = QDateTime::currentDateTime();
    m_lastWhy.clear();
    if (stable.version.isEmpty() && preview.version.isEmpty())
    {
        setStep(6, tr("GitHub's list has no Beat Link Trigger jar"));
        return;
    }
    m_stable = stable;
    m_preview = preview;

    // the one to offer: the newest release when it is newer than ours; a preview only when it is
    // newer than ours AND than the newest release; never one that did not start here before
    const QString have = version();
    const QString bad = readLine(file(QStringLiteral("bad.txt")));
    Release want;
    if (stable.version.isEmpty() == false && compareVersions(stable.version, have) > 0)
        want = stable;
    if (preview.version.isEmpty() == false && compareVersions(preview.version, have) > 0
        && (stable.version.isEmpty() || compareVersions(preview.version, stable.version) > 0))
        want = preview;
    if (want.version.isEmpty() == false && bad == want.version + QLatin1Char('|') + QString::fromLatin1(want.sha256))
        want = Release();
    if (want.version.isEmpty())
    {
        const bool had = m_cand.version.isEmpty() == false;
        m_cand = Release();
        m_candReady = false;
        setStep(0);
        if (had)
            log(QStringLiteral("CDJ reader: nothing newer than %1").arg(have));
        QTimer::singleShot(0, this, &TrackReader::cleanup);       // TRACK_APP_READERKEEP_R604: the offer is known
        return;
    }
    if (want.version == m_cand.version && want.sha256 == m_cand.sha256 && m_candReady)
    {
        setStep(0);
        QTimer::singleShot(0, this, &TrackReader::cleanup);       // TRACK_APP_READERKEEP_R604
        return;                         // already waiting for his two taps
    }
    m_cand = want;
    m_candReady = false;
    // already here (fetched before, or the one ROLL BACK left): only the SHA-256 again
    fetch(want);
}

/*********************************************************************
 * Fetch: download -> size -> SHA-256 -> in place
 *********************************************************************/

void TrackReader::fetch(const Release &r)
{
    const QString jar = jarOf(r.version);
    const QString part = jar + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(jar).absolutePath());
    if (QFileInfo(jar).size() == r.size)
    {
        // here already (fetched before, or the one ROLL BACK left): its SHA-256 where it lies
        m_hash.reset();
        m_hashFile.setFileName(jar);
        if (m_hashFile.open(QIODevice::ReadOnly) == false)
        {
            fetchDone(false, QStringLiteral("CDJ reader: %1 cannot be read").arg(r.version));
            return;
        }
        setStep(5, tr("checking %1 ...").arg(r.version));
        QTimer::singleShot(0, this, &TrackReader::slotHashChunk);
        return;
    }
    QFile::remove(part);
    setStep(5, tr("downloading %1 ...").arg(r.version));
    log(QStringLiteral("CDJ reader: Beat Link Trigger %1%2 is out - fetching its jar in the background")
            .arg(r.version, r.preview ? QStringLiteral(" (preview)") : QString()));
    QProcess *p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::finished, this, [this, p, part](int code, QProcess::ExitStatus status) {
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
        {
            QFile::remove(part);
            fetchDone(false, QStringLiteral("CDJ reader: %1 did not download (curl %2)").arg(m_cand.version).arg(code));
            return;
        }
        if (QFileInfo(part).size() != m_cand.size)
        {
            QFile::remove(part);
            fetchDone(false, QStringLiteral("CDJ reader: %1 is not whole").arg(m_cand.version));
            return;
        }
        m_hash.reset();
        m_hashFile.setFileName(part);
        if (m_hashFile.open(QIODevice::ReadOnly) == false)
        {
            fetchDone(false, QStringLiteral("CDJ reader: %1 cannot be read").arg(m_cand.version));
            return;
        }
        setStep(5, tr("checking %1 ...").arg(m_cand.version));
        QTimer::singleShot(0, this, &TrackReader::slotHashChunk);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        fetchDone(false, QStringLiteral("CDJ reader: curl would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--retry") << QStringLiteral("2")
             << QStringLiteral("--connect-timeout") << QStringLiteral("30")
             << QStringLiteral("--speed-limit") << QStringLiteral("2000") << QStringLiteral("--speed-time") << QStringLiteral("60")
             << QStringLiteral("--limit-rate") << QStringLiteral("4M")
             << QStringLiteral("-A") << QStringLiteral("track-app-cdj-reader")
             << QStringLiteral("-o") << part << r.url);
}

void TrackReader::slotHashChunk()
{
    if (m_hashFile.isOpen() == false)
        return;
    const QByteArray chunk = m_hashFile.read(kHashChunk);
    if (chunk.isEmpty() == false)
    {
        m_hash.addData(chunk);
        if (m_hashFile.atEnd() == false)
        {
            QTimer::singleShot(0, this, &TrackReader::slotHashChunk);
            return;
        }
    }
    const QString part = m_hashFile.fileName();
    m_hashFile.close();
    const bool inPlace = part.endsWith(QStringLiteral(".part")) == false;
    if (m_hash.result().toHex() != m_cand.sha256)
    {
        if (m_cand.version != m_current && m_cand.version != m_previous)
            QFile::remove(part);        // never the jar in use or ROLL BACK's
        fetchDone(false, QStringLiteral("CDJ reader: %1 does not match GitHub's SHA-256 - not used").arg(m_cand.version));
        return;
    }
    const QString jar = jarOf(m_cand.version);
    if (inPlace == false)
        QFile::remove(jar);
    if (inPlace == false && QFile::rename(part, jar) == false)
    {
        QFile::remove(part);
        fetchDone(false, QStringLiteral("CDJ reader: %1 could not be put in place").arg(m_cand.version));
        return;
    }
    m_candReady = true;
    log(QStringLiteral("CDJ reader: %1%2 is ready - UPDATE CDJ READER in SETUP (ADVANCED, SHOW OFF)")
            .arg(m_cand.version, m_cand.preview ? QStringLiteral(" (preview)") : QString()));
    fetchDone(true, QString());
}

void TrackReader::fetchDone(bool ok, const QString &why)
{
    if (ok)
    {
        m_lastWhy.clear();
        setStep(0);
        QTimer::singleShot(0, this, &TrackReader::cleanup);       // TRACK_APP_READERKEEP_R604: the offered one is in place - keep it
        return;
    }
    m_candReady = false;
    if (why != m_lastWhy)
        log(why + QStringLiteral(" - tries again later"));
    m_lastWhy = why;
    setStep(6, why.section(QStringLiteral(": "), 1));
}

/*********************************************************************
 * UPDATE / ROLL BACK: another jar, the helper starts again
 *********************************************************************/

void TrackReader::update(bool showOn)
{
    if (m_available == false || m_candReady == false || m_cand.version.isEmpty() || m_step == 1)
        return;
    if (showOn)
    {
        setStep(3, tr("SHOW OFF first - the CDJ reader starts again"));
        QTimer::singleShot(4000, this, [this]() { if (m_step == 3) setStep(0); });
        return;
    }
    const QString to = m_cand.version;
    m_cand = Release();
    m_candReady = false;
    switchTo(to, m_current.isEmpty() ? QString::fromLatin1(kBundled) : m_current, QStringLiteral("update"));
}

void TrackReader::rollback(bool showOn)
{
    const QString back = previous();
    if (m_available == false || back.isEmpty() || m_step == 1)
        return;
    if (showOn)
    {
        setStep(3, tr("SHOW OFF first - the CDJ reader starts again"));
        QTimer::singleShot(4000, this, [this]() { if (m_step == 3) setStep(0); });
        return;
    }
    const QString to = m_previous == QLatin1String(kBundled) ? QString() : m_previous;
    switchTo(to, m_current.isEmpty() ? QString::fromLatin1(kBundled) : m_current, QStringLiteral("roll back"));
}

void TrackReader::switchTo(const QString &version, const QString &previous, const QString &why)
{
    if (m_bridge == nullptr)
        return;
    m_switchFrom = m_current;
    m_switchFromPrev = m_previous;
    m_current = version;
    m_previous = previous;
    if (writeLine(file(QStringLiteral("current.txt")), m_current) == false
        || writeLine(file(QStringLiteral("previous.txt")), m_previous) == false)
    {
        m_current = m_switchFrom;
        m_previous = m_switchFromPrev;
        writeLine(file(QStringLiteral("current.txt")), m_current);
        writeLine(file(QStringLiteral("previous.txt")), m_previous);
        setStep(2, tr("could not write current.txt - stays on %1").arg(version.isEmpty() ? m_bundledVersion : version));
        return;
    }
    log(QStringLiteral("CDJ reader (%1): %2 -> %3 - the helper starts again")
            .arg(why, m_switchFrom.isEmpty() ? m_bundledVersion : m_switchFrom, this->version()));
    m_rollingBack = false;
    m_watching = true;
    m_switchClock.start();
    m_watch.start(kWatchMs);
    setStep(1, tr("starting %1 ...").arg(this->version()));
    apply();
    m_bridge->restart();
}

void TrackReader::slotStatus()
{
    if (m_watching == false || m_switchClock.elapsed() < kSettleMs)
        return;
    m_watching = false;
    m_watch.stop();
    log(QStringLiteral("CDJ reader: %1 runs").arg(version()));
    setStep(0, tr("%1 runs").arg(version()));
    QTimer::singleShot(30000, this, &TrackReader::cleanup);
}

void TrackReader::slotHelperExit(int code)
{
    // Beat Link Trigger open on the PC: the helper waits for it - that says nothing about the jar
    if (m_watching && code == TrackBridge::kBltRunning)
    {
        m_watching = false;
        m_watch.stop();
        setStep(0, tr("Beat Link Trigger is open - %1 starts when it closes").arg(version()));
    }
}

void TrackReader::slotWatchTimeout()
{
    if (m_watching == false)
        return;
    m_watching = false;
    const QString failed = version();
    const QString failedJar = m_current.isEmpty() ? QString() : jarOf(m_current);
    if (failedJar.isEmpty() == false)
    {
        // not offered again (until Deep Symmetry puts out another jar under that version)
        QFile f(failedJar);
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (f.open(QIODevice::ReadOnly) && h.addData(&f))
            writeLine(file(QStringLiteral("bad.txt")), failed + QLatin1Char('|') + QString::fromLatin1(h.result().toHex()));
    }
    m_current = m_switchFrom;
    m_previous = m_switchFromPrev;
    writeLine(file(QStringLiteral("current.txt")), m_current);
    writeLine(file(QStringLiteral("previous.txt")), m_previous);
    log(QStringLiteral("CDJ reader: %1 did not start within 60 s - back on %2").arg(failed, version()));
    apply();
    m_bridge->restart();
    setStep(2, tr("%1 did not start - back on %2").arg(failed, version()));
}

/*********************************************************************
 * Tidy: the jar in use, the one before and the one offered stay
 *********************************************************************/

void TrackReader::cleanup()
{
    if (m_available == false || m_proc != nullptr || m_watching)
        return;
    QStringList keep = QStringList() << m_current << m_previous << m_cand.version;
    QDir dir(QDir(m_root).filePath(QStringLiteral("cdjreader")));
    for (const QString &d : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (keep.contains(d))
            continue;
        if (QDir(dir.filePath(d)).removeRecursively())
            log(QStringLiteral("CDJ reader: removed %1 (neither in use nor the one before)").arg(d));
    }
}
