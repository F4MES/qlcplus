/*
  Q Light Controller Plus
  trackbridge.h

  TRACK app (track-app branch, Tobias 11-04): QLC+ starts and watches its own
  beat-link helper - trackbridge.jar on Beat Link Trigger's installed Java and
  beat-link - instead of Beat Link Trigger. The helper runs the same
  qlc_shared_functions.clj BLT runs and talks to the Track page over the same
  TCP port (9998). Only READS from the decks.

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

#ifndef TRACKBRIDGE_H
#define TRACKBRIDGE_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

class TrackBridge : public QObject
{
    Q_OBJECT

public:
    explicit TrackBridge(QObject *parent = nullptr);
    ~TrackBridge();

    /** Start the helper, and start it again whenever it stops, until stop() */
    void start();

    /** Close the helper's pipe (it stops itself), then kill it if it is still there */
    void stop();

    /** The helper's exit code when Beat Link Trigger holds the PC's beat-link lock */
    static const int kBltRunning = 3;

    /** R548_PLACES: where the logs go, and the runtime (Java + beat-link) shipped with TRACK
     *  (empty: Beat Link Trigger's installed Java and jar) */
    void setPlaces(const QString &logDir, const QString &runtimeDir);

    /** R548_CDJS: the CDJs the helper sees (its "STATUS cdjs=N" lines); 0 while it does not run */
    int cdjCount() const;
    /** TRACK_APP_BLTFEEDS_R575: Beat Link Trigger runs and feeds TRACK - the helper waits (no CDJ count of its own) */
    bool bltFeeds() const { return m_bltFeeds; }
    /** R560_DECKS: the tempo master (1-4, 0 none), the deck the lights follow (0 none yet), the decks
     *  the room hears (playing, fader up) and those playing - from the helper's "DECKS" lines */
    int deckMaster() const;
    int deckFollowed() const;
    QList<int> decksHeard() const;
    QList<int> decksPlaying() const;
    /** R561_NEXT: the deck the next track is probably on (0 none), "ready" (the lights have its sections),
     *  "analysing" or "failed", and its title - from the helper's "NEXT" lines (decksChanged) */
    int nextDeck() const;
    QString nextState() const;
    QString nextTrack() const;

    /** R557_READER: Beat Link Trigger's jar to run on instead of the runtime's ("" = the runtime's) */
    void setJarOverride(const QString &jar);

    /** R557_READER: the helper stops and starts again (another jar) - TRACK keeps running */
    void restart();

    /** One line in <dir>/track.log - never a reason to stop */
    static void writeLog(const QString &dir, const QString &what);

signals:
    void cdjCountChanged(int count);
    void decksChanged();                 // R560_DECKS
    /** R557_READER: a "STATUS cdjs=N" line - the helper runs and sees the network */
    void statusSeen();
    /** R557_READER: the helper ended (its exit code; -1 a crash) */
    void helperExited(int code);

private slots:
    void slotFinished(int exitCode, QProcess::ExitStatus status);
    void slotErrorOccurred(QProcess::ProcessError error);
    void slotLaunch();
    void slotOutput();

private:
    /** Beat Link Trigger's own Java and jar, the helper jar, and anything in trackbridge-lib first */
    bool locate(QString &java, QStringList &classpath, QString &why) const;
    QStringList bltRoots() const;
    void retryIn(int ms, const QString &why);
    void log(const QString &what) const;

    QProcess *m_process;
    QTimer m_retry;
    QElapsedTimer m_started;
    bool m_wanted;
    bool m_logStart;
    bool m_waitedForBlt;
    int m_failures;
    QString m_lastWhy;
    QString m_logDir;
    QString m_runtimeDir;
    QString m_jarOverride;       // R557_READER
    QByteArray m_outBuf;
    int m_cdjCount;
    bool m_bltFeeds = false;             // TRACK_APP_BLTFEEDS_R575
    int m_deckMaster = 0;                // R560_DECKS
    int m_deckFollowed = 0;
    QList<int> m_decksHeard;
    QList<int> m_decksPlaying;
    int m_nextDeck = 0;                  // R561_NEXT
    QString m_nextState;
    QString m_nextTrack;
};

#endif
