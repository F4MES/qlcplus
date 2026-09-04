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
#include <QStringList>
#include <QSettings>
#include <QRandomGenerator>
#include <QDebug>

#include "trackmanager.h"
#include "functionparent.h"
#include "mastertimer.h"
#include "function.h"
#include "doc.h"

/** Attribute index 0 is the Intensity attribute every Function registers */
#define TRACK_INTENSITY_ATTR 0

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
    , m_currentState(QStringLiteral("normal"))
    , m_trackTimeMs(0)
    , m_durationMs(0)
    , m_autoRun(false)
    , m_quantize(1)
    , m_runningFunction(Function::invalidId())
    , m_intensityAttrId(-1)
    , m_bpmLow(TRACK_DEFAULT_BPM_LOW)
    , m_bpmHigh(TRACK_DEFAULT_BPM_HIGH)
    , m_energyTrim(100)
    , m_liveBpm(0)
{
    QSettings settings;

    QVariant var = settings.value(SETTINGS_TRACK_PORT);
    if (var.isValid())
        m_port = var.toInt();

    var = settings.value(SETTINGS_TRACK_BPMLOW);
    if (var.isValid())
        m_bpmLow = var.toInt();

    var = settings.value(SETTINGS_TRACK_BPMHIGH);
    if (var.isValid())
        m_bpmHigh = var.toInt();

    var = settings.value(SETTINGS_TRACK_TRIM);
    if (var.isValid())
        m_energyTrim = var.toInt();

    var = settings.value(SETTINGS_TRACK_QUANTIZE);
    if (var.isValid())
        m_quantize = var.toInt();

    loadAssignments();

    m_server = new QTcpServer(this);
    connect(m_server, SIGNAL(newConnection()), this, SLOT(slotNewConnection()));
    restartServer();

    // the live tempo comes from Ableton Link via the MasterTimer, so poll it
    // rather than trying to hook a cross-thread notification
    m_energyTimer.setInterval(200);
    connect(&m_energyTimer, SIGNAL(timeout()), this, SLOT(slotEnergyTick()));
    m_energyTimer.start();
}

TrackManager::~TrackManager()
{
    stopCurrentFunction();

    if (m_server != nullptr)
        m_server->close();
}

QStringList TrackManager::stateNames()
{
    return QStringList() << QStringLiteral("normal")
                         << QStringLiteral("break")
                         << QStringLiteral("build")
                         << QStringLiteral("drop");
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
    m_durationMs = obj.value(QStringLiteral("duration")).toInt();

    m_waveform.clear();
    QJsonArray wf = obj.value(QStringLiteral("waveform")).toArray();
    for (int i = 0; i < wf.count(); i++)
        m_waveform.append(wf.at(i).toInt());

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
    m_trackTimeMs = 0;

    qDebug() << "[TrackManager] track:" << m_title << "-"
             << m_beatCount << "beats," << m_markers.count() << "markers";

    emit trackChanged();
    emit markersChanged();
    emit positionChanged();

    updateState();
}

void TrackManager::handlePosition(const QJsonObject &obj)
{
    int beat = obj.value(QStringLiteral("beat")).toInt();
    bool playing = obj.value(QStringLiteral("playing")).toBool(true);
    int timeMs = obj.value(QStringLiteral("time")).toInt();

    if (beat == m_currentBeat && playing == m_playing && timeMs == m_trackTimeMs)
        return;

    m_currentBeat = beat;
    m_playing = playing;
    m_trackTimeMs = timeMs;

    emit positionChanged();

    updateState();
}

/*********************************************************************
 * Sections
 *********************************************************************/

QString TrackManager::stateAtBeat(int beat) const
{
    // the state is the type of the latest marker at or before this beat
    QString state = QStringLiteral("normal");
    int bestBeat = -1;

    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap marker = m_markers.at(i).toMap();
        int mb = marker.value(QStringLiteral("beat")).toInt();
        if (mb <= beat && mb > bestBeat)
        {
            bestBeat = mb;
            state = marker.value(QStringLiteral("type")).toString();
        }
    }

    if (state.isEmpty())
        state = QStringLiteral("normal");

    return state;
}

void TrackManager::updateState()
{
    // quantise so a change lands on a musical boundary instead of mid-bar
    int beat = m_currentBeat;
    if (m_quantize > 1 && beat > 0)
        beat = ((beat - 1) / m_quantize) * m_quantize + 1;

    QString state = stateAtBeat(beat);
    if (state == m_currentState)
        return;

    m_currentState = state;
    emit stateChanged();

    if (m_autoRun)
        applyState();
}

void TrackManager::applyState()
{
    quint32 fid = stateFunction(m_currentState);

    if (fid == m_runningFunction)
    {
        applyEnergy();
        return;
    }

    stopCurrentFunction();

    if (fid == Function::invalidId())
        return;

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    func->start(m_doc->masterTimer(), FunctionParent::master());
    m_runningFunction = fid;

    // take an intensity override so the energy can ride on top of the Function
    m_intensityAttrId = func->requestAttributeOverride(TRACK_INTENSITY_ATTR, energy());

    qDebug() << "[TrackManager] state" << m_currentState << "-> function" << func->name();
}

void TrackManager::stopCurrentFunction()
{
    if (m_runningFunction == Function::invalidId())
        return;

    Function *func = m_doc->function(m_runningFunction);
    if (func != nullptr)
    {
        if (m_intensityAttrId >= 0)
            func->releaseAttributeOverride(m_intensityAttrId);
        func->stop(FunctionParent::master());
    }

    m_runningFunction = Function::invalidId();
    m_intensityAttrId = -1;
}

/*********************************************************************
 * Energy - guessed from tempo, since a track carries no energy value
 *********************************************************************/

void TrackManager::slotEnergyTick()
{
    int bpm = 0;
    if (m_doc != nullptr && m_doc->masterTimer() != nullptr)
        bpm = m_doc->masterTimer()->bpmNumber();

    if (bpm == m_liveBpm)
        return;

    m_liveBpm = bpm;
    emit energyChanged();

    applyEnergy();
}

qreal TrackManager::energy() const
{
    if (m_bpmHigh <= m_bpmLow)
        return 1.0;

    qreal e = qreal(m_liveBpm - m_bpmLow) / qreal(m_bpmHigh - m_bpmLow);
    if (e < 0.0)
        e = 0.0;
    if (e > 1.0)
        e = 1.0;

    e = e * (qreal(m_energyTrim) / 100.0);

    if (e < 0.0)
        e = 0.0;
    if (e > 1.0)
        e = 1.0;

    return e;
}

void TrackManager::applyEnergy()
{
    if (m_runningFunction == Function::invalidId() || m_intensityAttrId < 0)
        return;

    Function *func = m_doc->function(m_runningFunction);
    if (func != nullptr)
        func->adjustAttribute(energy(), m_intensityAttrId);
}

int TrackManager::liveBpm() const
{
    return m_liveBpm;
}

int TrackManager::energyTrim() const
{
    return m_energyTrim;
}

void TrackManager::setEnergyTrim(int percent)
{
    if (percent == m_energyTrim || percent < 0 || percent > 200)
        return;

    m_energyTrim = percent;

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_TRIM, m_energyTrim);

    emit energyChanged();
    applyEnergy();
}

int TrackManager::bpmLow() const
{
    return m_bpmLow;
}

void TrackManager::setBpmLow(int bpm)
{
    if (bpm == m_bpmLow || bpm <= 0 || bpm > 300)
        return;

    m_bpmLow = bpm;

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_BPMLOW, m_bpmLow);

    emit energyChanged();
    applyEnergy();
}

int TrackManager::bpmHigh() const
{
    return m_bpmHigh;
}

void TrackManager::setBpmHigh(int bpm)
{
    if (bpm == m_bpmHigh || bpm <= 0 || bpm > 300)
        return;

    m_bpmHigh = bpm;

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_BPMHIGH, m_bpmHigh);

    emit energyChanged();
    applyEnergy();
}

/*********************************************************************
 * Function assignment
 *********************************************************************/

QVariantList TrackManager::functionList() const
{
    QVariantList list;

    if (m_doc == nullptr)
        return list;

    // an explicit "none" entry so a state can be left empty
    QVariantMap none;
    none.insert(QStringLiteral("id"), QVariant::fromValue(Function::invalidId()));
    none.insert(QStringLiteral("name"), QObject::tr("(none)"));
    list.append(none);

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;

        // only things that make sense to fire as a look
        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QVariant::fromValue(func->id()));
        entry.insert(QStringLiteral("name"), func->name());
        list.append(entry);
    }

    return list;
}

void TrackManager::setStateFunction(QString state, quint32 fid)
{
    if (stateNames().contains(state) == false)
        return;

    m_stateFunctions.insert(state, fid);
    saveAssignments();
    emit functionsChanged();

    // if this is the state we're in, swap immediately
    if (state == m_currentState && m_autoRun)
        applyState();
}

quint32 TrackManager::stateFunction(QString state) const
{
    return m_stateFunctions.value(state, Function::invalidId());
}

QString TrackManager::stateFunctionName(QString state) const
{
    quint32 fid = stateFunction(state);
    if (fid == Function::invalidId() || m_doc == nullptr)
        return QObject::tr("(none)");

    Function *func = m_doc->function(fid);
    return func != nullptr ? func->name() : QObject::tr("(none)");
}

void TrackManager::randomize()
{
    QVariantList list = functionList();

    // drop the "(none)" entry - a random pick should always be a real Function
    if (list.count() <= 1)
        return;
    list.removeFirst();

    foreach (QString state, stateNames())
    {
        int idx = int(QRandomGenerator::global()->bounded(list.count()));
        quint32 fid = list.at(idx).toMap().value(QStringLiteral("id")).toUInt();
        m_stateFunctions.insert(state, fid);
    }

    saveAssignments();
    emit functionsChanged();

    if (m_autoRun)
        applyState();
}

void TrackManager::loadAssignments()
{
    QSettings settings;
    QString stored = settings.value(SETTINGS_TRACK_FUNCTIONS).toString();
    if (stored.isEmpty())
        return;

    // format: state:id,state:id,...
    foreach (QString pair, stored.split(',', Qt::SkipEmptyParts))
    {
        QStringList parts = pair.split(':');
        if (parts.count() != 2)
            continue;
        if (stateNames().contains(parts.at(0)) == false)
            continue;
        m_stateFunctions.insert(parts.at(0), parts.at(1).toUInt());
    }
}

void TrackManager::saveAssignments()
{
    QStringList parts;
    QMapIterator<QString, quint32> it(m_stateFunctions);
    while (it.hasNext())
    {
        it.next();
        parts.append(QString("%1:%2").arg(it.key()).arg(it.value()));
    }

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_FUNCTIONS, parts.join(','));
}

/*********************************************************************
 * Run control
 *********************************************************************/

bool TrackManager::autoRun() const
{
    return m_autoRun;
}

void TrackManager::setAutoRun(bool enable)
{
    if (enable == m_autoRun)
        return;

    m_autoRun = enable;

    if (m_autoRun)
        applyState();
    else
        stopCurrentFunction();

    emit autoRunChanged();
}

int TrackManager::quantize() const
{
    return m_quantize;
}

void TrackManager::setQuantize(int beats)
{
    if (beats == m_quantize || beats < 1)
        return;

    m_quantize = beats;

    QSettings settings;
    settings.setValue(SETTINGS_TRACK_QUANTIZE, m_quantize);

    emit quantizeChanged();
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

QString TrackManager::currentState() const
{
    return m_currentState;
}

/** Elapsed time within the track. Uses the exact CDJ time when BLT supplies it,
 *  otherwise derives it from the beat number and the tempo. */
int TrackManager::positionMs() const
{
    if (m_trackTimeMs > 0)
        return m_trackTimeMs;

    qreal b = m_bpm > 0 ? m_bpm : qreal(m_liveBpm);
    if (b <= 0 || m_currentBeat <= 0)
        return 0;

    return int((m_currentBeat - 1) * 60000.0 / b);
}

int TrackManager::durationMs() const
{
    if (m_durationMs > 0)
        return m_durationMs;

    qreal b = m_bpm > 0 ? m_bpm : qreal(m_liveBpm);
    if (b <= 0 || m_beatCount <= 0)
        return 0;

    return int(m_beatCount * 60000.0 / b);
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

    // moving a marker can change which section we are in right now
    updateState();
}

void TrackManager::clear()
{
    stopCurrentFunction();

    m_title.clear();
    m_bpm = 0;
    m_beatCount = 0;
    m_waveform.clear();
    m_markers.clear();
    m_currentBeat = 0;
    m_playing = false;
    m_currentState = QStringLiteral("normal");

    emit trackChanged();
    emit markersChanged();
    emit positionChanged();
    emit stateChanged();
}
