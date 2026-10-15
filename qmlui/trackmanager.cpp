/*
  Q Light Controller Plus
  trackmanager.cpp

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#include <QJsonDocument>
#include <QCoreApplication>         // R459_RM_APPINC
#include <QProcess>                 // R465_BLT_LIVE_INC
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
#include <cmath>                   // R450_TT_CMATH

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

    var = settings.value(SETTINGS_TRACK_PORT);        // R201_RESTORE_BOUNDS
    if (var.isValid() && var.toInt() > 0 && var.toInt() <= 65535) m_port = var.toInt();
    var = settings.value(SETTINGS_TRACK_BPMLOW);
    if (var.isValid() && var.toInt() > 0 && var.toInt() <= 300) m_bpmLow = var.toInt();
    var = settings.value(SETTINGS_TRACK_BPMHIGH);
    if (var.isValid() && var.toInt() > 0 && var.toInt() <= 300) m_bpmHigh = var.toInt();
    // R380_ENERGY_ZERO: ENERGY starts at 0 % every time - not last night's,
    // not the crash's - the start scene until the slider comes up
    m_energyTrim = 0;
    var = settings.value(SETTINGS_TRACK_QUANTIZE);
    if (var.isValid()) m_quantize = qBound(1, var.toInt(), 32);

    m_roleMode = true;
    m_colorCursor = 0;
    m_lastColorBar = -1;
    m_lastEngineBeat = -1;
    m_lastSecStart = -1;                  // R168_SECTION_INIT
    m_lastSecEnd = -1;
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
    m_dropKick = qBound(0.30, m_dropKick, 0.90);      // R201_KICK_BOUNDS
    m_breakKick = qBound(0.05, m_breakKick, qMin(0.50, m_dropKick - 0.10));   // R202_KICK_GAP
    m_lastPosMs = 0;
    m_linkStale = false;
    m_engine = new TrackEngine(m_doc, this);
    // R459_RM_REG: QLC's web server finds the phone remote's door here (it
    // does not link to the Track code)
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->setProperty("trackRemote", QVariant::fromValue<QObject *>(this));
    // R465_BLT_LIVE_START: is Beat Link Trigger running - now, and every 5 s
    connect(&m_bltProbeTimer, &QTimer::timeout, this, &TrackManager::probeBlt);
    m_bltProbeTimer.start(5000);
    QTimer::singleShot(0, this, &TrackManager::probeBlt);
    // R464_ZERO_AFTER: LAST TRACK over with ENERGY at 0 - the start scene, as a hand at 0
    connect(m_engine, &TrackEngine::lastTrackChanged, this, [this]() {
        const int st = m_engine != nullptr ? m_engine->lastTrack() : 0;
        if (st == 0 && m_lastStageSeen > 0 && m_autoRun && m_engine != nullptr && m_energyTrim == 0
            && m_engine->startScene() == false)
        {
            m_engine->setStartScene(true, true);
            m_startAuto = m_engine->startScene();
        }
        m_lastStageSeen = st;
    }, Qt::QueuedConnection);
    // ROOM (by clock, or a tap) is the ENERGY trim: one dial, not two
    connect(m_engine, &TrackEngine::roomChanged, this, &TrackManager::setEnergyTrim);
    // R184_START_SCENE_INIT: tonight's state, and the clock's ENERGY at once -
    // it used to arrive on the first beat, so SHOW ON before the music
    // started saw last night's slider
    m_showRanNight = QSettings().value(SETTINGS_TRACK_SHOWRAN).toString();   // R185_SHOWRAN_NIGHT
    // R185_START_AUTO_TRACK: however the start scene comes down - its tile,
    // release(), SHOW OFF - it is no longer the one SHOW ON put up
    connect(m_engine, &TrackEngine::liveChanged, this, [this]() {
        if (m_startAuto && m_engine->startScene() == false)
            m_startAuto = false;
    });
    m_engine->announceRoom();
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
    // R463_RM_FREE: the web server's door to the phone remote goes with us
    if (QCoreApplication::instance() != nullptr
        && QCoreApplication::instance()->property("trackRemote").value<QObject *>() == this)
        QCoreApplication::instance()->setProperty("trackRemote", QVariant());
    stopLook();
    if (m_server != nullptr)
        m_server->close();
}

QStringList TrackManager::stateNames()
{
    // intro and outro come from rekordbox' phrase analysis; the
    // engine runs them as quiet breaks at the ends of a track
    return QStringList() << QStringLiteral("normal")
                         << QStringLiteral("break")
                         << QStringLiteral("build")
                         << QStringLiteral("drop")
                         << QStringLiteral("intro")
                         << QStringLiteral("outro")
                         // a high groove / a drop with no build (BLT analysis 7): R169_DRIVE_STATE
                         << QStringLiteral("drive");
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
        forgetLinkState();                       // LINK_FORGET_FJ3: BLT starts afresh
        emit connectedChanged();
    }
}

void TrackManager::forgetLinkState()
{
    // LINK_FORGET_FJ3_FN: what BLT had told us about the other decks
    const bool had = m_mixing || m_nextTitle.isEmpty() == false || m_nextMarkers.isEmpty() == false;
    m_mixing = false;
    m_nextTitle.clear();
    m_nextMarkers.clear();
    if (m_engine != nullptr)
    {
        m_engine->setMixing(false);
        m_engine->setNextKey(QString());
    }
    if (had)
        emit mixChanged();
}

void TrackManager::slotDisconnected()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (sock == nullptr)
        return;
    m_clients.removeAll(sock);
    m_buffers.remove(sock);
    sock->deleteLater();

    // only the last client leaving is a broken link
    if (m_playing && m_clients.isEmpty() && m_linkStale == false && m_testTrack == false)   // R452_TT_STALE2
    {
        m_linkStale = true;
        emit linkChanged();
        if (m_engine != nullptr && m_autoRun && m_roleMode)   // R246_DISCONNECT_IDLE
        {
            m_lastEngineBeat = -1;
            m_engine->linkIdle();        // R463_LINK_IDLE_DISCONNECT
        }
    }
    // R187_MIX_ON_DISCONNECT: BLT forgets it was mixing when it reconnects
    // and only ever announces a mix starting - a mix that ended while it
    // was away held colour and calm for the whole next track
    if (m_clients.isEmpty())
    {
        forgetLinkState();                       // LINK_FORGET_FJ3_LAST
        m_cacheList.clear();                     // the editor's rows were BLT's
        emit cacheChanged();
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
    QList<QByteArray> lines;             // R303_POS_BURST: cut first, then handle
    while ((idx = buf.indexOf('\n', start)) >= 0)
    {
        QByteArray line = buf.mid(start, idx - start).trimmed();
        start = idx + 1;
        if (line.isEmpty() == false)
            lines.append(line);
    }
    // a playing pos with a later, higher playing pos right behind it: skip it
    auto playingBeat = [](const QByteArray &l) -> int {
        if (l.contains("\"pos\"") == false)
            return -1;
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (o.value(QStringLiteral("evt")).toString() != QStringLiteral("pos")
            || o.value(QStringLiteral("playing")).toBool(true) == false)
            return -1;
        return o.value(QStringLiteral("beat")).toInt();
    };
    for (int i = 0; i < lines.count(); i++)
    {
        if (i + 1 < lines.count())
        {
            const int here = playingBeat(lines.at(i));
            const int after = here > 0 ? playingBeat(lines.at(i + 1)) : -1;
            if (here > 0 && after > here                        // R305_POS_BURST_LANDING
                && ((here - 1 - m_downbeat) % 4 + 4) % 4 != 0
                && stateAtBeat(here) == stateAtBeat(after))
                continue;
        }
        handleLine(lines.at(i));
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
    {
        qWarning() << "[TrackManager] bad line from BLT:" << err.errorString()   // R207_BAD_LINE
                   << line.size() << "bytes";
        return;
    }

    QJsonObject obj = json.object();
    QString evt = obj.value(QStringLiteral("evt")).toString();

    // R450_TT_BLT: the test track plays - BLT's positions are not its; a real
    // track from BLT stops the test and plays
    if (m_testTrack && evt == QStringLiteral("pos"))
        return;
    if (m_testTrack && evt == QStringLiteral("track"))
        stopTestTrack();
    if (evt == QStringLiteral("track"))
        handleTrack(obj);
    else if (evt == QStringLiteral("pos"))
        handlePosition(obj);
    else
        handleExtra(evt, obj);
}

void TrackManager::handleTrack(const QJsonObject &obj)
{
    // R187_SAME_TRACK: the track that is playing, sent again - BLT resends
    // after every reconnect and when its shared functions are saved, and
    // (the same song from the OTHER deck is a new play, R188_TRACK_PLAYER -
    // R188_RESENT_COMMENT). That
    // reset the beat to 0 and restarted the engine mid-song: colour, cast
    // and moves thrown away, a drop landed a second time. Flags and
    // curves are still taken; the song's place and the engine are kept.
    const bool resent = m_playing && m_title.isEmpty() == false
        && obj.value(QStringLiteral("title")).toString() == m_title
        && obj.value(QStringLiteral("beats")).toInt() > 0
        && obj.value(QStringLiteral("beats")).toInt() == m_beatCount
        && (obj.value(QStringLiteral("player")).toInt(-1) < 0          // R188_TRACK_PLAYER_SAME
            || obj.value(QStringLiteral("player")).toInt(-1) == m_trackPlayer);
    m_trackPlayer = obj.value(QStringLiteral("player")).toInt(-1);
    m_title = obj.value(QStringLiteral("title")).toString();
    m_key = obj.value(QStringLiteral("key")).toString();      // "Am", "8A", "1m" - or nothing
    m_bpm = obj.value(QStringLiteral("bpm")).toDouble();
    m_beatCount = obj.value(QStringLiteral("beats")).toInt();
    // DOWNBEAT_R104_READ: where the track's first bar starts (0 = beat 1)
    m_downbeat = qBound(0, obj.value(QStringLiteral("downbeat")).toInt(0), 3);
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
    m_kickRaw.clear(); m_highRaw.clear();          // R323_RAW_PARSE
    QJsonArray kickRawArr = obj.value(QStringLiteral("kickRaw")).toArray();
    for (int i = 0; i < kickRawArr.count(); i++) m_kickRaw.append(kickRawArr.at(i).toInt());
    QJsonArray highRawArr = obj.value(QStringLiteral("highRaw")).toArray();
    for (int i = 0; i < highRawArr.count(); i++) m_highRaw.append(highRawArr.at(i).toInt());
    if (m_engine != nullptr)            // R356_TRACK_AUDIO: the silence, read ahead
        m_engine->setTrackAudio(m_waveform, m_kickRaw, m_highRaw);
    if (m_engine != nullptr)            // R357_FILLS: rekordbox' fill-ins
        m_engine->setTrackFills(obj.value(QStringLiteral("fills")).toArray().toVariantList());
    if (m_engine != nullptr)            // R358_RHYTHM: the mids and the highs' quarter-beat onsets
        m_engine->setTrackRhythm(obj.value(QStringLiteral("midRaw")).toArray().toVariantList(),
                                 obj.value(QStringLiteral("onsetHigh")).toArray().toVariantList());
    if (m_engine != nullptr)            // R359_PUMP: the fall between the kicks
        m_engine->setTrackPump(obj.value(QStringLiteral("pumpRaw")).toArray().toVariantList());
    if (m_engine != nullptr)            // R461_ONSETS: the bass's and the mids' quarter-beat onsets
        m_engine->setTrackOnsets(obj.value(QStringLiteral("onsetLow")).toArray().toVariantList(),
                                 obj.value(QStringLiteral("onsetMid")).toArray().toVariantList());
    if (m_engine != nullptr)            // R317_PUNCH: how hard, not how full
        m_engine->setTrackPunch(m_title,                // R318_PUNCH_TITLE: one entry per track
                                obj.value(QStringLiteral("kickRef")).toDouble(-1.0),
                                obj.value(QStringLiteral("lowRef")).toDouble(-1.0));

    const bool keepLocal = resent && m_markersManual;       // R213_KEEP_HAND_FLAGS
    if (keepLocal == false)
        m_markers.clear();
    QJsonArray mk = keepLocal ? QJsonArray() : obj.value(QStringLiteral("markers")).toArray();
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
    if (keepLocal)
    {
        fillMarkerEnergies(false);              // FJ2_KEEP_MEASURE: before they go back
        sendMarkers(true);                      // R213_KEEP_HAND_SEND: BLT missed them
    }
    else
        m_markersManual = obj.value(QStringLiteral("manual")).toBool(false);
    // ANALYSIS10_NO_SECOND_PASS: BLT's analysis 10 is measured on the kick already
    if (m_markersManual == false && keepLocal == false
        && obj.value(QStringLiteral("analysis")).toInt(0) < 10 && refineMarkers())
    {
        // measure the new flags BEFORE the correction goes back to the
        // cache, or BLT stores a -1 and hands it straight back next time
        fillMarkerEnergies(false);
        sendMarkers(false);
    }
    // flags from the cache or from rekordbox without an energy get one
    // now, before the engine or the page reads them
    fillMarkerEnergies(false);

    if (resent == false)                 // R187_SAME_TRACK_KEEP
    {
        m_currentBeat = 0;
        m_trackTimeMs = 0;
        m_movePick = Function::invalidId();    // a new track picks afresh
        m_undo.clear();
        m_lastMoveIndex = -1;
        m_lastEngineBeat = -1;
        m_lastSecStart = -1;
        m_lastSecEnd = -1;
        m_loopTop = -1;                   // R235_DJ_LOOP_RESET
        m_jumpFrom = m_jumpTo = -100;
    }
    if (m_nextTitle == m_title)
    {
        m_nextTitle.clear();
        m_nextMarkers.clear();
        if (m_engine != nullptr) m_engine->setNextKey(QString());   // R202_NEXT_KEY
        emit mixChanged(); // the preview now belongs to the playing deck
    }
    if (resent == false && m_overrideState.isEmpty() == false)   // R296_OVERRIDE_NEW_TRACK
    {
        m_overrideState.clear();
        emit stateChanged();
    }
    if (m_engine != nullptr && resent == false)   // R187_SAME_TRACK_ENGINE
        m_engine->trackLoaded(m_title, m_key);

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
    // (the stamp keeps its old spelling: an earlier patch looks for it)
    m_lastPosMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 now = m_lastPosMs;
    if (m_linkStale)
    {
        m_linkStale = false;
        emit linkChanged();
    }
    // R358_BRAKE: the deck's effective tempo from every status BLT sends,
    // before the repeats are dropped below - a brake happens between beats
    if (m_engine != nullptr && playing)
        m_engine->setDeckTempo(obj.value(QStringLiteral("bpm")).toDouble());
    // R187_LAGGING_BEAT_GUARD: BLT sends the beat twice - from the beat packet
    // and from the player's status, five times a second - and the status
    // lags: N, then N-1 a few ms later, then N again. Each of those ran a
    // full engine beat and the jump-back path: 862 times on 20 Sep, 16 of
    // them on a section boundary, where a drop landed twice in two styles.
    // A one-beat step back within 150 ms of a step forward is that echo; a
    // real jump back is repeated by every later status and gets through.
    if (playing && m_playing && beat == m_currentBeat - 1 && now - m_beatChangedMs < 150)
        return;
    if (beat == m_currentBeat && playing == m_playing && timeMs == m_trackTimeMs)
        return;

    // R235_DJ_LOOP_SEEN: the same short step back twice is a loop
    if (playing && m_playing && beat > 0 && beat < m_currentBeat && m_currentBeat - beat <= 32)
    {
        const bool again = qAbs(beat - m_jumpTo) <= 1 && qAbs(m_currentBeat - m_jumpFrom) <= 1;
        m_jumpTo = beat;
        m_jumpFrom = m_currentBeat;
        // R358_LOOP_FIRST: the first step back holds the section as well,
        // when it lands in a section of the kind it left, 16 beats at most.
        // Only the second jump did (R235): on 10-02 a 16-beat loop over an
        // inner flag of a drop (Timbaland 273) re-landed the drop with a new
        // colour on its first pass. A cue back into a build is another kind.
        const bool firstLoop = m_currentBeat - beat <= 16 && stateAtBeat(beat) == stateAtBeat(m_currentBeat);
        m_loopTop = (again || firstLoop) ? qMax(m_loopTop, m_currentBeat) : -1;
    }
    else if (m_loopTop > 0 && (beat > m_loopTop || beat < m_currentBeat || playing == false))
        m_loopTop = -1;                   // played past it, jumped away, or stopped
    if (beat != m_currentBeat)
        m_beatChangedMs = now;
    // R301_POS_BPM_READ: the deck's effective tempo (pitch included), if BLT sent it
    const qreal posBpm = obj.value(QStringLiteral("bpm")).toDouble();
    if (posBpm > 20.0 && posBpm < 400.0)
    {
        m_posBpm = posBpm;
        m_posBpmMs = now;
    }
    m_currentBeat = beat;
    // STOP_GRACE_R105_MARK: a stop starts the grace, a play ends it
    if (playing == false && m_playing)
        m_stopSinceMs = now;
    else if (playing)
        m_stopSinceMs = 0;
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
    if (m_quantize > 1 && beat > m_downbeat)      // DOWNBEAT_R104_QUANT: the track's own grid
        beat = ((beat - 1 - m_downbeat) / m_quantize) * m_quantize + 1 + m_downbeat;

    QString state = stateAtBeat(beat);
    if (state == m_analysedState)
        return;

    m_analysedState = state;
    // R429_SECTION_ONCE (Tobias 10-11, after Soltauw: "automatisk gaar tilbage til
    // 'auto' naeste gang den rammer en aendring"): a SECTION press holds until
    // the analysis moves to its next section - then the page follows the
    // music again. Pressed what the music now became (DROP early, the drop
    // flag comes): let go without a second landing.
    const QString held = m_overrideState;
    if (held.isEmpty() == false)
    {
        if (m_engine != nullptr)
            m_engine->noteSectionRelease(held, state);
        m_overrideState.clear();
    }
    emit stateChanged();
    if (held.isEmpty() == false && held == state)
        return;                                  // already on stage

    if (m_autoRun && m_overrideState.isEmpty() && m_loopTop <= 0)   // R235_DJ_LOOP_LOOK
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
    if (m_engine != nullptr)                    // R335_SECTION_SIG: the press on the tracklog
        m_engine->noteSectionOverride(state, m_analysedState);
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
            func->start(m_doc->masterTimer(), FunctionParent::track(), 0,
                        Function::defaultSpeed(), Function::defaultSpeed(),
                        uint(division), Function::Beats);
        }
        else
        {
            func->start(m_doc->masterTimer(), FunctionParent::track());
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
    if (m_autoRun == false)
        return;
    // R187_REROLL_STALE: with the link quiet runEngine() holds the last
    // look - and after the START SCENE tile (the only caller) took the
    // picture down there is none: the room sat black for up to 30 s
    if (m_linkStale && m_roleMode && m_engine != nullptr)
    {
        m_lastEngineBeat = -1;           // R188_REROLL_BEAT: the link's return runs at once
        m_engine->linkIdle();            // R463_LINK_IDLE_REROLL
        return;
    }
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
        if (func == nullptr || func->isVisible() == false
            || func->path(true).startsWith(QStringLiteral("AUTO Programs/Steps"))
            || func->path(true).startsWith(QStringLiteral("Light Rider/System")))   // R300_LR_SYSTEM
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
        if (func == nullptr || func->isVisible() == false
            || func->path(true).startsWith(QStringLiteral("AUTO Programs/Steps"))
            || func->path(true).startsWith(QStringLiteral("Light Rider/System")))   // R300_LR_SYSTEM
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
    // RETIRED (runde 150): kept so the invokable signature does not
    // change under any QML that still names it. It does nothing.
}

int TrackManager::stateIntensity(QString state) const
{
    // RETIRED (runde 150). The slider this trimmed was removed from the
    // Track page; a stored value from before that could still darken the
    // room with nothing on screen to say so. Full, for every section.
    Q_UNUSED(state)
    return 100;
}

void TrackManager::setStateDivision(QString state, int milliBeats)
{
    if (stateNames().contains(state) == false)
        return;
    if (milliBeats < 0 || milliBeats > 16000)
        return;

    // RETIRED (runde 150), with the LEVEL setter above.
}

int TrackManager::stateDivision(QString state) const
{
    // RETIRED (runde 150), with LEVEL. 0 = the programme's own tempo,
    // snapped to the beat grid by the engine - a halftime chase stays
    // halftime, a fast one stays fast. Nothing can force a step length
    // on everything any more; the old comment here said it could.
    Q_UNUSED(state)
    return 0;
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

    // LEVEL and STEP per section are retired (runde 150): not read back,
    // and the two keys are dropped below on the next save.
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

    // ... and the old keys go, so a build from before runde 150 cannot
    // pick them up again either
    settings.remove(SETTINGS_TRACK_INTENSITY);
    settings.remove(SETTINGS_TRACK_DIVISION);
}

/*********************************************************************
 * Energy
 *********************************************************************/

void TrackManager::slotEnergyTick()
{
    {   // R313_TEMPO_TICK: a deck that went quiet leaves the line at once
        const QString shown = tempoSource() + QLatin1Char('|') + QString::number(engineBpm(), 'f', 1);
        if (shown != m_tempoShown)
        {
            m_tempoShown = shown;
            emit tempoChanged();
        }
    }
    // watchdog: a playing track that stops sending positions for four
    // seconds is a broken link, not a pause
    // no client at all is a broken link right away
    bool stale = m_playing && ((m_lastPosMs > 0
              && QDateTime::currentMSecsSinceEpoch() - m_lastPosMs > 4000)
             || (m_clients.isEmpty() && m_testTrack == false));   // R452_TT_STALE1: the test track needs no BLT
    if (stale != m_linkStale)
    {
        m_linkStale = stale;
        emit linkChanged();
        if (stale && m_engine != nullptr && m_autoRun && m_roleMode)   // R244_STALE_IDLE
        {
            m_lastEngineBeat = -1;
            m_engine->linkIdle();        // R463_LINK_IDLE_STALE
        }
    }
    // half a minute without a word from BLT is not a hiccup: the track is
    // over as far as we know - fall back to the idle look instead of a
    // chase running on forever
    if (stale && m_playing && QDateTime::currentMSecsSinceEpoch() - m_lastPosMs > 30000)
    {
        m_playing = false;
        m_lastEngineBeat = -1;           // R187_WATCHDOG_BEAT: a resume on the same beat runs
        emit positionChanged();
        if (m_engine != nullptr && m_autoRun && m_roleMode)
            m_engine->idle();
    }
    // STOP_GRACE_R105_IDLE: a stop that lasted its second goes idle now
    if (m_playing == false && m_stopSinceMs > 0
        && QDateTime::currentMSecsSinceEpoch() - m_stopSinceMs >= 4000)   // R412_STOP_GRACE_IDLE
    {
        m_stopSinceMs = 0;
        runEngine(false);
    }
    if (m_engine != nullptr)
        m_engine->closingTick(m_autoRun);         // R211_CLOSING_TICK R213_SHOW_ON

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
        e *= 0.80 + 0.20 * se;    // ENERGY_APPLIED_V2

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

qreal TrackManager::engineBpm() const      // R301_ENGINE_BPM_FN
{
    MasterTimer *mt = m_doc != nullptr ? m_doc->masterTimer() : nullptr;
    if (mt != nullptr && mt->linkEnabled() && mt->linkPeers() > 0 && mt->linkBpm() > 20.0)
        return mt->linkBpm();
    if (m_posBpm > 20.0 && QDateTime::currentMSecsSinceEpoch() - m_posBpmMs < 3000)
        return m_posBpm;
    if (m_bpm > 0)
        return m_bpm;
    return qreal(m_liveBpm);
}

QString TrackManager::tempoSource() const    // R308_TEMPO_SOURCE_FN: as engineBpm() decides
{
    MasterTimer *mt = m_doc != nullptr ? m_doc->masterTimer() : nullptr;
    if (mt != nullptr && mt->linkEnabled() && mt->linkPeers() > 0 && mt->linkBpm() > 20.0)
        return QStringLiteral("LINK");
    if (m_posBpm > 20.0 && QDateTime::currentMSecsSinceEpoch() - m_posBpmMs < 3000)
        return QStringLiteral("DECK");
    if (m_bpm > 0)
        return QStringLiteral("TRACK");
    return QStringLiteral("CLOCK");
}
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
    // R190_LASER_NOW: no beat is coming (link lost, deck stopped) - the
    // laser bars are measured against the fader now, not in 30 s. With
    // beats arriving tick() does it on the next one (and a call from
    // inside tick, via the clock, is never this branch).
    // R336_LASER_HAND: ... and a hand on the slider reaches them at once,
    // beats or not; the clock does not (it sets roomPercent first)
    if (m_engine != nullptr && m_autoRun && m_roleMode
        && (m_linkStale || m_playing == false || percent != m_engine->roomPercent()))
        m_engine->laserFaderCheck(energy());
    noteShowRunning();                   // R184_START_SCENE_ENERGY
    // R379_ZERO_ENERGY: the slider at 0 with the show on - the start scene
    // again (the DJ's tiles kept); above 0 noteShowRunning() takes it down
    if (m_autoRun && m_engine != nullptr && m_energyTrim == 0 && m_engine->startScene() == false
        && m_engine->lastTrack() == 0)   // R463_LAST_ZERO: not over the last track or its light
    {
        m_engine->setStartScene(true, true);
        m_startAuto = m_engine->startScene();
    }
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

void TrackManager::noteShowRunning()
{
    // R184_START_SCENE_NOTE
    if (m_autoRun == false || m_energyTrim <= 0 || m_engine == nullptr)
        return;
    const QString night = TrackEngine::nightKey();
    if (m_showRanNight != night)        // R185_SHOWRAN_WRITE
    {
        m_showRanNight = night;
        QSettings().setValue(SETTINGS_TRACK_SHOWRAN, night);
    }
    // only the one SHOW ON put up: a start scene called by hand stays
    // until its tile is tapped again
    if (m_startAuto)
    {
        m_startAuto = false;
        if (m_engine->startScene())
            m_engine->setStartScene(false);
        // R185_IDLE_AFTER: with the deck stopped no beat comes to build
        // anything - the room faded to black until play was pressed.
        // (From inside tick() the deck is playing, so this is not reached
        // there.)
        if ((m_playing == false || m_linkStale) && m_roleMode)   // R295_IDLE_STALE
        {
            if (m_playing == false)
                m_engine->idle();
            else
                m_engine->linkIdle();    // R463_LINK_IDLE_ROLES
        }
    }
}

bool TrackManager::autoRun() const { return m_autoRun; }

void TrackManager::setAutoRun(bool enable)
{
    if (enable == m_autoRun)
        return;
    m_autoRun = enable;

    // SHOW ON: clear the Virtual Console once - every function it started
    // stops and its Level sliders let go - so nothing left over from
    // busking gets in the way. Nothing is locked; busking works during
    // the show, and restarting the show clears again. The Light Rider
    // box sends Art-Net straight to the node and is beyond QLC+.
    if (m_autoRun && m_engine != nullptr)
        m_engine->resetConsole();

    // the evening's opening picture is a hand-held look: switching the
    // show on ends it
    if (m_autoRun && m_engine != nullptr)
        m_engine->setStartScene(false);
    m_startAuto = false;

    // R184_START_SCENE_SHOW: ... unless it is the first SHOW ON of the night
    // and ENERGY is 0 - then the evening opens on it, until ENERGY moves.
    // The clock is asked first: it only speaks on a beat, and the music
    // may not have started yet. Decided BEFORE applyLook() (R185_BEFORE_LOOK):
    // after it, a playing track had the show's look started and replaced.
    if (m_autoRun && m_engine != nullptr && m_showRanNight != TrackEngine::nightKey()
        && QSettings().value(SETTINGS_ENGINE_NIGHT).toString() != TrackEngine::nightKey())
    {
        // R378_ROOM_OFF: ENERGY's clock stays off - only its AUTO tile turns it on (R201_NEW_NIGHT)
        const QStringList trimmed = m_engine->trims().keys();
        for (const QString &k : trimmed)
            m_engine->setGroupTrim(k, m_engine->startTrim(k));   // R437_STROBE_START: the strobes at 50 %
    }
    if (m_autoRun && m_engine != nullptr)
        m_engine->announceRoom();
    // R379_ZERO_START (Tobias 10-06: "naar energi slideren staar paa 0% skal
    // det bare vaere start scenen"): SHOW ON at ENERGY 0 opens on it - every
    // night, not only the first (R184) - until ENERGY moves
    if (m_autoRun && m_engine != nullptr && m_energyTrim == 0)
    {
        m_engine->setStartScene(true);
        m_startAuto = m_engine->startScene();
    }

    if (m_autoRun) reroll();              // R295_SHOW_ON_REROLL: a quiet link builds the idle look
    else stopLook();
    if (m_autoRun == false && m_engine != nullptr)      // R211_SHOW_OFF_ATMOS
    {
        m_engine->setHaze(0.0);
        m_engine->setFan(0.0);
    }
    noteShowRunning();

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
    updateState();                       // R172_QUANTISE_RESTATE: the state is on the new grid now
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
    beat = qBound(1, snapBar(beat), m_beatCount > 0 ? m_beatCount : beat);   // DOWNBEAT_R104_MOVE

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
        if (i == index || snapBar(m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt()) != beat)   // DOWNBEAT_R104_SAMEBAR_MOVE
            continue;
        m_markers.removeAt(i);
        if (i < index)
            index--;
        m_lastMoveIndex = index;
    }
    m_markers.replace(index, marker);
    fillMarkerEnergies(true);            // the moved flag and its neighbours span other beats now

    emit markersChanged();
    m_markersManual = true;              // FJ_MOVE_MANUAL_FIRST: before updateState() reads it
    updateState();
    sendMarkers(true);                   // let BLT remember the correction
}

// R465_BLT_LIVE_CPP
bool TrackManager::bltRunning() const { return m_bltRunning; }

void TrackManager::probeBlt()
{
    // R465_BLT_LIVE (Tobias 10-15): the program's list, read in a process of its own -
    // the GUI and the engine never wait for it. Never two at once; one that hangs is
    // stopped after 4 s (its finished() tidies up).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_bltProbe != nullptr)
    {
        if (now - m_bltProbeMs > 4000)
            m_bltProbe->kill();
        return;
    }
    QProcess *p = new QProcess(this);
    m_bltProbe = p;
    m_bltProbeMs = now;
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus status) {
        const QString out = QString::fromLocal8Bit(p->readAllStandardOutput()).toLower();
        if (m_bltProbe == p)
            m_bltProbe = nullptr;
        p->deleteLater();
        if (status != QProcess::NormalExit)
            return;                      // stopped: the answer we had stands
        // the program (Beat Link Trigger.exe) or its window's title (a java launch)
        const bool on = out.contains(QLatin1String("beat link trigger"))
                     || out.contains(QLatin1String("beat-link-trigger"));
        if (on != m_bltRunning)
        {
            m_bltRunning = on;
            emit bltRunningChanged();
        }
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;                      // finished() follows the others
        if (m_bltProbe == p)
            m_bltProbe = nullptr;
        p->deleteLater();
    });
#if defined(Q_OS_WIN)
    p->start(QStringLiteral("tasklist"), QStringList() << QStringLiteral("/V") << QStringLiteral("/FO")
                                                     << QStringLiteral("CSV") << QStringLiteral("/NH"));
#else
    p->start(QStringLiteral("ps"), QStringList() << QStringLiteral("-A") << QStringLiteral("-o") << QStringLiteral("args="));
#endif
}

// R459_RM_CPP
/* =====================================================================
 *  TRACK REMOTE (runde 459) - inserted into trackmanager.cpp by
 *  feature_trackview_engine.py from track_runtime/remote.inc.cpp.
 *
 *  Tobias, 2026-10-15 (mockups A-D approved): "en man faktisk kan trykke paa
 *  ... jeg gerne vil kunne styre det fra min telefon naar jeg gaar rundt i
 *  lokalet" / "Der skal ingen sikkerhed bygges paa ... QLC koerer paa et
 *  lukket netvaerk". The phone page (track_remote.html, served by QLC's own
 *  web server on the layout branch) sends one line per tap over its web
 *  socket: "TRACK|<command>|<args>". This is the one door those lines come
 *  through. Every command calls exactly what the desk's own button calls,
 *  so every rule of the engine (the lasers, ENERGY's lines, haze by hand)
 *  holds for the phone as it does for the desk. The last touch wins.
 *
 *  "state" answers with the page's state as JSON; the phone asks four times
 *  a second, and that is also its heartbeat: "state|blackout,flash" names
 *  what a finger still holds on that phone (BLACKOUT, STUTTER, FLASH WHITE).
 *  What is not named for 1.2 s lets go by itself (the Wi-Fi dropped, the
 *  screen locked) - per button, so a second phone does not keep it on
 *  (R463_RM_HELD). A latched one (slid off, as on the desk) stays.
 * ===================================================================== */

#define TRACK_REMOTE_GONE_MS 1200

// R463_RM_ARGS: a number, or nothing - a word where a number belongs is refused,
// not read as 100 (MASTER, a trim) or 0
static bool remoteArg(const QStringList &a, int i, int *v)
{
    bool ok = false;
    const int n = a.value(i).toInt(&ok);
    if (ok)
        *v = n;
    return ok;
}

static bool remoteHeldOn(TrackEngine *e, const QString &what)
{
    if (what == QLatin1String("blackout"))
        return e->blackout();
    if (what == QLatin1String("stutter"))
        return e->stutter();
    return e->flashing();
}

void TrackManager::remoteWatch()
{
    // R459_REMOTE: a held button on a phone that went quiet lets go -
    // R463_RM_HELD: per button, from the phone that holds it
    if (m_remoteHeld.isEmpty() || m_engine == nullptr)
    {
        m_remoteHeld.clear();
        m_remoteHeldMs.clear();
        m_remoteTimer.stop();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList held = m_remoteHeld.values();
    for (const QString &what : held)
    {
        // the desk (or another phone) has let it go already: forgotten, not let go
        // again over a later hand
        if (remoteHeldOn(m_engine, what) == false)
        {
            m_remoteHeld.remove(what);
            m_remoteHeldMs.remove(what);
            continue;
        }
        if (now - m_remoteHeldMs.value(what, 0) < TRACK_REMOTE_GONE_MS)
            continue;
        if (what == QLatin1String("blackout"))
            m_engine->setBlackout(false);
        else if (what == QLatin1String("stutter"))
            m_engine->setStutter(false);
        else
            m_engine->setFlash(false);
        qDebug() << "[TRACK remote] the phone went quiet - let go of" << what;
        m_remoteHeld.remove(what);
        m_remoteHeldMs.remove(what);
    }
    if (m_remoteHeld.isEmpty())
        m_remoteTimer.stop();
}

void TrackManager::remoteHeld(const QString &what, bool on, const QStringList &a)
{
    // "<what>|1|held" while a finger is on it, "<what>|1|latched" slid off, "<what>|0"
    if (on && a.value(2) == QStringLiteral("held"))
    {
        m_remoteHeld.insert(what);
        m_remoteHeldMs.insert(what, QDateTime::currentMSecsSinceEpoch());   // R463_RM_HELD
        if (m_remoteTimerOn == false)
        {
            connect(&m_remoteTimer, &QTimer::timeout, this, &TrackManager::remoteWatch);
            m_remoteTimer.setInterval(300);
            m_remoteTimerOn = true;
        }
        if (m_remoteTimer.isActive() == false)
            m_remoteTimer.start();
    }
    else
    {
        m_remoteHeld.remove(what);
        m_remoteHeldMs.remove(what);
    }
}

QString TrackManager::remoteState()
{
    QJsonObject o;
    o.insert(QStringLiteral("show"), m_autoRun);
    o.insert(QStringLiteral("title"), title());
    o.insert(QStringLiteral("state"), currentState());
    o.insert(QStringLiteral("override"), overrideState());
    o.insert(QStringLiteral("beat"), currentBeat());
    o.insert(QStringLiteral("beats"), beatCount());
    o.insert(QStringLiteral("playing"), playing());
    o.insert(QStringLiteral("bpm"), engineBpm());
    o.insert(QStringLiteral("tempo"), tempoSource());
    o.insert(QStringLiteral("connected"), connected());
    o.insert(QStringLiteral("test"), m_testTrack);
    o.insert(QStringLiteral("energy"), energyTrim());
    QJsonArray mk;
    const QVariantList markerList = markers();
    for (const QVariant &v : markerList)
    {
        const QVariantMap m = v.toMap();
        QJsonObject j;
        j.insert(QStringLiteral("b"), m.value(QStringLiteral("beat")).toInt());
        j.insert(QStringLiteral("t"), m.value(QStringLiteral("type")).toString());
        mk.append(j);
    }
    o.insert(QStringLiteral("markers"), mk);
    if (m_engine != nullptr)
    {
        TrackEngine *e = m_engine;           // (groups() and palette() are not const)
        o.insert(QStringLiteral("roomAuto"), e->roomAuto());
        o.insert(QStringLiteral("calm"), e->calmBarsLeft());
        o.insert(QStringLiteral("hold"), e->hold());
        o.insert(QStringLiteral("blackout"), e->blackout());
        o.insert(QStringLiteral("stutter"), e->stutter());
        o.insert(QStringLiteral("stutterRate"), e->stutterRate());
        o.insert(QStringLiteral("hitKind"), e->hitKind());
        o.insert(QStringLiteral("hitLast"), e->hitLast());
        o.insert(QStringLiteral("flash"), e->flashing());
        o.insert(QStringLiteral("colour"), e->currentColour());
        o.insert(QStringLiteral("overrides"), QJsonArray::fromStringList(e->colourOverrides()));
        o.insert(QStringLiteral("colourMode"), e->colourMode());
        o.insert(QStringLiteral("palette"), QJsonArray::fromVariantList(e->palette()));
        o.insert(QStringLiteral("position"), e->positionMode());
        o.insert(QStringLiteral("speed"), e->speed());
        o.insert(QStringLiteral("master"), e->master());
        o.insert(QStringLiteral("haze"), e->haze());
        o.insert(QStringLiteral("fan"), e->fan());
        // the groups' list is a walk over the show's functions - every 2 s is plenty
        // (a toggle or a trim from the phone refreshes it at once, below)
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_remoteGroups.isEmpty() || now - m_remoteGroupsMs > 2000)
        {
            m_remoteGroups = QJsonArray();
            const QVariantList groupList = e->groups();
            for (const QVariant &v : groupList)
            {
                const QVariantMap g = v.toMap();
                QJsonObject j;
                for (const char *k : { "key", "enabled", "base", "switchOnly", "strobes", "lasers" })
                    j.insert(QLatin1String(k), QJsonValue::fromVariant(g.value(QLatin1String(k))));
                m_remoteGroups.append(j);
            }
            m_remoteGroupsMs = now;
        }
        // R463_RM_GROUPS: on/off fresh every time - a desk tap held back for 2 s
        // made the phone's next tap do nothing
        for (int i = 0; i < m_remoteGroups.count(); i++)
        {
            QJsonObject j = m_remoteGroups.at(i).toObject();
            const bool en = e->groupEnabled(j.value(QStringLiteral("key")).toString());
            if (j.value(QStringLiteral("enabled")).toBool() != en)
            {
                j.insert(QStringLiteral("enabled"), en);
                m_remoteGroups.replace(i, j);
            }
        }
        o.insert(QStringLiteral("groups"), m_remoteGroups);
        o.insert(QStringLiteral("trims"), QJsonObject::fromVariantMap(e->trims()));
        QJsonObject last;
        last.insert(QStringLiteral("stage"), e->lastTrack());
        last.insert(QStringLiteral("shown"), e->lastTrackShown());
        last.insert(QStringLiteral("left"), e->lastTrackLeft());
        last.insert(QStringLiteral("closeMin"), e->lastTrackCloseMin());
        last.insert(QStringLiteral("closeAt"), e->lastTrackCloseAt());
        o.insert(QStringLiteral("last"), last);
    }
    return QStringLiteral("TRACKSTATE|") + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString TrackManager::remoteCommand(const QString &line)
{
    m_remoteSeenMs = QDateTime::currentMSecsSinceEpoch();
    const QStringList a = line.split(QLatin1Char('|'));
    const QString cmd = a.value(0);
    if (cmd == QStringLiteral("state") || cmd.isEmpty())
    {
        // R463_RM_HELD: what a finger still holds on this phone stays held
        const QStringList still = a.value(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &what : still)
        {
            if (m_remoteHeld.contains(what))
                m_remoteHeldMs.insert(what, m_remoteSeenMs);
        }
        return remoteState();
    }
    TrackEngine *e = m_engine;
    if (e == nullptr)
        return QString();
    const QString bad = QStringLiteral("TRACKERR|not a number: ") + line;   // R463_RM_ARGS
    int n = 0;

    // ---- the top: the show switch (the page asks SURE? before OFF), LAST TRACK
    if (cmd == QStringLiteral("show"))
        setAutoRun(a.value(1) == QStringLiteral("1"));
    else if (cmd == QStringLiteral("last"))
    {
        // R464_RM_LAST: the phone says what it wants - last|1 arm, last|0 undo. The desk
        // may have done it between the phone's two taps; a bare "last" toggles (an old page)
        const QString want = a.value(1);
        if ((want == QLatin1String("1") && e->lastTrack() > 0) || (want == QLatin1String("0") && e->lastTrack() == 0))
            return remoteState();
        e->toggleLastTrack();
    }
    // ---- LIVE
    else if (cmd == QStringLiteral("energy"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        if (e->roomAuto())
            e->setRoomAuto(false);           // a hand on ENERGY, as the desk's slider
        setEnergyTrim(qBound(0, n, 100));
    }
    else if (cmd == QStringLiteral("energyAuto"))
        e->setRoomAuto(true);
    else if (cmd == QStringLiteral("calm"))
        e->calm(e->calmBarsLeft() > 0 ? 0 : 16);
    else if (cmd == QStringLiteral("next"))
        e->next();
    else if (cmd == QStringLiteral("hold"))
        e->setHold(!e->hold());
    else if (cmd == QStringLiteral("blackout"))
    {
        const bool on = a.value(1) == QStringLiteral("1");
        e->setBlackout(on);
        remoteHeld(QStringLiteral("blackout"), on, a);
    }
    else if (cmd == QStringLiteral("stutter"))
    {
        const bool on = a.value(1) == QStringLiteral("1");
        e->setStutter(on);
        remoteHeld(QStringLiteral("stutter"), on, a);
    }
    else if (cmd == QStringLiteral("stutterRate"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        e->setStutterRate(n);
    }
    else if (cmd == QStringLiteral("hit"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        e->setHitKind(qBound(0, n, 3));
    }
    else if (cmd == QStringLiteral("flash"))
    {
        const bool on = a.value(1) == QStringLiteral("1");
        e->setFlash(on);
        remoteHeld(QStringLiteral("flash"), on, a);
    }
    // ---- LOOK
    else if (cmd == QStringLiteral("section"))
        setOverrideState(a.value(1) == overrideState() ? QString() : a.value(1));
    else if (cmd == QStringLiteral("colourAuto"))
        e->setColourOverride(QString());
    else if (cmd == QStringLiteral("colour"))
        e->toggleColourOverride(a.value(1));
    else if (cmd == QStringLiteral("colourMode"))
    {
        const int m = remoteArg(a, 1, &n) ? n : 0;
        if (m == 1 || m == 2)
            e->setColourMode(e->colourMode() == m ? 0 : m);
    }
    else if (cmd == QStringLiteral("position"))
        e->setPositionMode(a.value(1) == e->positionMode() ? QString() : a.value(1));
    else if (cmd == QStringLiteral("speed"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        e->setSpeed(qBound(-1, n, 1));
    }
    // ---- RIG
    else if (cmd == QStringLiteral("master"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        e->setMaster(qBound(0, n, 100) / 100.0);
    }
    else if (cmd == QStringLiteral("trim"))
    {
        // R463_RM_ARGS: a group that is one (an unknown key was saved for the night),
        // and the ends as on the desk's fader
        if (e->trims().contains(a.value(1)) == false)
            return QStringLiteral("TRACKERR|no group ") + a.value(1);
        if (remoteArg(a, 2, &n) == false)
            return bad;
        int v = qBound(0, n, 100);
        if (v > 97)
            v = 100;
        else if (v < 3)
            v = 0;
        e->setGroupTrim(a.value(1), v / 100.0);
    }
    else if (cmd == QStringLiteral("group"))
    {
        if (e->trims().contains(a.value(1)) == false)
            return QStringLiteral("TRACKERR|no group ") + a.value(1);
        e->setGroupEnabled(a.value(1), a.value(2) == QStringLiteral("1"));
        m_remoteGroupsMs = 0;                // the list again at once
    }
    else if (cmd == QStringLiteral("haze"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        int v = qBound(0, n, 100);
        if (v < 3)
            v = 0;                           // as the desk's fader: the bottom is off
        e->setHaze(v / 100.0);
    }
    else if (cmd == QStringLiteral("fan"))
    {
        if (remoteArg(a, 1, &n) == false)
            return bad;
        int v = qBound(0, n, 100);
        if (v < 3)
            v = 0;                           // R463_RM_ARGS: as the desk's fader
        e->setFan(v / 100.0);
    }
    else
        return QStringLiteral("TRACKERR|unknown command ") + cmd;
    return remoteState();
}

// R450_TT_CPP
/* =====================================================================
 *  TEST TRACK (runde 450) - put into trackmanager.cpp by
 *  feature_trackview_engine.py from track_runtime/testtrack.inc.cpp.
 *
 *  Tobias, 2026-10-14: "Lavede vi paa et tidspunkt ikke en knap hvor jeg
 *  kunne 'loade' et fiktivt track saa jeg kunne teste uden at skulle loade
 *  paa CDJS?" - mockup 3+4 approved: the button in the empty waveform,
 *  learns nothing, a real track takes over, a loop, lights on the rig.
 *
 *  A made-up track, played into the same two doors BLT uses (handleTrack,
 *  handlePosition), so the page and the engine cannot tell it from a deck -
 *  sim_track.py's track, in QLC+ itself. While it plays:
 *    - BLT's position messages are not listened to; a TRACK from BLT stops
 *      the test and is played (the real one takes over)
 *    - nothing is learned or logged (TrackEngine::setTestTrack - the
 *      verdicts, the section and colour taste, the track memory, the drop
 *      punch, the tracklog), and nothing goes back to BLT's cache
 *    - the tempo is Link's when the CDJs are on it (READ only - we never
 *      write to Link), else 128 BPM
 *    - it loops: after the outro it starts from the top, until STOP TEST
 * ===================================================================== */

bool TrackManager::testTrack() const { return m_testTrack; }

qreal TrackManager::testBpm() const { return m_testTrack ? m_testBpm : 0.0; }

void TrackManager::startTestTrack()
{
    if (m_testTrack || (m_beatCount > 0 && m_playing))
        return;                          // a real track is playing: it stays
    if (m_engine != nullptr && m_engine->lastTrack() > 0)
        return;                          // R464_NO_TEST: LAST TRACK is on - its undo would hide

    qreal bpm = 128.0;
    MasterTimer *mt = m_doc != nullptr ? m_doc->masterTimer() : nullptr;
    if (mt != nullptr && mt->linkEnabled() && mt->linkPeers() > 0
        && mt->linkBpm() >= 70.0 && mt->linkBpm() <= 180.0)
        bpm = mt->linkBpm();             // what the engine times on - read, never written

    // 104 bars (3:15 at 128): two drops, so DROP HIT and the drops' variety show
    static const struct { const char *type; int bars; double energy; } plan[] = {
        { "intro", 8, 0.40 }, { "normal", 16, 0.60 }, { "break", 8, 0.30 }, { "build", 8, 0.70 },
        { "drop", 16, 0.95 }, { "normal", 8, 0.60 }, { "break", 8, 0.30 }, { "build", 8, 0.70 },
        { "drop", 16, 0.95 }, { "outro", 8, 0.40 } };
    // the curves BLT would hear (sim_track.py's): drops loud with a kick on every
    // beat, breaks without the low end, builds ramping the highs
    quint32 seed = 450u;                 // the same track every time
    auto wobble = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (int((seed >> 8) % 1001u) - 500) / 10000.0;   // +-0.05
    };
    auto byte = [](double x) { return int(qRound(255.0 * qBound(0.0, x, 1.0))); };
    QJsonArray markers, level, low, high, kick;
    int beat = 1;
    for (const auto &sec : plan)
    {
        const QString type = QString::fromLatin1(sec.type);
        QJsonObject m;
        m.insert(QStringLiteral("beat"), beat);
        m.insert(QStringLiteral("type"), type);
        m.insert(QStringLiteral("energy"), sec.energy);
        markers.append(m);
        const int n = sec.bars * 4;
        for (int i = 0; i < n; i++)
        {
            const double t = i / double(qMax(1, n - 1));
            const double w = wobble();
            double lv, lo, hi, kk;
            if (type == QLatin1String("drop"))
            {
                lv = 0.90; lo = 0.85; hi = 0.65; kk = (i % 2 == 0) ? 0.90 : 0.70;
            }
            else if (type == QLatin1String("build"))
            {
                lv = 0.55 + 0.35 * t; lo = 0.30 + 0.30 * t; hi = 0.40 + 0.50 * t;
                kk = (i % 2 == 0) ? 0.20 + 0.60 * t : 0.10 + 0.30 * t;
            }
            else if (type == QLatin1String("normal"))
            {
                lv = 0.65; lo = 0.60; hi = 0.45; kk = (i % 2 == 0) ? 0.60 : 0.30;
            }
            else                         // break, intro, outro
            {
                lv = 0.45 - 0.10 * t; lo = 0.15; hi = 0.40; kk = 0.05;
            }
            lv += 0.04 * std::sin(i * 1.5707963267948966);   // a little swell in each bar
            level.append(byte(lv + w));
            low.append(byte(lo + w));
            high.append(byte(hi + w));
            kick.append(byte(kk + w * 0.5));
        }
        beat += n;
    }

    QJsonObject track;
    track.insert(QStringLiteral("evt"), QStringLiteral("track"));
    track.insert(QStringLiteral("title"), tr("Test track"));
    track.insert(QStringLiteral("bpm"), bpm);
    track.insert(QStringLiteral("beats"), beat - 1);
    track.insert(QStringLiteral("duration"), int((beat - 1) * 60000.0 / bpm));
    track.insert(QStringLiteral("waveform"), level);
    track.insert(QStringLiteral("low"), low);
    track.insert(QStringLiteral("high"), high);
    track.insert(QStringLiteral("kick"), kick);
    track.insert(QStringLiteral("manual"), false);
    track.insert(QStringLiteral("markers"), markers);
    track.insert(QStringLiteral("analysis"), 10);   // the flags are final: no second pass
    track.insert(QStringLiteral("downbeat"), 0);
    track.insert(QStringLiteral("key"), QStringLiteral("Am"));

    m_testBpm = bpm;
    m_testBeats = beat - 1;
    m_testBeat = 0;
    m_testTicks = 0;
    m_testTrack = true;                  // before the track: trackLoaded() learns nothing either
    if (m_engine != nullptr)
        m_engine->setTestTrack(true);
    emit testTrackChanged();
    handleTrack(track);
    m_testClock.start();
    testStep(++m_testSerial);
}

void TrackManager::testStep(quint32 serial)
{
    if (m_testTrack == false || serial != m_testSerial)
        return;                          // stopped, or another test since
    // the loop: after the last beat, the top again (a DJ cueing back)
    m_testBeat = m_testBeat >= m_testBeats ? 1 : m_testBeat + 1;
    const qreal beatMs = 60000.0 / m_testBpm;
    QJsonObject pos;
    pos.insert(QStringLiteral("evt"), QStringLiteral("pos"));
    pos.insert(QStringLiteral("beat"), m_testBeat);
    pos.insert(QStringLiteral("playing"), true);
    pos.insert(QStringLiteral("time"), int((m_testBeat - 1) * beatMs));
    pos.insert(QStringLiteral("bpm"), m_testBpm);
    handlePosition(pos);
    // on a clock of its own: the next beat is due n beats after the start,
    // so a late beat does not push every later one
    m_testTicks++;
    const qint64 due = qint64(m_testTicks * beatMs);
    const int wait = int(qMax<qint64>(1, due - m_testClock.elapsed()));
    QTimer::singleShot(wait, Qt::PreciseTimer, this, [this, serial]() { testStep(serial); });
}

void TrackManager::stopTestTrack()
{
    if (m_testTrack == false)
        return;
    m_testSerial++;                      // the beat on its way does nothing
    clear();                             // the page empty again: TEST TRACK comes back
    m_stopSinceMs = 0;
    m_lastEngineBeat = -1;
    // nothing playing: the start scene, as after a track (still not learned from)
    if (m_engine != nullptr && m_autoRun && m_roleMode)
        m_engine->idle();
    m_testTrack = false;
    if (m_engine != nullptr)
        m_engine->setTestTrack(false);
    emit testTrackChanged();
}

void TrackManager::clear()
{
    stopLook();

    m_title.clear();
    m_bpm = 0;
    m_beatCount = 0;
    m_downbeat = 0;                     // DOWNBEAT_R104_CLEAR
    m_durationMs = 0;
    m_waveform.clear();
    m_low.clear(); m_high.clear(); m_kick.clear();
    m_kickRaw.clear(); m_highRaw.clear();          // R323_RAW_CLEAR
    m_markers.clear();
    m_currentBeat = 0;
    m_trackTimeMs = 0;
    m_playing = false;
    m_analysedState = QStringLiteral("normal");
    m_loopTop = -1;                       // R236_LOOP_CLEAR
    m_jumpFrom = m_jumpTo = -100;

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
        if (func == nullptr || func->isVisible() == false
            || func->path(true).startsWith(QStringLiteral("AUTO Programs/Steps"))
            || func->path(true).startsWith(QStringLiteral("Light Rider/System")))   // R300_LR_SYSTEM
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
        if (func == nullptr || func->isVisible() == false
            || func->path(true).startsWith(QStringLiteral("AUTO Programs/Steps"))
            || func->path(true).startsWith(QStringLiteral("Light Rider/System")))   // R300_LR_SYSTEM
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
    // no track (a "pos" before the "track"): a fixed end on a 64-beat grid.
    // beat + 64 moved every beat, and every beat was a new section - a new
    // pick and colour on every beat until the track arrived (R172_FIXED_END)
    end = m_beatCount > 0 ? m_beatCount + 1 : ((beat - 1) / 64 + 1) * 64 + 1;     // exclusive, like a flag's beat

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
        // STOP_GRACE_R105_HOLD: not for the first second - a touched jog
        // says stop for a moment; slotEnergyTick() idles when it lasts
        if (m_stopSinceMs > 0 && QDateTime::currentMSecsSinceEpoch() - m_stopSinceMs < 4000)   // R412_STOP_GRACE
            return;
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
    const int prevEngineBeat = m_lastEngineBeat;     // R327_PREV_BEAT: the beat ticked before this one
    m_lastEngineBeat = beat;

    QString state = currentState();
    // the state was picked on a quantised beat: the bounds, the
    // look-ahead and the section energy must use the same one, or the
    // engine is told about a section it is not in
    int stateBeat = beat;
    if (m_quantize > 1 && beat > m_downbeat)      // DOWNBEAT_R104_QUANT_ENGINE
        stateBeat = ((beat - 1 - m_downbeat) / m_quantize) * m_quantize + 1 + m_downbeat;
    int secStart = 1, secEnd = 1;
    sectionBounds(stateBeat, secStart, secEnd);
    // R172_QUANTISED_BOUNDS. The state flips on the quantise grid, so the
    // section starts and ends there too: a drop flag at 21 with quantise 8
    // becomes the state at beat 25, and the engine was told the section
    // began at 21 - bar 1 on its first beat, so no landing, no hit, no
    // wait for the kick.
    if (m_quantize > 1)
    {
        // DOWNBEAT_R104_QUANT_BOUNDS: on the track's grid; the pickup before
        // the first bar line stays where it is
        if (secStart > 1 + m_downbeat)
            secStart = ((secStart - 1 - m_downbeat + m_quantize - 1) / m_quantize) * m_quantize + 1 + m_downbeat;
        if (secEnd > 1 + m_downbeat)
            secEnd = ((secEnd - 1 - m_downbeat + m_quantize - 1) / m_quantize) * m_quantize + 1 + m_downbeat;
    }

    // a jump to a cue, or two flags of the same type in a row: the state
    // string does not change but the section does
    // R198_SECTION_START: a section is where it STARTS - a moved end (the
    // next flag dragged, bar by bar) is not a new one. Without flags the
    // fixed 64-beat end still turns a section (R172_FIXED_END).
    // R235_DJ_LOOP_HOLD: inside a running DJ loop the section it wrapped in
    // is kept - its state, bounds and energy - until the loop is let go
    const bool loopHeld = m_loopTop > 0 && m_overrideState.isEmpty() && m_lastSecStart > 0
                       && secStart != m_lastSecStart && stateBeat <= m_loopTop;
    if (loopHeld)
    {
        stateBeat = m_lastSecStart;
        secStart = m_lastSecStart;
        secEnd = m_lastSecEnd;
        state = stateAtBeat(stateBeat);
    }
    const bool insideOld = m_lastSecStart > 0 && stateBeat >= m_lastSecStart
                        && stateBeat < m_lastSecEnd;          // R205_SAME_SECTION
    if (loopHeld == false && m_overrideState.isEmpty()   // R235_DJ_LOOP_GATE
        && ((secStart != m_lastSecStart && insideOld == false)
            || (secEnd != m_lastSecEnd && m_markers.isEmpty())))
        sectionChanged = true;
    m_lastSecStart = secStart;
    m_lastSecEnd = secEnd;

    // what comes next, so the engine can lean into a drop a bar early
    QString nextState;
    int beatsToNext = 0;
    nextSection(stateBeat, nextState, beatsToNext);
    // which section is next: from the quantised beat, so it agrees with
    // the state. How FAR it is: from the real beat, or a countdown that
    // has to hit exactly 1 never does once quantize > 1 - it would step
    // 8, 4, 0 and the pre-drop blink would never fire.
    // ... and to where the state actually flips (R172_QUANTISED_NEXT): the
    // flag itself is up to a grid step earlier, and the pre-drop blink came
    // that many beats before the drop. Decided on the count BEFORE the
    // subtraction (R187_QUANTISED_COUNTDOWN): a flag off the grid (21,
    // quantise 8) took it to 0 at the flag, the correction never ran, and
    // the last bar before the drop counted 0, 0, 0, 0 - no pre-drop, no blink.
    if (m_quantize > 1 && beatsToNext > 0)
    {
        beatsToNext = qMax(0, secEnd - beat);
        nextState = stateAtBeat(secEnd);        // R205_QUANTISED_NEXT_STATE
    }
    else if (beatsToNext > 0)
        beatsToNext = qMax(0, beatsToNext - (beat - stateBeat));

    // Energy = BPM dial x how loud this section is. The section's LEVEL slider
    // goes in separately as a brightness trim, so it cannot change how many
    // effects join.
    qreal se = sectionEnergy(stateBeat);
    qreal en = energy();
    if (se >= 0.0)
        en *= 0.80 + 0.20 * se;   // ENERGY_SE_V2: the fader keeps its top fifth
    qreal levelScale = qreal(stateIntensity(state)) / 100.0;

    // what the analysis heard on this very beat (0..1), -1 when BLT did not
    // send the curves: the pulse follows the kick, hats-only passages sparkle
    auto curveAt = [](const QVariantList &c, int b) {
        return (b >= 1 && b - 1 < c.count()) ? c.at(b - 1).toInt() / 255.0 : -1.0;
    };
    auto curveMean = [&curveAt](const QVariantList &c, int from, int to) {
        // mean over [from, to], -1 when fewer than half the beats are there
        qreal sum = 0.0; int n = 0;
        for (int b = from; b <= to; b++)
        {
            qreal v = curveAt(c, b);
            if (v >= 0.0) { sum += v; n++; }
        }
        return (n > 0 && n * 2 >= (to - from + 1)) ? sum / n : -1.0;
    };
    qreal kick = curveAt(m_kick, beat);
    qreal high = curveAt(m_high, beat);

    // What the curves say about this beat beyond its own kick and highs,
    // so the engine can put its changes where the music turns rather than
    // on a timer (Tobias, 2026-09-15: "naar det skifter skal det vaere stilet
    // og passe med musikken").
    //   turn  - the kick comes back after at least two beats without one (a
    //           fill's end), a crash on the highs (well above the last two
    //           bars), or the bass jumping a quarter above its four-bar mean
    //   riser - the highs over the last eight bars against the eight before
    //   hats  - the highs over the last two bars
    //   bass  - the lows over the last two bars (the pulse's weight)
    bool turn = false;
    qreal riser = 0.0;
    qreal hats = -1.0;
    qreal bass = -1.0;
    qreal kickAhead = -1.0;              // R192_KICK_AHEAD_DECL
    if (kick >= 0.0)
    {
        // R323_RAW_TURN (runde 323): the turn read on the RAW per-beat kick
        // and highs when BLT sends them. On the smoothed curves (four beats)
        // the three rules below were measured on 92 tracks of the library:
        // the kick coming back landed on beat 1 in 35 % of its fires, the
        // crash fired on 0.05 % of beats, and the bass jump - 7 % of beats -
        // was on beat 1 as often as chance (24 %). The raw kick back lands
        // on beat 1 in 71 % (38 % on a four-bar line, chance is 6 %); the raw
        // crash 35 %. The bass jump is left out: it was noise, and it was
        // most of the turns.
        const bool rawCurves = m_kickRaw.isEmpty() == false && m_kickRaw.count() == m_kick.count();
        if (rawCurves)
        {
            // R327_TURN_CONTEXT (review): the kick back only out of a four-on-
            // the-floor - kicks on the two beats before the gap, or a gap of
            // four or more. A kick on beat 1 only (half time, a broken drop)
            // matched "back after two quiet beats" on EVERY downbeat. Measured
            // on 92 tracks: beat 1 in 78 % of fires (71 % before), a four-bar
            // line in 50 % (38 %).
            const qreal r0 = curveAt(m_kickRaw, beat);
            const qreal q1 = curveAt(m_kickRaw, beat - 1), q2 = curveAt(m_kickRaw, beat - 2);
            const qreal q3 = curveAt(m_kickRaw, beat - 3), q4 = curveAt(m_kickRaw, beat - 4);
            const bool quiet12 = q1 >= 0.0 && q1 < 0.20 && q2 >= 0.0 && q2 < 0.20;
            const bool fourFloor = (q3 >= 0.45 && q4 >= 0.45)
                                || (q3 >= 0.0 && q3 < 0.20 && q4 >= 0.0 && q4 < 0.20);
            if (r0 >= 0.45 && quiet12 && fourFloor)
                turn = true;
            // the crash on the raw highs - or on the smoothed ones when BLT
            // sent a raw kick but no raw highs (R327_CRASH_FALLBACK)
            const bool rawHigh = m_highRaw.count() == m_kick.count();
            const QVariantList &hc = rawHigh ? m_highRaw : m_high;
            const qreal h0 = curveAt(hc, beat);
            qreal hm = -1.0;
            for (int i = 1; i <= 8; i++)
                hm = qMax(hm, curveAt(hc, beat - i));
            if (h0 >= 0.75 && hm >= 0.0 && h0 >= hm + 0.15)
                turn = true;
        }
        else
        {
        qreal k1 = curveAt(m_kick, beat - 3), k2 = curveAt(m_kick, beat - 4);   // R211_KICK_BACK
        if (kick >= 0.45 && k1 >= 0.0 && k1 < 0.20 && k2 >= 0.0 && k2 < 0.20)
            turn = true;
        qreal hMax = -1.0;
        for (int i = 1; i <= 8; i++)
            hMax = qMax(hMax, curveAt(m_high, beat - i));
        if (high >= 0.75 && hMax >= 0.0 && high >= hMax + 0.15)
            turn = true;
        qreal low = curveAt(m_low, beat);
        qreal lowMean = curveMean(m_low, beat - 16, beat - 1);
        if (low >= 0.0 && lowMean >= 0.0 && low >= lowMean + 0.25)
            turn = true;
        }
        qreal hNow = curveMean(m_high, beat - 8, beat - 1);
        qreal hThen = curveMean(m_high, beat - 32, beat - 9);
        if (hNow >= 0.0 && hThen >= 0.0)
            riser = qBound(0.0, hNow - hThen, 1.0);
        hats = curveMean(m_high, beat - 7, beat);
        bass = curveMean(m_low, beat - 7, beat);
        // R192_KICK_AHEAD: the kick over this beat and the seven after it -
        // where the flag and the music disagree, the engine follows this
        kickAhead = curveMean(m_kick, beat, beat + 7);
    }

    int rawGapNow = -1, rawQuietNow = -1;        // R324_RAW_FILL_CALC
    if (m_kickRaw.isEmpty() == false && m_kickRaw.count() == m_kick.count())
    {
        const qreal r0 = curveAt(m_kickRaw, beat), r1 = curveAt(m_kickRaw, beat - 1);
        const qreal h0 = curveAt(m_highRaw, beat);
        // R327_GAP_WINDOW (review, replaces R325_GAP_ONSET): the gap's onset on ANY beat since the last
        // tick - a burst or a lost message skips beats, and the one onset beat
        // was lost with it - and only out of a four-on-the-floor (kicks on the
        // two beats before the gap): a half-time kick opened a "gap" on beat 3
        // of every bar
        if (r0 >= 0.0 && r1 >= 0.0)
        {
            rawGapNow = 0;
            const int from = qMax(beat - 3, prevEngineBeat > 0 ? prevEngineBeat + 1 : beat);
            for (int b = from; b <= beat && rawGapNow == 0; b++)
            {
                const qreal g0 = curveAt(m_kickRaw, b), g1 = curveAt(m_kickRaw, b - 1);
                const qreal g2 = curveAt(m_kickRaw, b - 2), g3 = curveAt(m_kickRaw, b - 3);
                if (r0 < 0.20 && g0 >= 0.0 && g0 < 0.20 && g1 >= 0.0 && g1 < 0.20 && g2 >= 0.45 && g3 >= 0.45)
                    rawGapNow = 1;
            }
        }
        if (r0 >= 0.0 && h0 >= 0.0)
            rawQuietNow = (r0 < 0.20 && h0 < 0.20) ? 1 : 0;
    }
    if (m_markersManual)                        // R210_MANUAL_WINS
        kickAhead = -1.0;
    if (m_overrideState.isEmpty() == false)     // R199_OVERRIDE_WINS
    {
        kickAhead = -1.0;
        riser = 0.0;
        nextState.clear();
        beatsToNext = 0;
    }
    m_engine->setDropForced(m_overrideState == QStringLiteral("drop"));   // R361_DROP_FORCED
    {   // R363_NEXT_DROP: the next drop flag ahead - a build flagged in parts leads into it all the way
        int nextDrop = -1;
        for (int i = 0; i < m_markers.count() && m_overrideState.isEmpty(); i++)
        {
            const QVariantMap mk = m_markers.at(i).toMap();
            const int mb = mk.value(QStringLiteral("beat")).toInt();
            if (mb > beat && mk.value(QStringLiteral("type")).toString() == QStringLiteral("drop")
                && (nextDrop < 0 || mb < nextDrop))
                nextDrop = mb;
        }
        m_engine->setNextDropBeat(nextDrop);
    }
    int tickStart = secStart;                 // R236_LOOP_TICK_START
    if (m_loopTop > 0 && m_jumpTo < secStart && beat <= m_loopTop)
        tickStart = secStart > 32 ? secStart - 32 : 1 + (secStart - 1) % 4;
    // R295_DOWNBEAT_TICK_START: the first section starts on beat 1, its first
    // bar on 1 + m_downbeat - the engine counts every bar line from what it gets
    if (tickStart == 1 && m_downbeat > 0 && beat > m_downbeat)
        tickStart = 1 + m_downbeat;
    m_engine->tick(state, beat, tickStart, secEnd, en, se,
                   stateDivision(state), sectionChanged, nextState, beatsToNext,
                   engineBpm(), levelScale, kick, high,   // R301_ENGINE_BPM
                   turn, riser, hats, bass, kickAhead,
                   rawGapNow, rawQuietNow);   // R324_RAW_FILL

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
        func->start(m_doc->masterTimer(), FunctionParent::track(), 0,
                    Function::defaultSpeed(), Function::defaultSpeed(),
                    uint(division), Function::Beats);
    else
        func->start(m_doc->masterTimer(), FunctionParent::track());

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
    int nextBeat = -1;

    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap marker = m_markers.at(i).toMap();
        int mb = marker.value(QStringLiteral("beat")).toInt();
        if (mb <= beat && mb > bestBeat)
        {
            bestBeat = mb;
            best = marker.value(QStringLiteral("energy"), -1.0).toDouble();
        }
        if (mb > beat && (nextBeat < 0 || mb < nextBeat))
            nextBeat = mb;
    }

    // a flag without an energy: measure the section on the curves rather than
    // hand the engine a -1 (fillMarkerEnergies() normally gets there first)
    if (best < 0.0 && bestBeat > 0)
        best = curveEnergy(bestBeat, nextBeat > 0 ? nextBeat : (m_beatCount > 0 ? m_beatCount + 1 : beat + 1));

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
    if (m_testTrack)
        return;                          // R450_TT_NOCACHE: BLT never caches the made-up track
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

qreal TrackManager::kickMean(int fromBeat, int count, bool needHalf) const
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
    // half the window has to be there. A short curve used to average over
    // the one or two beats that were in range and invent a drop at the tail
    return n > 0 && (needHalf == false || n * 2 >= count) ? sum / n : -1.0;
}

qreal TrackManager::curveEnergy(int fromBeat, int toBeat) const
{
    // BLT's energy curve is min(display, low), smoothed over four beats and
    // averaged over the section; the same two curves arrive here as 0..255
    if (m_waveform.isEmpty() || m_low.isEmpty())
        return -1.0;
    qreal sum = 0.0;
    int n = 0;
    for (int b = fromBeat; b < toBeat; b++)
    {
        if (b < 1 || b - 1 >= m_waveform.count() || b - 1 >= m_low.count())
            continue;
        sum += qMin(m_waveform.at(b - 1).toInt(), m_low.at(b - 1).toInt()) / 255.0;
        n++;
    }
    return n > 0 ? sum / n : -1.0;
}

void TrackManager::fillMarkerEnergies(bool all)
{
    // A flag set by hand, added by the second pass or moved carries energy -1,
    // and -1 went back to BLT's cache as a correction, so it stayed -1 for
    // good: the engine then ran the section at the fader's energy alone, and
    // the waveform had no energy to show for it. Measure it here instead.
    if (m_waveform.isEmpty() || m_low.isEmpty() || m_markers.isEmpty())
        return;
    QList<int> order;
    for (int i = 0; i < m_markers.count(); i++)
        order.append(i);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return m_markers.at(a).toMap().value(QStringLiteral("beat")).toInt()
             < m_markers.at(b).toMap().value(QStringLiteral("beat")).toInt(); });
    int total = m_beatCount > 0 ? m_beatCount : m_waveform.count();
    for (int k = 0; k < order.count(); k++)
    {
        QVariantMap mk = m_markers.at(order.at(k)).toMap();
        if (all == false && mk.value(QStringLiteral("energy"), -1.0).toDouble() >= 0.0)
            continue;
        int from = mk.value(QStringLiteral("beat")).toInt();
        int to = k + 1 < order.count()
               ? m_markers.at(order.at(k + 1)).toMap().value(QStringLiteral("beat")).toInt()
               : total + 1;
        // a last flag on the last beat: an empty span would say 0, a dead outro
        qreal e = curveEnergy(from, qMax(to, from + 1));
        if (e < 0.0)
            continue;
        mk.insert(QStringLiteral("energy"), e);
        m_markers.replace(order.at(k), mk);
    }
}

static int tmSnapBar(int beat)
{
    // the nearest bar line (beats count from 1)
    int bar = (beat - 1 + 2) / 4;
    return qMax(1, bar * 4 + 1);
}

int TrackManager::snapBar(int beat) const
{
    // DOWNBEAT_R104_SNAP: the nearest bar line of THIS track - its first bar
    // starts on beat m_downbeat + 1. The beats before it (the pickup) go to
    // beat 1 when that is nearer, so the first flag stays where it is.
    const int shifted = tmSnapBar(qMax(1, beat - m_downbeat)) + m_downbeat;
    return (m_downbeat > 0 && beat - 1 < shifted - beat) ? 1 : shifted;
}

bool TrackManager::refineMarkers()
{
    if (m_beatCount < 64 || m_kick.isEmpty())
        return false;

    struct Flag { int beat; QString type; qreal energy; };
    QList<Flag> flags;
    bool changed = false;
    for (int i = 0; i < m_markers.count(); i++)
    {
        QVariantMap mk = m_markers.at(i).toMap();
        Flag f;
        // snapping rounds up, so the last flag can land past the end -
        // nextSection() then reports a section that never arrives
        f.beat = qBound(1, snapBar(mk.value(QStringLiteral("beat")).toInt()), m_beatCount);   // DOWNBEAT_R104_REFINE
        f.type = mk.value(QStringLiteral("type")).toString();
        f.energy = mk.value(QStringLiteral("energy"), -1.0).toDouble();
        if (f.type.isEmpty())
        {
            // only the rewrite at the end makes this stick, and that is
            // skipped when nothing else changed
            f.type = QStringLiteral("normal");
            changed = true;
        }
        flags.append(f);
    }
    std::sort(flags.begin(), flags.end(), [](const Flag &a, const Flag &b) { return a.beat < b.beat; });

    QStringList notes;                  // changed now lives above the loop

    // 1. sections shorter than four bars are noise: the later flag goes,
    //    unless it is a drop landing after a build (that is the point)
    for (int i = 1; i < flags.count(); i++)
    {
        // the drop-after-build exemption needs the drop to be somewhere
        // else: two flags on the same beat make a section of zero length
        if (flags.at(i).beat - flags.at(i - 1).beat < 16
            && !(flags.at(i).beat > flags.at(i - 1).beat
                 && flags.at(i).type == QStringLiteral("drop")
                 && flags.at(i - 1).type == QStringLiteral("build")))
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
        // A DRIVE is demoted on the same evidence as a drop. Analysis version
        // 7 renames the roughly half of BLT's drops that never earned the name
        // to "drive" (qlc_shared_functions.clj: 996 -> 525 drops + 471 drives),
        // and a drive is played nearly as hot: its star ceiling sits halfway
        // between a groove's and a drop's. Before version 7 those same
        // sections arrived here as "drop" and a kick well under the room's
        // drop threshold demoted them to a plain groove. Without this word
        // the upgrade would quietly take that net away from every one of
        // them. Nothing else changes: promotion still wants "normal", because
        // a drive is the analysis saying it looked for a build and found
        // none, and that is not ours to overrule from the kick alone.
        if ((was == QStringLiteral("drop") || was == QStringLiteral("drive"))
            && k < m_dropKick * 0.6)
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
    for (int b = 17 + m_downbeat; b + 16 <= m_beatCount + 1; b += 4)     // R210_STEP3_END DOWNBEAT_R104_STEP3
    {
        // R194_STEP3: "two bars without a kick" is BOTH bars - the two-bar
        // mean let a one-bar fill pass; and the four-bar minimum step 1
        // enforces holds here too, or the next load deletes what this added
        qreal before = qMax(kickMean(b - 8, 4), kickMean(b - 4, 4));
        qreal after = kickMean(b, 8);
        if (before < 0.0 || after < 0.0 || before >= m_breakKick || after < m_dropKick)
            continue;
        bool nearby = false;             // near is a macro in windows.h
        foreach (const Flag &f, flags)
            if (qAbs(f.beat - b) < 16)
                nearby = true;
        if (nearby)
            continue;
        // -1 is the SENTINEL for "measure me": fillMarkerEnergies() reads the
        // real value off the curves (curveEnergy) once the flags are in. Do
        // not guess one here - the ordering in handleTrack() is what let
        // these -1s survive into the cache, and that is fixed there.
        Flag drop;
        drop.beat = b;
        drop.type = QStringLiteral("drop");
        drop.energy = -1.0;
        flags.append(drop);
        // and the break that led into it, if none is flagged
        int start = b;
        while (start - 4 >= 1 && kickMean(start - 4, 4) >= 0.0 && kickMean(start - 4, 4) < m_breakKick)
            start -= 4;
        // R194_STEP3_BREAK: a break or a build leads in - a drop or a drive
        // there did not, and left drop -> drop; and the inserted break keeps
        // four bars itself, and four bars after the flag before it
        bool hasBreak = false;
        foreach (const Flag &f, flags)
            if (f.beat == start                    // anything already there
                || (f.beat >= start - 8 && f.beat < b
                    && (f.type == QStringLiteral("break") || f.type == QStringLiteral("build")))
                || (f.beat < start && start - f.beat < 16)
                // R195_STEP3_AFTER: nor a flag under four bars AFTER it - the
                // walk back can pass over one, and the next load deleted it
                || (f.beat > start && f.beat < b && f.beat - start < 16))
                hasBreak = true;
        if (hasBreak == false && b - start >= 16)
        {
            Flag brk;
            brk.beat = start;
            brk.type = QStringLiteral("break");
            brk.energy = -1.0;          // measured by fillMarkerEnergies()
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
    beat = qBound(1, snapBar(beat), m_beatCount);   // DOWNBEAT_R104_ADD

    // a flag already on this bar takes the new type instead
    for (int i = 0; i < m_markers.count(); i++)
    {
        // DOWNBEAT_R104_SAMEBAR: on this bar, not only on this beat - a hand
        // flag from before the track's own bar lines sits 1-3 beats off them
        if (snapBar(m_markers.at(i).toMap().value(QStringLiteral("beat")).toInt()) == beat)
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
    const QString oldType = mk.value(QStringLiteral("type")).toString();     // R210_RELABEL_LESSON
    const bool sameKick = (oldType == QStringLiteral("drop")
                           && (type == QStringLiteral("drive") || type == QStringLiteral("build")))
                       || (oldType == QStringLiteral("break") && type == QStringLiteral("build"));
    if (sameKick == false)
        learnFromFlag(oldType, mk.value(QStringLiteral("beat")).toInt(), false);
    mk.insert(QStringLiteral("type"), type);
    m_markers.replace(index, mk);
    learnFromFlag(type, mk.value(QStringLiteral("beat")).toInt(), true);
    markersEdited();
}

void TrackManager::markersEdited()
{
    m_markersManual = true;
    m_lastMoveIndex = -1;                // the next drag is its own undo step
    fillMarkerEnergies(true);            // a flag came or went: the neighbours' spans moved too
    emit markersChanged();
    // updateState() already runs the engine when the edit changed the state
    // (applyLook -> runEngine(true)); a second call drew the section's
    // programme and accent twice on one beat (R172_ONE_ENGINE_CALL)
    const QString stateBefore = m_analysedState;
    updateState();
    const bool engineRan = m_analysedState != stateBefore && m_overrideState.isEmpty()
                        && m_loopTop <= 0;   // R236_LOOP_EDIT
    if (m_autoRun && m_roleMode && engineRan == false)
    {
        m_lastEngineBeat = -1;
        runEngine(false);                // R198_EDIT_NO_RESTART: new bounds, same section
    }
    sendMarkers(true);                   // BLT keeps it as a hand-made correction
}

void TrackManager::learnFromFlag(const QString &type, int beat, bool added)
{
    if (m_testTrack)
        return;                          // R450_TT_NOLEARN: its flags teach nothing
    // The operator is the teacher. A drop they add at a soft kick lowers
    // what the second pass demands of a drop; a drop they delete at a hard
    // kick raises it. Breaks the other way round. Small steps, remembered.
    // whatever is in range: a flag two bars from the end still teaches us
    qreal k = kickMean(beat, 8, false);
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
    // and whether this was a hand-made set at all: undoing the one edit that
    // made it manual has to make it automatic again, or BLT caches it as a
    // correction and the second pass never runs for this track again
    snap.insert(QStringLiteral("manual"), m_markersManual);
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
    m_markersManual = snap.value(QStringLiteral("manual"), true).toBool();
    m_lastMoveIndex = -1;
    fillMarkerEnergies(false);
    emit markersChanged();
    const QString stateBefore = m_analysedState;     // R172_ONE_ENGINE_CALL_UNDO
    updateState();
    const bool engineRan = m_analysedState != stateBefore && m_overrideState.isEmpty()
                        && m_loopTop <= 0;   // R236_LOOP_UNDO
    if (m_autoRun && m_roleMode && engineRan == false)
    {
        m_lastEngineBeat = -1;
        runEngine(false);                // R198_UNDO_NO_RESTART
    }
    sendMarkers(m_markersManual);
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
    if (m_engine && title.isEmpty() == false)
        m_engine->forgetTrackMemory(title);   // R439_TRACK_MEMORY
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
    if (evt == QStringLiteral("mix-profile"))
    {
        if (m_engine != nullptr)
            m_engine->setIncomingProfile(obj.value("title").toString(),
                obj.value("state").toString(), obj.value("energy").toDouble(-1.0));
    }
    else if (evt == QStringLiteral("mix"))
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
        if (m_engine != nullptr)
            m_engine->setNextKey(obj.value(QStringLiteral("key")).toString());
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
        {
            m_nextTitle.clear();         // it is this track, not the next
            m_nextMarkers.clear();       // ... so these are not its flags either
            if (m_engine != nullptr) m_engine->setNextKey(QString());
        }
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
            // epoch ms - a track's play time, 0 for an entry from before BLT
            // kept one. qint64, not int: 2026 in milliseconds does not fit.
            row.insert(QStringLiteral("playedAt"),
                       qint64(t.value(QStringLiteral("played-at")).toDouble(0.0)));
            m_cacheList.append(row);
        }
        emit cacheChanged();
    }
}
