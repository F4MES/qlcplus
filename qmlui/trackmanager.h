/*
  Q Light Controller Plus
  trackmanager.h

  Receives track structure (waveform, beat grid, markers) and playback
  position from Beat Link Trigger over a line-delimited JSON TCP link,
  and exposes it to QML for the Track view.

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
#include <QHash>
#include <QList>

class QTcpServer;
class QTcpSocket;
class QQuickView;
class Doc;

#define SETTINGS_TRACK_PORT QStringLiteral("trackmanager/port")
#define TRACK_DEFAULT_PORT  9998

/** Receives per-track structure from Beat Link Trigger.
 *
 *  BLT analyses the whole track once on load and sends the plan here; QLC then
 *  looks up the current beat locally, so no per-beat traffic is needed. */
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

public:
    TrackManager(QQuickView *view, Doc *doc, QObject *parent = nullptr);
    ~TrackManager();

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

    /** Move a marker to a new (already beat-snapped) position.
     *  Local only - nothing is ever sent back to BLT. */
    Q_INVOKABLE void moveMarker(int index, int beat);

    /** Drop the current track data */
    Q_INVOKABLE void clear();

signals:
    void listenPortChanged();
    void listeningChanged();
    void connectedChanged();
    void trackChanged();
    void markersChanged();
    void positionChanged();

protected slots:
    void slotNewConnection();
    void slotReadyRead();
    void slotDisconnected();

protected:
    void restartServer();
    void handleLine(const QByteArray &line);
    void handleTrack(const QJsonObject &obj);
    void handlePosition(const QJsonObject &obj);

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
};

#endif // TRACKMANAGER_H
