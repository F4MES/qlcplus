/*
  Q Light Controller Plus
  trackmanager.cpp

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QQuickView>
#include <QSettings>
#include <QDebug>

#include "trackmanager.h"
#include "doc.h"

TrackManager::TrackManager(QQuickView *view, Doc *doc, QObject *parent)
    : QObject(parent)
    , m_view(view)
    , m_doc(doc)
    , m_server(nullptr)
    , m_port(TRACK_DEFAULT_PORT)
    , m_bpm(0)
    , m_beatCount(0)
    , m_currentBeat(0)
    , m_playing(false)
{
    QSettings settings;
    QVariant var = settings.value(SETTINGS_TRACK_PORT);
    if (var.isValid())
        m_port = var.toInt();

    m_server = new QTcpServer(this);
    connect(m_server, SIGNAL(newConnection()), this, SLOT(slotNewConnection()));

    restartServer();
}

TrackManager::~TrackManager()
{
    if (m_server != nullptr)
        m_server->close();
}

/*********************************************************************
 * Server
 *********************************************************************/

void TrackManager::restartServer()
{
    if (m_server == nullptr)
        return;

    if (m_server->isListening())
        m_server->close();

    if (m_server->listen(QHostAddress::Any, quint16(m_port)))
        qDebug() << "[TrackManager] listening on port" << m_port;
    else
        qWarning() << "[TrackManager] could not listen on port" << m_port
                   << ":" << m_server->errorString();

    emit listeningChanged();
}

int TrackManager::listenPort() const
{
    return m_port;
}

void TrackManager::setListenPort(int port)
{
    if (port == m_port || port <= 0 || port > 65535)
        return;

    m_port = port;

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_PORT, m_port);

    restartServer();
    emit listenPortChanged();
}

bool TrackManager::listening() const
{
    return m_server != nullptr && m_server->isListening();
}

bool TrackManager::connected() const
{
    return m_clients.isEmpty() == false;
}

void TrackManager::slotNewConnection()
{
    while (m_server->hasPendingConnections())
    {
        QTcpSocket *sock = m_server->nextPendingConnection();
        if (sock == nullptr)
            continue;

        m_clients.append(sock);
        m_buffers.insert(sock, QByteArray());

        connect(sock, SIGNAL(readyRead()), this, SLOT(slotReadyRead()));
        connect(sock, SIGNAL(disconnected()), this, SLOT(slotDisconnected()));

        qDebug() << "[TrackManager] client connected from" << sock->peerAddress().toString();
        emit connectedChanged();
    }
}

void TrackManager::slotDisconnected()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (sock == nullptr)
        return;

    m_clients.removeAll(sock);
    m_buffers.remove(sock);
    sock->deleteLater();

    qDebug() << "[TrackManager] client disconnected";
    emit connectedChanged();
}

void TrackManager::slotReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (sock == nullptr)
        return;

    QByteArray buf = m_buffers.value(sock);
    buf.append(sock->readAll());

    // process every complete line; keep the remainder for next time
    int idx;
    while ((idx = buf.indexOf('\n')) >= 0)
    {
        QByteArray line = buf.left(idx).trimmed();
        buf.remove(0, idx + 1);
        if (line.isEmpty() == false)
            handleLine(line);
    }

    // guard against a peer that never sends a newline
    if (buf.size() > 8 * 1024 * 1024)
        buf.clear();

    m_buffers.insert(sock, buf);
}

/*********************************************************************
 * Protocol
 *********************************************************************/

void TrackManager::handleLine(const QByteArray &line)
{
    QJsonParseError err;
    QJsonDocument json = QJsonDocument::fromJson(line, &err);

    if (err.error != QJsonParseError::NoError || json.isObject() == false)
    {
        qWarning() << "[TrackManager] bad JSON:" << err.errorString();
        return;
    }

    QJsonObject obj = json.object();
    QString evt = obj.value(QStringLiteral("evt")).toString();

    if (evt == QStringLiteral("track"))
        handleTrack(obj);
    else if (evt == QStringLiteral("pos"))
        handlePosition(obj);
    else
        qDebug() << "[TrackManager] ignoring unknown evt:" << evt;
}

void TrackManager::handleTrack(const QJsonObject &obj)
{
    m_title = obj.value(QStringLiteral("title")).toString();
    m_bpm = obj.value(QStringLiteral("bpm")).toDouble();
    m_beatCount = obj.value(QStringLiteral("beats")).toInt();

    m_waveform.clear();
    QJsonArray wf = obj.value(QStringLiteral("waveform")).toArray();
    for (int i = 0; i < wf.count(); i++)
        m_waveform.append(wf.at(i).toInt());

    // if no explicit beat count was given, derive it from the waveform
    if (m_beatCount <= 0)
        m_beatCount = m_waveform.count();

    m_markers.clear();
    QJsonArray mk = obj.value(QStringLiteral("markers")).toArray();
    for (int i = 0; i < mk.count(); i++)
    {
        QJsonObject mo = mk.at(i).toObject();
        QVariantMap marker;
        marker.insert(QStringLiteral("beat"), mo.value(QStringLiteral("beat")).toInt());
        marker.insert(QStringLiteral("type"), mo.value(QStringLiteral("type")).toString());
        m_markers.append(marker);
    }

    m_currentBeat = 0;

    qDebug() << "[TrackManager] track:" << m_title << "-" << m_bpm << "BPM,"
             << m_beatCount << "beats," << m_markers.count() << "markers";

    emit trackChanged();
    emit markersChanged();
    emit positionChanged();
}

void TrackManager::handlePosition(const QJsonObject &obj)
{
    int beat = obj.value(QStringLiteral("beat")).toInt();
    bool playing = obj.value(QStringLiteral("playing")).toBool(true);

    if (beat == m_currentBeat && playing == m_playing)
        return;

    m_currentBeat = beat;
    m_playing = playing;

    emit positionChanged();
}

/*********************************************************************
 * Data
 *********************************************************************/

QString TrackManager::title() const
{
    return m_title;
}

qreal TrackManager::bpm() const
{
    return m_bpm;
}

int TrackManager::beatCount() const
{
    return m_beatCount;
}

QVariantList TrackManager::waveform() const
{
    return m_waveform;
}

QVariantList TrackManager::markers() const
{
    return m_markers;
}

int TrackManager::currentBeat() const
{
    return m_currentBeat;
}

bool TrackManager::playing() const
{
    return m_playing;
}

void TrackManager::moveMarker(int index, int beat)
{
    if (index < 0 || index >= m_markers.count())
        return;

    if (beat < 1)
        beat = 1;
    if (m_beatCount > 0 && beat > m_beatCount)
        beat = m_beatCount;

    QVariantMap marker = m_markers.at(index).toMap();
    if (marker.value(QStringLiteral("beat")).toInt() == beat)
        return;

    marker.insert(QStringLiteral("beat"), beat);
    m_markers.replace(index, marker);

    emit markersChanged();
}

void TrackManager::clear()
{
    m_title.clear();
    m_bpm = 0;
    m_beatCount = 0;
    m_waveform.clear();
    m_markers.clear();
    m_currentBeat = 0;
    m_playing = false;

    emit trackChanged();
    emit markersChanged();
    emit positionChanged();
}
