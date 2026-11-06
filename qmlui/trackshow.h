/*
  Q Light Controller Plus
  trackshow.h

  The TRACK app's show (runde 558, Tobias 11-05: "PSMAIN (showet) skal jo ogsaa hentes/aabnes ligges
  naar man opdatere. Jeg lavede lige en ny genvej, og saa loadede den ikke projektet, fordi den er sat
  til at loade i genvejen ... evt. ogsaa saa man kan aendre stien i advanced").

  TRACK knows its show itself - a shortcut needs no arguments:

      %LOCALAPPDATA%\TRACK\
          show.txt          the show TRACK opens (SETUP > ADVANCED > SHOW: CHANGE)
          show-rig.txt      the SHA-256 of the rig's PSMAIN.qxw TRACK put there (or found there) last
          show-follow.txt   "off": the rig's PSMAIN is not followed
          show-next.qxw     the rig's newer PSMAIN.qxw, downloaded and checked, until UPDATE puts it in place

  - At the start: show.txt; without one the shortcut's file (and it is written to show.txt);
    without that Documents\TRACK\PSMAIN.qxw; without that the rig's PSMAIN.qxw is downloaded there.
  - 45 s after the start, every 15 min and when a new TRACK build turns up: the rig release's
    PSMAIN.qxw (rig-latest, GitHub's SHA-256). A newer one is downloaded and checked in the background
    and comes with UPDATE TRACK (the chip says so; alone it makes the chip too). The old file is kept
    beside it as <name>.bak-<time>.
  - Only a show called PSMAIN.qxw follows the rig. A PSMAIN.qxw changed since TRACK put it there (a
    save) is never written over by itself: SETUP asks (TAKE THE RIG'S, two taps).

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

#ifndef TRACKSHOW_H
#define TRACKSHOW_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>

#include <functional>

class TrackShow : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY changed)
    Q_PROPERTY(QString opened READ opened NOTIFY changed)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY changed)
    Q_PROPERTY(bool follow READ follow NOTIFY changed)
    Q_PROPERTY(bool follows READ follows NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(QString rigDate READ rigDate NOTIFY changed)
    Q_PROPERTY(QString checkedAt READ checkedAt NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)

public:
    /** root: the installed TRACK's folder ("" = not installed: the given file is opened, nothing else) */
    TrackShow(const QString &root, const QString &logDir, QObject *parent = nullptr);
    ~TrackShow();

    /** The file to open now (given: the shortcut's, "" none) - "" when there is none yet */
    QString atStart(const QString &given);
    /** Opens a show TRACK had to download first (main.cpp: App::loadWorkspace) */
    void setLoader(std::function<bool(const QString &)> loader);
    /** The checks (and the download of a missing show) */
    void start();
    /** UPDATE TRACK, just before the restart: the rig's newer PSMAIN in place (the old one kept) */
    bool apply();

    enum State
    {
        Unknown = 0,        // not checked yet
        Newest,             // the rig's newest
        Ready,              // the rig's newer one is here - comes with UPDATE TRACK
        Changed,            // his file changed since TRACK put it there, the rig has another - SETUP asks
        Own,                // not PSMAIN.qxw - his own show, not followed
        Fetching,           // downloading the rig's
        Failed,             // no answer / not checkable - tries again
        NotFollowing,       // FOLLOW THE RIG off, the rig has another
        Missing             // no show here - downloading the rig's
    };

    bool available() const;
    QString path() const;
    QString fileName() const;
    QString folder() const;
    QUrl folderUrl() const;             // CHANGE's dialog starts there
    QString opened() const;
    bool restartNeeded() const;
    bool follow() const;
    bool follows() const;           // follow and the file is a PSMAIN.qxw
    int state() const;
    QString note() const;
    QString rigDate() const;
    QString checkedAt() const;
    bool ready() const;

    /** CHANGE: a .qxw (a path or a file:/// URL) - opened at the next start; false: not a show */
    Q_INVOKABLE bool setPath(const QString &pathOrUrl);
    Q_INVOKABLE void setFollow(bool on);
    Q_INVOKABLE void checkNow();
    /** TAKE THE RIG'S (his file changed): the rig's comes with the next UPDATE */
    Q_INVOKABLE void takeRig();

signals:
    void changed();
    void readyChanged();

private slots:
    void slotCheck();
    void slotHashChunk();

private:
    struct Rig
    {
        QString url;
        qint64 size = 0;
        QByteArray sha256;          // hex
        QString date;
    };

    QString file(const QString &rel) const;
    static QString readLine(const QString &path);
    static bool writeLine(const QString &path, const QString &text);
    static QString defaultShow();
    QString curl() const;
    QString apiUrl() const;
    void log(const QString &what) const;
    void setState(int state, const QString &note = QString());
    void hash(const QString &path, std::function<void(const QByteArray &)> done);
    void releaseRead(const QByteArray &json);
    void decide(const QByteArray &localSha);
    void fetch(const QString &target);
    void fetchDone(bool ok, const QString &why);
    void missingLoaded();

    bool m_available;
    QString m_root, m_logDir;
    QString m_path;                 // the show (show.txt)
    QString m_opened;               // the one opened at this start
    bool m_missing;                 // nothing to open at the start
    bool m_follow;
    bool m_accepted;                // TAKE THE RIG'S
    bool m_ready;
    int m_state;
    QString m_note;
    QDateTime m_checked;
    Rig m_rig;
    QTimer m_checkTimer;
    QProcess *m_proc;
    bool m_busy;
    int m_gen = 0;                  // R570_SHOW: bumped by CHANGE / FOLLOW THE RIG - a download or hash for the old choice is dropped
    QString m_fetchTarget;
    QString m_lastWhy;
    std::function<bool(const QString &)> m_loader;
    // the hash in steps
    QFile m_hashFile;
    QCryptographicHash m_hash;
    std::function<void(const QByteArray &)> m_hashDone;
    // a check of the local file, reused while it does not change
    QString m_localKey;
    QByteArray m_localSha;
};

#endif
