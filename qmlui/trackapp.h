/*
  Q Light Controller Plus
  trackapp.h

  The TRACK app (track-app branch). One object for what is TRACK's own:
   - its beat-link helper (TrackBridge) and the CDJs it sees;
   - its own update (runde 548, Tobias 11-04: "bygge opdateringen bedre, saa den bare
     opdatere inde i qlc eller lign. og genstarter sig selv"): the new build is fetched
     and checked in the background, the R474 chip shows it when it is ready, two taps
     with SHOW OFF switch to it and TRACK starts again by itself.

  Installed layout (tools/get-track-app.cmd), %LOCALAPPDATA%\TRACK:
      TRACK.exe              the launcher (trackbridge/launcher/track_launcher.c)
      current.txt            the build it starts; previous.txt the one before
      pending.txt            a build started for the first time (the launcher goes back
                             to previous.txt when it dies within 15 s)
      app\<build>\           TRACK-engine.exe (this program), trackbridge.jar, runtime.txt
      runtime\<id>\          java\ + beat-link-trigger.jar (Java and beat-link, R548_RUNTIME)
      downloads\  logs\

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

#ifndef TRACKAPP_H
#define TRACKAPP_H

#include <functional>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QTimer>

#include "trackbridge.h"
#include "trackreader.h"
#include "trackshow.h"

#include <functional>

class TrackApp : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(QString build READ build CONSTANT)
    Q_PROPERTY(int cdjCount READ cdjCount NOTIFY cdjCountChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(QString updateLatest READ updateLatest NOTIFY updateChanged)
    Q_PROPERTY(int updateStep READ updateStep NOTIFY updateChanged)
    /** R557_READER: the CDJ reader's own updates (Beat Link Trigger's jar) - SETUP, ADVANCED */
    Q_PROPERTY(QObject *reader READ reader CONSTANT)
    /** R558_SHOW: the show TRACK opens and the rig's PSMAIN.qxw - SETUP, ADVANCED */
    Q_PROPERTY(QObject *show READ show CONSTANT)
    /** R558_SHOW: what UPDATE TRACK brings ("build 5e5581d is ready", "... + the rig's new show") */
    Q_PROPERTY(QString updateWhat READ updateWhat NOTIFY updateChanged)

public:
    /** beforeQuit: what the restart does just before QLC+ quits (main.cpp: no "save?" box) */
    TrackApp(bool active, std::function<void()> beforeQuit, QObject *parent = nullptr);
    ~TrackApp();

    /** --restart-after <pid>: wait (max 30 s) until the TRACK that started this one is gone -
     *  before the single-instance guard, which would otherwise turn this start away */
    static void waitForPredecessor(int argc, char *argv[]);

    /** The helper, and (installed TRACK only) the update checks */
    void start();

    bool active() const;
    QString build() const;
    int cdjCount() const;
    bool updateAvailable() const;
    QString updateLatest() const;
    /** As TrackManager's (R474): 0, 1 started, 2 did not start, 3 SHOW OFF first */
    int updateStep() const;
    QObject *reader() const;
    QObject *show() const;
    QString updateWhat() const;

    /** R558_SHOW (main.cpp): the show to open now (given: the shortcut's file) and how to open
     *  one TRACK had to download first */
    QString showAtStart(const QString &given);
    void setShowLoader(std::function<bool(const QString &)> loader);

    /** Two taps on the chip: switch to the fetched build and start again (SHOW OFF only) */
    Q_INVOKABLE void startUpdate(bool showOn);
    /** R558_SHOW: SETUP's OPEN NOW (another show chosen) - TRACK starts again (SHOW OFF only) */
    Q_INVOKABLE void restartTrack(bool showOn);

signals:
    void cdjCountChanged();
    void updateChanged();

private slots:
    void slotCheck();
    void slotHashChunk();

private:
    struct Fetch
    {
        QString asset;          // the release's file name
        qint64 size = 0;
        QByteArray sha256;      // hex
        QString target;         // the folder it unpacks to
        QString marker;         // a file that must be in it afterwards
    };

    void log(const QString &what) const;
    void setStep(int step);
    QString file(const QString &rel) const;
    static QString readLine(const QString &path);
    static bool writeLine(const QString &path, const QString &text);
    QString baseUrl() const;
    QString curl() const;
    void manifestRead(const QByteArray &json);
    void nextFetch();
    void fetchDone(bool ok, const QString &why);
    void download();
    void extract();
    void cleanup();
    void ensureFixtures();
    bool relaunch(const QString &why);

    std::function<void()> m_beforeQuit;
    bool m_active;
    TrackBridge m_bridge;
    TrackReader *m_reader;      // R557_READER
    TrackShow *m_show;          // R558_SHOW

    // the installed layout
    bool m_installed;
    QString m_root, m_appDir, m_rev, m_runtimeId, m_logDir;

    // the update
    QTimer m_checkTimer;
    QProcess *m_proc;
    bool m_busy;
    QList<Fetch> m_jobs;
    Fetch m_job;
    QString m_staged;           // a build fetched, checked and unpacked
    QString m_lastWhy;
    int m_step;
    QFile m_hashFile;
    QCryptographicHash m_hash;
};

#endif
