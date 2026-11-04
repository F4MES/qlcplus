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

#include <QElapsedTimer>
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

private slots:
    void slotFinished(int exitCode, QProcess::ExitStatus status);
    void slotErrorOccurred(QProcess::ProcessError error);
    void slotLaunch();

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
};

#endif
