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
#include <QVariant>

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
    Q_PROPERTY(bool bltFeeds READ bltFeeds NOTIFY cdjCountChanged)   // TRACK_APP_BLTFEEDS_R575
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    /** TRACK_APP_LOADTEST_R605: a fetched build that cannot start on this PC ("e7fe1f0|C0000139"; "" none) */
    Q_PROPERTY(QString updateBlocked READ updateBlocked NOTIFY updateChanged)
    Q_PROPERTY(QString updateLatest READ updateLatest NOTIFY updateChanged)
    Q_PROPERTY(int updateStep READ updateStep NOTIFY updateChanged)
    /** R557_READER: the CDJ reader's own updates (Beat Link Trigger's jar) - SETUP, ADVANCED */
    Q_PROPERTY(QObject *reader READ reader CONSTANT)
    /** R558_SHOW: the show TRACK opens and the rig's PSMAIN.qxw - SETUP, ADVANCED */
    Q_PROPERTY(QObject *show READ show CONSTANT)
    /** R558_SHOW: what UPDATE TRACK brings ("build 5e5581d is ready", "... + the rig's new show") */
    Q_PROPERTY(QString updateWhat READ updateWhat NOTIFY updateChanged)
    /** R560_DECKS (Tobias 11-05: the light stood still while a deck played): which deck the lights follow */
    Q_PROPERTY(int deckMaster READ deckMaster NOTIFY decksChanged)
    Q_PROPERTY(int deckFollowed READ deckFollowed NOTIFY decksChanged)
    Q_PROPERTY(QVariantList decksHeard READ decksHeard NOTIFY decksChanged)
    Q_PROPERTY(QVariantList decksPlaying READ decksPlaying NOTIFY decksChanged)
    /** "DECK 2 · MASTER" / "DECK 3 · HEARD" ("" none yet) */
    Q_PROPERTY(QString deckText READ deckText NOTIFY decksChanged)
    /** after 8 s: another deck plays while the followed one stands still ("" all well) */
    Q_PROPERTY(QString deckWarn READ deckWarn NOTIFY decksChanged)
    /** R561_NEXT: the next track - its deck (0 none), "ready" / "analysing" / "failed", its title */
    Q_PROPERTY(int nextDeck READ nextDeck NOTIFY decksChanged)
    Q_PROPERTY(QString nextState READ nextState NOTIFY decksChanged)
    Q_PROPERTY(QString nextTrack READ nextTrack NOTIFY decksChanged)
    /** TRACK_APP_BUILDNOTE_R601: the round the build was made in ("R601"; "" when the release did not say) */
    Q_PROPERTY(QString buildRound READ buildRound CONSTANT)
    /** TRACK_APP_BUILDNOTE_R601: once, on the first start after UPDATE TRACK: 1 updated, 2 the new build did
     *  not start and the launcher went back (0 nothing to say) - and the line that goes with it */
    Q_PROPERTY(int updateNoteKind READ updateNoteKind NOTIFY updateNoteChanged)
    Q_PROPERTY(QString updateNote READ updateNote NOTIFY updateNoteChanged)

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
    bool bltFeeds() const;
    bool updateAvailable() const;
    QString updateBlocked() const;       // TRACK_APP_LOADTEST_R605
    QString updateLatest() const;
    /** As TrackManager's (R474): 0, 1 started, 2 did not start, 3 SHOW OFF first */
    int updateStep() const;
    QObject *reader() const;
    QObject *show() const;
    QString updateWhat() const;
    int deckMaster() const;
    int deckFollowed() const;
    QVariantList decksHeard() const;
    QVariantList decksPlaying() const;
    QString deckText() const;
    QString deckWarn() const;
    int nextDeck() const;
    QString nextState() const;
    QString nextTrack() const;
    QString buildRound() const;          // TRACK_APP_BUILDNOTE_R601
    int updateNoteKind() const;
    QString updateNote() const;

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
    void decksChanged();                 // R560_DECKS
    void updateNoteChanged();            // TRACK_APP_BUILDNOTE_R601

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
    // R560_DECKS: the warning's candidate, how long it has held, and what is shown
    QTimer m_deckTimer;
    QString m_deckCand;
    qint64 m_deckCandSince = 0;
    QString m_deckWarn;
    void deckCheck();

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
    QString m_blocked;          // TRACK_APP_LOADTEST_R605: "<build>|<code>" - fetched, but Windows will not start it here
    void loadTest(const QString &rev);
    QString m_lastWhy;
    int m_step;
    QFile m_hashFile;
    QCryptographicHash m_hash;

    // TRACK_APP_BUILDNOTE_R601: the round, and the note on the first start of a new build
    QString m_round;
    int m_noteKind = 0;
    QString m_note;
    void r601Note();
};

#endif
