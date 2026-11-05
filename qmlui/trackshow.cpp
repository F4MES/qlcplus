/*
  Q Light Controller Plus
  trackshow.cpp

  The TRACK app's show (runde 558) - see trackshow.h.

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
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include "trackshow.h"
#include "trackbridge.h"

namespace
{
const int kFirstCheckMs = 45 * 1000;
// one ~5 kB answer from GitHub's API (60 an hour are allowed without a login; the CDJ reader's
// check is one every 6 h) - every 15 min is enough for a show that changes with a rig round
const int kCheckEveryMs = 15 * 60 * 1000;
const qint64 kHashChunk = 4 * 1024 * 1024;
const char *kRigName = "PSMAIN.qxw";
}

TrackShow::TrackShow(const QString &root, const QString &logDir, QObject *parent)
    : QObject(parent)
    , m_available(root.isEmpty() == false)
    , m_root(root)
    , m_logDir(logDir)
    , m_missing(false)
    , m_follow(true)
    , m_accepted(false)
    , m_ready(false)
    , m_state(Unknown)
    , m_proc(nullptr)
    , m_busy(false)
    , m_hash(QCryptographicHash::Sha256)
{
    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, &TrackShow::slotCheck);
    if (m_available == false)
        return;
    m_path = readLine(file(QStringLiteral("show.txt")));
    m_follow = readLine(file(QStringLiteral("show-follow.txt"))) != QLatin1String("off");
}

TrackShow::~TrackShow()
{
    m_checkTimer.stop();
    if (m_proc != nullptr && m_proc->state() != QProcess::NotRunning)
    {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

/*********************************************************************
 * Small things
 *********************************************************************/

QString TrackShow::file(const QString &rel) const
{
    return QDir(m_root).filePath(rel);
}

QString TrackShow::readLine(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text) == false)
        return QString();
    QString s = QString::fromUtf8(f.readLine(4096)).trimmed();
    if (s.startsWith(QChar(0xFEFF)))            // a BOM (Windows PowerShell's UTF-8)
        s.remove(0, 1);
    return s.trimmed();
}

bool TrackShow::writeLine(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text) == false)
        return false;
    f.write(text.toUtf8() + "\n");
    return f.commit();
}

QString TrackShow::defaultShow()
{
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(docs).filePath(QStringLiteral("TRACK/") + QLatin1String(kRigName));
}

QString TrackShow::curl() const
{
#if defined(Q_OS_WIN)
    return QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))).filePath(QStringLiteral("System32/curl.exe"));
#else
    return QStringLiteral("curl");
#endif
}

QString TrackShow::apiUrl() const
{
    // QLC_TRACK_SHOW_API: another release's API answer (tests)
    const QString env = qEnvironmentVariable("QLC_TRACK_SHOW_API");
    if (env.isEmpty() == false)
        return env;
    return QStringLiteral("https://api.github.com/repos/F4MES/qlcplus/releases/tags/rig-latest");
}

void TrackShow::log(const QString &what) const
{
    TrackBridge::writeLog(m_logDir, what);
}

void TrackShow::setState(int state, const QString &note)
{
    if (state == m_state && note == m_note)
        return;
    m_state = state;
    m_note = note;
    emit changed();
}

/*********************************************************************
 * Properties
 *********************************************************************/

bool TrackShow::available() const { return m_available; }
QString TrackShow::path() const { return QDir::toNativeSeparators(m_path); }
QString TrackShow::fileName() const { return QFileInfo(m_path).fileName(); }
QString TrackShow::folder() const { return QDir::toNativeSeparators(QFileInfo(m_path).absolutePath()); }
QUrl TrackShow::folderUrl() const
{
    const QString dir = m_path.isEmpty() ? QFileInfo(defaultShow()).absolutePath() : QFileInfo(m_path).absolutePath();
    return QUrl::fromLocalFile(QDir(dir).exists() ? dir : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}
QString TrackShow::opened() const { return QDir::toNativeSeparators(m_opened); }
bool TrackShow::restartNeeded() const
{
    return m_available && m_path.isEmpty() == false && m_opened.isEmpty() == false
        && QFileInfo(m_path).absoluteFilePath().compare(QFileInfo(m_opened).absoluteFilePath(), Qt::CaseInsensitive) != 0;
}
bool TrackShow::follow() const { return m_follow; }
bool TrackShow::follows() const
{
    return m_follow && QFileInfo(m_path).fileName().compare(QLatin1String(kRigName), Qt::CaseInsensitive) == 0;
}
int TrackShow::state() const { return m_state; }
QString TrackShow::note() const { return m_note; }
QString TrackShow::rigDate() const { return m_rig.date; }
QString TrackShow::checkedAt() const
{
    return m_checked.isValid() ? QLocale::c().toString(m_checked, QStringLiteral("HH:mm")) : QString();
}
bool TrackShow::ready() const { return m_ready; }

/*********************************************************************
 * The start: which file
 *********************************************************************/

QString TrackShow::atStart(const QString &given)
{
    if (m_available == false)
    {
        m_opened = given;
        return given;
    }
    QString pick;
    QString why;
    if (m_path.isEmpty() == false && QFileInfo(m_path).isFile())
    {
        pick = m_path;
        why = QStringLiteral("show.txt");
        if (given.isEmpty() == false && QFileInfo(given).absoluteFilePath().compare(QFileInfo(m_path).absoluteFilePath(), Qt::CaseInsensitive) != 0)
            why += QStringLiteral(" (the shortcut's %1 is not used - SETUP > ADVANCED > SHOW decides)").arg(QDir::toNativeSeparators(given));
    }
    else if (given.isEmpty() == false && QFileInfo(given).isFile())
    {
        pick = QFileInfo(given).absoluteFilePath();
        why = m_path.isEmpty() ? QStringLiteral("the shortcut's - kept in show.txt")
                               : QStringLiteral("the shortcut's - show.txt's %1 is gone").arg(QDir::toNativeSeparators(m_path));
    }
    else if (QFileInfo(defaultShow()).isFile())
    {
        pick = defaultShow();
        why = QStringLiteral("Documents\\TRACK - kept in show.txt");
    }
    if (pick.isEmpty())
    {
        // nothing here: the rig's PSMAIN.qxw is downloaded where show.txt says (or Documents\TRACK)
        m_missing = true;
        if (m_path.isEmpty() || QFileInfo(m_path).fileName().compare(QLatin1String(kRigName), Qt::CaseInsensitive) != 0)
            m_path = defaultShow();
        writeLine(file(QStringLiteral("show.txt")), QDir::toNativeSeparators(m_path));
        log(QStringLiteral("show: no show to open - the rig's PSMAIN.qxw is downloaded to %1").arg(QDir::toNativeSeparators(m_path)));
        m_state = Missing;
        m_note = QStringLiteral("downloading the rig's PSMAIN.qxw");
        return QString();
    }
    if (QFileInfo(pick).absoluteFilePath() != QFileInfo(m_path).absoluteFilePath())
    {
        m_path = QFileInfo(pick).absoluteFilePath();
        writeLine(file(QStringLiteral("show.txt")), QDir::toNativeSeparators(m_path));
    }
    m_opened = m_path;
    log(QStringLiteral("show: %1 (%2)").arg(QDir::toNativeSeparators(m_path), why));
    return m_path;
}

void TrackShow::setLoader(std::function<bool(const QString &)> loader)
{
    m_loader = loader;
}

void TrackShow::start()
{
    if (m_available == false)
        return;
    if (m_missing)
    {
        m_checkTimer.start(100);         // the rig's, now
        return;
    }
    bool isInt = false;
    const int first = qEnvironmentVariableIntValue("QLC_TRACK_SHOW_FIRST_MS", &isInt);
    m_checkTimer.start(isInt && first > 0 ? first : kFirstCheckMs);
}

/*********************************************************************
 * SETUP
 *********************************************************************/

bool TrackShow::setPath(const QString &pathOrUrl)
{
    if (m_available == false)
        return false;
    QString p = pathOrUrl.trimmed();
    const QUrl url(p);
    if (url.isLocalFile())
        p = url.toLocalFile();
    const QFileInfo fi(p);
    if (fi.isFile() == false || fi.suffix().compare(QStringLiteral("qxw"), Qt::CaseInsensitive) != 0)
    {
        setState(m_state, QStringLiteral("not a show (.qxw): %1").arg(QDir::toNativeSeparators(p)));
        return false;
    }
    m_path = fi.absoluteFilePath();
    writeLine(file(QStringLiteral("show.txt")), QDir::toNativeSeparators(m_path));
    log(QStringLiteral("show: SETUP chose %1 - opened at the next start").arg(QDir::toNativeSeparators(m_path)));
    // another file: what was known of the old one is not this one's
    m_localKey.clear();
    m_accepted = false;
    if (m_ready)
    {
        m_ready = false;
        emit readyChanged();
    }
    m_state = Unknown;
    m_note.clear();
    emit changed();
    checkNow();
    return true;
}

void TrackShow::setFollow(bool on)
{
    if (m_available == false || on == m_follow)
        return;
    m_follow = on;
    writeLine(file(QStringLiteral("show-follow.txt")), on ? QStringLiteral("on") : QStringLiteral("off"));
    log(QStringLiteral("show: FOLLOW THE RIG %1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
    if (on == false && m_ready)
    {
        m_ready = false;
        QFile::remove(file(QStringLiteral("show-next.qxw")));
        emit readyChanged();
    }
    emit changed();
    checkNow();
}

void TrackShow::checkNow()
{
    if (m_available == false)
        return;
    m_checkTimer.start(0);
}

void TrackShow::takeRig()
{
    if (m_available == false || m_state != Changed)
        return;
    m_accepted = true;
    log(QStringLiteral("show: TAKE THE RIG'S - %1 is written over at the next UPDATE (the old one kept)").arg(fileName()));
    checkNow();
}

/*********************************************************************
 * The check: the rig release's PSMAIN.qxw
 *********************************************************************/

void TrackShow::slotCheck()
{
    m_checkTimer.start(kCheckEveryMs);
    if (m_busy || m_available == false)
        return;
    m_busy = true;
    if (m_state == Unknown || m_state == Failed)
        setState(m_state, QStringLiteral("checking the rig's show"));
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
            fetchDone(false, QStringLiteral("show: no answer from GitHub (curl %1)").arg(code));
            return;
        }
        releaseRead(out);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        m_busy = false;
        fetchDone(false, QStringLiteral("show: curl.exe would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--max-time") << QStringLiteral("45")
             << QStringLiteral("-A") << QStringLiteral("track-app-show")
             << QStringLiteral("-H") << QStringLiteral("Accept: application/vnd.github+json")
             << apiUrl());
}

void TrackShow::releaseRead(const QByteArray &json)
{
    const QJsonArray assets = QJsonDocument::fromJson(json).object().value(QStringLiteral("assets")).toArray();
    Rig rig;
    for (const QJsonValue &v : assets)
    {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("name")).toString() != QLatin1String(kRigName))
            continue;
        rig.url = a.value(QStringLiteral("browser_download_url")).toString();
        rig.size = qint64(a.value(QStringLiteral("size")).toDouble());
        const QString digest = a.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QStringLiteral("sha256:")))
            rig.sha256 = digest.mid(7).toLatin1().toLower();
        const QDateTime at = QDateTime::fromString(a.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);
        if (at.isValid())
            rig.date = QLocale::c().toString(at.toLocalTime(), QStringLiteral("d MMM"));
    }
    // only GitHub's own download address for the real release; any for a test's
    const bool test = qEnvironmentVariable("QLC_TRACK_SHOW_API").isEmpty() == false;
    const bool urlOk = test ? (rig.url.startsWith(QStringLiteral("https://")) || rig.url.startsWith(QStringLiteral("http://")))
                            : rig.url.startsWith(QStringLiteral("https://github.com/F4MES/qlcplus/releases/download/"));
    if (urlOk == false || rig.size <= 0 || rig.sha256.size() != 64)
    {
        fetchDone(false, QStringLiteral("show: the rig release has no PSMAIN.qxw TRACK can vouch for"));
        return;
    }
    m_rig = rig;
    m_checked = QDateTime::currentDateTime();
    m_lastWhy.clear();

    if (m_missing)
    {
        fetch(m_path);                  // straight to where it is opened from
        return;
    }
    const QFileInfo fi(m_path);
    if (fi.isFile() == false)
    {
        setState(Failed, QStringLiteral("%1 is gone - CHANGE picks another").arg(QDir::toNativeSeparators(m_path)));
        return;
    }
    // the local file's SHA-256, again only when it changed (size + time)
    const QString key = QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    if (key == m_localKey && m_localSha.isEmpty() == false)
    {
        decide(m_localSha);
        return;
    }
    m_busy = true;                      // one hash at a time
    hash(m_path, [this, key](const QByteArray &sha) {
        m_busy = false;
        m_localKey = sha.isEmpty() ? QString() : key;
        m_localSha = sha;
        decide(sha);
    });
}

void TrackShow::decide(const QByteArray &localSha)
{
    if (localSha.isEmpty())
    {
        setState(Failed, QStringLiteral("%1 cannot be read").arg(fileName()));
        return;
    }
    const QString rigFile = file(QStringLiteral("show-rig.txt"));
    const QByteArray put = readLine(rigFile).toLatin1().toLower();
    auto drop = [this]() {
        QFile::remove(file(QStringLiteral("show-next.qxw")));
        if (m_ready)
        {
            m_ready = false;
            emit readyChanged();
        }
    };
    if (localSha == m_rig.sha256)
    {
        if (put != localSha)
            writeLine(rigFile, QString::fromLatin1(localSha));
        drop();
        m_accepted = false;
        setState(Newest, QString());
        return;
    }
    if (follows() == false)
    {
        drop();
        if (QFileInfo(m_path).fileName().compare(QLatin1String(kRigName), Qt::CaseInsensitive) != 0)
            setState(Own, QString());
        else
            setState(NotFollowing, QString());
        return;
    }
    // saved since TRACK put the rig's there (a change of his) - only with TAKE THE RIG'S
    if (put.isEmpty() == false && localSha != put && m_accepted == false)
    {
        drop();
        setState(Changed, QString());
        return;
    }
    // the rig's newer one: here already?
    const QString next = file(QStringLiteral("show-next.qxw"));
    if (m_ready && QFileInfo(next).size() == m_rig.size)
    {
        setState(Ready, QString());
        return;
    }
    if (QFileInfo(next).size() == m_rig.size)
    {
        m_busy = true;
        hash(next, [this, next](const QByteArray &sha) {
            m_busy = false;
            if (sha == m_rig.sha256)
            {
                m_ready = true;
                setState(Ready, QString());
                emit readyChanged();
                return;
            }
            QFile::remove(next);
            fetch(next);
        });
        return;
    }
    fetch(next);
}

/*********************************************************************
 * Download: .part -> size -> SHA-256 -> in place
 *********************************************************************/

void TrackShow::fetch(const QString &target)
{
    m_busy = true;
    m_fetchTarget = target;
    setState(m_missing ? Missing : Fetching, QStringLiteral("downloading the rig's PSMAIN.qxw (%1)").arg(m_rig.date));
    QDir().mkpath(QFileInfo(target).absolutePath());
    const QString part = target + QStringLiteral(".part");
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
            fetchDone(false, QStringLiteral("show: PSMAIN.qxw did not download (curl %1)").arg(code));
            return;
        }
        if (QFileInfo(part).size() != m_rig.size)
        {
            QFile::remove(part);
            fetchDone(false, QStringLiteral("show: PSMAIN.qxw is not whole (%1 of %2 bytes)").arg(QFileInfo(part).size()).arg(m_rig.size));
            return;
        }
        hash(part, [this, part](const QByteArray &sha) {
            if (sha != m_rig.sha256)
            {
                QFile::remove(part);
                fetchDone(false, QStringLiteral("show: PSMAIN.qxw does not match the release's SHA-256 - not used"));
                return;
            }
            QFile::remove(m_fetchTarget);
            if (QFile::rename(part, m_fetchTarget) == false)
            {
                QFile::remove(part);
                fetchDone(false, QStringLiteral("show: PSMAIN.qxw could not be put in place"));
                return;
            }
            m_busy = false;
            if (m_missing)
            {
                missingLoaded();
                return;
            }
            m_ready = true;
            log(QStringLiteral("show: the rig's PSMAIN.qxw (%1) is ready - comes with UPDATE TRACK").arg(m_rig.date));
            setState(Ready, QString());
            emit readyChanged();
        });
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_proc == p)
            m_proc = nullptr;
        p->deleteLater();
        fetchDone(false, QStringLiteral("show: curl.exe would not start"));
    });
    p->setStandardInputFile(QProcess::nullDevice());
    p->start(curl(), QStringList() << QStringLiteral("-sSfL") << QStringLiteral("--retry") << QStringLiteral("2")
             << QStringLiteral("--connect-timeout") << QStringLiteral("30")
             << QStringLiteral("--speed-limit") << QStringLiteral("2000") << QStringLiteral("--speed-time") << QStringLiteral("60")
             << QStringLiteral("--limit-rate") << QStringLiteral("4M")
             << QStringLiteral("-A") << QStringLiteral("track-app-show")
             << QStringLiteral("-o") << part << m_rig.url);
}

void TrackShow::fetchDone(bool ok, const QString &why)
{
    m_busy = false;
    if (ok)
        return;
    if (why != m_lastWhy)
        log(why + QStringLiteral(" - tries again later"));
    m_lastWhy = why;
    if (m_missing)
    {
        setState(Missing, QStringLiteral("could not download the rig's PSMAIN.qxw - tries again in 1 min"));
        m_checkTimer.start(60 * 1000);
        return;
    }
    if (m_state != Ready && m_state != Changed && m_state != Newest)
        setState(Failed, QStringLiteral("the rig's show could not be checked - tries again"));
}

void TrackShow::missingLoaded()
{
    m_missing = false;
    writeLine(file(QStringLiteral("show-rig.txt")), QString::fromLatin1(m_rig.sha256));
    m_localKey.clear();
    const bool ok = m_loader ? m_loader(m_path) : false;
    m_opened = m_path;
    log(QStringLiteral("show: the rig's PSMAIN.qxw (%1) downloaded to %2%3").arg(m_rig.date, QDir::toNativeSeparators(m_path),
        ok ? QStringLiteral(" and opened") : QStringLiteral(" - did not open")));
    setState(ok ? Newest : Failed, ok ? QString() : QStringLiteral("%1 did not open").arg(fileName()));
}

/*********************************************************************
 * UPDATE TRACK: in place
 *********************************************************************/

bool TrackShow::apply()
{
    if (m_available == false || m_ready == false)
        return false;
    const QString next = file(QStringLiteral("show-next.qxw"));
    if (QFileInfo(next).size() != m_rig.size || QFileInfo(m_path).isFile() == false)
    {
        log(QStringLiteral("show: the rig's PSMAIN.qxw is not here any more - the show stays"));
        return false;
    }
    // the old one beside it, as update-qlc.cmd does on the rig
    const QString backup = m_path + QStringLiteral(".bak-") + QLocale::c().toString(QDateTime::currentDateTime(), QStringLiteral("yyyyMMdd-HHmmss"));
    if (QFile::copy(m_path, backup) == false)
    {
        log(QStringLiteral("show: %1 could not be kept as %2 - the show stays").arg(fileName(), QFileInfo(backup).fileName()));
        return false;
    }
    const QString fresh = m_path + QStringLiteral(".new");
    QFile::remove(fresh);
    if (QFile::copy(next, fresh) == false)
    {
        log(QStringLiteral("show: the rig's PSMAIN.qxw could not be copied beside %1 - the show stays").arg(fileName()));
        return false;
    }
    if (QFile::remove(m_path) == false || QFile::rename(fresh, m_path) == false)
    {
        // whatever happened: the old one back
        if (QFileInfo(m_path).isFile() == false)
            QFile::copy(backup, m_path);
        QFile::remove(fresh);
        log(QStringLiteral("show: %1 could not be written over (in use?) - the show stays").arg(fileName()));
        return false;
    }
    writeLine(file(QStringLiteral("show-rig.txt")), QString::fromLatin1(m_rig.sha256));
    QFile::remove(next);
    m_ready = false;
    m_accepted = false;
    m_localKey.clear();
    m_state = Newest;
    m_note.clear();
    log(QStringLiteral("show: the rig's PSMAIN.qxw (%1) in place at %2 - the old one is %3")
            .arg(m_rig.date, QDir::toNativeSeparators(m_path), QFileInfo(backup).fileName()));
    emit readyChanged();
    emit changed();
    return true;
}

/*********************************************************************
 * The SHA-256 in 4 MB steps: the page never waits for it
 *********************************************************************/

void TrackShow::hash(const QString &path, std::function<void(const QByteArray &)> done)
{
    m_hash.reset();
    m_hashDone = done;
    m_hashFile.setFileName(path);
    if (m_hashFile.open(QIODevice::ReadOnly) == false)
    {
        m_hashDone = nullptr;
        done(QByteArray());
        return;
    }
    QTimer::singleShot(0, this, &TrackShow::slotHashChunk);
}

void TrackShow::slotHashChunk()
{
    if (m_hashFile.isOpen() == false)
        return;
    const QByteArray chunk = m_hashFile.read(kHashChunk);
    if (chunk.isEmpty() == false)
    {
        m_hash.addData(chunk);
        if (m_hashFile.atEnd() == false)
        {
            QTimer::singleShot(0, this, &TrackShow::slotHashChunk);
            return;
        }
    }
    m_hashFile.close();
    std::function<void(const QByteArray &)> done = m_hashDone;
    m_hashDone = nullptr;
    if (done)
        done(m_hash.result().toHex());
}
