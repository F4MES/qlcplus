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
    , m_trackTimeMs(0)
    , m_durationMs(0)
    , m_analysedState(QStringLiteral("normal"))
    , m_autoRun(false)
    , m_quantize(1)
    , m_bpmLow(TRACK_DEFAULT_BPM_LOW)
    , m_bpmHigh(TRACK_DEFAULT_BPM_HIGH)
    , m_energyTrim(100)
    , m_liveBpm(0)
{
    // slot names default to the groups a club rig is usually built from
    m_slotNames << tr("Laser Bars") << tr("Position") << tr("Animation")
                << tr("Strobes") << tr("Colors");
    for (int i = 0; i < TRACK_SLOT_COUNT; i++)
        m_slotFolders << QString();

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

    loadSettingsMaps();

    m_server = new QTcpServer(this);
    connect(m_server, SIGNAL(newConnection()), this, SLOT(slotNewConnection()));
    restartServer();

    m_energyTimer.setInterval(200);
    connect(&m_energyTimer, SIGNAL(timeout()), this, SLOT(slotEnergyTick()));
    m_energyTimer.start();
}

TrackManager::~TrackManager()
{
    stopLook();
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

int TrackManager::slotCount() const
{
    return TRACK_SLOT_COUNT;
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
        qWarning() << "[TrackManager] cannot listen on" << m_port << m_server->errorString();
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
    QSettings().setValue(SETTINGS_TRACK_PORT, m_port);
    restartServer();
    emit listenPortChanged();
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
        qDebug() << "[TrackManager] client connected";
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
        return;

    QJsonObject obj = json.object();
    QString evt = obj.value(QStringLiteral("evt")).toString();

    if (evt == QStringLiteral("track"))
        handleTrack(obj);
    else if (evt == QStringLiteral("pos"))
        handlePosition(obj);
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

    qDebug() << "[TrackManager] track:" << m_title << m_beatCount << "beats,"
             << m_markers.count() << "markers";

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

    return state.isEmpty() ? QStringLiteral("normal") : state;
}

void TrackManager::updateState()
{
    int beat = m_currentBeat;
    if (m_quantize > 1 && beat > 0)
        beat = ((beat - 1) / m_quantize) * m_quantize + 1;

    QString state = stateAtBeat(beat);
    if (state == m_analysedState)
        return;

    m_analysedState = state;
    emit stateChanged();

    if (m_autoRun && m_overrideState.isEmpty())
        applyLook();
}

QString TrackManager::currentState() const
{
    return m_overrideState.isEmpty() ? m_analysedState : m_overrideState;
}

QString TrackManager::overrideState() const
{
    return m_overrideState;
}

void TrackManager::setOverrideState(QString state)
{
    if (state == m_overrideState)
        return;
    if (state.isEmpty() == false && stateNames().contains(state) == false)
        return;

    m_overrideState = state;
    emit stateChanged();

    if (m_autoRun)
        applyLook();
}

QString TrackManager::runningLook() const
{
    QStringList names;
    foreach (quint32 fid, m_runningFunctions)
    {
        Function *func = m_doc->function(fid);
        if (func != nullptr)
            names.append(func->name());
    }
    return names.isEmpty() ? tr("(nothing)") : names.join(" + ");
}

/*********************************************************************
 * Looks
 *********************************************************************/

QString TrackManager::mapKey(QString state, int slot) const
{
    return QString("%1/%2").arg(state).arg(slot);
}

quint32 TrackManager::resolveSlot(QString state, int slot) const
{
    if (lookRandom(state, slot))
    {
        QVariantList list = slotFunctions(slot);
        if (list.isEmpty())
            return Function::invalidId();
        int idx = int(QRandomGenerator::global()->bounded(list.count()));
        return list.at(idx).toMap().value(QStringLiteral("id")).toUInt();
    }

    return m_lookFunctions.value(mapKey(state, slot), Function::invalidId());
}

void TrackManager::applyLook()
{
    stopLook();

    QString state = currentState();

    for (int slot = 0; slot < TRACK_SLOT_COUNT; slot++)
    {
        quint32 fid = resolveSlot(state, slot);
        if (fid == Function::invalidId())
            continue;

        Function *func = m_doc->function(fid);
        if (func == nullptr)
            continue;

        func->start(m_doc->masterTimer(), FunctionParent::master());
        m_runningFunctions.append(fid);
        m_runningAttrIds.insert(fid,
            func->requestAttributeOverride(TRACK_INTENSITY_ATTR, appliedEnergy()));
    }

    qDebug() << "[TrackManager] look for" << state << ":" << runningLook();
    emit stateChanged();
}

void TrackManager::stopLook()
{
    foreach (quint32 fid, m_runningFunctions)
    {
        Function *func = m_doc->function(fid);
        if (func == nullptr)
            continue;

        int attr = m_runningAttrIds.value(fid, -1);
        if (attr >= 0)
            func->releaseAttributeOverride(attr);
        func->stop(FunctionParent::master());
    }

    m_runningFunctions.clear();
    m_runningAttrIds.clear();
}

void TrackManager::reroll()
{
    if (m_autoRun)
        applyLook();
}

/*********************************************************************
 * Slots and folders
 *********************************************************************/

QString TrackManager::slotName(int slot) const
{
    if (slot < 0 || slot >= m_slotNames.count())
        return QString();
    return m_slotNames.at(slot);
}

void TrackManager::setSlotName(int slot, QString name)
{
    if (slot < 0 || slot >= m_slotNames.count())
        return;
    m_slotNames[slot] = name;
    QSettings().setValue(SETTINGS_TRACK_SLOTNAMES, m_slotNames.join('|'));
    emit looksChanged();
}

QString TrackManager::slotFolder(int slot) const
{
    if (slot < 0 || slot >= m_slotFolders.count())
        return QString();
    return m_slotFolders.at(slot);
}

void TrackManager::setSlotFolder(int slot, QString folder)
{
    if (slot < 0 || slot >= m_slotFolders.count())
        return;
    m_slotFolders[slot] = folder;
    QSettings().setValue(SETTINGS_TRACK_SLOTDIRS, m_slotFolders.join('|'));
    emit looksChanged();
}

QVariantList TrackManager::folderList() const
{
    QVariantList list;
    QStringList seen;

    QVariantMap all;
    all.insert(QStringLiteral("name"), tr("(all functions)"));
    all.insert(QStringLiteral("path"), QString());
    list.append(all);

    if (m_doc == nullptr)
        return list;

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;

        QString path = func->path(true);
        if (path.isEmpty() || seen.contains(path))
            continue;

        seen.append(path);
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), path);
        entry.insert(QStringLiteral("path"), path);
        list.append(entry);
    }

    return list;
}

QVariantList TrackManager::slotFunctions(int slot) const
{
    QVariantList list;
    if (m_doc == nullptr)
        return list;

    QString folder = slotFolder(slot);

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;

        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

        if (folder.isEmpty() == false && func->path(true) != folder)
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QVariant::fromValue(func->id()));
        entry.insert(QStringLiteral("name"), func->name());
        list.append(entry);
    }

    return list;
}

void TrackManager::setLookFunction(QString state, int slot, quint32 fid)
{
    if (stateNames().contains(state) == false)
        return;

    m_lookFunctions.insert(mapKey(state, slot), fid);
    saveLooks();
    emit looksChanged();

    if (state == currentState() && m_autoRun)
        applyLook();
}

quint32 TrackManager::lookFunction(QString state, int slot) const
{
    return m_lookFunctions.value(mapKey(state, slot), Function::invalidId());
}

void TrackManager::setLookRandom(QString state, int slot, bool random)
{
    if (stateNames().contains(state) == false)
        return;

    m_lookRandom.insert(mapKey(state, slot), random);
    saveLooks();
    emit looksChanged();
}

bool TrackManager::lookRandom(QString state, int slot) const
{
    return m_lookRandom.value(mapKey(state, slot), false);
}

void TrackManager::setStateIntensity(QString state, int percent)
{
    if (stateNames().contains(state) == false || percent < 0 || percent > 100)
        return;

    m_stateIntensity.insert(state, percent);
    saveLooks();
    emit energyChanged();
    applyEnergy();
}

int TrackManager::stateIntensity(QString state) const
{
    int dflt = 100;
    if (state == QStringLiteral("break"))
        dflt = 35;
    else if (state == QStringLiteral("build"))
        dflt = 70;

    return m_stateIntensity.value(state, dflt);
}

/*********************************************************************
 * Persistence
 *********************************************************************/

void TrackManager::loadSettingsMaps()
{
    QSettings settings;

    QString names = settings.value(SETTINGS_TRACK_SLOTNAMES).toString();
    if (names.isEmpty() == false)
    {
        QStringList parts = names.split('|');
        for (int i = 0; i < parts.count() && i < m_slotNames.count(); i++)
            m_slotNames[i] = parts.at(i);
    }

    QString dirs = settings.value(SETTINGS_TRACK_SLOTDIRS).toString();
    if (dirs.isEmpty() == false)
    {
        QStringList parts = dirs.split('|');
        for (int i = 0; i < parts.count() && i < m_slotFolders.count(); i++)
            m_slotFolders[i] = parts.at(i);
    }

    // looks: state/slot:fid,...
    foreach (QString pair, settings.value(SETTINGS_TRACK_LOOKS).toString()
                                   .split(',', Qt::SkipEmptyParts))
    {
        QStringList parts = pair.split(':');
        if (parts.count() == 2)
            m_lookFunctions.insert(parts.at(0), parts.at(1).toUInt());
    }

    foreach (QString key, settings.value(SETTINGS_TRACK_RANDOM).toString()
                                  .split(',', Qt::SkipEmptyParts))
        m_lookRandom.insert(key, true);

    foreach (QString pair, settings.value(SETTINGS_TRACK_INTENSITY).toString()
                                   .split(',', Qt::SkipEmptyParts))
    {
        QStringList parts = pair.split(':');
        if (parts.count() == 2 && stateNames().contains(parts.at(0)))
            m_stateIntensity.insert(parts.at(0), parts.at(1).toInt());
    }
}

void TrackManager::saveLooks()
{
    QSettings settings;

    QStringList looks;
    QMapIterator<QString, quint32> it(m_lookFunctions);
    while (it.hasNext())
    {
        it.next();
        looks.append(QString("%1:%2").arg(it.key()).arg(it.value()));
    }
    settings.setValue(SETTINGS_TRACK_LOOKS, looks.join(','));

    QStringList rnd;
    QMapIterator<QString, bool> rit(m_lookRandom);
    while (rit.hasNext())
    {
        rit.next();
        if (rit.value())
            rnd.append(rit.key());
    }
    settings.setValue(SETTINGS_TRACK_RANDOM, rnd.join(','));

    QStringList ints;
    QMapIterator<QString, int> iit(m_stateIntensity);
    while (iit.hasNext())
    {
        iit.next();
        ints.append(QString("%1:%2").arg(iit.key()).arg(iit.value()));
    }
    settings.setValue(SETTINGS_TRACK_INTENSITY, ints.join(','));
}

/*********************************************************************
 * Energy
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
    e = qBound(0.0, e, 1.0) * (qreal(m_energyTrim) / 100.0);
    return qBound(0.0, e, 1.0);
}

qreal TrackManager::appliedEnergy() const
{
    return qBound(0.0, energy() * (qreal(stateIntensity(currentState())) / 100.0), 1.0);
}

void TrackManager::applyEnergy()
{
    foreach (quint32 fid, m_runningFunctions)
    {
        Function *func = m_doc->function(fid);
        int attr = m_runningAttrIds.value(fid, -1);
        if (func != nullptr && attr >= 0)
            func->adjustAttribute(appliedEnergy(), attr);
    }
}

int TrackManager::liveBpm() const { return m_liveBpm; }
int TrackManager::energyTrim() const { return m_energyTrim; }

void TrackManager::setEnergyTrim(int percent)
{
    if (percent == m_energyTrim || percent < 0 || percent > 200)
        return;
    m_energyTrim = percent;
    QSettings().setValue(SETTINGS_TRACK_TRIM, m_energyTrim);
    emit energyChanged();
    applyEnergy();
}

int TrackManager::bpmLow() const { return m_bpmLow; }

void TrackManager::setBpmLow(int bpm)
{
    if (bpm == m_bpmLow || bpm <= 0 || bpm > 300)
        return;
    m_bpmLow = bpm;
    QSettings().setValue(SETTINGS_TRACK_BPMLOW, m_bpmLow);
    emit energyChanged();
    applyEnergy();
}

int TrackManager::bpmHigh() const { return m_bpmHigh; }

void TrackManager::setBpmHigh(int bpm)
{
    if (bpm == m_bpmHigh || bpm <= 0 || bpm > 300)
        return;
    m_bpmHigh = bpm;
    QSettings().setValue(SETTINGS_TRACK_BPMHIGH, m_bpmHigh);
    emit energyChanged();
    applyEnergy();
}

/*********************************************************************
 * Run control and data
 *********************************************************************/

bool TrackManager::autoRun() const { return m_autoRun; }

void TrackManager::setAutoRun(bool enable)
{
    if (enable == m_autoRun)
        return;
    m_autoRun = enable;

    if (m_autoRun)
        applyLook();
    else
        stopLook();

    emit autoRunChanged();
}

int TrackManager::quantize() const { return m_quantize; }

void TrackManager::setQuantize(int beats)
{
    if (beats == m_quantize || beats < 1)
        return;
    m_quantize = beats;
    QSettings().setValue(SETTINGS_TRACK_QUANTIZE, m_quantize);
    emit quantizeChanged();
}

QString TrackManager::title() const { return m_title; }
qreal TrackManager::bpm() const { return m_bpm; }
int TrackManager::beatCount() const { return m_beatCount; }
QVariantList TrackManager::waveform() const { return m_waveform; }
QVariantList TrackManager::markers() const { return m_markers; }
int TrackManager::currentBeat() const { return m_currentBeat; }
bool TrackManager::playing() const { return m_playing; }

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

    beat = qBound(1, beat, m_beatCount > 0 ? m_beatCount : beat);

    QVariantMap marker = m_markers.at(index).toMap();
    if (marker.value(QStringLiteral("beat")).toInt() == beat)
        return;

    marker.insert(QStringLiteral("beat"), beat);
    m_markers.replace(index, marker);

    emit markersChanged();
    updateState();
}

void TrackManager::clear()
{
    stopLook();

    m_title.clear();
    m_bpm = 0;
    m_beatCount = 0;
    m_durationMs = 0;
    m_waveform.clear();
    m_markers.clear();
    m_currentBeat = 0;
    m_trackTimeMs = 0;
    m_playing = false;
    m_analysedState = QStringLiteral("normal");

    emit trackChanged();
    emit markersChanged();
    emit positionChanged();
    emit stateChanged();
}
