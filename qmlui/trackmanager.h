/*
  Q Light Controller Plus
  trackmanager.h

  Receives track structure (waveform, beat grid, markers) and playback position
  from Beat Link Trigger over a line-delimited JSON TCP link, derives the
  current section of the track, and drives the QLC+ Functions assigned to each
  section.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#ifndef TRACKMANAGER_H
#define TRACKMANAGER_H

#include <QVariantList>
#include <QJsonObject>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QList>
#include <QMap>

class QTcpServer;
class QTcpSocket;
class QQuickView;
class Function;
class Doc;

#define SETTINGS_TRACK_PORT      QStringLiteral("trackmanager/port")
#define SETTINGS_TRACK_BPMLOW    QStringLiteral("trackmanager/bpmlow")
#define SETTINGS_TRACK_BPMHIGH   QStringLiteral("trackmanager/bpmhigh")
#define SETTINGS_TRACK_TRIM      QStringLiteral("trackmanager/trim")
#define SETTINGS_TRACK_QUANTIZE  QStringLiteral("trackmanager/quantize")
#define SETTINGS_TRACK_FUNCTIONS QStringLiteral("trackmanager/functions")

#define TRACK_DEFAULT_PORT     9998
#define TRACK_DEFAULT_BPM_LOW  80
#define TRACK_DEFAULT_BPM_HIGH 140

/** Receives per-track structure from Beat Link Trigger and runs the Functions
 *  assigned to each section of the track.
 *
 *  BLT analyses the whole track once on load and sends the plan here, so QLC+
 *  can look up the current beat locally and no per-beat traffic is needed. */
class TrackManager : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(TrackManager)

    Q_PROPERTY(int listenPort READ listenPort WRITE setListenPort NOTIFY listenPortChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(qreal bpm READ bpm NOTIFY trackChanged)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY trackChanged)
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY trackChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)

    Q_PROPERTY(int currentBeat READ currentBeat NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY positionChanged)
    Q_PROPERTY(QString currentState READ currentState NOTIFY stateChanged)
    Q_PROPERTY(int positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(int durationMs READ durationMs NOTIFY trackChanged)

    Q_PROPERTY(bool autoRun READ autoRun WRITE setAutoRun NOTIFY autoRunChanged)
    Q_PROPERTY(int quantize READ quantize WRITE setQuantize NOTIFY quantizeChanged)

    Q_PROPERTY(int liveBpm READ liveBpm NOTIFY energyChanged)
    Q_PROPERTY(qreal energy READ energy NOTIFY energyChanged)
    Q_PROPERTY(int energyTrim READ energyTrim WRITE setEnergyTrim NOTIFY energyChanged)
    Q_PROPERTY(int bpmLow READ bpmLow WRITE setBpmLow NOTIFY energyChanged)
    Q_PROPERTY(int bpmHigh READ bpmHigh WRITE setBpmHigh NOTIFY energyChanged)

public:
    TrackManager(QQuickView *view, Doc *doc, QObject *parent = nullptr);
    ~TrackManager();

    /** The four sections a track is divided into */
    static QStringList stateNames();

    int listenPort() const;
    void setListenPort(int port);
    bool listening() const;
    bool connected() const;

    QString title() const;
    qreal bpm() const;
    int beatCount() const;
    QVariantList waveform() const;
    QVariantList markers() const;
    int currentBeat() const;
    bool playing() const;
    QString currentState() const;
    int positionMs() const;
    int durationMs() const;

    bool autoRun() const;
    void setAutoRun(bool enable);
    int quantize() const;
    void setQuantize(int beats);

    int liveBpm() const;
    qreal energy() const;
    int energyTrim() const;
    void setEnergyTrim(int percent);
    int bpmLow() const;
    void setBpmLow(int bpm);
    int bpmHigh() const;
    void setBpmHigh(int bpm);

    /** Move a marker to a new (already beat-snapped) position.
     *  Local only - nothing is ever sent back to BLT. */
    Q_INVOKABLE void moveMarker(int index, int beat);

    /** The state that applies at a given beat */
    Q_INVOKABLE QString stateAtBeat(int beat) const;

    /** Functions of this project that can be assigned to a state */
    Q_INVOKABLE QVariantList functionList() const;

    /** Get/Set the Function assigned to a state */
    Q_INVOKABLE void setStateFunction(QString state, quint32 fid);
    Q_INVOKABLE quint32 stateFunction(QString state) const;
    Q_INVOKABLE QString stateFunctionName(QString state) const;

    /** Assign a random Function to every state and re-apply the current one */
    Q_INVOKABLE void randomize();

    /** Stop whatever we started and forget the track */
    Q_INVOKABLE void clear();

signals:
    void listenPortChanged();
    void listeningChanged();
    void connectedChanged();
    void trackChanged();
    void markersChanged();
    void positionChanged();
    void stateChanged();
    void autoRunChanged();
    void quantizeChanged();
    void energyChanged();
    void functionsChanged();

protected slots:
    void slotNewConnection();
    void slotReadyRead();
    void slotDisconnected();
    void slotEnergyTick();

protected:
    void restartServer();
    void handleLine(const QByteArray &line);
    void handleTrack(const QJsonObject &obj);
    void handlePosition(const QJsonObject &obj);

    void updateState();
    void applyState();
    void stopCurrentFunction();
    void applyEnergy();
    void loadAssignments();
    void saveAssignments();

private:
    QQuickView *m_view;
    Doc *m_doc;

    QTcpServer *m_server;
    QList<QTcpSocket *> m_clients;
    QHash<QTcpSocket *, QByteArray> m_buffers;

    int m_port;

    QString m_title;
    qreal m_bpm;
    int m_beatCount;
    QVariantList m_waveform;
    QVariantList m_markers;

    int m_currentBeat;
    bool m_playing;
    QString m_currentState;
    int m_trackTimeMs;
    int m_durationMs;

    bool m_autoRun;
    int m_quantize;

    /** state name -> Function ID */
    QMap<QString, quint32> m_stateFunctions;

    /** the Function we started, and its intensity override handle */
    quint32 m_runningFunction;
    int m_intensityAttrId;

    int m_bpmLow;
    int m_bpmHigh;
    int m_energyTrim;
    int m_liveBpm;

    QTimer m_energyTimer;
};

#endif // TRACKMANAGER_H
