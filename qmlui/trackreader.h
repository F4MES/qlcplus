/*
  Q Light Controller Plus
  trackreader.h

  The TRACK app's CDJ reader updates (runde 557, Tobias 11-04: "Kan vi ogsaa checke og opdatere
  blt-hjaelperen ligesom vi checker for track? ... saet knappen til at dukke op under settings";
  previews too, "men der skal ogsaa vaere en knap til at rulle tilbage til den sidst installerede").

  TRACK's helper (trackbridge.jar) runs on Beat Link Trigger's jar - the one TRACK's runtime
  brought (runtime\<id>\beat-link-trigger.jar) unless a newer one was fetched here:

      %LOCALAPPDATA%\TRACK\cdjreader\
          <version>\beat-link-trigger.jar     a release's jar from Deep Symmetry's GitHub
          current.txt                         the version in use ("" or missing: the runtime's own)
          previous.txt                        the one before it - ROLL BACK goes there
          bad.txt                             a version that did not start (not offered again)

  - The check reads Deep Symmetry's releases (GitHub's API) 2 min after start, every 6 h and on
    CHECK NOW: the newest release and the newest preview, each with its jar and GitHub's SHA-256.
  - A newer one (a preview only when it is newer than the newest release) is downloaded in the
    background (at most 4 MB/s) and checked against that SHA-256 before it is offered.
  - UPDATE (two taps on the page, SHOW OFF) and ROLL BACK switch the jar and start only the helper
    again - TRACK keeps running. A jar that has not shown the CDJ count within 60 s is switched
    back by itself.
  - Never more than the jar in use and the one before are kept.

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

#ifndef TRACKREADER_H
#define TRACKREADER_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QProcess>
#include <QTimer>

class TrackBridge;

class TrackReader : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(bool bundled READ bundled NOTIFY changed)
    Q_PROPERTY(QString bundledVersion READ bundledVersion CONSTANT)
    Q_PROPERTY(QString latestStable READ latestStable NOTIFY changed)
    Q_PROPERTY(QString latestPreview READ latestPreview NOTIFY changed)
    Q_PROPERTY(QString candidate READ candidate NOTIFY changed)
    Q_PROPERTY(bool candidatePreview READ candidatePreview NOTIFY changed)
    Q_PROPERTY(bool candidateReady READ candidateReady NOTIFY changed)
    Q_PROPERTY(QString candidateDate READ candidateDate NOTIFY changed)
    Q_PROPERTY(QString previous READ previous NOTIFY changed)
    Q_PROPERTY(int step READ step NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(QString checkedAt READ checkedAt NOTIFY changed)

public:
    /** root: the installed TRACK's folder ("" = not installed: nothing here acts);
     *  bundledJar / bundledVersion: the runtime's own beat-link-trigger.jar */
    TrackReader(TrackBridge *bridge, const QString &root, const QString &logDir,
                const QString &bundledJar, const QString &bundledVersion, QObject *parent = nullptr);
    ~TrackReader();

    /** Before the helper starts: the jar to use; then the checks */
    void start();

    bool available() const;
    QString version() const;            // the jar in use ("8.0.0", "8.1.0-Preview")
    bool bundled() const;               // the runtime's own jar
    QString bundledVersion() const;
    QString latestStable() const;
    QString latestPreview() const;
    QString candidate() const;          // a newer one being fetched / ready ("" none)
    bool candidatePreview() const;
    bool candidateReady() const;
    QString candidateDate() const;      // its release date, d.M
    QString previous() const;           // ROLL BACK's target ("" none)
    /** 0 idle, 1 switching (the helper starts again), 2 it did not start - back on the old one,
     *  3 SHOW OFF first, 4 checking, 5 downloading, 6 the check failed */
    int step() const;
    QString note() const;
    QString checkedAt() const;

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void update(bool showOn);
    Q_INVOKABLE void rollback(bool showOn);

    /** "8.1.0-Preview" > "8.0.0" > "7.4.1"; a preview is older than its own release */
    static int compareVersions(const QString &a, const QString &b);
    /** the version in a runtime id ("jre21.0.5_11-LTS-blt8.0.0" -> "8.0.0") */
    static QString versionOfRuntime(const QString &runtimeId);

signals:
    void changed();

private slots:
    void slotCheck();
    void slotHashChunk();
    void slotStatus();
    void slotHelperExit(int code);
    void slotWatchTimeout();

private:
    struct Release
    {
        QString version;
        QString url;
        qint64 size = 0;
        QByteArray sha256;      // hex
        bool preview = false;
        QString date;
    };

    QString file(const QString &rel) const;
    QString jarOf(const QString &version) const;
    static QString readLine(const QString &path);
    static bool writeLine(const QString &path, const QString &text);
    QString curl() const;
    QString apiUrl() const;
    void log(const QString &what) const;
    void setStep(int step, const QString &note = QString());
    void apply();
    void releasesRead(const QByteArray &json);
    void fetch(const Release &r);
    void fetchDone(bool ok, const QString &why);
    void switchTo(const QString &version, const QString &previous, const QString &why);
    void cleanup();

    TrackBridge *m_bridge;
    bool m_available;
    QString m_root, m_logDir, m_bundledJar, m_bundledVersion;
    QString m_current;          // "" = the runtime's own
    QString m_previous;         // "" none, "bundled" the runtime's own, else a version
    Release m_stable, m_preview, m_cand;
    bool m_candReady;
    int m_step;
    QString m_note;
    QDateTime m_checked;
    QTimer m_checkTimer;
    QProcess *m_proc;
    QFile m_hashFile;
    QCryptographicHash m_hash;
    QString m_lastWhy;
    // the switch being watched
    QTimer m_watch;
    QElapsedTimer m_switchClock;
    QString m_switchFrom;       // what to go back to ("" = the runtime's own)
    QString m_switchFromPrev;
    bool m_watching;
    bool m_rollingBack;
};

#endif
