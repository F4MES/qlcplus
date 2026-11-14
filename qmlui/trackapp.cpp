/*
  Q Light Controller Plus
  trackapp.cpp

  The TRACK app (track-app branch): the helper's CDJ count and TRACK's own update.
  See trackapp.h for the installed layout.

  The update (runde 548): every 5 min since r551 (first after 30 s) Windows' own curl.exe reads the
  release's track-app.json. A newer build is downloaded (curl.exe, at most 4 MB/s), its
  size and SHA-256 checked, and unpacked with Windows' own tar.exe into app\<build> - the
  runtime (Java + beat-link) too, when the new build needs another one. Nothing changes
  until then; the chip appears only when the build is ready. No PowerShell (R517: what
  Windows Defender hunted).

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
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "trackapp.h"

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace
{
const int kFirstCheckMs = 30 * 1000;
// R551 (Tobias 11-04: "hvert 15 min fremfor 30 min, saa laenge det ikke gaar ud over performance, maaske endda
// oftere"): a check is one curl.exe reading a ~400-byte file, off the UI thread - 5 min costs nothing
const int kCheckEveryMs = 5 * 60 * 1000;
const qint64 kHashChunk = 4 * 1024 * 1024;
}

TrackApp::TrackApp(bool active, std::function<void()> beforeQuit, QObject *parent)
    : QObject(parent)
    , m_beforeQuit(beforeQuit)
    , m_active(active)
    , m_bridge(this)
    , m_reader(nullptr)
    , m_show(nullptr)
    , m_installed(false)
    , m_proc(nullptr)
    , m_busy(false)
    , m_step(0)
    , m_hash(QCryptographicHash::Sha256)
{
    // the installed layout: <root>\app\<build>\ holds this program
    m_appDir = QCoreApplication::applicationDirPath();
    QDir up(m_appDir);
    if (up.cdUp() && up.dirName() == QStringLiteral("app") && up.cdUp()
        && QFileInfo::exists(up.filePath(QStringLiteral("current.txt"))))
    {
        m_installed = true;
        m_root = up.absolutePath();
        m_logDir = up.filePath(QStringLiteral("logs"));
    }
    m_rev = readLine(QDir(m_appDir).filePath(QStringLiteral("track-app-build.txt")));
    if (m_rev.isEmpty())
        m_rev = QDir(m_appDir).dirName();
    m_runtimeId = readLine(QDir(m_appDir).filePath(QStringLiteral("runtime.txt")));
    // TRACK_APP_BUILDNOTE_R601: the release writes the round from the commit ("R601"); a build without it says nothing
    m_round = readLine(QDir(m_appDir).filePath(QStringLiteral("track-app-round.txt")));
    if (QRegularExpression(QStringLiteral("^R[0-9]{1,5}$")).match(m_round).hasMatch() == false)
        m_round.clear();

    QString runtimeDir;
    if (m_installed && m_runtimeId.isEmpty() == false)
        runtimeDir = file(QStringLiteral("runtime/") + m_runtimeId);
    m_bridge.setPlaces(m_logDir, runtimeDir);
    // R557_READER: a newer Beat Link Trigger fetched in SETUP runs instead of the runtime's own
    m_reader = new TrackReader(&m_bridge, m_installed && m_active ? m_root : QString(), m_logDir,
                               runtimeDir.isEmpty() ? QString() : QDir(runtimeDir).filePath(QStringLiteral("beat-link-trigger.jar")),
                               TrackReader::versionOfRuntime(m_runtimeId), this);
    // R558_SHOW: the show TRACK opens (show.txt) and the rig's PSMAIN.qxw with UPDATE TRACK
    m_show = new TrackShow(m_installed && m_active ? m_root : QString(), m_logDir, this);
    connect(m_show, &TrackShow::readyChanged, this, &TrackApp::updateChanged);
    connect(&m_bridge, &TrackBridge::cdjCountChanged, this, &TrackApp::cdjCountChanged);
    // R560_DECKS: the deck the lights follow; the warning is checked every second
    connect(&m_bridge, &TrackBridge::decksChanged, this, [this]() { deckCheck(); emit decksChanged(); });
    m_deckTimer.setInterval(1000);
    connect(&m_deckTimer, &QTimer::timeout, this, &TrackApp::deckCheck);

    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, &TrackApp::slotCheck);

    // TRACK_APP_PHONE_R549: TrackManager (the phone's UPDATE, its state) finds this object here
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->setProperty("trackAppCtl", QVariant::fromValue<QObject *>(this));

    // before App::startup() loads the fixture definitions (main.cpp makes this object first)
    if (m_active)
        ensureFixtures();
}

TrackApp::~TrackApp()
{
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->setProperty("trackAppCtl", QVariant());
    m_checkTimer.stop();
    if (m_proc != nullptr && m_proc->state() != QProcess::NotRunning)
    {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
    m_bridge.stop();
}

/*********************************************************************
 * The restart's handshake
 *********************************************************************/

void TrackApp::waitForPredecessor(int argc, char *argv[])
{
    qint64 pid = 0;
    for (int i = 1; i + 1 < argc; i++)
    {
        if (qstrcmp(argv[i], "--restart-after") == 0)
            pid = QByteArray(argv[i + 1]).toLongLong();
    }
    if (pid <= 0)
        return;
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (h != nullptr)
    {
        WaitForSingleObject(h, 30000);
        CloseHandle(h);
    }
#else
    for (int i = 0; i < 300 && kill(pid_t(pid), 0) == 0; i++)
        usleep(100 * 1000);
#endif
}

/*********************************************************************
 * Properties
 *********************************************************************/

bool TrackApp::active() const { return m_active; }
QString TrackApp::build() const { return m_rev; }
int TrackApp::cdjCount() const { return m_active ? m_bridge.cdjCount() : 0; }
bool TrackApp::bltFeeds() const { return m_active && m_bridge.bltFeeds(); }   // TRACK_APP_BLTFEEDS_R575
bool TrackApp::updateAvailable() const { return m_active && (m_staged.isEmpty() == false || m_show->ready()); }
QString TrackApp::updateLatest() const { return m_staged.isEmpty() && m_show->ready() ? QStringLiteral("PSMAIN") : m_staged; }
int TrackApp::updateStep() const { return m_step; }
QObject *TrackApp::reader() const { return m_reader; }
QObject *TrackApp::show() const { return m_show; }
QString TrackApp::buildRound() const { return m_round; }          // TRACK_APP_BUILDNOTE_R601
int TrackApp::updateNoteKind() const { return m_active ? m_noteKind : 0; }
QString TrackApp::updateNote() const { return m_active ? m_note : QString(); }

QString TrackApp::updateWhat() const
{
    const bool show = m_show->ready();
    if (m_staged.isEmpty() == false)
        return show ? tr("build %1 + show").arg(m_staged) : tr("build %1 is ready").arg(m_staged);
    return show ? tr("new show of %1").arg(m_show->rigDate()) : QString();
}

/*********************************************************************
 * R560_DECKS: which deck the lights follow, and when that looks wrong
 *********************************************************************/

int TrackApp::deckMaster() const { return m_active ? m_bridge.deckMaster() : 0; }
int TrackApp::deckFollowed() const { return m_active ? m_bridge.deckFollowed() : 0; }

QVariantList TrackApp::decksHeard() const
{
    QVariantList out;
    if (m_active)
        for (int d : m_bridge.decksHeard())
            out << d;
    return out;
}

QVariantList TrackApp::decksPlaying() const
{
    QVariantList out;
    if (m_active)
        for (int d : m_bridge.decksPlaying())
            out << d;
    return out;
}

QString TrackApp::deckText() const
{
    const int f = deckFollowed();
    if (f <= 0)
        return QString();
    return f == deckMaster() ? tr("DECK %1 · MASTER").arg(f) : tr("DECK %1 · HEARD").arg(f);
}

QString TrackApp::deckWarn() const { return m_deckWarn; }

// R561_NEXT: the next track, as the helper sees it (nothing while TRACK is not the app)
int TrackApp::nextDeck() const { return m_active ? m_bridge.nextDeck() : 0; }
QString TrackApp::nextState() const { return m_active ? m_bridge.nextState() : QString(); }
QString TrackApp::nextTrack() const { return m_active ? m_bridge.nextTrack() : QString(); }

void TrackApp::deckCheck()
{
    // Another deck the room hears has played for 8 s while the followed one stands still (or none is
    // followed yet): the lights are on the wrong deck. Only shown with SHOW ON (the page and the phone).
    QString cand;
    if (m_active && m_bridge.cdjCount() > 0)
    {
        const int f = m_bridge.deckFollowed();
        const QList<int> heard = m_bridge.decksHeard();
        const QList<int> playing = m_bridge.decksPlaying();
        int other = 0;
        for (int d : heard)
        {
            if (d != f)
            {
                other = d;
                break;
            }
        }
        if (other > 0 && (f <= 0 || playing.contains(f) == false))
            cand = f > 0 ? tr("DECK %1 PLAYS · LIGHT ON DECK %2").arg(other).arg(f)
                         : tr("DECK %1 PLAYS · NO DECK FOLLOWED").arg(other);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cand != m_deckCand)
    {
        m_deckCand = cand;
        m_deckCandSince = now;
    }
    const QString shown = (cand.isEmpty() == false && now - m_deckCandSince >= 8000) ? cand : QString();
    if (shown != m_deckWarn)
    {
        m_deckWarn = shown;
        log(shown.isEmpty() ? QStringLiteral("decks: the lights follow the deck the room hears again")
                            : QStringLiteral("decks: ") + shown);
        emit decksChanged();
    }
}

QString TrackApp::showAtStart(const QString &given)
{
    return m_active ? m_show->atStart(given) : given;
}

void TrackApp::setShowLoader(std::function<bool(const QString &)> loader)
{
    m_show->setLoader(loader);
}

void TrackApp::start()
{
    if (m_active == false)
        return;
    m_reader->start();                   // R557_READER: the jar, before the helper starts
    m_bridge.start();
    m_show->start();                     // R558_SHOW
    m_deckTimer.start();                 // R560_DECKS
    if (m_installed == false)
    {
        log(QStringLiteral("not an installed TRACK (%1) - no self-update").arg(QDir::toNativeSeparators(m_appDir)));
        return;
    }
    log(QStringLiteral("TRACK %1 started (runtime %2)").arg(m_rev, m_runtimeId.isEmpty() ? QStringLiteral("-") : m_runtimeId));
    r601Note();                          // TRACK_APP_BUILDNOTE_R601 - before the minute that clears pending.txt
    // a build that runs a minute is no longer "pending" (R570_LAUNCH: the launcher waits 65 s for it)
    QTimer::singleShot(60000, this, [this]() {
        const QString pending = file(QStringLiteral("pending.txt"));
        if (readLine(pending) == m_rev)
            QFile::remove(pending);
    });
    QTimer::singleShot(20000, this, &TrackApp::cleanup);
    // QLC_TRACK_UPDATE_FIRST_MS: the first check sooner (tests)
    bool isInt = false;
    const int first = qEnvironmentVariableIntValue("QLC_TRACK_UPDATE_FIRST_MS", &isInt);
    m_checkTimer.start(isInt && first > 0 ? first : kFirstCheckMs);
}

void TrackApp::r601Note()
{
    // TRACK_APP_BUILDNOTE_R601 (Tobias 11-14: "kan du give et build nummer eller noget et sted i track appen saa jeg/vi
    // kan se at track faktisk er opdateret ... en lille boks der fader vaek igen, der viser at appen er opdateret, og
    // ikke fejlet"). From 8 to 14 Nov every new build stopped at its first start on the lys-pc and the launcher went
    // quietly back - nothing on the page said so. Now, once:
    // - the first start of the build UPDATE TRACK switched to: pending.txt names it (UPDATE writes it, this build or
    //   the launcher clears it after a minute) -> "TRACK UPDATED"
    // - the build the launcher just went back to: bad-build.txt was written in the last 10 minutes -> "UPDATE DID
    //   NOT START"
    // note-shown.txt keeps it to once (a restart in the same minute says nothing again)
    const QString shownPath = file(QStringLiteral("note-shown.txt"));
    const QString shown = readLine(shownPath);
    const QString pending = readLine(file(QStringLiteral("pending.txt")));
    if (pending.isEmpty() == false && pending == m_rev)
    {
        if (shown == QStringLiteral("ok:") + m_rev)
            return;
        const QString was = readLine(file(QStringLiteral("previous.txt")));
        m_noteKind = 1;
        m_note = tr("build %1").arg(m_rev) + (m_round.isEmpty() ? QString() : QStringLiteral("  \u00B7  ") + m_round);
        if (was.isEmpty() == false && was != m_rev)
            m_note += QStringLiteral("  \u00B7  <font color='#8C8C96'>") + tr("was %1").arg(was) + QStringLiteral("</font>");
        writeLine(shownPath, QStringLiteral("ok:") + m_rev);
        log(QStringLiteral("update: TRACK is now %1%2 (was %3) - TRACK UPDATED on the page")
                .arg(m_rev, m_round.isEmpty() ? QString() : QStringLiteral(" ") + m_round, was.isEmpty() ? QStringLiteral("-") : was));
    }
    else
    {
        const QString badPath = file(QStringLiteral("bad-build.txt"));
        const QString bad = readLine(badPath);
        const QFileInfo badInfo(badPath);
        if (bad.isEmpty() || bad == m_rev || badInfo.exists() == false || shown == QStringLiteral("bad:") + bad)
            return;
        const qint64 age = badInfo.lastModified().secsTo(QDateTime::currentDateTime());
        if (age < -60 || age > 600)
            return;                      // an old one (or a clock that jumped) - nothing just happened
        m_noteKind = 2;
        m_note = tr("build %1 stopped at its first start").arg(bad) + QStringLiteral("  \u00B7  <font color='#8C8C96'>")
               + tr("still on %1 - it is not offered again").arg(m_rev) + QStringLiteral("</font>");
        writeLine(shownPath, QStringLiteral("bad:") + bad);
        log(QStringLiteral("update: build %1 did not start - still on %2 - UPDATE DID NOT START on the page").arg(bad, m_rev));
    }
    emit updateNoteChanged();
}

void TrackApp::log(const QString &what) const
{
    TrackBridge::writeLog(m_logDir, what);
}

void TrackApp::setStep(int step)
{
    if (step == m_step)
        return;
    m_step = step;
    emit updateChanged();
}

QString TrackApp::file(const QString &rel) const
{
    return QDir(m_root).filePath(rel);
}

QString TrackApp::readLine(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text) == false)
        return QString();
    return QString::fromUtf8(f.readLine(256)).trimmed();
}

bool TrackApp::writeLine(const QString &path, const QString &text)
{
    QSaveFile f(path);              // whole or not at all: the launcher reads it
    if (f.open(QIODevice::WriteOnly | QIODevice::Text) == false)
        return false;
    f.write(text.toUtf8() + "\n");
    return f.commit();
}

QString TrackApp::baseUrl() const
{
    QString base = qEnvironmentVariable("QLC_TRACK_UPDATE_BASE");
    if (base.isEmpty())
        base = QStringLiteral("https://github.com/F4MES/qlcplus/releases/download/track-app-latest/");
    if (base.endsWith(QLatin1Char('/')) == false)
        base += QLatin1Char('/');
    return base;
}

QString TrackApp::curl() const
{
#if defined(Q_OS_WIN)
    return QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/curl.exe"));
#else
    return QStringLiteral("curl");
#endif
}

/*********************************************************************
 * Check: the release's track-app.json
 *********************************************************************/

void TrackApp::slotCheck()
{
    m_checkTimer.start(kCheckEveryMs);
    if (m_busy || m_installed == false)
        return;
    m_busy = true;
    QProcess *p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus status) {
        const QByteArray out = p->readAllStandardOutput();
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        m_busy = false;
        if (status != QProcess::NormalExit || code != 0)
        {
            fetchDone(false, QStringLiteral("update check: no answer from GitHub (curl %1)").arg(code));
            return;
        }
        manifestRead(out);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        m_busy = false;
        fetchDone(false, QStringLiteral("update check: curl.exe would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--max-time") << QStringLiteral("45")
             << QStringLiteral("-A") << QStringLiteral("track-app-update")
             << baseUrl() + QStringLiteral("track-app.json"));
}

void TrackApp::manifestRead(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString rev = o.value(QStringLiteral("rev")).toString().trimmed();
    const QJsonObject appO = o.value(QStringLiteral("app")).toObject();
    const QJsonObject rtO = o.value(QStringLiteral("runtime")).toObject();
    const QString rtId = rtO.value(QStringLiteral("id")).toString().trimmed();
    const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
    if (safe.match(rev).hasMatch() == false || safe.match(rtId).hasMatch() == false
        || appO.isEmpty() || rtO.isEmpty())
    {
        fetchDone(false, QStringLiteral("update check: the release's track-app.json is not readable"));
        return;
    }
    if (rev == m_rev || rev == m_staged)
        return;                         // nothing new, or already waiting for his two taps
    // R570_LAUNCH (bug round 570): a build the launcher sent back (it stopped at its first start) is not
    // fetched and offered again - every start downloaded it, every UPDATE went dark and back
    if (rev == readLine(file(QStringLiteral("bad-build.txt"))))
    {
        if (m_lastWhy != QStringLiteral("bad:") + rev)
            log(QStringLiteral("update: build %1 stopped at its first start here - not offered again").arg(rev));
        m_lastWhy = QStringLiteral("bad:") + rev;
        return;
    }

    const QString engine = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    m_jobs.clear();
    if (QFileInfo::exists(file(QStringLiteral("runtime/%1/beat-link-trigger.jar").arg(rtId))) == false)
    {
        Fetch rt;
        rt.asset = rtO.value(QStringLiteral("asset")).toString();
        rt.size = qint64(rtO.value(QStringLiteral("size")).toDouble());
        rt.sha256 = rtO.value(QStringLiteral("sha256")).toString().toLatin1().toLower();
        rt.target = file(QStringLiteral("runtime/") + rtId);
        rt.marker = QStringLiteral("beat-link-trigger.jar");
        m_jobs << rt;
    }
    if (QFileInfo::exists(file(QStringLiteral("app/%1/%2").arg(rev, engine))) == false)
    {
        Fetch ap;
        ap.asset = appO.value(QStringLiteral("asset")).toString();
        ap.size = qint64(appO.value(QStringLiteral("size")).toDouble());
        ap.sha256 = appO.value(QStringLiteral("sha256")).toString().toLatin1().toLower();
        ap.target = file(QStringLiteral("app/") + rev);
        ap.marker = engine;
        m_jobs << ap;
    }
    for (const Fetch &f : m_jobs)
    {
        if (safe.match(f.asset).hasMatch() == false || f.size <= 0 || f.sha256.size() != 64)
        {
            m_jobs.clear();
            fetchDone(false, QStringLiteral("update check: track-app.json lists a file it cannot vouch for"));
            return;
        }
    }
    m_show->checkNow();                 // R558_SHOW: the rig's show comes with it, when it changed
    log(QStringLiteral("update: build %1 in the release - fetching it in the background (%2 file%3)")
            .arg(rev).arg(m_jobs.size()).arg(m_jobs.size() == 1 ? QString() : QStringLiteral("s")));
    m_job = Fetch();
    m_job.target = rev;                 // remembered for when the jobs are done
    const bool hadStaged = m_staged.isEmpty() == false;
    m_staged.clear();
    if (hadStaged)
        emit updateChanged();           // R570_FIX: the chip for the older build goes (it did nothing when tapped)
    m_jobs << m_job;                    // the last entry: "ready" (no asset)
    nextFetch();
}

/*********************************************************************
 * Fetch: download -> size -> SHA-256 -> unpack -> marker -> in place
 *********************************************************************/

void TrackApp::nextFetch()
{
    if (m_jobs.isEmpty())
        return;
    m_job = m_jobs.takeFirst();
    if (m_job.asset.isEmpty())
    {
        // every file in place: the chip
        m_staged = m_job.target;
        m_lastWhy.clear();
        log(QStringLiteral("update: build %1 is ready - UPDATE TRACK on the page (SHOW OFF)").arg(m_staged));
        emit updateChanged();
        return;
    }
    download();
}

void TrackApp::download()
{
    m_busy = true;
    QDir().mkpath(file(QStringLiteral("downloads")));
    const QString part = file(QStringLiteral("downloads/") + m_job.asset + QStringLiteral(".part"));
    QFile::remove(part);
    QProcess *p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::finished, this, [this, p, part](int code, QProcess::ExitStatus status) {
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
        {
            QFile::remove(part);
            fetchDone(false, QStringLiteral("update: %1 did not download (curl %2)").arg(m_job.asset).arg(code));
            return;
        }
        if (QFileInfo(part).size() != m_job.size)
        {
            QFile::remove(part);
            fetchDone(false, QStringLiteral("update: %1 is not whole (%2 of %3 bytes)")
                                 .arg(m_job.asset).arg(QFileInfo(part).size()).arg(m_job.size));
            return;
        }
        // the SHA-256 in 4 MB steps: the page never waits for it
        m_hash.reset();
        m_hashFile.setFileName(part);
        if (m_hashFile.open(QIODevice::ReadOnly) == false)
        {
            fetchDone(false, QStringLiteral("update: %1 cannot be read").arg(m_job.asset));
            return;
        }
        QTimer::singleShot(0, this, &TrackApp::slotHashChunk);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        fetchDone(false, QStringLiteral("update: curl.exe would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--retry") << QStringLiteral("2")
             << QStringLiteral("--connect-timeout") << QStringLiteral("30")
             << QStringLiteral("--speed-limit") << QStringLiteral("2000") << QStringLiteral("--speed-time") << QStringLiteral("60")
             << QStringLiteral("--limit-rate") << QStringLiteral("4M")
             << QStringLiteral("-A") << QStringLiteral("track-app-update")
             << QStringLiteral("-o") << part << baseUrl() + m_job.asset);
}

void TrackApp::slotHashChunk()
{
    if (m_hashFile.isOpen() == false)
        return;
    const QByteArray chunk = m_hashFile.read(kHashChunk);
    if (chunk.isEmpty() == false)
    {
        m_hash.addData(chunk);
        if (m_hashFile.atEnd() == false)
        {
            QTimer::singleShot(0, this, &TrackApp::slotHashChunk);
            return;
        }
    }
    const QString part = m_hashFile.fileName();
    m_hashFile.close();
    const QByteArray got = m_hash.result().toHex();
    if (got != m_job.sha256)
    {
        QFile::remove(part);
        fetchDone(false, QStringLiteral("update: %1 does not match the release's SHA-256 - not used").arg(m_job.asset));
        return;
    }
    extract();
}

void TrackApp::extract()
{
    const QString part = file(QStringLiteral("downloads/") + m_job.asset + QStringLiteral(".part"));
    const QString zip = file(QStringLiteral("downloads/") + m_job.asset);
    QFile::remove(zip);
    QFile::rename(part, zip);
    const QString unpack = m_job.target + QStringLiteral(".unpacking");
    QDir(unpack).removeRecursively();
    QDir().mkpath(unpack);

    QProcess *p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::finished, this, [this, p, zip, unpack](int code, QProcess::ExitStatus status) {
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        const bool unpacked = status == QProcess::NormalExit && code == 0
                              && QFileInfo::exists(QDir(unpack).filePath(m_job.marker));
        if (unpacked == false)
        {
            QDir(unpack).removeRecursively();
            fetchDone(false, QStringLiteral("update: %1 did not unpack (code %2)").arg(m_job.asset).arg(code));
            return;
        }
        QDir(m_job.target).removeRecursively();         // a half one from before - never the running build
        if (QDir().rename(unpack, m_job.target) == false)
        {
            QDir(unpack).removeRecursively();
            fetchDone(false, QStringLiteral("update: %1 could not be put in place").arg(m_job.asset));
            return;
        }
        QFile::remove(zip);
        m_busy = false;
        nextFetch();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, unpack](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        QDir(unpack).removeRecursively();
        fetchDone(false, QStringLiteral("update: the unpacker would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
#if defined(Q_OS_WIN)
    // Windows' own tar.exe (Windows 10 1803 and later) unpacks zip files
    p->start(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/tar.exe")),
             QStringList() << QStringLiteral("-xf") << QDir::toNativeSeparators(zip)
                           << QStringLiteral("-C") << QDir::toNativeSeparators(unpack));
#else
    p->start(QStringLiteral("unzip"), QStringList() << QStringLiteral("-q") << QStringLiteral("-o") << zip
                                                    << QStringLiteral("-d") << unpack);
#endif
}

void TrackApp::fetchDone(bool ok, const QString &why)
{
    m_busy = false;
    if (ok)
        return;
    m_jobs.clear();
    // said once until something else happens (the check runs every 5 min)
    if (why != m_lastWhy)
        log(why + QStringLiteral(" - tries again later"));
    m_lastWhy = why;
}

/*********************************************************************
 * The two taps: switch and start again
 *********************************************************************/

void TrackApp::startUpdate(bool showOn)
{
    const bool show = m_show->ready();  // R558_SHOW: the rig's new show - with a build or alone
    if (m_active == false || m_installed == false || (m_staged.isEmpty() && show == false) || m_step == 1)
        return;
    if (showOn)
    {
        // SHOW OFF first - the restart takes the light with it for a few seconds
        setStep(3);
        QTimer::singleShot(4000, this, [this]() { if (m_step == 3) setStep(0); });
        return;
    }

    if (m_staged.isEmpty())
    {
        // R558_SHOW: only the show - in place, and TRACK starts again on it
        if (m_show->apply() == false || relaunch(QStringLiteral("update: the rig's show in place, TRACK starts again")) == false)
        {
            setStep(2);
            QTimer::singleShot(8000, this, [this]() { if (m_step == 2) setStep(0); });
        }
        return;
    }

    const QString launcher = file(QStringLiteral("TRACK.exe"));
    const QString fresh = file(QStringLiteral("app/%1/TRACK-launcher.exe").arg(m_staged));
    // the launcher does not run while TRACK does - a newer one comes with the build
    if (QFileInfo::exists(fresh))
    {
        QFile a(fresh), b(launcher);
        const bool same = a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && a.readAll() == b.readAll();
        a.close();
        b.close();
        if (same == false)
        {
            QFile::remove(launcher + QStringLiteral(".new"));
            if (QFile::copy(fresh, launcher + QStringLiteral(".new")))
            {
                QFile::remove(launcher + QStringLiteral(".old"));
                QFile::rename(launcher, launcher + QStringLiteral(".old"));
                if (QFile::rename(launcher + QStringLiteral(".new"), launcher) == false)
                    QFile::rename(launcher + QStringLiteral(".old"), launcher);
            }
        }
    }

    const QString current = readLine(file(QStringLiteral("current.txt")));
    if (writeLine(file(QStringLiteral("previous.txt")), current.isEmpty() ? m_rev : current) == false
        || writeLine(file(QStringLiteral("pending.txt")), m_staged) == false
        || writeLine(file(QStringLiteral("current.txt")), m_staged) == false)
    {
        writeLine(file(QStringLiteral("current.txt")), current.isEmpty() ? m_rev : current);
        QFile::remove(file(QStringLiteral("pending.txt")));
        log(QStringLiteral("update: could not write current.txt - stays on %1").arg(m_rev));
        setStep(2);
        QTimer::singleShot(8000, this, [this]() { if (m_step == 2) setStep(0); });
        return;
    }

    // R558_SHOW: the rig's new show with it (one that cannot be written stays as it is - the build goes on)
    if (show)
        m_show->apply();

    if (relaunch(QStringLiteral("update: %1 -> %2, TRACK starts again").arg(m_rev, m_staged)) == false)
    {
        writeLine(file(QStringLiteral("current.txt")), current.isEmpty() ? m_rev : current);
        QFile::remove(file(QStringLiteral("pending.txt")));
        log(QStringLiteral("update: the launcher would not start - stays on %1").arg(m_rev));
        setStep(2);
        QTimer::singleShot(8000, this, [this]() { if (m_step == 2) setStep(0); });
    }
}

void TrackApp::restartTrack(bool showOn)
{
    // R558_SHOW: SETUP's OPEN NOW - the show chosen there, at once
    if (m_active == false || m_installed == false || m_step == 1)
        return;
    if (showOn)
    {
        setStep(3);
        QTimer::singleShot(4000, this, [this]() { if (m_step == 3) setStep(0); });
        return;
    }
    if (relaunch(QStringLiteral("show: OPEN NOW - TRACK starts again on %1").arg(m_show->path())) == false)
    {
        log(QStringLiteral("show: the launcher would not start - TRACK stays as it is"));
        setStep(2);
        QTimer::singleShot(8000, this, [this]() { if (m_step == 2) setStep(0); });
    }
}

bool TrackApp::relaunch(const QString &why)
{
    // the same start as this one (-f; the show is show.txt's), after this TRACK is gone
    const QString launcher = file(QStringLiteral("TRACK.exe"));
    QStringList args = QCoreApplication::arguments().mid(1);
    const int at = args.indexOf(QStringLiteral("--restart-after"));
    if (at >= 0)
        args.erase(args.begin() + at, args.begin() + qMin(at + 2, int(args.size())));
    args << QStringLiteral("--restart-after") << QString::number(QCoreApplication::applicationPid());
    if (QProcess::startDetached(launcher, args, m_root) == false)
        return false;
    log(why);
    setStep(1);
    if (m_beforeQuit)
        m_beforeQuit();
    QTimer::singleShot(400, qApp, &QCoreApplication::quit);
    return true;
}

/*********************************************************************
 * R548_FIXTURES: the rig's own definitions on a clean PC
 *********************************************************************/

void TrackApp::ensureFixtures()
{
    // the build's TRACK-fixtures\*.qxf (Mini 4-eyes, Shark Zoom Wash, the Yuer lasers and strobes -
    // none of them in QLC+'s library) into the user's QLC+ fixture folder, where QLC+ reads them
    // first. A model that already has a definition there is left alone: his files win, always
    const QDir src(QDir(m_appDir).filePath(QStringLiteral("TRACK-fixtures")));
    if (src.exists() == false)
        return;
#if defined(Q_OS_WIN)
    const QString home = qEnvironmentVariable("USERPROFILE", QDir::homePath());
    const QDir dst(home + QStringLiteral("/QLC+/Fixtures"));
#elif defined(Q_OS_MACOS)
    const QDir dst(QDir::homePath() + QStringLiteral("/Library/Application Support/QLC+/Fixtures"));
#else
    const QDir dst(QDir::homePath() + QStringLiteral("/.qlcplus/fixtures"));
#endif
    const QRegularExpression man(QStringLiteral("<Manufacturer>([^<]*)</Manufacturer>"));
    const QRegularExpression mod(QStringLiteral("<Model>([^<]*)</Model>"));
    auto key = [&man, &mod](const QString &path) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) == false)
            return QString();
        const QString text = QString::fromUtf8(f.read(65536));
        const QRegularExpressionMatch a = man.match(text);
        const QRegularExpressionMatch b = mod.match(text);
        if (a.hasMatch() == false || b.hasMatch() == false)
            return QString();
        return a.captured(1).trimmed() + QLatin1Char('/') + b.captured(1).trimmed();
    };
    QStringList have;
    for (const QString &f : dst.entryList(QStringList() << QStringLiteral("*.qxf"), QDir::Files))
        have << key(dst.filePath(f));
    for (const QString &f : src.entryList(QStringList() << QStringLiteral("*.qxf"), QDir::Files))
    {
        const QString k = key(src.filePath(f));
        if (k.isEmpty() || have.contains(k) || QFileInfo::exists(dst.filePath(f)))
            continue;
        QDir().mkpath(dst.absolutePath());
        if (QFile::copy(src.filePath(f), dst.filePath(f)))
        {
            have << k;
            log(QStringLiteral("fixture %1 copied to %2 (it was not there)").arg(k, QDir::toNativeSeparators(dst.absolutePath())));
        }
    }
}

/*********************************************************************
 * Tidy: builds and runtimes no one goes back to
 *********************************************************************/

void TrackApp::cleanup()
{
    if (m_installed == false || m_busy)
        return;
    QStringList keepApps = QStringList() << m_rev
        << readLine(file(QStringLiteral("current.txt"))) << readLine(file(QStringLiteral("previous.txt")));
    if (m_staged.isEmpty() == false)
        keepApps << m_staged;
    QStringList keepRuntimes;
    QDir apps(file(QStringLiteral("app")));
    for (const QString &d : apps.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (keepApps.contains(d))
        {
            keepRuntimes << readLine(apps.filePath(d + QStringLiteral("/runtime.txt")));
            continue;
        }
        if (QDir(apps.filePath(d)).removeRecursively())
            log(QStringLiteral("tidy: build %1 removed").arg(d));
    }
    QDir rts(file(QStringLiteral("runtime")));
    for (const QString &d : rts.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        if (keepRuntimes.contains(d) == false && QDir(rts.filePath(d)).removeRecursively())
            log(QStringLiteral("tidy: runtime %1 removed").arg(d));
    }
    QDir dl(file(QStringLiteral("downloads")));
    for (const QString &f : dl.entryList(QDir::Files))
        dl.remove(f);
    QFile::remove(file(QStringLiteral("TRACK.exe.old")));
}
