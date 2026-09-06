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
#include "trackengine.h"
#include <QQmlContext>
#include <QDateTime>
#include "functionparent.h"
#include "mastertimer.h"
#include "function.h"
#include "doc.h"
#include "qlcchannel.h"
#include "fixture.h"
#include "scene.h"
#include <QRegularExpression>
#include <algorithm>

#define TRACK_INTENSITY_ATTR 0

static int tmSnapBar(int beat);

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
    m_slotNames << tr("Laser Bars") << tr("Position") << tr("Animation")
                << tr("Strobes") << tr("Moving Heads") << tr("Colors");

    for (int i = 0; i < TRACK_SLOT_COUNT; i++)
    {
        m_slotFolders << QString();
        // chases follow the section tempo by default, movement does not
        m_slotSpeed << (i != 1 && i != 5);
    }

    QSettings settings;
    QVariant var;

    var = settings.value(SETTINGS_TRACK_PORT);
    if (var.isValid()) m_port = var.toInt();
    var = settings.value(SETTINGS_TRACK_BPMLOW);
    if (var.isValid()) m_bpmLow = var.toInt();
    var = settings.value(SETTINGS_TRACK_BPMHIGH);
    if (var.isValid()) m_bpmHigh = var.toInt();
    var = settings.value(SETTINGS_TRACK_TRIM);
    if (var.isValid()) m_energyTrim = var.toInt();
    var = settings.value(SETTINGS_TRACK_QUANTIZE);
    if (var.isValid()) m_quantize = var.toInt();

    m_roleMode = true;
    m_colorCursor = 0;
    m_lastColorBar = -1;
    m_lastEngineBeat = -1;
    m_movePick = Function::invalidId();
    for (int i = 0; i < TRACK_ROLE_COUNT; i++)
        m_roleFunctions.append(QList<quint32>());

    loadSettingsMaps();
    loadRoles();

    m_lastPosMs = 0;
    m_linkStale = false;
    m_markersManual = false;
    m_lastMoveIndex = -1;
    m_lastMoveMs = 0;
    m_mixing = false;
    m_dropKick = settings.value(SETTINGS_TRACK_DROPKICK, 0.55).toDouble();
    m_breakKick = settings.value(SETTINGS_TRACK_BREAKKICK, 0.30).toDouble();
    m_lastPosMs = 0;
    m_linkStale = false;
    m_engine = new TrackEngine(m_doc, this);
    // ROOM (by clock, or a tap) is the ENERGY trim: one dial, not two
    connect(m_engine, &TrackEngine::roomChanged, this, &TrackManager::setEnergyTrim);
    if (m_view != nullptr)
        m_view->rootContext()->setContextProperty("trackEngine", m_engine);

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

int TrackManager::slotCount() const { return TRACK_SLOT_COUNT; }

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

int TrackManager::listenPort() const { return m_port; }

void TrackManager::setListenPort(int port)
{
    if (port == m_port || port <= 0 || port > 65535)
        return;
    m_port = port;
    QSettings().setValue(SETTINGS_TRACK_PORT, m_port);
    restartServer();
    emit listenPortChanged();
}

bool TrackManager::connected() const { return m_clients.isEmpty() == false; }

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
        emit connectedChanged();
    }
}

void TrackManager::slotDisconnected()
{
    if (m_playing && m_linkStale == false)
    {
        m_linkStale = true;
        emit linkChanged();
    }

    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (sock == nullptr)
        return;
    m_clients.removeAll(sock);
    m_buffers.remove(sock);
    sock->deleteLater();

    // only the last client leaving is a broken link
    if (m_playing && m_clients.isEmpty() && m_linkStale == false)
    {
        m_linkStale = true;
        emit linkChanged();
    }
    emit connectedChanged();
}

void TrackManager::slotReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (sock == nullptr)
        return;

    // out of the hash while we parse: handling a line may add or drop
    // a client, and that would move the buffers under us
    QByteArray buf = m_buffers.take(sock);
    buf.append(sock->readAll());
    if (buf.size() > 8 * 1024 * 1024)          // a client with no newlines
        buf.clear();

    // walk it once and cut it once: a packet of many lines otherwise
    // moves the rest of the buffer per line
    int start = 0, idx;
    while ((idx = buf.indexOf('\n', start)) >= 0)
    {
        QByteArray line = buf.mid(start, idx - start).trimmed();
        start = idx + 1;
        if (line.isEmpty() == false)
            handleLine(line);
    }
    if (start > 0)
        buf.remove(0, start);
    if (m_clients.contains(sock))
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
    else
        handleExtra(evt, obj);
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

    // the analysis curves, if BLT sends them (older BLT code does not)
    m_low.clear(); m_high.clear(); m_kick.clear();
    QJsonArray lowArr = obj.value(QStringLiteral("low")).toArray();
    for (int i = 0; i < lowArr.count(); i++) m_low.append(lowArr.at(i).toInt());
    QJsonArray highArr = obj.value(QStringLiteral("high")).toArray();
    for (int i = 0; i < highArr.count(); i++) m_high.append(highArr.at(i).toInt());
    QJsonArray kickArr = obj.value(QStringLiteral("kick")).toArray();
    for (int i = 0; i < kickArr.count(); i++) m_kick.append(kickArr.at(i).toInt());

    m_markers.clear();
    QJsonArray mk = obj.value(QStringLiteral("markers")).toArray();
    for (int i = 0; i < mk.count(); i++)
    {
        QJsonObject mo = mk.at(i).toObject();
        QVariantMap marker;
        marker.insert(QStringLiteral("beat"), mo.value(QStringLiteral("beat")).toInt());
        marker.insert(QStringLiteral("type"), mo.value(QStringLiteral("type")).toString());
        marker.insert(QStringLiteral("energy"),
                      mo.value(QStringLiteral("energy")).toDouble(-1.0));
        m_markers.append(marker);
    }

    // hand-made flags are the truth; a fresh analysis gets the second pass,
    // run once the flags are in, and what it changed goes back to BLT's
    // cache (as automatic)
    m_markersManual = obj.value(QStringLiteral("manual")).toBool(false);
    if (m_markersManual == false && refineMarkers())
        sendMarkers(false);

    m_currentBeat = 0;
    m_trackTimeMs = 0;
    m_movePick = Function::invalidId();    // a new track picks afresh
    m_undo.clear();
    m_lastMoveIndex = -1;
    m_lastEngineBeat = -1;
    m_lastSecStart = -1;
    m_lastSecEnd = -1;
    if (m_nextTitle == m_title)
        m_nextTitle.clear();
    if (m_engine != nullptr)
        m_engine->trackLoaded();

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

    // the link is alive, even when it repeats itself
    m_lastPosMs = QDateTime::currentMSecsSinceEpoch();
    if (m_linkStale)
    {
        m_linkStale = false;
        emit linkChanged();
    }
    if (beat == m_currentBeat && playing == m_playing && timeMs == m_trackTimeMs)
        return;

    m_currentBeat = beat;
    m_playing = playing;
    m_trackTimeMs = timeMs;

    emit positionChanged();
    updateState();
    runEngine(false);
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

QString TrackManager::overrideState() const { return m_overrideState; }

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
    if (m_roleMode)
    {
        // The engine owns the output in role mode. Force a fresh decision
        // rather than waiting for the next beat.
        m_lastEngineBeat = -1;
        runEngine(true);
        return;
    }

    stopLook();

    QString state = currentState();
    int division = stateDivision(state);

    for (int slot = 0; slot < TRACK_SLOT_COUNT; slot++)
    {
        quint32 fid = resolveSlot(state, slot);
        if (fid == Function::invalidId())
            continue;

        Function *func = m_doc->function(fid);
        if (func == nullptr)
            continue;

        // Passing the division as an overrideDuration with tempo type Beats
        // keeps Ableton Link as the single timing source: we only change how
        // many beats a step lasts, never the clock itself.
        if (division > 0 && slotFollowsSpeed(slot))
        {
            func->start(m_doc->masterTimer(), FunctionParent::master(), 0,
                        Function::defaultSpeed(), Function::defaultSpeed(),
                        uint(division), Function::Beats);
        }
        else
        {
            func->start(m_doc->masterTimer(), FunctionParent::master());
        }

        if (m_runningFunctions.contains(fid))
            continue;                        // one override per function, not per slot
        m_runningFunctions.append(fid);
        m_runningAttrIds.insert(fid,
            func->requestAttributeOverride(TRACK_INTENSITY_ATTR, appliedEnergy()));
    }

    qDebug() << "[TrackManager] look for" << state << "division" << division
             << ":" << runningLook();
    emit stateChanged();
}

void TrackManager::stopLook()
{
    stopAllRoles();

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

bool TrackManager::slotFollowsSpeed(int slot) const
{
    if (slot < 0 || slot >= m_slotSpeed.count())
        return false;
    return m_slotSpeed.at(slot);
}

void TrackManager::setSlotFollowsSpeed(int slot, bool follow)
{
    if (slot < 0 || slot >= m_slotSpeed.count())
        return;

    m_slotSpeed[slot] = follow;

    QStringList flags;
    for (int i = 0; i < m_slotSpeed.count(); i++)
        flags.append(m_slotSpeed.at(i) ? "1" : "0");
    QSettings().setValue(SETTINGS_TRACK_SLOTSPEED, flags.join(','));

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
    // the engine sets the level per tier itself; this is a trim on top
    int dflt = 100;
    return m_stateIntensity.value(state, dflt);
}

void TrackManager::setStateDivision(QString state, int milliBeats)
{
    if (stateNames().contains(state) == false)
        return;
    if (milliBeats < 0 || milliBeats > 16000)
        return;

    m_stateDivision.insert(state, milliBeats);
    saveLooks();
    emit looksChanged();

    if (state == currentState() && m_autoRun)
        applyLook();
}

int TrackManager::stateDivision(QString state) const
{
    // calm defaults for a minimal style: a chase steps once a beat in the
    // groove, twice a beat in builds and drops, and runs at its own pace
    // (tempo-matched by the engine) in a break
    // 0 = the function's own tempo, snapped to the beat grid by the
    // engine (a halftime chase stays halftime, a fast one stays fast);
    // a value forces one step length on everything
    int dflt = 0;
    Q_UNUSED(state)
    return m_stateDivision.value(state, dflt);
}

/*********************************************************************
 * Persistence
 *********************************************************************/

void TrackManager::loadSettingsMaps()
{
    QSettings settings;

    QString dirs = settings.value(SETTINGS_TRACK_SLOTDIRS).toString();
    if (dirs.isEmpty() == false)
    {
        QStringList parts = dirs.split('|');
        for (int i = 0; i < parts.count() && i < m_slotFolders.count(); i++)
            m_slotFolders[i] = parts.at(i);
    }

    QString speed = settings.value(SETTINGS_TRACK_SLOTSPEED).toString();
    if (speed.isEmpty() == false)
    {
        QStringList parts = speed.split(',');
        for (int i = 0; i < parts.count() && i < m_slotSpeed.count(); i++)
            m_slotSpeed[i] = (parts.at(i) == "1");
    }

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

    foreach (QString pair, settings.value(SETTINGS_TRACK_DIVISION).toString()
                                   .split(',', Qt::SkipEmptyParts))
    {
        QStringList parts = pair.split(':');
        if (parts.count() == 2 && stateNames().contains(parts.at(0)))
            m_stateDivision.insert(parts.at(0), parts.at(1).toInt());
    }
}

void TrackManager::saveLooks()
{
    QSettings settings;

    QStringList looks;
    QMapIterator<QString, quint32> it(m_lookFunctions);
    while (it.hasNext()) { it.next();
        looks.append(QString("%1:%2").arg(it.key()).arg(it.value())); }
    settings.setValue(SETTINGS_TRACK_LOOKS, looks.join(','));

    QStringList rnd;
    QMapIterator<QString, bool> rit(m_lookRandom);
    while (rit.hasNext()) { rit.next();
        if (rit.value()) rnd.append(rit.key()); }
    settings.setValue(SETTINGS_TRACK_RANDOM, rnd.join(','));

    QStringList ints;
    QMapIterator<QString, int> iit(m_stateIntensity);
    while (iit.hasNext()) { iit.next();
        ints.append(QString("%1:%2").arg(iit.key()).arg(iit.value())); }
    settings.setValue(SETTINGS_TRACK_INTENSITY, ints.join(','));

    QStringList divs;
    QMapIterator<QString, int> dit(m_stateDivision);
    while (dit.hasNext()) { dit.next();
        divs.append(QString("%1:%2").arg(dit.key()).arg(dit.value())); }
    settings.setValue(SETTINGS_TRACK_DIVISION, divs.join(','));
}

/*********************************************************************
 * Energy
 *********************************************************************/

void TrackManager::slotEnergyTick()
{
    // watchdog: a playing track that stops sending positions for four
    // seconds is a broken link, not a pause
    // no client at all is a broken link right away
    bool stale = m_playing && ((m_lastPosMs > 0
              && QDateTime::currentMSecsSinceEpoch() - m_lastPosMs > 4000) || m_clients.isEmpty());
    if (stale != m_linkStale)
    {
        m_linkStale = stale;
        emit linkChanged();
    }
    // half a minute without a word from BLT is not a hiccup: the track is
    // over as far as we know - fall back to the idle look instead of a
    // chase running on forever
    if (stale && m_playing && QDateTime::currentMSecsSinceEpoch() - m_lastPosMs > 30000)
    {
        m_playing = false;
        emit positionChanged();
        if (m_engine != nullptr && m_autoRun && m_roleMode)
            m_engine->idle();
    }

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
    // the slider is the energy, 0..100. BPM barely moves in a set and only
    // shifted the scale; the section's loudness still shapes it downstream
    return qBound(0.0, qreal(m_energyTrim) / 100.0, 1.0);
}

qreal TrackManager::appliedEnergy() const
{
    qreal e = energy() * (qreal(stateIntensity(currentState())) / 100.0);

    // The analysis knows how loud THIS section is relative to the rest of
    // the track. Fold that in gently: BPM sets the ceiling, the section
    // decides how much of it we use.
    qreal se = sectionEnergy(m_currentBeat);
    if (se >= 0.0)
        e *= 0.65 + 0.35 * se;

    return qBound(0.0, e, 1.0);
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
    // a hand on the slider takes ROOM off the clock - except when the
    // clock itself is what moved it (announceRoom sets roomPercent first)
    if (m_engine != nullptr && m_engine->roomAuto() && percent != m_engine->roomPercent())
        m_engine->setRoomAuto(false);
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

    if (m_autoRun) applyLook();
    else stopLook();

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
int TrackManager::beatCount() const { return m_beatCount; }
QVariantList TrackManager::waveform() const { return m_waveform; }
QVariantList TrackManager::lowCurve() const { return m_low; }
QVariantList TrackManager::highCurve() const { return m_high; }
QVariantList TrackManager::kickCurve() const { return m_kick; }
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

    // a dragged flag snaps to the bar line
    beat = qBound(1, tmSnapBar(beat), m_beatCount > 0 ? m_beatCount : beat);

    QVariantMap marker = m_markers.at(index).toMap();
    if (marker.value(QStringLiteral("beat")).toInt() == beat)
        return;

    // one undo step per drag, not one per bar it passes
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (index != m_lastMoveIndex || nowMs - m_lastMoveMs > 1500)
        pushUndo();
    m_lastMoveIndex = index;
    m_lastMoveMs = nowMs;
    marker.insert(QStringLiteral("beat"), beat);
    // two flags on one bar make no sense: the one already there goes
    for (int i = m_markers.count() - 1; i >= 0; i--)
    {
        if (i == index || m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt() != beat)
            continue;
        m_markers.removeAt(i);
        if (i < index)
            index--;
        m_lastMoveIndex = index;
    }
    m_markers.replace(index, marker);

    emit markersChanged();
    updateState();
    m_markersManual = true;
    sendMarkers(true);                   // let BLT remember the correction
}

void TrackManager::clear()
{
    stopLook();

    m_title.clear();
    m_bpm = 0;
    m_beatCount = 0;
    m_durationMs = 0;
    m_waveform.clear();
    m_low.clear(); m_high.clear(); m_kick.clear();
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


/*********************************************************************
 * Roles
 *
 * A busking project is almost all static scenes, so assigning "a program
 * per section" asks for chases that do not exist. Instead every function
 * is given a role - what it DOES - and the engine below decides which
 * role runs when, and rides its intensity. That is what the operator's
 * hands do on the Virtual Console; here it happens on its own.
 *********************************************************************/

bool TrackManager::roleMode() const { return m_roleMode; }

void TrackManager::setRoleMode(bool enable)
{
    if (enable == m_roleMode)
        return;

    stopLook();
    m_roleMode = enable;
    QSettings().setValue(SETTINGS_TRACK_ROLEMODE, m_roleMode);

    emit looksChanged();
    if (m_autoRun)
        applyLook();
}

int TrackManager::roleCount() const { return TRACK_ROLE_COUNT; }

QString TrackManager::roleName(int role) const
{
    switch (role)
    {
    case TRACK_ROLE_COLOR:    return tr("Colour");
    case TRACK_ROLE_MOVEMENT: return tr("Movement");
    case TRACK_ROLE_BEAM:     return tr("Beams");
    case TRACK_ROLE_STROBE:   return tr("Strobe");
    case TRACK_ROLE_DARK:     return tr("Ambient");
    default:                  return tr("Unused");
    }
}

QString TrackManager::roleHint(int role) const
{
    switch (role)
    {
    case TRACK_ROLE_COLOR:
        return tr("Base look. Rotates on bar lines, faster as energy rises.");
    case TRACK_ROLE_MOVEMENT:
        return tr("Runs from the groove onward, holds still through a break.");
    case TRACK_ROLE_BEAM:
        return tr("Drops, and the second half of a build.");
    case TRACK_ROLE_STROBE:
        return tr("The last bar before a drop, and the drop's first hit.");
    case TRACK_ROLE_DARK:
        return tr("Breakdowns and intros.");
    default:
        return QString();
    }
}

QString TrackManager::roleKey(QString state, int role) const
{
    return QString("%1/%2").arg(state).arg(role);
}

int TrackManager::roleOf(quint32 fid) const
{
    for (int r = 0; r < m_roleFunctions.count(); r++)
        if (m_roleFunctions.at(r).contains(fid))
            return r;
    return -1;
}

void TrackManager::assignRole(quint32 fid, int role)
{
    for (int r = 0; r < m_roleFunctions.count(); r++)
        m_roleFunctions[r].removeAll(fid);

    if (role >= 0 && role < m_roleFunctions.count())
        m_roleFunctions[role].append(fid);

    saveRoles();
    emit looksChanged();

    if (m_autoRun && m_roleMode)
    {
        m_lastEngineBeat = -1;
        runEngine(true);
    }
}

QVariantList TrackManager::roleFunctions(int role) const
{
    QVariantList list;
    if (m_doc == nullptr || role < 0 || role >= m_roleFunctions.count())
        return list;

    foreach (quint32 fid, m_roleFunctions.at(role))
    {
        Function *func = m_doc->function(fid);
        if (func == nullptr)
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QVariant::fromValue(func->id()));
        entry.insert(QStringLiteral("name"), func->name());
        list.append(entry);
    }

    return list;
}

QVariantList TrackManager::roleTable() const
{
    QVariantList list;
    if (m_doc == nullptr)
        return list;

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;

        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QVariant::fromValue(func->id()));
        entry.insert(QStringLiteral("name"), func->name());
        entry.insert(QStringLiteral("path"), func->path(true));
        entry.insert(QStringLiteral("role"), roleOf(func->id()));
        list.append(entry);
    }

    return list;
}

/** Guess what a function does. The name wins when it says anything useful -
 *  the operator chose it - and otherwise we look at which channel groups the
 *  scene actually touches. */
int TrackManager::classifyFunction(Function *func) const
{
    if (func == nullptr || m_doc == nullptr)
        return -1;

    QString n = (func->name() + QLatin1Char(' ') + func->path(true)).toLower();

    // What the scene DOES beats what it is on. "Laser green" is a colour,
    // "Laser up" is a position - "laser" only decides when nothing else does.
    static const QStringList strobeWords = { "strob", "blind", "flash", "blitz" };
    static const QStringList moveWords   = { "move", "pan", "tilt", "pos", "sweep",
        "fan", "circle", "cirkel", "wave", "anim", "position", "up", "down", "left", "right",
        "center", "centre", "midt", "op", "ned", "venstre", "hoejre", "højre" };
    static const QStringList colourWords = { "colour", "color", "farve", "red",
        "green", "blue", "cyan", "magenta", "magneta", "yellow", "white", "amber",
        "pink", "purple", "orange", "uv", "lime", "roed", "rød", "groen", "grøn",
        "blaa", "blå", "gul", "hvid", "lilla", "rgb", "rainbow", "regnbue" };
    static const QStringList darkWords   = { "dark", "ambient", "chill", "intro",
        "break", "moerk", "mørk", "dim", "low" };
    static const QStringList beamWords   = { "laser", "beam", "gobo", "prism", "spot" };

    auto hasWord = [](const QString &t, const QStringList &words) {
        foreach (const QString &w, words)
        {
            // short words must stand alone so "up" does not match "group"
            if (w.length() <= 3)
            {
                QRegularExpression re(QStringLiteral("(^|[^a-z\\x{00e6}\\x{00f8}\\x{00e5}])%1([^a-z\\x{00e6}\\x{00f8}\\x{00e5}]|$)").arg(w));
                if (re.match(t).hasMatch()) return true;
            }
            else if (t.contains(w))
                return true;
        }
        return false;
    };

    // "laserup" -> "up": strip the fixture words so what is left can be
    // matched on its own
    QString core = n;
    foreach (const QString &w, beamWords)
        core.replace(w, QStringLiteral(" "));

    if (hasWord(core, strobeWords)) return TRACK_ROLE_STROBE;
    if (hasWord(core, moveWords))   return TRACK_ROLE_MOVEMENT;
    if (hasWord(core, colourWords)) return TRACK_ROLE_COLOR;
    if (hasWord(core, darkWords))   return TRACK_ROLE_DARK;
    if (hasWord(n, beamWords))      return TRACK_ROLE_BEAM;

    if (func->type() == Function::EFXType)
        return TRACK_ROLE_MOVEMENT;
    if (func->type() == Function::RGBMatrixType)
        return TRACK_ROLE_COLOR;

    Scene *scene = qobject_cast<Scene *>(func);
    if (scene == nullptr)
        return TRACK_ROLE_COLOR;

    bool hasPos = false, hasShutter = false, hasColour = false, hasBeam = false;
    int dimCount = 0, dimSum = 0;

    foreach (SceneValue sv, scene->values())
    {
        Fixture *fxi = m_doc->fixture(sv.fxi);
        if (fxi == nullptr)
            continue;

        const QLCChannel *ch = fxi->channel(sv.channel);
        if (ch == nullptr)
            continue;

        switch (ch->group())
        {
        case QLCChannel::Pan:
        case QLCChannel::Tilt:
            hasPos = true;
            break;
        case QLCChannel::Shutter:
            // a shutter parked wide open is not a strobe look
            if (sv.value > 0 && sv.value < 250)
                hasShutter = true;
            break;
        case QLCChannel::Colour:
            hasColour = true;
            break;
        case QLCChannel::Gobo:
        case QLCChannel::Beam:
        case QLCChannel::Prism:
            hasBeam = true;
            break;
        case QLCChannel::Intensity:
            if (ch->colour() != QLCChannel::NoColour)
                hasColour = true;
            else
            {
                dimCount++;
                dimSum += int(sv.value);
            }
            break;
        default:
            break;
        }
    }

    if (hasPos)     return TRACK_ROLE_MOVEMENT;
    if (hasShutter) return TRACK_ROLE_STROBE;
    if (hasColour)  return TRACK_ROLE_COLOR;
    if (hasBeam)    return TRACK_ROLE_BEAM;

    if (dimCount > 0)
    {
        int avg = dimSum / dimCount;
        if (avg > 200) return TRACK_ROLE_STROBE;   // a plain dimmer at full: blinder
        if (avg < 80)  return TRACK_ROLE_DARK;     // a low wash: ambient
    }

    return TRACK_ROLE_COLOR;
}

void TrackManager::autoAssignRoles(bool force)
{
    if (m_doc == nullptr)
        return;

    if (force)
    {
        for (int r = 0; r < m_roleFunctions.count(); r++)
            m_roleFunctions[r].clear();
    }
    else
    {
        // drop anything that no longer exists in the project
        for (int r = 0; r < m_roleFunctions.count(); r++)
        {
            QList<quint32> keep;
            foreach (quint32 fid, m_roleFunctions.at(r))
                if (m_doc->function(fid) != nullptr)
                    keep.append(fid);
            m_roleFunctions[r] = keep;
        }
    }

    int added = 0;

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;

        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

        if (roleOf(func->id()) >= 0)
            continue;                       // never overwrite a manual choice

        int role = classifyFunction(func);
        if (role >= 0 && role < m_roleFunctions.count())
        {
            m_roleFunctions[role].append(func->id());
            added++;
        }
    }

    qDebug() << "[TrackManager] auto-assigned" << added << "functions to roles";

    saveRoles();
    emit looksChanged();
}

/*********************************************************************
 * Advanced: pin a role inside one section
 *********************************************************************/

void TrackManager::setForcedRole(QString state, int role, quint32 fid)
{
    if (stateNames().contains(state) == false)
        return;

    if (fid == Function::invalidId())
        m_forcedRole.remove(roleKey(state, role));
    else
        m_forcedRole.insert(roleKey(state, role), fid);

    saveRoles();
    emit looksChanged();

    if (state == currentState() && m_autoRun)
        applyLook();
}

quint32 TrackManager::forcedRole(QString state, int role) const
{
    return m_forcedRole.value(roleKey(state, role), Function::invalidId());
}

void TrackManager::setRoleEnabled(QString state, int role, bool enable)
{
    if (stateNames().contains(state) == false)
        return;

    if (enable)
        m_roleOff.remove(roleKey(state, role));
    else
        m_roleOff.insert(roleKey(state, role), true);

    saveRoles();
    emit looksChanged();

    if (state == currentState() && m_autoRun)
        applyLook();
}

bool TrackManager::roleEnabled(QString state, int role) const
{
    return m_roleOff.value(roleKey(state, role), false) == false;
}

/*********************************************************************
 * The engine
 *********************************************************************/

void TrackManager::sectionBounds(int beat, int &start, int &end) const
{
    start = 1;
    end = m_beatCount > 0 ? m_beatCount + 1 : beat + 64;     // exclusive, like a flag's beat

    for (int i = 0; i < m_markers.count(); i++)
    {
        int mb = m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt();
        if (mb <= beat && mb > start)
            start = mb;
        if (mb > beat && mb < end)
            end = mb;
    }

    if (end <= start)
        end = start + 32;
}

quint32 TrackManager::pickRole(QString state, int role, int cursor) const
{
    if (roleEnabled(state, role) == false)
        return Function::invalidId();

    quint32 forced = forcedRole(state, role);
    if (forced != Function::invalidId() && m_doc != nullptr
        && m_doc->function(forced) != nullptr)
        return forced;

    if (role < 0 || role >= m_roleFunctions.count())
        return Function::invalidId();

    const QList<quint32> &list = m_roleFunctions.at(role);
    if (list.isEmpty())
        return Function::invalidId();

    return list.at(qAbs(cursor) % list.count());
}

void TrackManager::runEngine(bool sectionChanged)
{
    if (m_roleMode == false || m_autoRun == false || m_doc == nullptr || m_engine == nullptr)
        return;

    if (m_playing == false)
    {
        // nothing playing: the start scene, not darkness
        m_engine->idle();
        m_lastEngineBeat = -1;
        return;
    }

    // the link went quiet mid-track: hold the last look rather than guess
    // (checked after the pause, so a paused track still reaches idle)
    if (m_linkStale)
        return;

    int beat = m_currentBeat;
    if (beat <= 0)
        return;
    if (sectionChanged == false && beat == m_lastEngineBeat)
        return;
    m_lastEngineBeat = beat;

    QString state = currentState();
    // the state was picked on a quantised beat: the bounds, the
    // look-ahead and the section energy must use the same one, or the
    // engine is told about a section it is not in
    int stateBeat = beat;
    if (m_quantize > 1)
        stateBeat = ((beat - 1) / m_quantize) * m_quantize + 1;
    int secStart = 1, secEnd = 1;
    sectionBounds(stateBeat, secStart, secEnd);

    // a jump to a cue, or two flags of the same type in a row: the state
    // string does not change but the section does
    if (secStart != m_lastSecStart || secEnd != m_lastSecEnd)
        sectionChanged = true;
    m_lastSecStart = secStart;
    m_lastSecEnd = secEnd;

    // what comes next, so the engine can lean into a drop a bar early
    QString nextState;
    int beatsToNext = 0;
    nextSection(stateBeat, nextState, beatsToNext);

    // Energy = BPM dial x how loud this section is. The section's LEVEL slider
    // goes in separately as a brightness trim, so it cannot change how many
    // effects join.
    qreal se = sectionEnergy(stateBeat);
    qreal en = energy();
    if (se >= 0.0)
        en *= 0.65 + 0.35 * se;
    qreal levelScale = qreal(stateIntensity(state)) / 100.0;

    // what the analysis heard on this very beat (0..1), -1 when BLT did not
    // send the curves: the pulse follows the kick, hats-only passages sparkle
    qreal kick = (beat >= 1 && beat - 1 < m_kick.count()) ? m_kick.at(beat - 1).toInt() / 255.0 : -1.0;
    qreal high = (beat >= 1 && beat - 1 < m_high.count()) ? m_high.at(beat - 1).toInt() / 255.0 : -1.0;

    m_engine->tick(state, beat, secStart, secEnd, en, se,
                   stateDivision(state), sectionChanged, nextState, beatsToNext,
                   m_liveBpm > 0 ? qreal(m_liveBpm) : m_bpm, levelScale, kick, high);

    if (sectionChanged)
        emit stateChanged();
    emit engineChanged();
}

void TrackManager::driveRole(int role, quint32 fid, qreal level, int division,
                             bool hard)
{
    if (fid == Function::invalidId())
    {
        stopRole(role);
        return;
    }

    level = qBound(0.0, level, 1.0);

    // already the right function: just ride its intensity, never restart it
    if (m_roleActive.value(role, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_roleAttr.value(role, -1);
        if (func != nullptr && attr >= 0)
            func->adjustAttribute(level, attr);
        m_roleLevel.insert(role, level);
        return;
    }

    if (hard)
        stopRole(role);
    else
        beginFade(role);

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    // Coming straight back to a function that is still fading out: keep
    // it running and just take it over again.
    if (m_fadeAttr.contains(fid))
    {
        func->releaseAttributeOverride(m_fadeAttr.take(fid));
        m_fadeLevel.remove(fid);
        m_roleActive.insert(role, fid);
        m_roleLevel.insert(role, level);
        m_roleAttr.insert(role,
            func->requestAttributeOverride(TRACK_INTENSITY_ATTR, level));
        return;
    }

    // The division goes in as an overrideDuration with tempo type Beats, so
    // Ableton Link stays the only clock: we change how long a step lasts,
    // never the timing source.
    if (division > 0)
        func->start(m_doc->masterTimer(), FunctionParent::master(), 0,
                    Function::defaultSpeed(), Function::defaultSpeed(),
                    uint(division), Function::Beats);
    else
        func->start(m_doc->masterTimer(), FunctionParent::master());

    m_roleActive.insert(role, fid);
    m_roleLevel.insert(role, level);
    m_roleAttr.insert(role,
        func->requestAttributeOverride(TRACK_INTENSITY_ATTR, level));
}

void TrackManager::stopRole(int role)
{
    quint32 fid = m_roleActive.value(role, Function::invalidId());
    if (fid == Function::invalidId())
        return;

    Function *func = m_doc->function(fid);
    if (func != nullptr)
    {
        int attr = m_roleAttr.value(role, -1);
        if (attr >= 0)
            func->releaseAttributeOverride(attr);
        func->stop(FunctionParent::master());
    }

    m_roleActive.remove(role);
    m_roleAttr.remove(role);
    m_roleLevel.remove(role);
}

void TrackManager::stopAllRoles()
{
    if (m_engine != nullptr)
        m_engine->release();

    foreach (int role, m_roleActive.keys())
        stopRole(role);

    foreach (quint32 fid, m_fadeAttr.keys())
    {
        Function *func = m_doc ? m_doc->function(fid) : nullptr;
        if (func != nullptr)
        {
            func->releaseAttributeOverride(m_fadeAttr.value(fid));
            func->stop(FunctionParent::master());
        }
    }
    m_fadeAttr.clear();
    m_fadeLevel.clear();
    m_movePick = Function::invalidId();
    emit engineChanged();
}

QString TrackManager::roleReport() const
{
    if (m_roleMode == false)
        return runningLook();

    QStringList parts;
    for (int role = 0; role < TRACK_ROLE_COUNT; role++)
    {
        quint32 fid = m_roleActive.value(role, Function::invalidId());
        if (fid == Function::invalidId())
            continue;
        Function *func = m_doc ? m_doc->function(fid) : nullptr;
        if (func != nullptr)
            parts << QString("%1: %2").arg(roleName(role)).arg(func->name());
    }

    return parts.isEmpty() ? tr("(nothing)") : parts.join("   ");
}

/*********************************************************************
 * Role persistence
 *********************************************************************/

void TrackManager::saveRoles()
{
    QStringList roles;
    for (int r = 0; r < m_roleFunctions.count(); r++)
    {
        QStringList ids;
        foreach (quint32 fid, m_roleFunctions.at(r))
            ids << QString::number(fid);
        roles << ids.join(',');
    }
    QSettings().setValue(SETTINGS_TRACK_ROLES, roles.join(';'));

    QStringList forced;
    QMapIterator<QString, quint32> fit(m_forcedRole);
    while (fit.hasNext())
    {
        fit.next();
        forced << QString("%1=%2").arg(fit.key()).arg(fit.value());
    }
    QSettings().setValue(SETTINGS_TRACK_FORCED, forced.join(';'));

    QSettings().setValue(SETTINGS_TRACK_ROLEOFF,
                         QStringList(m_roleOff.keys()).join(';'));
}

void TrackManager::loadRoles()
{
    QSettings settings;

    m_roleMode = settings.value(SETTINGS_TRACK_ROLEMODE, true).toBool();

    QString stored = settings.value(SETTINGS_TRACK_ROLES, QString()).toString();
    if (stored.isEmpty() == false)
    {
        QStringList roles = stored.split(';');
        for (int r = 0; r < roles.count() && r < m_roleFunctions.count(); r++)
        {
            m_roleFunctions[r].clear();
            foreach (QString id, roles.at(r).split(',', Qt::SkipEmptyParts))
                m_roleFunctions[r].append(id.toUInt());
        }
    }

    foreach (QString entry,
             settings.value(SETTINGS_TRACK_FORCED, QString()).toString()
                     .split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split('=');
        if (parts.count() == 2)
            m_forcedRole.insert(parts.at(0), parts.at(1).toUInt());
    }

    foreach (QString key,
             settings.value(SETTINGS_TRACK_ROLEOFF, QString()).toString()
                     .split(';', Qt::SkipEmptyParts))
        m_roleOff.insert(key, true);
}

/*********************************************************************
 * Crossfade and activity
 *********************************************************************/

void TrackManager::beginFade(int role)
{
    quint32 fid = m_roleActive.value(role, Function::invalidId());
    if (fid == Function::invalidId())
        return;

    int attr = m_roleAttr.value(role, -1);
    if (attr < 0 || m_doc == nullptr || m_doc->function(fid) == nullptr)
    {
        stopRole(role);
        return;
    }

    // hand the running function over to the fade list; it keeps its
    // attribute override and gets stepped down beat by beat
    m_fadeAttr.insert(fid, attr);
    m_fadeLevel.insert(fid, m_roleLevel.value(role, 1.0));

    m_roleActive.remove(role);
    m_roleAttr.remove(role);
    m_roleLevel.remove(role);
}

void TrackManager::tickFades()
{
    if (m_fadeAttr.isEmpty() || m_doc == nullptr)
        return;

    const qreal step = 1.0 / 4.0;          // one bar from full to silent

    foreach (quint32 fid, m_fadeAttr.keys())
    {
        Function *func = m_doc->function(fid);
        int attr = m_fadeAttr.value(fid);
        qreal level = m_fadeLevel.value(fid, 0.0) - step;

        if (func == nullptr || level <= 0.0)
        {
            if (func != nullptr)
            {
                func->releaseAttributeOverride(attr);
                func->stop(FunctionParent::master());
            }
            m_fadeAttr.remove(fid);
            m_fadeLevel.remove(fid);
        }
        else
        {
            func->adjustAttribute(level, attr);
            m_fadeLevel.insert(fid, level);
        }
    }
}

QVariantList TrackManager::roleActivity() const
{
    QVariantList list;
    for (int role = 0; role < TRACK_ROLE_COUNT; role++)
        list.append(m_roleActive.contains(role));
    return list;
}

qreal TrackManager::sectionEnergy(int beat) const
{
    qreal best = -1.0;
    int bestBeat = -1;

    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap marker = m_markers.at(i).toMap();
        int mb = marker.value(QStringLiteral("beat")).toInt();
        if (mb <= beat && mb > bestBeat)
        {
            bestBeat = mb;
            best = marker.value(QStringLiteral("energy"), -1.0).toDouble();
        }
    }

    return best;
}

/*********************************************************************
 * Look-ahead, link watchdog, marker feedback
 *********************************************************************/

bool TrackManager::linkStale() const { return m_linkStale; }

void TrackManager::nextSection(int beat, QString &state, int &beatsToNext) const
{
    state.clear();
    beatsToNext = 0;
    int best = -1;
    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap marker = m_markers.at(i).toMap();
        int mb = marker.value(QStringLiteral("beat")).toInt();
        if (mb > beat && (best < 0 || mb < best))
        {
            best = mb;
            state = marker.value(QStringLiteral("type")).toString();
        }
    }
    if (best > 0)
        beatsToNext = best - beat;
}

void TrackManager::sendMarkers(bool manual)
{
    // The operator moved a flag. Tell BLT, so the correction is cached with
    // the track and comes back right the next time it is played.
    QJsonArray arr;
    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap marker = m_markers.at(i).toMap();
        QJsonObject mo;
        mo.insert(QStringLiteral("beat"), marker.value(QStringLiteral("beat")).toInt());
        mo.insert(QStringLiteral("type"), marker.value(QStringLiteral("type")).toString());
        mo.insert(QStringLiteral("energy"), marker.value(QStringLiteral("energy"), -1.0).toDouble());
        arr.append(mo);
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("evt"), QStringLiteral("markers"));
    obj.insert(QStringLiteral("title"), m_title);
    obj.insert(QStringLiteral("manual"), manual);
    obj.insert(QStringLiteral("markers"), arr);
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";

    foreach (QTcpSocket *client, m_clients)
        if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
            client->write(line);
}

/*********************************************************************
 * Marker editing and the second pass
 *
 * BLT analyses the track; QLC+ has the beat curves too, and the operator.
 * Flags the operator sets are the truth and go back to BLT's cache as
 * manual. Flags from a fresh analysis get a second pass here - snapped to
 * bars, too-short sections merged, drops and breaks checked against the
 * kick, missed drops added - and two thresholds that learn from every
 * correction the operator makes: what counts as a drop's kick, and as a
 * break's silence, for THIS room's music.
 *********************************************************************/

qreal TrackManager::kickMean(int fromBeat, int count) const
{
    // mean kick 0..1 over [fromBeat, fromBeat + count), -1 without a curve
    if (m_kick.isEmpty())
        return -1.0;
    qreal sum = 0.0;
    int n = 0;
    for (int b = fromBeat; b < fromBeat + count; b++)
    {
        if (b < 1 || b - 1 >= m_kick.count())
            continue;
        sum += m_kick.at(b - 1).toInt() / 255.0;
        n++;
    }
    return n > 0 ? sum / n : -1.0;
}

static int tmSnapBar(int beat)
{
    // the nearest bar line (beats count from 1)
    int bar = (beat - 1 + 2) / 4;
    return qMax(1, bar * 4 + 1);
}

bool TrackManager::refineMarkers()
{
    if (m_beatCount < 64 || m_kick.isEmpty())
        return false;

    struct Flag { int beat; QString type; qreal energy; };
    QList<Flag> flags;
    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap mk = m_markers.at(i).toMap();
        Flag f;
        f.beat = tmSnapBar(mk.value(QStringLiteral("beat")).toInt());
        f.type = mk.value(QStringLiteral("type")).toString();
        f.energy = mk.value(QStringLiteral("energy"), -1.0).toDouble();
        if (f.type.isEmpty())
            f.type = QStringLiteral("normal");
        flags.append(f);
    }
    std::sort(flags.begin(), flags.end(), [](const Flag &a, const Flag &b) { return a.beat < b.beat; });

    bool changed = false;
    QStringList notes;

    // 1. sections shorter than four bars are noise: the later flag goes,
    //    unless it is a drop landing after a build (that is the point)
    for (int i = 1; i < flags.count(); i++)
    {
        if (flags.at(i).beat - flags.at(i - 1).beat < 16
            && !(flags.at(i).type == QStringLiteral("drop") && flags.at(i - 1).type == QStringLiteral("build")))
        {
            notes << QString("merged %1@%2").arg(flags.at(i).type).arg(flags.at(i).beat);
            flags.removeAt(i);
            i--;
            changed = true;
        }
    }

    // 2. what the kick says about each flag
    for (int i = 0; i < flags.count(); i++)
    {
        int end = i + 1 < flags.count() ? flags.at(i + 1).beat : m_beatCount + 1;
        int len = qMin(8, end - flags.at(i).beat);
        qreal k = kickMean(flags.at(i).beat, qMax(1, len));
        if (k < 0.0)
            continue;
        QString was = flags.at(i).type;
        if (was == QStringLiteral("drop") && k < m_dropKick * 0.6)
            flags[i].type = QStringLiteral("normal");
        else if (was == QStringLiteral("break") && k > qMax(0.6, m_breakKick * 2.0))
            flags[i].type = QStringLiteral("normal");
        else if (was == QStringLiteral("normal") && k >= m_dropKick && i > 0
                 && (flags.at(i - 1).type == QStringLiteral("break") || flags.at(i - 1).type == QStringLiteral("build")))
            flags[i].type = QStringLiteral("drop");
        if (flags.at(i).type != was)
        {
            notes << QString("%1@%2 -> %3").arg(was).arg(flags.at(i).beat).arg(flags.at(i).type);
            changed = true;
        }
    }

    // 3. a drop the analysis missed: two bars without a kick, then two bars
    //    of kick, on a bar line, and no flag within two bars of it
    for (int b = 17; b + 8 <= m_beatCount; b += 4)
    {
        qreal before = kickMean(b - 8, 8);
        qreal after = kickMean(b, 8);
        if (before < 0.0 || after < 0.0 || before >= m_breakKick || after < m_dropKick)
            continue;
        bool nearby = false;             // near is a macro in windows.h
        foreach (const Flag &f, flags)
            if (qAbs(f.beat - b) <= 8)
                nearby = true;
        if (nearby)
            continue;
        Flag drop;
        drop.beat = b;
        drop.type = QStringLiteral("drop");
        drop.energy = -1.0;
        flags.append(drop);
        // and the break that led into it, if none is flagged
        int start = b;
        while (start - 4 >= 1 && kickMean(start - 4, 4) >= 0.0 && kickMean(start - 4, 4) < m_breakKick)
            start -= 4;
        bool hasBreak = false;
        foreach (const Flag &f, flags)
            if (f.beat >= start - 8 && f.beat < b && f.type != QStringLiteral("normal"))
                hasBreak = true;
        if (hasBreak == false && start < b)
        {
            Flag brk;
            brk.beat = start;
            brk.type = QStringLiteral("break");
            brk.energy = -1.0;
            flags.append(brk);
        }
        std::sort(flags.begin(), flags.end(), [](const Flag &x, const Flag &y) { return x.beat < y.beat; });
        notes << QString("drop added @%1").arg(b);
        changed = true;
    }

    // snapping alone counts as a change only if a beat moved
    if (changed == false)
    {
        for (int i = 0; i < flags.count() && i < m_markers.count(); i++)
            if (flags.at(i).beat != m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt())
                changed = true;
    }
    if (changed == false)
        return false;

    m_markers.clear();
    foreach (const Flag &f, flags)
    {
        QVariantMap mk;
        mk.insert(QStringLiteral("beat"), f.beat);
        mk.insert(QStringLiteral("type"), f.type);
        mk.insert(QStringLiteral("energy"), f.energy);
        m_markers.append(mk);
    }
    qDebug() << "[TrackManager] second pass:" << notes.join(", ");
    emit markersChanged();
    return true;
}

void TrackManager::addMarker(int beat, QString type)
{
    if (m_beatCount <= 0 || stateNames().contains(type) == false)
        return;
    beat = qBound(1, tmSnapBar(beat), m_beatCount);

    // a flag already on this bar takes the new type instead
    for (int i = 0; i < m_markers.count(); i++)
    {
        if (m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt() == beat)
        {
            setMarkerType(i, type);
            return;
        }
    }

    pushUndo();
    QVariantMap mk;
    mk.insert(QStringLiteral("beat"), beat);
    mk.insert(QStringLiteral("type"), type);
    mk.insert(QStringLiteral("energy"), -1.0);
    m_markers.append(mk);
    learnFromFlag(type, beat, true);
    markersEdited();
}

void TrackManager::removeMarker(int index)
{
    if (index < 0 || index >= m_markers.count())
        return;
    QVariantMap mk = m_markers.at(index).toMap();
    pushUndo();
    learnFromFlag(mk.value(QStringLiteral("type")).toString(), mk.value(QStringLiteral("beat")).toInt(), false);
    m_markers.removeAt(index);
    markersEdited();
}

void TrackManager::setMarkerType(int index, QString type)
{
    if (index < 0 || index >= m_markers.count() || stateNames().contains(type) == false)
        return;
    QVariantMap mk = m_markers.at(index).toMap();
    if (mk.value(QStringLiteral("type")).toString() == type)
        return;
    pushUndo();
    learnFromFlag(mk.value(QStringLiteral("type")).toString(), mk.value(QStringLiteral("beat")).toInt(), false);
    mk.insert(QStringLiteral("type"), type);
    m_markers.replace(index, mk);
    learnFromFlag(type, mk.value(QStringLiteral("beat")).toInt(), true);
    markersEdited();
}

void TrackManager::markersEdited()
{
    m_markersManual = true;
    m_lastMoveIndex = -1;                // the next drag is its own undo step
    emit markersChanged();
    updateState();
    if (m_autoRun && m_roleMode)
    {
        m_lastEngineBeat = -1;
        runEngine(true);
    }
    sendMarkers(true);                   // BLT keeps it as a hand-made correction
}

void TrackManager::learnFromFlag(const QString &type, int beat, bool added)
{
    // The operator is the teacher. A drop they add at a soft kick lowers
    // what the second pass demands of a drop; a drop they delete at a hard
    // kick raises it. Breaks the other way round. Small steps, remembered.
    qreal k = kickMean(beat, 8);
    if (k < 0.0)
        return;
    const qreal rate = 0.2;
    if (type == QStringLiteral("drop"))
    {
        if (added && k < m_dropKick)
            m_dropKick = m_dropKick * (1.0 - rate) + k * rate;
        else if (added == false && k > m_dropKick)
            m_dropKick = m_dropKick * (1.0 - rate) + k * rate;
    }
    else if (type == QStringLiteral("break"))
    {
        if (added && k > m_breakKick)
            m_breakKick = m_breakKick * (1.0 - rate) + k * rate;
        else if (added == false && k < m_breakKick)
            m_breakKick = m_breakKick * (1.0 - rate) + k * rate;
    }
    m_dropKick = qBound(0.30, m_dropKick, 0.90);
    m_breakKick = qBound(0.05, m_breakKick, qMin(0.50, m_dropKick - 0.10));
    QSettings settings;
    settings.setValue(SETTINGS_TRACK_DROPKICK, m_dropKick);
    settings.setValue(SETTINGS_TRACK_BREAKKICK, m_breakKick);
    qDebug() << "[TrackManager] learned: drop kick" << m_dropKick << "break kick" << m_breakKick;
}

bool TrackManager::markersManual() const { return m_markersManual; }

/*********************************************************************
 * Undo, the mix, the next track, the cache editor, generic events
 *********************************************************************/

void TrackManager::pushUndo()
{
    // a snapshot of the flags and the two learned thresholds, so an undo
    // takes the lesson back with the flag
    QVariantMap snap;
    snap.insert(QStringLiteral("markers"), m_markers);
    snap.insert(QStringLiteral("drop"), m_dropKick);
    snap.insert(QStringLiteral("break"), m_breakKick);
    m_undo.append(snap);
    while (m_undo.count() > 30)
        m_undo.removeFirst();
}

bool TrackManager::canUndoMarkers() const { return m_undo.isEmpty() == false; }

void TrackManager::undoMarkers()
{
    if (m_undo.isEmpty())
        return;
    QVariantMap snap = m_undo.takeLast();
    m_markers = snap.value(QStringLiteral("markers")).toList();
    m_dropKick = snap.value(QStringLiteral("drop"), m_dropKick).toDouble();
    m_breakKick = snap.value(QStringLiteral("break"), m_breakKick).toDouble();
    QSettings settings;
    settings.setValue(SETTINGS_TRACK_DROPKICK, m_dropKick);
    settings.setValue(SETTINGS_TRACK_BREAKKICK, m_breakKick);
    m_markersManual = true;
    m_lastMoveIndex = -1;
    emit markersChanged();
    updateState();
    if (m_autoRun && m_roleMode)
    {
        m_lastEngineBeat = -1;
        runEngine(true);
    }
    sendMarkers(true);
}

void TrackManager::sendEvent(const QJsonObject &obj)
{
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    foreach (QTcpSocket *client, m_clients)
        if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
            client->write(line);
}

bool TrackManager::mixing() const { return m_mixing; }
QString TrackManager::nextTitle() const { return m_nextTitle; }
QVariantList TrackManager::nextMarkers() const { return m_nextMarkers; }

int TrackManager::nextFirstDrop() const
{
    // the bar the next track's first drop lands on, or 0
    int best = 0;
    foreach (const QVariant &v, m_nextMarkers)
    {
        QVariantMap mk = v.toMap();
        if (mk.value(QStringLiteral("type")).toString() == QStringLiteral("drop"))
        {
            int beat = mk.value(QStringLiteral("beat")).toInt();
            if (best == 0 || beat < best)
                best = beat;
        }
    }
    return best > 0 ? (best - 1) / 4 + 1 : 0;
}

QVariantList TrackManager::cacheList() const { return m_cacheList; }

void TrackManager::requestCache()
{
    QJsonObject obj;
    obj.insert(QStringLiteral("evt"), QStringLiteral("cache-list"));
    sendEvent(obj);
}

void TrackManager::forgetTrack(QString title)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("evt"), QStringLiteral("cache-forget"));
    obj.insert(QStringLiteral("title"), title);
    sendEvent(obj);
}

void TrackManager::forgetAutomatic()
{
    QJsonObject obj;
    obj.insert(QStringLiteral("evt"), QStringLiteral("cache-forget-auto"));
    sendEvent(obj);
}

void TrackManager::handleExtra(const QString &evt, const QJsonObject &obj)
{
    if (evt == QStringLiteral("mix"))
    {
        // two decks on air: a transition. The engine holds colour and calm.
        bool mixing = obj.value(QStringLiteral("mixing")).toBool(false);
        if (mixing != m_mixing)
        {
            m_mixing = mixing;
            if (m_engine != nullptr)
                m_engine->setMixing(mixing);
            emit mixChanged();
        }
    }
    else if (evt == QStringLiteral("next"))
    {
        // what is loaded on the other deck, analysed ahead of time
        m_nextTitle = obj.value(QStringLiteral("title")).toString();
        m_nextMarkers.clear();
        QJsonArray mk = obj.value(QStringLiteral("markers")).toArray();
        for (int i = 0; i < mk.count(); i++)
        {
            QJsonObject mo = mk.at(i).toObject();
            QVariantMap marker;
            marker.insert(QStringLiteral("beat"), mo.value(QStringLiteral("beat")).toInt());
            marker.insert(QStringLiteral("type"), mo.value(QStringLiteral("type")).toString());
            marker.insert(QStringLiteral("energy"), mo.value(QStringLiteral("energy")).toDouble(-1.0));
            m_nextMarkers.append(marker);
        }
        if (m_nextTitle == m_title)
            m_nextTitle.clear();         // it is this track, not the next
        emit mixChanged();
    }
    else if (evt == QStringLiteral("cache"))
    {
        m_cacheList.clear();
        QJsonArray tracks = obj.value(QStringLiteral("tracks")).toArray();
        for (int i = 0; i < tracks.count(); i++)
        {
            QJsonObject t = tracks.at(i).toObject();
            QVariantMap row;
            row.insert(QStringLiteral("title"), t.value(QStringLiteral("title")).toString());
            row.insert(QStringLiteral("flags"), t.value(QStringLiteral("flags")).toInt());
            row.insert(QStringLiteral("manual"), t.value(QStringLiteral("manual")).toBool(false));
            row.insert(QStringLiteral("version"), t.value(QStringLiteral("version")).toInt());
            m_cacheList.append(row);
        }
        emit cacheChanged();
    }
}
