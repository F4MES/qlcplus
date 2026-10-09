/*
  Q Light Controller Plus
  trackengine.cpp

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSettings>
#include <QDebug>
#include <functional>
#include <algorithm>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>        // runde 304: colourOverrides in the settings log
#include <QDateTime>
#include <QTime>
#include <QTextStream>
#include <QPair>
#include <QDir>
#include <QSaveFile>
#include <cmath>

#include "trackengine.h"
#include "trackmanager.h"          // the trackmanager/* settings keys
#include "inputoutputmap.h"
#include "functionparent.h"
#include "universe.h"
#include "fixturegroup.h"
#include "qlcfixturedef.h"
#include "qlccapability.h"
#include "mastertimer.h"
#include "collection.h"
#include "qlcchannel.h"
#include "rgbmatrix.h"
#include "virtualconsole.h"   // usageList(): which functions a VC widget points at
#include "vcbutton.h"         // releaseAllHolds(): SHOW ON (runde 300)
#include "efxfixture.h"
#include "function.h"
#include "fixture.h"
#include "chaser.h"
#include "scene.h"
#include "efx.h"
#include "doc.h"

/** Scatter a name across the whole 32-bit range. FNV-1a, written out rather
 *  than taken from qHash() because Qt 6 randomises qHash's seed per process:
 *  the candidate order would then be different on every launch, and "it chose
 *  that one last night" would stop being a question anyone could answer.
 *  Read once per function into TrackFuncInfo::scatter; see candidates(). */
static quint32 nameScatter(const QString &name)
{
    quint32 h = 2166136261u;
    for (qsizetype i = 0; i < name.length(); i++)
    {
        h ^= quint32(name.at(i).unicode() & 0xFF);
        h *= 16777619u;
    }
    return h;
}

#define ENGINE_INTENSITY_ATTR 0
#define ENGINE_DIMMER_PREFIX  QStringLiteral("TRACK Dimmer: ")
// gen_programs.py files its ~2700 chase step scenes here. See ensureTable().
#define ENGINE_STEP_PATH      QStringLiteral("AUTO Programs/Steps")
// The folder gen_programs.py writes into. What is in there is not the
// operator's hand-made work - it is the engine's own material, generated
// from this rig's fixtures and named in the engine's own language (tier,
// colour, stars), and FULL AUTO is what it was built for.
#define ENGINE_AUTO_PATH      QStringLiteral("AUTO Programs")
// R410_LAB: LASER LAB's kept looks, and the steps of its beat variants (under
// ENGINE_STEP_PATH, so the table never takes a step for a look)
#define ENGINE_LAB_PATH       QStringLiteral("AUTO Programs/Laser Lab")
#define ENGINE_LAB_STEP_PATH  QStringLiteral("AUTO Programs/Steps/Laser Lab")
// How long a group stays dark while its beams walk home at the top of a
// break. Four bars: long enough for the motor, short enough to be a pause.
// A break programme may run on the BASE only if it keeps this much of the
// group lit, averaged over its steps: the room must not go dark because a
// chase walks one head at a time (Tobias, 2026-09-15: "det jeg skriver med
// lyset slukker, er basen der slukker").
#define ENGINE_BREAK_LIT      0.60
// A programme that keeps every lamp of its group at least this high in every
// step may own the dimmers - on the base too (runde 231, Tobias: "Gør som du
// foreslår" - BACKLOG 70). The room cannot go dark under it.
#define ENGINE_OWN_FLOOR      0.35
// how far from the home aim a laser position may take the beams before the
// engine refuses to run it on its own: 24 units is about 17 degrees, and our
// own tilt figures are clamped to 14 (gen_programs.py, BAR_TILT_REACH)
#define ENGINE_AIM_REACH      24
// Below this much of the ENERGY fader a DROP is not shown as one: no drop
// programmes, no strobes, no flashes, no landing - it runs as a groove. And
// no build or pre-drop blink towards one. Tobias, 2026-09-18: "under 30 %
// energi skal et drop aldrig visualiseres ... det er en restaurant der
// bliver til en klub." From here up the drop machinery ramps with the fader.
#define ENGINE_DROP_SHOW      0.30
// The strobes do not light at all until there is a dance floor, and from
// there they follow the slider up. Tobias, 2026-09-22: "de skal heller ikke
// lyse overhovedet foer energien er der hvor der er dansegulv, og saa skal de
// selvfoelgelig foelge slideren op og blive vildere og vildere." Six very
// powerful lamps hung across the ceiling two to three metres apart: on a room
// that is still a bar, they are the wrong instrument entirely.
//
// This ONE number is the whole line between a bar and a club. It used to be
// ENGINE_DROP_SHOW (0.30), which let them in from a third of the way up.
// Everything else about them ramps from here to the top of the slider.
// RUNDE 313 - back to 0.30, Tobias' decision (2026-09-29): "stroberne må
// være med på 1 slag pr. trin allerede fra 50% og fra 30% 2 slag pr trin. Og
// så hurtigere derfra." They come on at 30 %, two beats a step or slower;
// from 50 % a beat a step; from 75 % in a drop an eighth. strobePaceFloor().
#define ENGINE_STROBE_ON      0.30
#define ENGINE_BARS_AMOK      0.995   // runde 348: the laser bars go wild at 100 % only
#define ENGINE_HWSTROBE_ON    0.55    // the hardware shutter's own line (runde 313)
// runde 344, Tobias 10-03: "animationslaseren skal foerst komme i spil ved 70%"
#define ENGINE_ANI_ON         0.70
#define ENGINE_DARK_BARS      4
#define ENGINE_COLOUR_PREFIX  QStringLiteral("TRACK Colour: ")
#define ENGINE_POS_PREFIX     QStringLiteral("TRACK Pos: ")
#define ENGINE_HOME_PREFIX    QStringLiteral("TRACK Home: ")
#define ENGINE_HOMEB_PREFIX   QStringLiteral("TRACK Home B: ")
#define ENGINE_EFX_PREFIX     QStringLiteral("TRACK EFX: ")
#define ENGINE_ZOOM_PREFIX    QStringLiteral("TRACK Zoom: ")
#define ENGINE_STROBE_PREFIX  QStringLiteral("TRACK Strobe: ")
#define ENGINE_OFF_PREFIX     QStringLiteral("TRACK Off: ")
#define ENGINE_HOLD_PREFIX    QStringLiteral("TRACK Hold: ")   // R378_POSITIONS
// How many strobe rates ensureStrobeScenes() builds per group. driveStrobe()
// draws an index in this range, so the two must never disagree.
#define ENGINE_STROBE_RATES   6
// The strobe budget: how many beats of hardware strobe a 64-beat window may
// hold, at a third of the ENERGY fader and at the stop (a ramp between). The
// drop's landing and the build's riser are outside it - they are the music -
// the "and again" and the groove tastes are inside it. A three-hour night is
// not a three-hour strobe.
#define ENGINE_STROBE_WINDOW  64
// 2026-09-15, Tobias: "der er lidt for meget strob ift. chases paa
// strobe-lamperne". The ceiling came down from 16 to 10 beats per 64 - a
// third less at the top of the fader, unchanged at the bottom.
#define ENGINE_STROBE_BUDGET_LOW   2
// 2026-10-03 (runde 338), the other way: "slet ikke nogle hurtige strob".
// 16 at the top - still a quarter of the window, and the bottom unchanged.
#define ENGINE_STROBE_BUDGET_HIGH  16

/* colours the house does not like: never in the palette, never as an accent,
 * never generated - even when a scene of that colour exists */
static bool engineBannedColour(const QString &colour)
{
    return colour == QLatin1String("yellow");
}

// gen_programs' HARMONY without white - the pairs the mix draws from, the
// look's partner colour (runde 243) and, since review 305, the tiles' partner
// with three colours or more. At file scope so setPartnerOf() can ask it.
static const QMap<QString, QStringList> &engineMixesWith()
{
    static const QMap<QString, QStringList> mixesWith =
    {
        { "red",     { "magenta", "orange", "blue" } },
        { "orange",  { "red", "blue" } },
        { "magenta", { "blue", "red", "cyan" } },
        { "blue",    { "magenta", "cyan", "orange", "red" } },
        { "cyan",    { "blue", "green", "magenta" } },
        { "green",   { "cyan" } },
        { "white",   { "blue", "cyan", "magenta" } },
    };
    return mixesWith;
}

static bool engineColoursPair(const QString &a, const QString &b)
{
    return engineMixesWith().value(a).contains(b) || engineMixesWith().value(b).contains(a);
}
#define ENGINE_HAZE_SCENE     QStringLiteral("TRACK Haze")
#define ENGINE_FAN_SCENE      QStringLiteral("TRACK Fan")

/*********************************************************************
 * Setup
 *********************************************************************/

TrackEngine::TrackEngine(Doc *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_dirty(true)
    , m_building(false)
    , m_hazeScene(0)
    , m_fanScene(0)
    , m_haze(0.0)
    , m_fan(0.0)
    , m_showAll(false)
    , m_accent(true)
    , m_holdBars(32)
    , m_colourBar(-1)
    , m_colourSince(-1)
    , m_holdNow(32)
    , m_keyBias(-1)
    , m_nextKeyBias(-1)
    , m_cooldownMs(-1)
    , m_accentWasWhite(false)
    , m_hatsOut(false)
    , m_barsLead(false)
    , m_castCursor(0)
    , m_motionCursor(0)
    , m_master(1.0)
    , m_blackout(false)
    , m_mixing(false)
    , m_mixBeat(-1)
    , m_speed(0)
    , m_flash(false)
    , m_effects(0)
    , m_effectsBefore(0)
    , m_starCeil(0)
    , m_lastBeat(0)
    , m_calmUntil(0)
    , m_logEnabled(true)
    , m_dropStyle(0)
    , m_kickGone(0)
    , m_kickBeat(-1)
    , m_echoFid(Function::invalidId())
    , m_echoBeat(-100)
    , m_strobeUntil(-1)
    , m_strobeSeen(-1)
    , m_strobeRate(0)
    , m_strobeWindow(-1)
    , m_strobeSpent(0)
    , m_beatMs(500.0)
    , m_beatStartMs(0)
    , m_beatIndex(0)
    , m_testIndex(0)
    , m_room(2)
    , m_roomAuto(false)                  // R378_ROOM_OFF
    , m_roomSent(-1)
    , m_fullAuto(false)
    , m_hold(false)
    , m_startScene(false)
    , m_startLevel(1.0)       // MASTER is the brightness, nothing else
    , m_startColour(false)
    , m_forceNext(false)
{
    QSettings settings;
    m_logEnabled = settings.value(SETTINGS_ENGINE_LOG, true).toBool();
    // runde 317: the kick/bass of the last tracks played, "kick:low:title"
    // each (the title after the second colon, so a colon in it is kept)
    foreach (const QString &entry, settings.value(SETTINGS_ENGINE_PUNCH).toStringList())
    {
        bool okK = false, okL = false;
        const qreal k = entry.section(QLatin1Char(':'), 0, 0).toDouble(&okK);
        const qreal l = entry.section(QLatin1Char(':'), 1, 1).toDouble(&okL);
        if (okK && okL && k > 0.0 && l > 0.0)
        {
            m_punchKick.append(k);
            m_punchLow.append(l);
            m_punchTitle.append(entry.section(QLatin1Char(':'), 2));
        }
    }
    m_docTimer.setSingleShot(true);
    m_docTimer.setInterval(0);         // the next turn of the event loop
    connect(&m_docTimer, SIGNAL(timeout()), this, SLOT(slotDocSettled()));

    m_fadeTimer.setInterval(20);       // 50 a second: a fade, not a staircase
    connect(&m_fadeTimer, SIGNAL(timeout()), this, SLOT(slotFadeTimer()));
    m_clock.start();
    m_pulseTimer.setInterval(20);      // the breath is a slow sine: 25 Hz showed
    connect(&m_pulseTimer, SIGNAL(timeout()), this, SLOT(slotPulseTimer()));
    m_chopTimer.setSingleShot(true);   // runde 356
    m_chopTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_chopTimer, SIGNAL(timeout()), this, SLOT(slotChopTimer()));
    m_testTimer.setInterval(2000);
    connect(&m_testTimer, SIGNAL(timeout()), this, SLOT(slotSelfTestStep()));
    m_layerTimer.setInterval(40);      // R370: 25 a second - a glide, not steps
    connect(&m_layerTimer, SIGNAL(timeout()), this, SLOT(slotLayerTimer()));
    m_echoTimer.setSingleShot(true);
    m_echoOffTimer.setSingleShot(true);
    connect(&m_echoTimer, SIGNAL(timeout()), this, SLOT(slotEchoOn()));
    connect(&m_echoOffTimer, SIGNAL(timeout()), this, SLOT(slotEchoOff()));
    m_beatWatch.setSingleShot(true);
    connect(&m_beatWatch, SIGNAL(timeout()), this, SLOT(slotBeatWatch()));
    // MASTER is deliberately not restored: a night that starts at 40 %
    // because someone dimmed last time is worse than one that starts bright
    m_master = 1.0;
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = qBound(4, settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt(), 128);   // as setHoldBars (runde 220)
    m_holdAuto = settings.value(SETTINGS_ENGINE_HOLDAUTO, true).toBool();
    loadClockCurve(settings);
    m_closingOn = settings.value(SETTINGS_ENGINE_CLOSING, true).toBool();
    // runde 184: ENERGY by clock and the group faders come back after a
    // restart - the same night only. A new evening starts as it always has:
    // the clock on and every group at 100 %.
    if (settings.value(SETTINGS_ENGINE_NIGHT).toString() == nightKey())
    {
        m_roomAuto = false;              // R378_ROOM_OFF (Tobias 10-06): off at every start-up - only its AUTO tile turns it on
        foreach (const QString &entry, settings.value(SETTINGS_ENGINE_GROUPTRIM, QString())
                                               .toString().split(';', Qt::SkipEmptyParts))
        {
            const int eq = entry.lastIndexOf(QLatin1Char('='));
            bool ok = false;
            const qreal level = eq > 0 ? entry.mid(eq + 1).toDouble(&ok) : 0.0;
            if (ok)
                m_groupTrim.insert(entry.left(eq), qBound(0.0, level, 1.0));
        }
    }
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_fullAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
    // Read once, here. loadRoles() does not touch the group switches (the
    // per-show fingerprint they were meant to hang on never came - see the
    // note over saveRoles()); importSettings() re-reads them from the file.
    foreach (QString key, settings.value(SETTINGS_ENGINE_GROUPOFF, QString())
                                  .toString().split(';', Qt::SkipEmptyParts))
        m_groupOff.insert(key);

    if (m_doc != nullptr)
    {
        connect(m_doc, SIGNAL(loaded()), this, SLOT(slotDocChanged()));
        // runde 345: the start picture's watchdog - queued, the timer thread emits it
        if (m_doc->masterTimer() != nullptr)
            connect(m_doc->masterTimer(), SIGNAL(functionStopped(quint32)),
                    this, SLOT(slotFunctionStopped(quint32)), Qt::QueuedConnection);
        connect(m_doc, SIGNAL(cleared()), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(functionRemoved(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureRemoved(quint32)), this, SLOT(slotDocChanged()));
        // Patching a fixture into an existing group used to leave it undriven:
        // no hidden dimmer scene, no entry in g.parts, and not covered by the
        // group's OFF mask either - so switching that group off left the new
        // lamp lit. Renaming a group left the whole table stale the same way.
        connect(m_doc, SIGNAL(fixtureAdded(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureChanged(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureGroupAdded(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureGroupRemoved(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureGroupChanged(quint32)), this, SLOT(slotDocChanged()));
    }
}

TrackEngine::~TrackEngine()
{
    stopAll();
}

void TrackEngine::slotFadeTimer()
{
    // a project was just loaded: the ids below may be the NEW show's, and
    // slotDocSettled() on this turn of the event loop clears it all (runde 202)
    if (m_docTimer.isActive())
        return;
    tickFades();
    if (m_fadeAttr.isEmpty())
        m_fadeTimer.stop();
}

void TrackEngine::slotDocChanged()
{
    labShutdown(false);                  // R410_LAB: an edit or a load under the lab
    m_labScene[0] = m_labScene[1] = 0xFFFFFFFFu;
    // a function or fixture went: the index's m_doc->function() test is stale
    invalidateCandidates();
    m_autoStageKeys.clear();
    m_verdictAutoKeys.clear();
    m_verdictMs = -1;
    // Loading a project emits functionRemoved once per function, fixtureRemoved
    // once per fixture and fixtureGroupRemoved once per group, and then the
    // same again on the way in - hundreds of signals for one event. Tearing the
    // show down and rebuilding the table on each of them is not only wasted
    // work: ensureTable() adds hidden scenes, and Doc::clearContents() iterates
    // a SNAPSHOT of its function list, so scenes added in that window survive
    // the clear and leak into the next project holding values for fixtures that
    // are about to be deleted. One rebuild once the storm has passed.
    m_dirty = true;
    // A PROJECT LOAD: the document has just been emptied, and every id the
    // engine holds names a function that no longer exists. By the time
    // slotDocSettled() runs, the new show is in, numbered from 0 like every
    // show - and stopAll() there stopped whichever NEW functions had the old
    // ids, the new show's startup function among them (runde 217). Forget
    // them now, while they are dead.
    // ... and only then (fejljagt 09-27): Doc starts out Cleared and stays so
    // until a file is loaded, so on a workspace nobody had loaded yet every
    // fixture edit took this branch and forgot functions that were still
    // running. Right after clearContents() the function list is empty.
    if (m_doc != nullptr && m_doc->loadStatus() == Doc::Cleared
        && m_doc->functions().isEmpty())
    {
        m_active.clear();
        m_activeAttr.clear();
        m_activeLevel.clear();
        m_activeOut.clear();
        m_fadeAttr.clear();
        m_fadeLevel.clear();
    }
    if (m_docTimer.isActive() == false)
        m_docTimer.start();
}

// THE START PICTURE'S WATCHDOG (runde 345). Tobias, 2026-10-02: the start
// scene "resetter en gang i mellem, spinner heads lige rundt og gaar tilbage
// til start". The start picture is held by functions the engine started once
// (startLook) and nothing re-runs while it stands - no beats, no tick. One
// of them is the rider's START scene, which is also on a Virtual Console
// button: a press of that button (or anything else that stops it) stops it
// for everyone - Function::stop() with ManualVCWidget clears every source -
// and pan/tilt let go, so the heads swing home until the next startLook().
// Now a stop of one of the start picture's own functions puts the picture
// back at once. Stops the engine makes itself are not seen: stopSlot() takes
// the slot out of m_active before the queued signal lands, and the
// stop-then-start restart (runde 337) is running again by then. A burst of
// stops (STOP ALL) is one restart, and more than three in 10 s is someone
// meaning it - the watchdog lets go until the picture is set again.
void TrackEngine::slotFunctionStopped(quint32 fid)
{
    if (m_startScene == false || m_doc == nullptr || m_testTimer.isActive())
        return;
    bool ours = false;
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it)
    {
        if (it.value() == fid)
        {
            ours = true;
            break;
        }
    }
    if (ours == false || m_startWatchPending)
        return;
    Function *f = m_doc->function(fid);
    if (f == nullptr || (f->isRunning() && f->stopped() == false))
        return;
    m_startWatchPending = true;
    QTimer::singleShot(0, this, SLOT(slotStartWatch()));
}

void TrackEngine::slotStartWatch()
{
    m_startWatchPending = false;
    if (m_startScene == false || m_doc == nullptr)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_startWatchSince > 10000)
    {
        m_startWatchSince = now;
        m_startWatchCount = 0;
    }
    if (++m_startWatchCount > 3)
        return;
    qDebug() << "[TRACK] start picture: a function of it was stopped from outside - put back";
    startLook();
}

void TrackEngine::slotDocSettled()
{
    // the between-tracks look was up (no section, an idle: slot running): an
    // edit in QLC+ took it down with everything else, and nothing but a hand
    // on ENERGY or a track brought it back - a dark room (runde 217)
    bool wasIdle = false;
    if (m_lastState.isEmpty())
    {
        // B22: idle()'s held base has no idle: slot, and a project load has
        // already emptied m_active (slotDocChanged) - idle()'s report still
        // says what was up
        wasIdle = m_report == tr("(idle - base held)") || m_report == tr("(start scene)");
        foreach (const QString &slot, m_active.keys())
        {
            if (slot.startsWith(QStringLiteral("idle:")))
            {
                wasIdle = true;
                break;
            }
        }
    }
    m_darkUntil.clear();
    // same as setFullAuto(): the rebuild drops the unsaved stage counts. On
    // an ordinary edit in the Function Manager this is the current show; on
    // a project switch m_funcs still holds the previous show, and that is
    // exactly what any save before the switch would have written.
    saveRoles();
    m_dirty = true;
    m_position.clear();
    m_moves.clear();
    m_sweep.clear();
    m_sweepShown.clear();
    m_sweepFunc.clear();
    m_splitScenes.clear();
    m_chaseScenes.clear();               // runde 370
    m_holdScenes.clear();                // R378_POSITIONS
    m_chaseShown.clear();                // runde 371
    m_chaseSide.clear();
    m_layerFids.clear();
    m_layerOwned.clear();
    // runde 230: a held programme id of the old show could name an unrelated
    // function of the new one, and run it past every group and ban filter
    m_sectionMotion.clear();
    m_zoomScenes.clear();
    m_strobeScenes.clear();
    m_offScenes.clear();
    m_strobeUntil = -1;
    m_strobeSeen = -1;
    m_strobeRate = 0;
    m_strobeHeadsOnly = false;           // runde 346
    m_strobeWindow = -1;
    m_strobeSpent = 0;
    m_mixBeat = -1;
    // setHaze/setFan early-return on an unchanged value, so a stale reading
    // here left the slider dead until it was moved somewhere else first.
    // And the HAZER ITSELF is put at nought before the reading is: the
    // "TRACK Haze" scene is not in m_active, so stopAll() below does not
    // touch it, and after a rebuild the slider read 0 while the machine kept
    // hazing at the level it had (an LTP channel holds its last value). A
    // Function Manager edit mid-set was enough to trigger this. If the scene
    // is already gone with the project, applyAtmos() finds nothing and does
    // nothing.
    //
    // ... and only while the id still names OUR scene. On a project switch
    // these are the old show's ids, and function ids are small numbers every
    // show reuses: applyAtmos() would write a nought into one of the new
    // show's own scenes (Scene::setValue adds the channel), to be saved with
    // it (runde 168).
    Function *hazeFunc = m_doc != nullptr ? m_doc->function(m_hazeScene) : nullptr;
    Function *fanFunc = m_doc != nullptr ? m_doc->function(m_fanScene) : nullptr;
    if (m_haze > 0.0 && hazeFunc != nullptr && hazeFunc->name() == ENGINE_HAZE_SCENE)
        applyAtmos(m_hazeScene, m_hazeChannels, 0.0);
    if (m_fan > 0.0 && fanFunc != nullptr && fanFunc->name() == ENGINE_FAN_SCENE)
        applyAtmos(m_fanScene, m_fanChannels, 0.0);
    m_haze = 0.0;
    m_fan = 0.0;
    m_flash = false;
    m_zoom.clear();
    m_zoomMode.clear();
    m_floorRound = false;
    // Everything the engine had running keeps running otherwise, with its
    // intensity override stuck where it was - and this fires on an ordinary
    // "delete a function" in the Function Manager, not only on a project load.
    // stopAll() releases the overrides and stops the functions properly.
    stopAll();
    m_active.clear();
    m_activeAttr.clear();
    m_activeLevel.clear();
    m_activeOut.clear();
    m_fadeAttr.clear();
    m_fadeLevel.clear();
    m_liveMove.clear();
    m_patterned.clear();
    m_flashHeld.clear();
    m_cast.clear();
    m_baseCover.clear();                 // runde 260: only tick() says who covers the base
    m_pulseDepth.clear();
    m_pulseStart.clear();
    m_breathe.clear();
    m_texture.clear();
    m_moveHistory.clear();
    m_sweepHistory.clear();
    m_conflictBeats.clear();
    m_lastPan.clear();
    m_headMoveBeats.clear();
    m_hitBeats.clear();
    m_accentPick.clear();
    m_accentGroup.clear();
    m_hatsOut = false;
    m_motionDim.clear();
    m_pulseTimer.stop();
    stopEcho();
    m_fadeTimer.stop();
    // every 'live' property (cast, report, warnings, colour, trims) notifies
    // on liveChanged: without it they keep showing the last project's state
    // until a beat happens to arrive
    m_report.clear();
    m_warnings.clear();
    // stopAll() took the opening picture down with everything else but left
    // its flag up - and tick() returns at once while the flag is up, so the
    // stage stayed dark (tile still lit) until someone touched the tile or
    // ENERGY. Now that SHOW ON opens on it every night, a Function Manager
    // edit before the floor opens was enough (runde 185).
    if (m_startScene)
        startLook();
    else if (wasIdle)
        idle();
    emit tableChanged();
    emit liveChanged();
}

void TrackEngine::slotPulseTimer()
{
    if (m_docTimer.isActive())          // see slotFadeTimer() (runde 202)
        return;
    // between two beats: let every breathing group's dimmers fall back from
    // the level the beat set, so the light pumps with the kick
    bool any = false;
    qint64 now = m_clock.elapsed();
    foreach (const QString &key, m_cast)
    {
        // a held flash is full: nothing steps or pulses under it
        if (m_flashHeld.contains(key))
        {
            // R359_LAND_16: ... except the drop's own landing hit on the strobe
            // lamps (not the operator's FLASH): it blinks on the sixteenths
            // like the rest of the landing
            const TrackGroup &lg = m_groups.value(key);
            if (m_flash == false && m_landSixteenths && blink16Now() && lg.strobes && m_beatMs > 0.0)   // R362: or the mini landing
            {
                any = true;
                const qreal q = std::fmod(qMax(0.0, qreal(now - m_beatStartMs)) / m_beatMs * 4.0, 1.0);
                const qreal gate = q < 0.40 ? 1.0 : 0.0;
                for (int i = 0; i < lg.parts.count(); i++)
                {
                    const QString slot = partSlot(key, i);
                    if (m_active.contains(slot) == false)
                        continue;
                    Function *func = m_doc->function(m_active.value(slot));
                    if (func == nullptr)
                        continue;
                    const qreal o = gate * m_activeLevel.value(slot, 0.0);
                    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, o));
                    m_activeOut.insert(slot, o);
                }
            }
            continue;
        }
        // R358_GROOVE: the effects hit again on the quarters the drums hit
        // between the beats (an off-beat hat, a dembow's 3-3-2) - not the
        // base (the room stays steady), not the strobes (the backbeat), not
        // a group already stepping eighths or sixteenths of its own
        if (m_grooveSlots != 0 && m_beatMs > 0.0 && m_pulseDepth.value(key, 0.0) > 0.0
            && key != m_compositionBase && m_groups.value(key).strobes == false
            && m_liveMove.value(key).subSteps <= 1)
        {
            const int q = int(qBound(0.0, qreal(now - m_beatStartMs) / m_beatMs, 0.999) * 4.0);
            if (q > 0 && (m_grooveSlots & (1 << q)) != 0 && m_grooveSeen.value(key, 0) < q)
            {
                m_grooveSeen.insert(key, q);
                const int nib = onsetHigh(m_lastBeat, q);
                if (nib >= 4)
                {
                    m_pulseStart.insert(key, now);
                    m_pulseStrength.insert(key, 0.55 + 0.45 * qreal(nib) / 15.0);
                }
            }
        }
        // eighths and sixteenths: the pattern steps between the beats
        TrackMove mv = m_liveMove.value(key);
        if (mv.subSteps > 1 && m_patterned.value(key, false) && m_beatMs > 0.0)
        {
            any = true;
            qreal within = qBound(0.0, qreal(now - m_beatStartMs) / m_beatMs, 0.999);
            int sub = int(within * mv.subSteps);
            int step = m_beatIndex * mv.subSteps + sub + mv.phase;
            // every sub-step is a hit of its own: without this the pulse
            // decayed from the beat, and on a bare chase the eighths and
            // sixteenths landed at 37, 14 and 5 per cent - a trail, not steps
            // ... on a beat that HAS a pulse: tick() starts one only on the
            // pulse's beats and on a kick. On any other beat m_subStepSeen still
            // held the last beat's sub-step, so sub 0 restarted the pulse at
            // full on exactly the beat that should have none (runde 171).
            if (mv.pulse > 0.0 && m_pulseStart.value(key, -1) >= m_beatStartMs
                && sub != m_subStepSeen.value(key, 0))
            {
                m_subStepSeen.insert(key, sub);
                m_pulseStart.insert(key, now);
            }
            QVector<qreal> mask = patternMask(key, mv, step, 1.0);
            qreal level = m_moveLevel.value(key, 0.0);
            for (int i = 0; i < mask.count(); i++)
            {
                // Only parts we are already holding. setPart() STARTS a scene
                // it does not find, so without this the sub-beat timer would
                // restart every dimmer part of a group that had just been
                // switched off - twenty milliseconds after the switch, and
                // then fifty times a second against its own OFF mask.
                if (m_active.contains(partSlot(key, i)) == false)
                    continue;
                setPart(key, i, level * mask.at(i));
            }
            continue;
        }
        if (m_pulseDepth.value(key, 0.0) <= 0.0 && m_breathe.value(key, 0) <= 0
            && !m_sequenceGroups.contains(key)
            && !(m_landSixteenths && blink16Now() && m_groups.value(key).strobes))   // runde 359 (R362: or the mini landing)
            continue;
        any = true;
        // the chase owns the dimmers: the pulse rides on ITS intensity, since
        // the parts are handing it nought (see tick())
        if (m_motionDim.contains(key))
        {
            QString slot = "mot:" + key;
            quint32 fid = m_active.value(slot, Function::invalidId());
            Function *func = fid == Function::invalidId() ? nullptr : m_doc->function(fid);
            if (func != nullptr)
            {
                // MASTER and the trim through slotScale, as run() does (runde 174)
                qreal out = qBound(0.0, m_activeLevel.value(slot, 1.0) * slotScale(slot, fid)
                                        * pulseFactor(key), 1.0);
                // painting the colour too: out squared - see run()
                if (canOwnDimmers(m_funcs.value(fid), key == m_compositionBase)     // runde 231
                    && m_funcs.value(fid).setsColour && m_funcs.value(fid).coversColour
                    && m_groups.value(key).rgb)
                    out = std::sqrt(out);
                if (lightsOut())
                    out = 0.0;
                int attr = m_activeAttr.value(slot, -1);
                if (attr >= 0)
                    func->adjustAttribute(out, attr);
                m_activeOut.insert(slot, out);
            }
            continue;
        }
        const TrackGroup &g = m_groups.value(key);
        qreal f = pulseFactor(key);
        for (int i = 0; i < g.parts.count(); i++)
        {
            QString slot = partSlot(key, i);
            if (m_active.contains(slot) == false)
                continue;
            Function *func = m_doc->function(m_active.value(slot));
            if (func != nullptr)
            {
                const qreal scaleNow = (g.strobes && landBurstNow()) ? 1.0 : m_groupTrim.value(key, 1.0) * masterOut();   // runde 357
                qreal out = lightsOut() ? 0.0 : qBound(0.0, m_activeLevel.value(slot, 0.0) * f * scaleNow, 1.0);
                // the same on/off squaring setPart() does: an animation
                // laser's dimmer is a switch, and 0.7 written to it 20 ms
                // after the beat is whatever the fixture makes of it
                if (g.patternDevice)
                    out = out > 0.10 ? 1.0 : 0.0;
                m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
                m_activeOut.insert(slot, out);
            }
        }
    }
    if (any == false)
        m_pulseTimer.stop();
}

int TrackEngine::roleCount() const { return ENGINE_ROLE_COUNT; }

QString TrackEngine::roleName(int role) const
{
    switch (role)
    {
    case ENGINE_ROLE_COLOR:    return tr("Colour");
    case ENGINE_ROLE_MOTION:   return tr("Motion");
    case ENGINE_ROLE_POSITION: return tr("Position");
    case ENGINE_ROLE_FLASH:    return tr("Flash");
    case ENGINE_ROLE_IDLE:     return tr("Start");
    default:                  return tr("Off");
    }
}

QString TrackEngine::roleHint(int role) const
{
    switch (role)
    {
    case ENGINE_ROLE_COLOR:
        return tr("A static colour. The palette puts the same colour on every lit group.");
    case ENGINE_ROLE_MOTION:
        return tr("Chases, patterns, EFX. Only in drops and the back half of a build.");
    case ENGINE_ROLE_POSITION:
        return tr("Held, never stopped. Changes only inside a dark beat at a break.");
    case ENGINE_ROLE_FLASH:
        return tr("Hits: the last bar of a build, the drop, and the FLASH button.");
    case ENGINE_ROLE_IDLE:
        return tr("The start of the evening: runs while AUTO is on and nothing plays.");
    default:
        return QString();
    }
}

/*********************************************************************
 * Table
 *********************************************************************/

bool TrackEngine::hasWord(const QString &text, const QStringList &words) const
{
    foreach (const QString &w, words)
    {
        if (w.length() <= 3)
        {
            // short words must stand alone, so "up" does not match "group".
            // By hand: this built (and JIT-compiled, ~26 us) a regular
            // expression per short word per call - some 100,000-250,000 of
            // them in one ensureTable(). The same test: w with no a-z, æ, ø
            // or å right before or right after it.
            auto letter = [](QChar c) {
                const ushort u = c.unicode();
                return (u >= 'a' && u <= 'z') || u == 0x00e6 || u == 0x00f8 || u == 0x00e5;
            };
            for (qsizetype at = text.indexOf(w); at >= 0; at = text.indexOf(w, at + 1))
            {
                const qsizetype end = at + w.length();
                if ((at == 0 || letter(text.at(at - 1)) == false)
                    && (end >= text.length() || letter(text.at(end)) == false))
                    return true;
            }
        }
        else if (text.contains(w))
        {
            return true;
        }
    }
    return false;
}

QString TrackEngine::colourOf(const QString &text) const
{
    // colour words may sit inside compound names ("WaveRed", "AniBlue"), so
    // they match anywhere - after the known false friends are removed
    QString t = text;
    t.remove("fredagain").remove("fred again").remove("fred");

    static const QList<QPair<QString, QStringList> > table =
    {
        { "magenta", { "magenta", "magneta", "pink", "lilla", "purple" } },
        { "orange",  { "orange" } },
        { "amber",   { "amber" } },
        { "yellow",  { "yellow" } },
        { "cyan",    { "cyan" } },
        { "green",   { "green", "grøn", "groen" } },
        { "blue",    { "blue", "blå", "blaa" } },
        { "red",     { "red", "rød", "roed" } },
        { "white",   { "white", "hvid" } },
    };

    for (int i = 0; i < table.count(); i++)
    {
        foreach (const QString &w, table.at(i).second)
        {
            if (t.contains(w))
                return table.at(i).first;
        }
    }

    // the two that are too short to trust inside other words
    if (hasWord(t, QStringList() << "uv")) return "uv";
    if (hasWord(t, QStringList() << "gul")) return "yellow";    // "gulv" is the floor, not yellow
    if (hasWord(t, QStringList() << "w"))  return "white";

    // "Strobe Strobes MediumB" - a single capital suffix after a lowercase run
    static const QRegularExpression suffix(QStringLiteral("[a-z]([BRWG])\\s*$"));
    QRegularExpressionMatch m = suffix.match(text);
    if (m.hasMatch())
    {
        switch (m.captured(1).at(0).toLatin1())
        {
        case 'B': return "blue";
        case 'R': return "red";
        case 'W': return "white";
        case 'G': return "green";
        }
    }
    return QString();
}

QString TrackEngine::groupOfFixture(quint32 fid) const
{
    Fixture *fxi = m_doc->fixture(fid);
    if (fxi == nullptr)
        return QString();

    QString model = fxi->fixtureDef() ? fxi->fixtureDef()->model().toLower() : fxi->name().toLower();
    if (model.contains("haze") || model.contains("smoke") || model.contains("fog")
        || model.contains("hazer") || model.contains(" fan"))
        return QString();                      // atmosphere is not light

    // the largest fixture group the operator put this fixture in wins
    FixtureGroup *best = nullptr;
    foreach (FixtureGroup *grp, m_doc->fixtureGroups())
    {
        if (grp == nullptr || grp->fixtureList().contains(fid) == false)
            continue;
        if (best == nullptr || grp->fixtureList().count() > best->fixtureList().count())
            best = grp;
    }
    if (best != nullptr)
        return best->name();

    if (fxi->fixtureDef() != nullptr)
        return fxi->fixtureDef()->manufacturer() + " " + fxi->fixtureDef()->model();
    return fxi->name();
}

QList<SceneValue> TrackEngine::valuesOf(const Scene *scene) const
{
    // B23 H3: Scene::values() builds a new list from the scene's QMap on
    // every call. Inside a table build nothing writes a scene before
    // ensureColourScenes(), so one copy per scene serves every helper.
    if (scene == nullptr)
        return QList<SceneValue>();
    if (m_valuesCacheOn == false)
        return scene->values();
    QHash<quint32, QList<SceneValue> >::const_iterator it = m_valuesCache.constFind(scene->id());
    if (it != m_valuesCache.constEnd())
        return it.value();
    return m_valuesCache.insert(scene->id(), scene->values()).value();
}

QSet<quint32> TrackEngine::fixturesOf(Function *func, int depth) const
{
    QSet<quint32> out;
    if (func == nullptr || depth > 3)
        return out;

    switch (func->type())
    {
    case Function::SceneType:
    {
        Scene *scene = qobject_cast<Scene *>(func);
        if (scene != nullptr)
        {
            quint32 last = Fixture::invalidId();          // B23 H4
            for (const SceneValue &sv : valuesOf(scene))
            {
                if (sv.fxi == last)
                    continue;                              // already in the set
                last = sv.fxi;
                out.insert(sv.fxi);
            }
        }
        break;
    }
    case Function::ChaserType:
    case Function::SequenceType:
    {
        Chaser *chaser = qobject_cast<Chaser *>(func);
        if (chaser != nullptr)
        {
            foreach (ChaserStep step, chaser->steps())
                out.unite(fixturesOf(m_doc->function(step.fid), depth + 1));
        }
        break;
    }
    case Function::CollectionType:
    {
        Collection *coll = qobject_cast<Collection *>(func);
        if (coll != nullptr)
        {
            foreach (quint32 child, coll->functions())
                out.unite(fixturesOf(m_doc->function(child), depth + 1));
        }
        break;
    }
    case Function::EFXType:
    {
        EFX *efx = qobject_cast<EFX *>(func);
        if (efx != nullptr)
        {
            foreach (EFXFixture *ef, efx->fixtures())
            {
                if (ef != nullptr)
                    out.insert(ef->head().fxi);
            }
        }
        break;
    }
    case Function::RGBMatrixType:
    {
        RGBMatrix *matrix = qobject_cast<RGBMatrix *>(func);
        if (matrix != nullptr)
        {
            FixtureGroup *grp = m_doc->fixtureGroup(matrix->fixtureGroup());
            if (grp != nullptr)
            {
                foreach (quint32 fid, grp->fixtureList())
                    out.insert(fid);
            }
        }
        break;
    }
    default:
        break;
    }

    return out;
}

int TrackEngine::classify(const TrackFuncInfo &info) const
{
    if (info.junk || info.groups.isEmpty())
        return -1;

    QString n = (info.name + " " + info.path).toLower();

    // "laserupp" -> " upp": strip the fixture words so short position words
    // can stand on their own
    QString core = n;
    core.replace("laser", " ").replace("beam", " ").replace("strobe", " ");

    static const QStringList idleWords     = { "start scene", "startscene", "opening", "aabning",
                                               "åbning", "standby", "idle", "aften", "evening" };
    static const QStringList flashWords    = { "flash", "blink", "hold", "blitz", "bump" };
    static const QStringList strobeWords   = { "strob" };
    static const QStringList positionWords = { "up", "upp", "down", "updow", "offset", "wiggle",
                                               "position", "pos", "movewith", "tilt", "pan" };
    static const QStringList patternWords  = { "wave", "bølge", "boelge", "vifte", "kanon",
                                               "flower", "dryp", "fingre", "finger", "flat",
                                               "static", "satic", "fy fy", "single", "moving",
                                               "chase", "beat", "loop", "random",
                                               "pingpong", "ping pong", "pp", "shot", "fade" };

    bool laserOnly = true;
    foreach (const QString &g, info.groups)
    {
        if (m_groups.contains(g) == false || m_groups.value(g).lasers == false)
            laserOnly = false;
    }

    if (hasWord(n, idleWords))
        return ENGINE_ROLE_IDLE;

    switch (info.type)
    {
    case Function::CollectionType:
        if (hasWord(n, flashWords))   return ENGINE_ROLE_FLASH;
        if (info.colour.isEmpty() == false) return ENGINE_ROLE_COLOR;
        return ENGINE_ROLE_MOTION;

    case Function::EFXType:
        // a sweep on lasers is a position; on heads it is motion
        return laserOnly ? ENGINE_ROLE_POSITION : ENGINE_ROLE_MOTION;

    case Function::ChaserType:
    case Function::SequenceType:
    {
        // a chaser whose every step is a position is itself a position
        // (UP/DOWN on the beat) and must obey the same safety rules
        Chaser *chaser = qobject_cast<Chaser *>(m_doc->function(info.id));
        bool allPos = chaser != nullptr && chaser->steps().isEmpty() == false;
        if (chaser != nullptr)
        {
            foreach (ChaserStep step, chaser->steps())
            {
                Function *sf = m_doc->function(step.fid);
                QString sn = sf ? sf->name().toLower() : QString();
                sn.replace("laser", " ").replace("beam", " ");
                if (sf == nullptr || hasWord(sn, positionWords) == false)
                {
                    allPos = false;
                    break;               // one step that is not an aim decides it
                }
            }
        }
        return allPos ? ENGINE_ROLE_POSITION : ENGINE_ROLE_MOTION;
    }
    case Function::RGBMatrixType:
        return ENGINE_ROLE_MOTION;

    default:
        break;
    }

    // scenes
    if (hasWord(n, flashWords))
        return ENGINE_ROLE_FLASH;
    if (hasWord(core, positionWords))
        return ENGINE_ROLE_POSITION;
    if (hasWord(n, patternWords))
        return ENGINE_ROLE_MOTION;
    if (hasWord(n, strobeWords) && info.colour.isEmpty())
        return ENGINE_ROLE_FLASH;
    return ENGINE_ROLE_COLOR;
}

void TrackEngine::ensureTable()
{
    if (m_dirty == false || m_doc == nullptr || m_building)
        return;
    // Adding a generated scene emits Doc::functionAdded, and anything that
    // reacts to that by reading the table would see it half-built: the groups
    // populated but m_funcs still empty. m_dirty alone did not stop that,
    // because it is cleared before any of the work starts.
    m_building = true;
    m_dirty = false;
    invalidateCandidates();          // m_funcs is about to be cleared and refilled
    m_eyeCache.clear();              // runde 338: the fixtures may have changed
    m_dimmerCache.clear();

    /* ---- groups ---- */
    m_groups.clear();
    m_groupOrder.clear();

    foreach (Fixture *fxi, m_doc->fixtures())
    {
        if (fxi == nullptr)
            continue;
        QString key = groupOfFixture(fxi->id());
        if (key.isEmpty())
            continue;

        if (m_groups.contains(key) == false)
        {
            TrackGroup g;
            g.key = key;
            QString low = key.toLower();
            g.strobes = low.contains("strob") || low.contains("blind");
            g.lasers  = low.contains("laser");
            m_groups.insert(key, g);
            m_groupOrder.append(key);
        }
        TrackGroup &grp = m_groups[key];
        grp.fixtures.append(fxi->id());
        // pan and tilt on a non-laser: a moving head. ANY fixture of the
        // group - not the first one the document happens to list
        bool pan = false, tilt = false;
        for (quint32 ch = 0; ch < fxi->channels(); ch++)
        {
            const QLCChannel *qch = fxi->channel(ch);
            if (qch == nullptr) continue;
            if (qch->group() == QLCChannel::Pan)  pan = true;
            if (qch->group() == QLCChannel::Tilt) tilt = true;
        }
        if (pan && tilt && grp.lasers == false)
            grp.heads = true;
    }
    // Every group in HANGING order - DMX address order - not the order Doc
    // lists the fixtures in (by id). The sweeps were sorted this way on
    // 2026-09-20 (ensureSweeps), but the parts, and so every figure the
    // engine draws itself (a chase, halves, odd/even, the position fan),
    // walked the id order: the wash visited lamps 1, 4, 6, 5, 2, 3, 7 of the
    // row, the strobes 3, 4, 5, 1, 2, 6 (runde 176). The part scenes follow
    // by themselves: ensureDimmerScenes() re-points "#n" at its new fixture.
    foreach (const QString &key, m_groupOrder)
    {
        QList<quint32> &hung = m_groups[key].fixtures;
        std::sort(hung.begin(), hung.end(), [this](quint32 a, quint32 b) {
            Fixture *fa = m_doc->fixture(a);
            Fixture *fb = m_doc->fixture(b);
            if (fa == nullptr || fb == nullptr)
                return a < b;
            if (fa->universe() != fb->universe())
                return fa->universe() < fb->universe();
            return fa->address() != fb->address() ? fa->address() < fb->address() : a < b;
        });
    }

    // R410_LAB: the looks kept in LASER LAB, written into the show before the
    // table reads it (they are found by name and rewritten every build)
    labEnsureShow();

    /* ---- functions ---- */
    // A scene that blends instead of adding is a modifier, not a look: the
    // Light Rider page's colour masks filter what is already on, and its
    // GATE SHUT drives every channel to zero. Neither one, and nothing built
    // out of them, may end up in the table the auto mode picks from.
    // Mask cuts what is already on and Subtractive takes from it; Additive
    // only adds, which is a perfectly good accent step and stays allowed.
    QSet<quint32> modifiers;
    foreach (Function *func, m_doc->functions())
        if (func != nullptr && (func->blendMode() == Universe::MaskBlend
                                || func->blendMode() == Universe::SubtractiveBlend
                                || func->blendMode() == Universe::ReplaceBlend
                                || func->blendMode() == Universe::FilterBlend))
            modifiers.insert(func->id());

    QSet<quint32> steps;
    QSet<quint32> usesModifier;
    for (int pass = 0; pass < 4; pass++)
    {
        int before = usesModifier.count();
        foreach (Function *func, m_doc->functions())
        {
            Chaser *chaser = qobject_cast<Chaser *>(func);
            if (chaser != nullptr)
            {
                foreach (ChaserStep step, chaser->steps())
                {
                    if (pass == 0)
                        steps.insert(step.fid);
                    if (modifiers.contains(step.fid) || usesModifier.contains(step.fid))
                        usesModifier.insert(func->id());
                }
            }
            Collection *coll = qobject_cast<Collection *>(func);
            if (coll != nullptr)
            {
                foreach (quint32 fid, coll->functions())
                    if (modifiers.contains(fid) || usesModifier.contains(fid))
                        usesModifier.insert(func->id());
            }
        }
        // a collection of a chaser of a mask scene: keep going until it settles
        if (usesModifier.count() == before)
            break;
    }

    static const QStringList junkWords = { "blackout", "reset", "new scene", "new chaser",
                                           "new sequence", "new rgb", "copy", "filler",
                                           "speedtest", "fractest", "for advanced", "off" };

    QHash<quint32, TrackFuncInfo> old = m_funcs;
    m_funcs.clear();
    m_blendSkipped.clear();
    QHash<quint32, QString> groupCache;   // B23 H2: fixture -> groupOfFixture(), this rebuild only
    // B23 H3: every scene's values copied out of its QMap ONCE for the
    // read-only part of the build (up to ensureColourScenes(), the first
    // function that writes a scene)
    m_valuesCache.clear();
    m_valuesCacheOn = true;

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;
        // The generated chase steps. They were Hidden until 2026-09-15, which
        // is how they stayed out of here - but QLC+ writes a hidden scene's
        // values as ZERO when it saves (engine/src/scene.cpp: "if a Scene is
        // hidden, so used as a container by some Sequences, it must be saved
        // with values set to zero"), so one save in QLC+ emptied every AUTO
        // program in the show. They are visible functions in their own folder
        // now, and this is the line that keeps them out of the table.
        if (func->path(true).startsWith(ENGINE_STEP_PATH))
            continue;
        // R410_LAB: a lab look no longer kept stays in the file, out of here
        if (func->path(true) == ENGINE_LAB_PATH && m_labLive.contains(func->id()) == false)
            continue;
        if (func->name().startsWith(ENGINE_DIMMER_PREFIX)
            || func->name().startsWith(ENGINE_STROBE_PREFIX)
            || func->name().startsWith(ENGINE_OFF_PREFIX)
            || func->name() == ENGINE_HAZE_SCENE || func->name() == ENGINE_FAN_SCENE)
            continue;
        // A scene that blends instead of adding is a modifier, not a look.
        // The Light Rider page's mask scenes gate what is already on, and its
        // GATE SHUT drives every channel to zero - picked up as a colour by
        // the table below, one of those would filter, or black out, the whole
        // room in the middle of full auto.
        if (modifiers.contains(func->id()) || usesModifier.contains(func->id()))
        {
            if (modifiers.contains(func->id()) == false)
                m_blendSkipped.insert(func->name());
            continue;
        }
        if (func->path(true).startsWith(QStringLiteral("Light Rider/System")))
            continue;

        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

        // An empty function cannot light anything, but it still showed up in
        // the SETUP list as a row to give a role to - and a role given to it
        // is a slot that goes silently dead for that whole section. It gets
        // past every guard below: classify() hands it -1 because it has no
        // groups, but a role set BY HAND is loaded straight back in, and
        // candidates() skips the group test when picking for the whole room.
        // PSMAIN.qxw has four of them today ("New Chaser 1" in
        // Strobes All/Chases/Chases, "New Chaser 406", "Filler",
        // "AnimationWaveRed1"). Emptiness is read from the function itself,
        // not from fixturesOf(), which gives up past three levels of nesting
        // and would drop a deep collection that is perfectly fine.
        if (t == Function::SceneType)
        {
            Scene *hollow = qobject_cast<Scene *>(func);
            if (hollow != nullptr && valuesOf(hollow).isEmpty())
                continue;
        }
        else if (t == Function::ChaserType || t == Function::SequenceType)
        {
            Chaser *hollow = qobject_cast<Chaser *>(func);
            if (hollow != nullptr && hollow->steps().isEmpty())
                continue;
        }

        TrackFuncInfo info;
        info.id = func->id();
        info.name = func->name();
        info.path = func->path(true);
        info.type = int(t);
        info.step = steps.contains(func->id());

        QString n = (info.name + " " + info.path).toLower();
        // "Preset Red" is not a reset: strip the word before the junk test
        info.junk = hasWord(QString(n).replace(QStringLiteral("preset"), QStringLiteral(" ")), junkWords);
        info.colour = colourOf(n);
        if (info.colour.isEmpty())
            info.colour = colourOf(info.name);          // case-sensitive suffix rule
        // the partner of a two-colour programme: gen_programs names it by a
        // tag, never by its colour, so the lead is what colourOf() reads
        // (runde 243). One tag per name, right after the lead colour.
        if (info.path.startsWith(ENGINE_AUTO_PATH))
        {
            static const QMap<QString, QString> partnerTag =
                { { "Fire", "red" }, { "Ember", "orange" }, { "Lime", "green" }, { "Ice", "cyan" },
                  { "Deep", "blue" }, { "Rose", "magenta" }, { "Frost", "white" } };
            foreach (const QString &w, info.name.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            {
                if (partnerTag.contains(w))
                {
                    info.partner = partnerTag.value(w);
                    break;
                }
            }
        }

        QSet<quint32> touched = fixturesOf(func, 0);
        info.fixtureCount = touched.count();
        info.litShare = litShareOf(func, touched);
        info.minLit = minLitOf(func, touched);
        info.peakLit = peakLitOf(func, touched);
        info.setsColour = setsColourOf(func);
        info.aims = aimsOf(func);
        foreach (quint32 fid, touched)
        {
            // B23 H2: the Doc does not change inside this loop
            QHash<quint32, QString>::const_iterator gc = groupCache.constFind(fid);
            const QString key = gc != groupCache.constEnd() ? gc.value()
                                                            : groupCache.insert(fid, groupOfFixture(fid)).value();
            if (key.isEmpty() == false)
                info.groups.insert(key);
        }
        // runde 338, only where they are read (the table rebuild grew by a
        // tenth with both on every function): the beams on a laser group's
        // programme, the full bank on a strobe group's
        info.beamShare = info.litShare;
        info.fullBank = false;
        foreach (const QString &gk, info.groups)
        {
            const TrackGroup &tg = m_groups.value(gk);
            if (tg.lasers && tg.patternDevice == false)
                info.beamShare = beamShareOf(func, touched);
            if (tg.strobes)
                info.fullBank = fullBankOf(func, touched);
        }
        // after the groups are known, not before: this info is still a local
        // and is not in m_funcs yet, so the helper cannot look itself up
        info.coversColour = coversColourOf(func, info.groups);
        info.family = familyOf(info.name);
        // Once here, not once per comparison: candidates() sorts on this and
        // runs several times a beat, on a machine that also has to paint a
        // waveform.
        info.scatter = nameScatter(info.name);

        // A scene that carries the master dimmer itself cannot be dimmed by
        // the group dimmer (HTP: the higher value wins), so the engine has to
        // scale such a scene through its own intensity attribute instead.
        Scene *scene = qobject_cast<Scene *>(func);
        if (scene != nullptr)
        {
            foreach (SceneValue sv, valuesOf(scene))
            {
                Fixture *fxi = m_doc->fixture(sv.fxi);
                if (fxi != nullptr && sv.channel == dimmerChannel(fxi) && sv.value > 0)
                    info.dimmer = true;
            }
        }
        // ... and a CHASE that works the dimmers, read off its steps (runde
        // 173). This flag was only ever set for a scene, and "the chase owns
        // the dimmers" in tick() (motionOwns, runde 69) asks for a chase WITH
        // it - so from the day it was written it never once came true: the
        // engine's own dimmer parts (ReplaceBlend) wrote over every AUTO
        // chase's dimmer figure, and the room only ever showed the engine's
        // figures. Tobias, 2026-09-23: "Slaa AUTO-programmernes egne
        // lysmoenstre til." A Sequence keeps its values in the step itself;
        // a chaser in its step scenes.
        Chaser *dimChase = qobject_cast<Chaser *>(func);
        if (dimChase != nullptr)
        {
            foreach (const ChaserStep &step, dimChase->steps())
            {
                QList<SceneValue> stepValues = step.values;
                if (t != Function::SequenceType)
                {
                    Scene *stepScene = qobject_cast<Scene *>(m_doc->function(step.fid));
                    if (stepScene == nullptr)
                        continue;
                    stepValues = valuesOf(stepScene);
                }
                foreach (const SceneValue &sv, stepValues)
                {
                    Fixture *fxi = m_doc->fixture(sv.fxi);
                    if (fxi != nullptr && sv.channel == dimmerChannel(fxi) && sv.value > 0)
                    {
                        info.dimmer = true;
                        break;
                    }
                }
                if (info.dimmer)
                    break;
            }
        }

        info.tier = tierOf(n);
        info.sweep = (t == Function::EFXType);

        // how long one step lasts, in ms or in beats - the chaser's own
        // tempo, which the engine snaps to the beat instead of overriding
        info.durationMs = 0;
        info.beats = 0.0;
        info.oneShot = false;
        if (t == Function::ChaserType || t == Function::SequenceType)
        {
            Chaser *chaser = qobject_cast<Chaser *>(func);
            if (chaser != nullptr)
            {
                uint sum = 0;
                int stepCount = 0;
                bool inBeats = chaser->tempoType() == Function::Beats;
                if (chaser->durationMode() == Chaser::Common)
                {
                    if (chaser->duration() < 600000)
                    {
                        sum = chaser->duration();
                        stepCount = 1;
                    }
                }
                else
                {
                    foreach (ChaserStep step, chaser->steps())
                    {
                        Function *sf = m_doc->function(step.fid);
                        uint d = chaser->durationMode() == Chaser::PerStep ? step.duration
                                                                             : (sf ? sf->duration() : 0);
                        if (chaser->durationMode() != Chaser::PerStep && sf != nullptr)
                            inBeats = sf->tempoType() == Function::Beats;
                        if (d > 0 && d < 600000)
                        {
                            sum += d;
                            stepCount++;
                        }
                    }
                }
                if (stepCount > 0 && sum > 0)
                {
                    if (inBeats)
                        info.beats = qreal(sum) / qreal(stepCount) / 1000.0;
                    else
                        info.durationMs = sum / uint(stepCount);
                }
                info.oneShot = chaser->runOrder() == Function::SingleShot;
                // A chaser whose own duration is "infinite" and whose steps
                // gave no usable time either never advances: press it and the
                // first step stands there for ever. Two of the show's own
                // chasers are like that (RoeD CHASE 1, Blaa Chase, both at
                // 4294964736 ms - found 2026-09-16). AUTO must not pick one:
                // the group would simply freeze mid-section, which reads as a
                // fault. It stays in the table and on the Virtual Console -
                // this is only about what the engine chooses on its own.
                if (stepCount == 0 && func->duration() >= 600000)
                    info.frozen = true;
                // ... and a chaser with no steps at all is not a programme
                // either. The show has two ("New Chaser 1", "New Chaser 406")
                // - somebody pressed New and walked away. In full auto they
                // are shut out anyway, but with the operator's own functions
                // in play one of them would be a motion that does nothing,
                // and the group would sit still for a whole section.
                if (chaser->steps().isEmpty())
                    info.frozen = true;
            }
        }
        else if ((t == Function::EFXType || t == Function::RGBMatrixType) && func->duration() < 600000)
        {
            // in Beats tempo the duration is beats x 1000, not milliseconds:
            // read as ms a four-beat figure came out as eight and ran at half speed
            if (func->tempoType() == Function::Beats)
                info.beats = qreal(func->duration()) / 1000.0;
            else
                info.durationMs = func->duration();
        }

        info.role = old.contains(info.id) ? old.value(info.id).role : -2;   // -2 = not decided yet
        m_funcs.insert(info.id, info);
    }

    // guesses need the groups to exist first
    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
    {
        it.value().guess = classify(it.value());
        it.value().starsGuess = guessStars(it.value());
        it.value().stars = it.value().starsGuess;
    }

    loadRoles();

    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
        if (it.value().role == -2)
            it.value().role = it.value().step ? -1 : it.value().guess;
    labApplyTags();                      // R410_LAB: what he said, over the names

    learnGroups();

    /* ---- palette: colours that exist on at least two groups - through
     *      the user's scenes, or through what the engine can make ---- */
    QMap<QString, QSet<QString> > coverage;
    QSet<QString> userColours;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.colour.isEmpty())
            continue;
        // a colour scene inside a chaser is their taste (an RGB group can make
        // it) but not coverage: it cannot be run on its own
        if (info.role != ENGINE_ROLE_COLOR)
        {
            if (info.step && info.role == -1 && info.guess == ENGINE_ROLE_COLOR)
                userColours.insert(info.colour);
            continue;
        }
        coverage[info.colour].unite(info.groups);
        userColours.insert(info.colour);
    }
    // an RGB group can make any colour the show uses anywhere (their taste,
    // not the whole rainbow); a macro group the colours it learned
    userColours.insert("white");
    for (QMap<QString, TrackGroup>::const_iterator git = m_groups.constBegin(); git != m_groups.constEnd(); ++git)
    {
        const TrackGroup &g = git.value();
        if (g.generatable() == false)
            continue;
        if (g.rgb)
        {
            foreach (const QString &col, userColours)
                coverage[col].insert(g.key);
        }
        foreach (quint32 fid, g.colourValue.keys())
        {
            const QMap<quint32, QMap<QString, uchar> > &chans = g.colourValue.value(fid);
            for (QMap<quint32, QMap<QString, uchar> >::const_iterator cit = chans.constBegin(); cit != chans.constEnd(); ++cit)
            {
                foreach (const QString &col, cit.value().keys())
                    coverage[col].insert(g.key);
            }
        }
    }

    m_palette.clear();
    QStringList singles;
    QMapIterator<QString, QSet<QString> > cit(coverage);
    while (cit.hasNext())
    {
        cit.next();
        if (engineBannedColour(cit.key()))
            continue;
        if (cit.value().count() >= 2)
            m_palette.append(cit.key());
        else
            singles.append(cit.key());
    }
    if (m_palette.count() < 2)
        m_palette.append(singles);
    // a colour the show no longer has leaves the tiles' set (review 305)
    if (m_overrideSet.isEmpty() == false)
    {
        QStringList kept;
        foreach (const QString &c, m_overrideSet)
            if (m_palette.contains(c)) kept << c;
        if (kept != m_overrideSet)
        {
            m_overrideSet = kept;
            m_overrideIdx = kept.contains(m_override) ? int(kept.indexOf(m_override)) : 0;
            // white never leads a set (review 307) - not after the palette
            // took the lead away either (B27 point 3)
            if (kept.count() >= 2 && kept.at(m_overrideIdx) == QStringLiteral("white"))
                m_overrideIdx = (m_overrideIdx + 1) % int(kept.count());
            m_override = kept.isEmpty() ? QString() : kept.at(m_overrideIdx);
        }
    }

    // the groups that have a programme made for builds (runde 227)
    m_climbGroups.clear();
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        // ... that can SHOW: a dimmer chase that keeps every lamp lit never
        // owns the dimmers (litShare >= 0.99, motionOwns), and on the base
        // nothing does - there the climb only peeks above the engine's own
        // level, and the build's first half is better left to the engine's
        // own growing fill (runde 230, review)
        // (runde 231: or one that keeps every lamp at ENGINE_OWN_FLOOR, which
        // may own them on the base too - canOwnDimmers)
        if (it.value().groups.count() == 1
            && (it.value().litShare < 0.99 || it.value().minLit >= ENGINE_OWN_FLOOR
                || it.value().peakLit >= ENGINE_OWN_FLOOR)                         // runde 259
            && it.value().name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
            m_climbGroups.insert(*it.value().groups.constBegin());
    }

    m_valuesCacheOn = false;          // B23 H3: scenes are written from here on
    m_valuesCache.clear();
    ensureColourScenes();
    ensureStrobeScenes();
    ensureOffScenes();
    learnHome();
    ensurePositionScenes();
    ensureSweeps();
    ensureZoomScenes();
    ensureDimmerScenes();
    ensureAtmosScenes();

    invalidateCandidates();          // (belt and braces: nothing is cached while building)
    m_building = false;
    qDebug() << "[TrackEngine]" << m_groups.count() << "groups," << m_funcs.count()
             << "functions, palette" << m_palette;
}

int TrackEngine::guessStars(const TrackFuncInfo &info) const
{
    // Energy 1..3: how hot the music must be before this may run. Words in
    // the name first; then the tempo - a step every two beats is calm, one
    // a beat is the groove, a half beat or a one-shot per beat is a drop.
    static const QStringList hot  = { "fast", "hard", "high", "drop", "dobbelt", "double", "peak", "hurtig" };
    static const QStringList cool = { "slow", "low", "calm", "break", "halftime", "half", "langsom", "soft" };
    // runde 251: "lift" is the MIDDLE of the fader. A group that is not the
    // rhythm lead only takes programmes of two beats a step or more, and the
    // tempo rule makes every one of those one star - so for the support
    // groups ENERGY 30 % and 70 % drew from the same pool. A "Lift" twin
    // (same figure, snappier, deeper contrast) is two stars: low = glide,
    // middle = lift, top = peak.
    static const QStringList mid  = { "lift" };
    QString n = info.name.toLower();

    if (info.type == int(Function::SceneType) || info.type == int(Function::CollectionType))
        return 1;                                   // a look, not a movement
    if (hasWord(n, hot))
        return 3;
    if (hasWord(n, cool))
        return 1;
    if (hasWord(n, mid))
        return 2;
    if (info.oneShot)
        return 3;
    if (info.type == int(Function::EFXType))
        return 2;

    qreal b = info.beats;
    if (b <= 0.0 && info.durationMs > 0)
        b = qreal(info.durationMs) / 470.0;         // ~128 bpm, close enough for a guess
    if (b <= 0.0)
        return 2;
    if (b >= 1.75)
        return 1;
    if (b >= 0.75)
        return 2;
    return 3;
}

// runde 317: where x stands among the values, 0..1 (ties count half)
static qreal engineRankIn(const QList<qreal> &values, qreal x)
{
    if (values.isEmpty())
        return 0.5;
    qreal below = 0.0;
    foreach (qreal v, values)
        below += v < x ? 1.0 : (qFuzzyCompare(v + 1.0, x + 1.0) ? 0.5 : 0.0);
    return below / qreal(values.count());
}

void TrackEngine::setTrackPunch(const QString &title, qreal kickRef, qreal lowRef)
{
    // Runde 322 (review): only the numbers for the track on the decks now.
    // The history is filled by the drops themselves (tick(), the lock) - it
    // held each TRACK's reference (its 90th-percentile kick) and a drop's
    // kick was ranked against those, but a drop reads 0.7 of that reference
    // on average (measured, 282 drops in the library), so a drop sat low
    // in its own history every time and 58 of 282 went to eighths for sure
    // against 11 on the kick. Drops against drops now.
    m_kickRef = kickRef;
    m_lowRef = lowRef;
    m_punchNow = title;
}

void TrackEngine::noteSectionOverride(const QString &state, const QString &analysed)
{
    // RUNDE 335: "sig:section:<forced>:was:<analysed>" or
    // "sig:section-auto:was:<analysed>" - the analysis' state at the press, so
    // the line says what the operator corrected without scanning back. The
    // engine's choices are untouched: logSignal() only writes the log.
    const QString was = analysed.isEmpty() ? QStringLiteral("-") : analysed;
    logSignal(state.isEmpty() ? QStringLiteral("sig:section-auto:was:") + was
                              : QStringLiteral("sig:section:") + state + QStringLiteral(":was:") + was);
}

void TrackEngine::rememberDropPunch(qreal kick, qreal low)
{
    // runde 327 (review): the same drop again - a DJ loop over the build/drop
    // line locks at bar 2 on every pass - is not a new drop: the last entry
    // of this track with (nearly) the same kick is replaced, not added to
    if (m_punchTitle.isEmpty() == false && m_punchTitle.last() == m_punchNow
        && qAbs(m_punchKick.last() - kick) < 0.02 && qAbs(m_punchLow.last() - low) < 0.02)
    {
        m_punchKick.removeLast();
        m_punchLow.removeLast();
        m_punchTitle.removeLast();
    }
    m_punchKick.append(kick);
    m_punchLow.append(low);
    m_punchTitle.append(m_punchNow);
    while (m_punchKick.count() > 60)
    {
        m_punchKick.removeFirst();
        m_punchLow.removeFirst();
        m_punchTitle.removeFirst();
    }
    QStringList keep;
    for (int i = 0; i < m_punchKick.count(); i++)
        keep << QString::number(m_punchKick.at(i), 'f', 4) + QLatin1Char(':')
                + QString::number(m_punchLow.at(i), 'f', 4) + QLatin1Char(':') + m_punchTitle.at(i);
    QSettings().setValue(SETTINGS_ENGINE_PUNCH, keep);
}

qreal TrackEngine::strobePaceFloor(int tier) const
{
    // Runde 313, Tobias (2026-09-29): "stroberne må altså gerne gå 1 slag pr.
    // trin. Det er de fleste af vores egne chases allerede ... 1 slag pr. trin
    // allerede fra 50% og fra 30% 2 slag pr trin. Og så hurtigere derfra."
    // Runde 312 held them to two beats a step (the support pace - the strobes
    // are never the rhythm lead) with one door at 70 % in a drop. Now the
    // slider is the door, read on every pick: a hand pulled down slows them
    // at once. A break is a slow walk (runde 235); a groove never steps
    // under a beat (runde 219: an eighth at most, and only in a drop).
    const qreal f = qBound(0.0, m_faderNow, 1.0);
    if (tier == 0 || f < 0.50)
        return 2.0;
    if (f < 0.75 || tier != 2)
        return 1.0;
    // runde 316: over 75 % in a drop the kick decides - a hard one a beat
    // a step, a soft one eighths (tick(), m_strobeOnKick, held per drop)
    return m_strobeOnKick ? 1.0 : 0.5;
}

qreal TrackEngine::stepBeats(const TrackFuncInfo &info, qreal bpm) const
{
    if (info.beats > 0.0)
        return info.beats;
    if (info.durationMs > 0 && bpm > 0.0)
        return qreal(info.durationMs) / (60000.0 / bpm);
    return 0.0;
}

int TrackEngine::divisionFor(const TrackFuncInfo &info, qreal bpm, int division) const
{
    // The SPEED slider, when set, still forces a step length. Otherwise the
    // function's own tempo is snapped to the beat grid - a quarter, a half,
    // one, two, four, eight, sixteen beats - so "halftime" stays halftime
    // and a fast chase stays fast, but both land on the beat.
    if (division > 0)
        return division;
    if (info.oneShot)
        return 0;                                   // its own time, retriggered on the beat
    qreal b = stepBeats(info, bpm);
    if (b <= 0.0)
    {
        // The engine could not read a step time out of this chaser at all -
        // no per-step times and a chaser duration of nought or "infinite".
        // Started on its own terms, nought means it advances every tick of
        // the master timer (a flicker: "8eyeChaseBeatWhitePingPongBeat" in
        // this show) and infinite means it never advances at all. Neither is
        // a look. One beat a step is the honest reading of a chase nobody
        // timed, and it is what the rest of the rig is doing anyway.
        return (info.type == int(Function::ChaserType)
                || info.type == int(Function::SequenceType)) ? 1000 : 0;
    }
    static const qreal grid[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    qreal best = grid[0];
    qreal bestDist = 99.0;
    for (int i = 0; i < 7; i++)
    {
        qreal dist = qAbs(std::log2(b / grid[i]));
        if (dist < bestDist)
        {
            bestDist = dist;
            best = grid[i];
        }
    }
    if (m_speed < 0)
        best *= 2.0;
    else if (m_speed > 0)
        best = qMax(0.125, best / 2.0);
    // The quiet hour: under ENGINE_DROP_SHOW every programme walks at half
    // its pace - "hjem position med en langsom beatchase henover lamperne"
    // is what a still effect is (Tobias, 2026-09-18). The 2x tile still wins.
    else if (m_dropShown == false)   // the bar line's reading (runde 279), like the tier
        best *= 2.0;
    if (m_halfTime && m_speed == 0 && m_dropShown)
        best *= 2.0;                 // R358_HALFTIME: a half-time groove walks at half pace
    else if (m_density != 0 && m_speed == 0 && m_dropShown)
        best = m_density > 0 ? qMax(0.25, best / 2.0) : best * 2.0;   // R359_DENSITY
    return int(best * 1000.0);
}

void TrackEngine::learnGroups()
{
    // What can the engine make on its own for each group? RGB channels give
    // any colour. Where colour is a value on some other channel - a macro,
    // or the laser bars' eight per-eye channels where one value is one
    // colour - the values are read off the user's own colour scenes: every
    // channel that changes with the colour is a colour channel, and what
    // the "green" scene set it to is what green is. Channels every colour
    // scene of a fixture sets alike (shutter open, an effect mode, a speed)
    // are the fixture's base and come along in every generated scene.
    // Animation lasers, whose scenes are patterns, keep their own scenes.
    for (QMap<QString, TrackGroup>::iterator it = m_groups.begin(); it != m_groups.end(); ++it)
    {
        TrackGroup &g = it.value();
        g.rgb = false;
        g.patternDevice = hasWord(g.key.toLower(), QStringList() << "anim" << "animation" << "pattern");
        g.colourValue.clear();
        g.baseValue.clear();

        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            bool r = false, gr = false, b = false, pan = false, tilt = false;
            int effects = 0;
            for (quint32 i = 0; i < fxi->channels(); i++)
            {
                const QLCChannel *qch = fxi->channel(i);
                if (qch == nullptr)
                    continue;
                if (qch->group() == QLCChannel::Intensity && qch->colour() == QLCChannel::Red) r = true;
                if (qch->group() == QLCChannel::Intensity && qch->colour() == QLCChannel::Green) gr = true;
                if (qch->group() == QLCChannel::Intensity && qch->colour() == QLCChannel::Blue) b = true;
                if (qch->group() == QLCChannel::Effect) effects++;
                if (qch->group() == QLCChannel::Pan)  pan = true;
                if (qch->group() == QLCChannel::Tilt) tilt = true;
            }
            if (r && gr && b)
                g.rgb = true;
            // three effect channels and no colour mixing reads as an
            // animation laser - unless it pans and tilts: a gobo or CMY spot
            // has prism, prism rotation and a macro channel too, and making
            // it a pattern device took its positions, zoom and figure away
            else if (effects >= 3 && (pan && tilt) == false)
                g.patternDevice = true;
        }

        // EVERY colour scene of the operator's that puts a value on this
        // group - not only the ones that drive nothing else. A whole-room
        // look is still them telling us what "red" means on these fixtures,
        // and for a lamp whose colour sits on a single mixing channel it is
        // usually the only place it is written down anywhere.
        QMap<quint32, QMap<quint32, QList<uchar> > > seen;             // fixture -> channel -> values
        QMap<quint32, QMap<quint32, QMap<QString, uchar> > > byColour;  // fixture -> channel -> colour -> value
        QSet<QString> coloursSeen;
        for (QHash<quint32, TrackFuncInfo>::const_iterator fit = m_funcs.constBegin(); fit != m_funcs.constEnd(); ++fit)
        {
            const TrackFuncInfo &info = fit.value();
            // a colour scene that sits inside a chaser is not RUN on its own
            // (role -1), but it still says what "red" is on these fixtures -
            // and in a show built as colour chases it is the only place that
            // is written down
            bool colourStep = info.step && info.role == -1 && info.guess == ENGINE_ROLE_COLOR;
            if ((info.role != ENGINE_ROLE_COLOR && colourStep == false) || info.generated || info.colour.isEmpty()
                || info.type != int(Function::SceneType)
                || info.groups.contains(g.key) == false)
                continue;
            Scene *scene = qobject_cast<Scene *>(m_doc->function(info.id));
            if (scene == nullptr)
                continue;
            bool touched = false;
            foreach (SceneValue sv, valuesOf(scene))
            {
                if (g.fixtures.contains(sv.fxi) == false)
                    continue;              // the rest of a whole-room look is not ours
                seen[sv.fxi][sv.channel].append(sv.value);
                byColour[sv.fxi][sv.channel].insert(info.colour, sv.value);
                touched = true;
            }
            if (touched)
                coloursSeen.insert(info.colour);
        }
        if (coloursSeen.count() < 2)
            continue;

        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr || seen.contains(fid) == false)
                continue;
            quint32 dim = dimmerChannel(fxi);
            const QMap<quint32, QList<uchar> > &channels = seen.value(fid);
            for (QMap<quint32, QList<uchar> >::const_iterator cit = channels.constBegin(); cit != channels.constEnd(); ++cit)
            {
                quint32 ch = cit.key();
                const QList<uchar> &vals = cit.value();
                const QLCChannel *qch = fxi->channel(ch);
                if (qch == nullptr || ch == dim)
                    continue;
                if (qch->group() == QLCChannel::Intensity && qch->colour() != QLCChannel::NoColour)
                    continue;                                   // the colour itself

                // A whole-room look does not set every fixture, so counting
                // against the number of scenes no longer works: a channel is
                // "the base" when every scene that writes it writes the same
                // value, and it is a colour channel when at least two DIFFERENT
                // colours write it differently. One sample proves nothing.
                int coloursHere = byColour.value(fid).value(ch).count();
                bool constant = true;
                for (int i = 1; i < vals.count() && constant; i++)
                    if (vals.at(i) != vals.first())
                        constant = false;

                // Two different COLOURS agreeing, not two scenes: three red
                // room looks are one colour's opinion, not a constant. And
                // pan, tilt and speed never belong in a colour scene - a
                // whole-room look carries them now that we read those, and
                // baking an aim into every colour would fight the positions.
                if (constant && coloursHere >= 2
                    && qch->group() != QLCChannel::Pan
                    && qch->group() != QLCChannel::Tilt
                    && qch->group() != QLCChannel::Speed)
                    g.baseValue[fid].insert(ch, vals.first());
                // ... and nor are the shutter, the zoom (Beam) or a speed: the
                // Light Rider colour scenes differ on them ("STROB HVID", a
                // zoom per look), and learned as colour they made eight
                // colourless zoom programmes read as painting a colour of
                // their own - out of every colour's pool (runde 176)
                else if (constant == false && coloursHere >= 2
                         && qch->group() != QLCChannel::Pan && qch->group() != QLCChannel::Tilt
                         && qch->group() != QLCChannel::Shutter && qch->group() != QLCChannel::Beam
                         && qch->group() != QLCChannel::Speed)
                    g.colourValue[fid].insert(ch, byColour.value(fid).value(ch));
            }
        }

        // per-eye colour channels ("Laser Color 1..8", "Eye 3"): four or more
        // of them, and the group can wear two colours on one lamp
        g.perEye = false;
        foreach (quint32 fid, g.colourValue.keys())
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            int eyes = 0;
            foreach (quint32 ch, g.colourValue.value(fid).keys())
            {
                const QLCChannel *qch = fxi->channel(ch);
                static const QRegularExpression eyeName(QStringLiteral("(colou?r|eye)\\s*\\d+"),
                                                        QRegularExpression::CaseInsensitiveOption);
                if (qch != nullptr && qch->name().contains(eyeName))
                    eyes++;
            }
            if (eyes >= 4)
                g.perEye = true;
        }
    }
}

quint32 TrackEngine::splitColourFunction(const QString &group, const QString &a, const QString &b)
{
    // Two palette colours on one laser bar: colour a on the even eyes, b on
    // the odd ones, from the values the user's own colour scenes taught us.
    // Still two colours in the room - the accent just moved onto the eyes.
    QString key = group + "|" + a + "|" + b;
    if (m_splitScenes.contains(key) && m_doc->function(m_splitScenes.value(key)) != nullptr)
        return m_splitScenes.value(key);

    const TrackGroup &g = m_groups.value(group);
    if (g.perEye == false)
        return Function::invalidId();

    QRegularExpression eye(QStringLiteral("(colou?r|eye)\\s*\\d+"), QRegularExpression::CaseInsensitiveOption);
    QList<SceneValue> values;
    int touched = 0;
    foreach (quint32 fid, g.fixtures)
    {
        Fixture *fxi = m_doc->fixture(fid);
        if (fxi == nullptr || g.colourValue.contains(fid) == false)
            continue;
        const QMap<quint32, QMap<QString, uchar> > &chans = g.colourValue.value(fid);
        int index = 0;
        bool ok = false;
        for (QMap<quint32, QMap<QString, uchar> >::const_iterator cit = chans.constBegin(); cit != chans.constEnd(); ++cit)
        {
            const QLCChannel *qch = fxi->channel(cit.key());
            bool isEye = qch != nullptr && qch->name().contains(eye);
            const QString &colour = (isEye && (index % 2) == 1) ? b : a;
            if (isEye)
                index++;
            if (cit.value().contains(colour) == false)
                continue;
            values.append(SceneValue(fid, cit.key(), cit.value().value(colour)));
            ok = true;
        }
        if (ok == false)
            continue;
        const QMap<quint32, uchar> base = g.baseValue.value(fid);
        for (QMap<quint32, uchar>::const_iterator bit = base.constBegin(); bit != base.constEnd(); ++bit)
            values.append(SceneValue(fid, bit.key(), bit.value()));
        touched++;
    }
    if (touched == 0)
        return Function::invalidId();

    QString name = ENGINE_COLOUR_PREFIX + QString("%1 %2+%3").arg(group).arg(a).arg(b);
    Scene *scene = nullptr;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name() == name)
            scene = qobject_cast<Scene *>(func);
    }
    if (scene != nullptr)
    {
        foreach (SceneValue old, scene->values())
            scene->unsetValue(old.fxi, old.channel);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
    }
    else
    {
        scene = new Scene(m_doc);
        scene->setName(name);
        scene->setVisible(false);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
        if (m_doc->addFunction(scene) == false)
        {
            delete scene;
            return Function::invalidId();
        }
    }
    m_splitScenes.insert(key, scene->id());
    return scene->id();
}

void TrackEngine::learnHome()
{
    // 'TRACK Home: <group>' and 'TRACK Home B: <group>' are written by
    // lr_import.py from the operator's Light Rider show: the pan/tilt each
    // head really points at. Without them the engine can only guess that the
    // middle of the range is the middle of the room, which is wrong on any
    // rig where the heads hang turned differently.
    QMap<QString, quint32> homeId, homeBId;              // group -> scene
    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->type() != Function::SceneType)
            continue;
        QString n = func->name();
        if (n.startsWith(ENGINE_HOMEB_PREFIX))
            homeBId.insert(n.mid(ENGINE_HOMEB_PREFIX.length()), func->id());
        else if (n.startsWith(ENGINE_HOME_PREFIX))
            homeId.insert(n.mid(ENGINE_HOME_PREFIX.length()), func->id());
    }

    foreach (const QString &key, m_groupOrder)
    {
        TrackGroup &g = m_groups[key];
        g.home.clear();
        g.homeB.clear();
        if (homeId.contains(key) == false && homeBId.contains(key) == false)
            continue;

        for (int pass = 0; pass < 2; pass++)
        {
            // an id of zero is a real function: only a scene we actually found
            quint32 fid = pass == 0 ? homeId.value(key, Function::invalidId())
                                    : homeBId.value(key, Function::invalidId());
            if (fid == Function::invalidId())
                continue;
            Scene *scene = qobject_cast<Scene *>(m_doc->function(fid));
            if (scene == nullptr)
                continue;
            QMap<quint32, QPair<int, int> > aim;          // fixture -> pan, tilt
            QSet<quint32> gotPan, gotTilt;
            foreach (SceneValue sv, scene->values())
            {
                Fixture *fxi = m_doc->fixture(sv.fxi);
                const QLCChannel *qch = fxi ? fxi->channel(sv.channel) : nullptr;
                if (qch == nullptr)
                    continue;
                bool isPan = qch->preset() == QLCChannel::PositionPan
                          || (qch->group() == QLCChannel::Pan && qch->controlByte() == QLCChannel::MSB);
                bool isTilt = qch->preset() == QLCChannel::PositionTilt
                           || (qch->group() == QLCChannel::Tilt && qch->controlByte() == QLCChannel::MSB);
                if (isPan)
                {
                    aim[sv.fxi].first = int(sv.value);
                    gotPan.insert(sv.fxi);
                }
                else if (isTilt)
                {
                    aim[sv.fxi].second = int(sv.value);
                    gotTilt.insert(sv.fxi);
                }
            }
            foreach (quint32 fxid, aim.keys())
            {
                // half an aim is no aim
                if (g.fixtures.contains(fxid) == false
                    || gotPan.contains(fxid) == false || gotTilt.contains(fxid) == false)
                    continue;
                // ... and nought/nought is no aim either. lr_import.py used to
                // write these scenes HIDDEN (until runde B17 - a re-import now
                // replaces them in place, visible, with real values), and QLC+
                // saves a hidden scene's values as zero (Scene::saveXML, see
                // CLAUDE.md): in a file nobody has re-imported both Home scenes
                // of the wash are 0/0 on all seven heads. The guard stays for
                // those files (bane B, B17 point 2). Learned as an
                // aim, the fan folded onto one side of the range (runde 176).
                // Without it the engine falls back to the middle, as it does
                // with no Home scene at all.
                if (aim.value(fxid).first == 0 && aim.value(fxid).second == 0)
                    continue;
                QPoint p(aim.value(fxid).first, aim.value(fxid).second);
                if (pass == 0)
                    g.home.insert(fxid, p);
                else
                    g.homeB.insert(fxid, p);
            }
        }
    }
}

void TrackEngine::ensurePositionScenes()
{
    // Positions for every group of RGB moving heads, made from the pan/tilt
    // channels. slope fans the pan out per head (negative crosses them),
    // panOff shifts them all, split sends the two halves apart, zig
    // alternates the tilt head by head, tiltSlope tilts across the row.
    // Zoom is its own move now (ensureZoomScenes). Names carry the tier
    // words the picker reads: center/low = break, fan = groove,
    // cross/high/wide = drop; the rest go anywhere.
    struct PosDef { const char *name; qreal slope; int tilt; int panOff; int split; int zig; qreal tiltSlope; };
    // The heads hang upside down from the ceiling: tilt 128 is straight
    // down at the floor, and every position stays within about 55 degrees
    // of that - the light belongs on the floor and the room, not the ceiling.
    static const PosDef defs[] = {
        { "Center",      0.0, 128,   0,  0,  0, 0.0 },
        { "Fan",        14.0, 128,   0,  0,  0, 0.0 },
        { "Wide Fan",   26.0, 130,   0,  0,  0, 0.0 },
        { "Cross",     -18.0, 112,   0,  0,  0, 0.0 },
        { "Tight Cross",-8.0, 120,   0,  0,  0, 0.0 },
        { "High",        6.0,  84,   0,  0,  0, 0.0 },
        { "Low",        10.0, 172,   0,  0,  0, 0.0 },
        { "Left",        4.0, 124, -40,  0,  0, 0.0 },
        { "Right",       4.0, 124,  40,  0,  0, 0.0 },
        { "Zigzag",      8.0, 124,   0,  0, 28, 0.0 },
        { "Wave",        8.0, 116,   0,  0,  0, 9.0 },
        { "Floor",       0.0, 138,   0,  0,  0, 0.0 },
        { "Converge",  -26.0,  98,   0,  0,  0, 0.0 },
        { "Split",       0.0, 124,   0, 36,  0, 0.0 },
        { "Fan High",   18.0,  88,   0,  0,  0, 0.0 },
        { "Fan Low",    14.0, 164,   0,  0,  0, 0.0 },
        { "Cross Low", -18.0, 150,   0,  0,  0, 0.0 },
        { "Wall",        0.0,  96,   0,  0,  0, 0.0 },
        { "Cross Zig", -14.0, 118,   0,  0, 22, 0.0 },
        { "Wide Wave",  22.0, 124,   0,  0,  0, -8.0 } };

    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_POS_PREFIX))
            existing.insert(func->name().mid(ENGINE_POS_PREFIX.length()), func->id());
    }

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.heads == false || g.patternDevice)
            continue;

        int n = g.fixtures.count();
        qreal mid = (n - 1) / 2.0;
        for (uint d = 0; d < sizeof(defs) / sizeof(defs[0]); d++)
        {
            QList<SceneValue> values;
            for (int i = 0; i < n; i++)
            {
                Fixture *fxi = m_doc->fixture(g.fixtures.at(i));
                if (fxi == nullptr)
                    continue;
                quint32 pan = QLCChannel::invalid(), panF = QLCChannel::invalid(),
                        tilt = QLCChannel::invalid(), tiltF = QLCChannel::invalid(),
                        speed = QLCChannel::invalid(), zoom = QLCChannel::invalid();
                bool speedFastSlow = true, zoomSmallBig = true;
                for (quint32 ch = 0; ch < fxi->channels(); ch++)
                {
                    const QLCChannel *qch = fxi->channel(ch);
                    if (qch == nullptr)
                        continue;
                    switch (qch->preset())
                    {
                        case QLCChannel::PositionPan:          pan = ch; break;
                        case QLCChannel::PositionPanFine:      panF = ch; break;
                        case QLCChannel::PositionTilt:         tilt = ch; break;
                        case QLCChannel::PositionTiltFine:     tiltF = ch; break;
                        case QLCChannel::SpeedPanTiltFastSlow: speed = ch; speedFastSlow = true; break;
                        case QLCChannel::SpeedPanTiltSlowFast: speed = ch; speedFastSlow = false; break;
                        case QLCChannel::BeamZoomSmallBig:     zoom = ch; zoomSmallBig = true; break;
                        case QLCChannel::BeamZoomBigSmall:     zoom = ch; zoomSmallBig = false; break;
                        default: break;
                    }
                    if (pan == QLCChannel::invalid() && qch->group() == QLCChannel::Pan && qch->controlByte() == QLCChannel::MSB) pan = ch;
                    if (tilt == QLCChannel::invalid() && qch->group() == QLCChannel::Tilt && qch->controlByte() == QLCChannel::MSB) tilt = ch;
                }
                if (pan == QLCChannel::invalid() || tilt == QLCChannel::invalid())
                    continue;
                qreal off = i - mid;
                // where this head lives. With a learned home the whole set is
                // built around the operator's own aim; without one the middle
                // of the range has to do
                bool learned = g.home.contains(fxi->id());
                // STRAIGHT DOWN is the reference. Every head hangs exactly
                // upside down from the ceiling, so the middle of the tilt
                // range is the floor beneath it - whatever way the body is
                // turned on its clamp, and whatever the rider's own aims were.
                // (Those were the median of a set of static busking looks:
                // a point, not a home. Half of them pointed at the ceiling,
                // and every generated position was built around that.)
                // Pan keeps its learned value only as the direction a lean
                // goes in; straight down, pan does not matter at all.
                QPoint aim = QPoint(learned ? g.home.value(fxi->id()).x() : 128, 128);

                // the definition's tilt, 80..176 around straight down, is how
                // far the beam leans out; pan fans the heads apart across the
                // group. Both signs of the lean go out across the floor - one
                // the way the head is turned, one the other way - so neither
                // is 'up' and neither is damped.
                qreal dPan = defs[d].slope * off + defs[d].panOff
                           + (defs[d].split ? (off < 0 ? -defs[d].split : defs[d].split) : 0);
                qreal dTilt = (defs[d].tilt - 128.0)
                            + ((i % 2) ? defs[d].zig : -defs[d].zig) / 2.0
                            + defs[d].tiltSlope * off;
                // the fan, not a new aim: a head never swings more than this
                // far from the way it is turned
                dPan = qBound(-45.0, dPan, 45.0);
                // the floor cone: 48 units is about 50 degrees off vertical,
                // which is as far out as this ceiling lets a beam go before it
                // is on a wall rather than on the room
                dTilt = qBound(-48.0, dTilt, 48.0);
                // a head whose aim sits near the end of its travel would just
                // stand at the stop: send it the other way instead, so every
                // head in the group actually moves
                if (aim.x() + dPan < 4.0 || aim.x() + dPan > 251.0)
                    dPan = -dPan;
                if (aim.y() + dTilt < 4.0 || aim.y() + dTilt > 251.0)
                    dTilt = -dTilt;
                int panVal = qBound(0, int(qRound(aim.x() + dPan)), 255);
                int tiltVal = qBound(0, int(qRound(aim.y() + dTilt)), 255);
                tiltVal = qBound(80, tiltVal, 176);       // always inside the floor cone
                values.append(SceneValue(fxi->id(), pan, uchar(panVal)));
                values.append(SceneValue(fxi->id(), tilt, uchar(tiltVal)));
                if (panF != QLCChannel::invalid()) values.append(SceneValue(fxi->id(), panF, 0));
                if (tiltF != QLCChannel::invalid()) values.append(SceneValue(fxi->id(), tiltF, 0));
                if (speed != QLCChannel::invalid()) values.append(SceneValue(fxi->id(), speed, uchar(speedFastSlow ? 60 : 195)));
                Q_UNUSED(zoom)
                Q_UNUSED(zoomSmallBig)
            }
            if (values.isEmpty())
                continue;

            QString name = QString("%1 %2").arg(key).arg(QLatin1String(defs[d].name));
            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                foreach (SceneValue old, scene->values())
                    scene->unsetValue(old.fxi, old.channel);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
            }
            else
            {
                scene = new Scene(m_doc);
                scene->setName(ENGINE_POS_PREFIX + name);
                scene->setVisible(false);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
                if (m_doc->addFunction(scene) == false)
                {
                    delete scene;
                    continue;
                }
            }

            TrackFuncInfo info;
            info.id = scene->id();
            info.name = scene->name();
            info.scatter = nameScatter(info.name);
            info.type = int(Function::SceneType);
            info.role = ENGINE_ROLE_POSITION;
            info.guess = ENGINE_ROLE_POSITION;
            info.groups.insert(key);
            info.generated = true;
            info.tier = tierOf(QString(QLatin1String(defs[d].name)).toLower());
            info.stars = 1;
            info.starsGuess = 1;
            info.fixtureCount = n;
            m_funcs.insert(info.id, info);
        }
    }
}

void TrackEngine::ensureZoomScenes()
{
    // Hidden zoom scenes per group of moving heads, so the beam is a move of
    // its own: tight beams for big figures in a drop, a wide wash in a break,
    // wide for a bar when a drop lands.
    // Runde 214 (Tobias): nine steps from the sharpest beam to the widest -
    // "Narrow" is 0 now, the sharp beam his own scenes use (124 of them), not
    // 40 - so a build can tighten a step a bar; and two alternating scenes
    // (every other head sharp, the rest wide, and the other way round). Each
    // fades in over 300 ms, so a step is a zoom moving, not a snap.
    static const char *names[11] = { "Narrow", "Zoom 1", "Zoom 2", "Zoom 3", "Mid",
                                     "Zoom 5", "Zoom 6", "Zoom 7", "Wide", "Alt A", "Alt B" };
    static const int levels[9] = { 0, 28, 56, 84, 112, 140, 168, 196, 225 };

    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_ZOOM_PREFIX))
            existing.insert(func->name().mid(ENGINE_ZOOM_PREFIX.length()), func->id());
    }

    m_zoomScenes.clear();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.heads == false || g.patternDevice)
            continue;
        QList<quint32> ids;
        for (int z = 0; z < 11; z++)
        {
            QList<SceneValue> values;
            int headIndex = 0;
            foreach (quint32 fid, g.fixtures)
            {
                Fixture *fxi = m_doc->fixture(fid);
                if (fxi == nullptr)
                    continue;
                // the alternating pair: A = even heads sharp, odd wide; B the other way
                const int level = z < 9 ? levels[z]
                                : (((headIndex % 2) == 0) == (z == 9) ? levels[0] : levels[8]);
                headIndex++;
                for (quint32 ch = 0; ch < fxi->channels(); ch++)
                {
                    const QLCChannel *qch = fxi->channel(ch);
                    if (qch == nullptr)
                        continue;
                    if (qch->preset() == QLCChannel::BeamZoomSmallBig)
                        values.append(SceneValue(fid, ch, uchar(level)));
                    else if (qch->preset() == QLCChannel::BeamZoomBigSmall)
                        values.append(SceneValue(fid, ch, uchar(255 - level)));
                }
            }
            if (values.isEmpty())
                break;
            QString name = QString("%1 %2").arg(key).arg(QLatin1String(names[z]));
            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                foreach (SceneValue old, scene->values())
                    scene->unsetValue(old.fxi, old.channel);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
            }
            else
            {
                scene = new Scene(m_doc);
                scene->setName(ENGINE_ZOOM_PREFIX + name);
                scene->setVisible(false);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
                if (m_doc->addFunction(scene) == false)
                {
                    delete scene;
                    break;
                }
            }
            scene->setFadeInSpeed(300);
            ids.append(scene->id());
        }
        if (ids.count() == 11)
            m_zoomScenes.insert(key, ids);
    }
}

void TrackEngine::ensureSweeps()
{
    // One hidden EFX per group of moving heads, run RELATIVE to the aimed
    // position: whatever position scene holds the heads, the figure the
    // engine draws for a section rides on top of it. Shape, size, tempo and
    // how the heads relate are set on the fly - this only makes the function
    // and puts the heads in it.
    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->type() == Function::EFXType && func->name().startsWith(ENGINE_EFX_PREFIX))
            existing.insert(func->name().mid(ENGINE_EFX_PREFIX.length()), func->id());
    }

    m_sweepFunc.clear();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        // heads always; lasers too, but their figure is pan-only and small,
        // and it runs in FULL AUTO only (their aim stays the user's)
        if (g.lasers == false && (g.heads == false || g.patternDevice))
            continue;

        EFX *efx = nullptr;
        if (existing.contains(key))
            efx = qobject_cast<EFX *>(m_doc->function(existing.value(key)));
        if (efx == nullptr)
        {
            efx = new EFX(m_doc);
            efx->setName(ENGINE_EFX_PREFIX + key);
            efx->setVisible(false);
            if (m_doc->addFunction(efx) == false)
            {
                delete efx;
                continue;
            }
        }
        m_sweepFunc.insert(key, efx->id());
        if (efx->isRunning())
            continue;                       // the heads are changed at rest only

        // The heads it drives: every pan/tilt head of every fixture in the
        // group, IN DMX ADDRESS ORDER - which is the order they hang in.
        //
        // The order is not cosmetic: drawSweep gives head i a start offset of
        // fan x i, so i IS the position in the row. g.fixtures is whatever
        // order Doc lists the fixtures in (by id), and on this rig that is
        // not the physical order - the laser bars came out 319, 300, 338,
        // 357, 376 and the wash 1, 59, 132, 96, 23, 41, 179. Every "wave
        // across the row" this engine has ever drawn was therefore a
        // scramble. Found 2026-09-20 while checking the wave Tobias asked
        // for: "det skal vaere et offset ift. den laser der er ved siden af
        // ... saa de 'foelger' hinanden."
        QList<QPair<quint32, int> > want;
        QList<quint32> ordered = g.fixtures;
        std::sort(ordered.begin(), ordered.end(), [this](quint32 a, quint32 b) {
            Fixture *fa = m_doc->fixture(a);
            Fixture *fb = m_doc->fixture(b);
            // universe first, as ensureTable() sorts g.fixtures (runde 176):
            // by address alone a row over two universes came out interleaved
            quint32 ua = fa != nullptr ? fa->universe() : 0;
            quint32 ub = fb != nullptr ? fb->universe() : 0;
            if (ua != ub)
                return ua < ub;
            quint32 aa = fa != nullptr ? fa->address() : 0;
            quint32 ab = fb != nullptr ? fb->address() : 0;
            return aa != ab ? aa < ab : a < b;
        });
        foreach (quint32 fid, ordered)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            for (int hd = 0; hd < fxi->heads(); hd++)
            {
                if (fxi->channelNumber(QLCChannel::Pan, QLCChannel::MSB, hd) == QLCChannel::invalid()
                    && fxi->channelNumber(QLCChannel::Tilt, QLCChannel::MSB, hd) == QLCChannel::invalid())
                    continue;
                want.append(qMakePair(fid, hd));
            }
        }
        QList<QPair<quint32, int> > have;
        foreach (EFXFixture *ef, efx->fixtures())
            have.append(qMakePair(ef->head().fxi, ef->head().head));
        if (have != want)
        {
            QList<EFXFixture *> olds = efx->fixtures();
            foreach (EFXFixture *ef, olds)
            {
                efx->removeFixture(ef);
                delete ef;
            }
            for (int i = 0; i < want.count(); i++)
                efx->addFixture(want.at(i).first, want.at(i).second);
        }
        efx->setIsRelative(true);
        // not under a running figure (runde 219): a table rebuild put a bar
        // figure's centre back on the aim, and one drawn up-only dipped its
        // whole height under it until the next beat. applySweep() sets both
        // at every start.
        if (efx->isRunning() == false)
        {
            efx->setXOffset(127);
            efx->setYOffset(127);
        }
        efx->setFadeInSpeed(0);
        efx->setFadeOutSpeed(0);
    }
}

bool TrackEngine::userAllowed(const TrackFuncInfo &info, const QString &group) const
{
    // In FULL AUTO the user's functions step aside wherever the engine can
    // make its own: only pattern devices keep theirs, and laser positions
    // stay in the user's hands - a generated tilt is not a safe tilt.
    if (m_fullAuto == false || info.generated || info.role == ENGINE_ROLE_IDLE)
        return true;
    // ... but the "AUTO Programs" folder is not the user's work. It is built
    // by gen_programs.py from this rig's own fixtures, one group per
    // programme, tagged with tier, colour and stars in the engine's own
    // language, and every step locked to the beat. Tobias asked for it in so
    // many words - 2026-09-15, "saa skal du bygge mange flere programmer til
    // FULL-AUTO" - and then it was shut out of full auto by the very rule
    // that keeps hand-made chases from hijacking the engine. 2367 programmes
    // that the only mode this room runs in could not reach (found 2026-09-16,
    // after Tobias said "vi kommer nok aldrig til at bruge track uden
    // full-auto"). The engine's own built-in figures still run: these join
    // the draw, they do not replace it.
    if (info.path.startsWith(ENGINE_AUTO_PATH))
        return true;
    if (info.groups.isEmpty())
        return true;

    // The group we are picking FOR decides. This used to walk every group the
    // function touches and allow it as soon as one of them could not be
    // generated for - so a chase tagged with the heads and the strobes stayed
    // allowed on the heads, became their motion, and the engine's own figures
    // never ran at all. A whole night of one chase.
    if (group.isEmpty() == false)
    {
        const TrackGroup &tg = m_groups.value(group);
        if (tg.generatable() == false)
            return true;
        if (info.role == ENGINE_ROLE_POSITION && tg.lasers)
            return true;
        return false;
    }

    // no group asked for: allowed if it is allowed anywhere
    foreach (const QString &g, info.groups)
    {
        const TrackGroup &tg = m_groups.value(g);
        if (tg.generatable() == false)
            return true;
        if (info.role == ENGINE_ROLE_POSITION && tg.lasers)
            return true;
    }
    return false;
}

void TrackEngine::genFlash(bool on, const QString &colour)
{
    // The flash without a scene: the strobe groups take every part to full,
    // whatever the cast is doing; off again, the parts of groups outside the
    // cast are cut hard, the rest fall back on the next beat.
    // It used to be white every time. White is punctuation - it belongs on
    // the downbeat of a drop, not on every accent all night.
    QString hue = colour.isEmpty() ? QStringLiteral("white") : colour;
    if (on)
    {
        foreach (const QString &key, m_groupOrder)
        {
            const TrackGroup &g = m_groups.value(key);
            if (g.strobes == false || g.generatable() == false || m_groupOff.contains(key))
                continue;
            // An automatic hit (a colour is given) flashes only strobes that
            // are ON STAGE. Every drop landing lit the whole bank at full from
            // 30 % on the fader - where the strobes are kept out of the cast
            // until ENGINE_STROBE_ON - and a group out of the cast has no
            // pulse, so the flash stood static for the beat. Tobias: "de skal
            // heller ikke lyse overhovedet foer energien er der hvor der er
            // dansegulv" (runde 171). The FLASH button (no colour) still takes
            // every strobe: that is the operator's hand.
            if (colour.isEmpty() == false && m_cast.contains(key) == false)
                continue;
            // full means full: no pulse, no breath, and on the button no
            // MASTER and no group trim. slotScale() and setPart() both read
            // m_flashHeld, so the mark goes in BEFORE the colour scene is run
            // and the dimmer driven - after it, run() below asked slotScale()
            // with the group not yet held and the white came out at MASTER x
            // trim: dark at MASTER 0 with the dimmers at full (runde 201). It
            // stays, so moving a fader mid-flash does not pull it down either.
            m_flashHeld.insert(key);
            quint32 fid = colourFunction(key, hue);
            if (fid == Function::invalidId())
                fid = colourFunction(key, QStringLiteral("white"));
            if (fid != Function::invalidId())
            {
                // Colour channels are HTP, so a red accent over a running
                // blue base would mix to magenta. The colour scene steps
                // aside for the flash and comes back on the next beat.
                // White too, since runde 221+2: "255,255,255 wins every
                // channel" stopped being true when strobe white became the
                // white lamp or RGB at 178 (09-22) - the white hit came out
                // lavender in a blue room, pink in a red one (119 of 172
                // white flash beats on 09-20 had the room colour under them).
                stopSlot("col:" + key, true);
                run("flash:" + key, fid, 1.0, 0, true);
            }
            qreal keepDepth = m_pulseDepth.value(key, 0.0);
            int keepBreath = m_breathe.value(key, 0);
            m_pulseDepth.insert(key, 0.0);
            m_breathe.insert(key, 0);
            // an automatic hit (a colour is given) follows the slider (runde 292)
            // full: the button, and since runde 344 the only automatic hit
            // left - the drop's landing - "100% paa droppet" (it followed the
            // slider from 0.70, runde 292)
            setDimmer(key, 1.0);
            m_pulseDepth.insert(key, keepDepth);
            m_breathe.insert(key, keepBreath);
        }
    }
    else
    {
        foreach (const QString &key, m_flashHeld)
        {
            stopSlot("flash:" + key, true);
            if (m_cast.contains(key) == false)
            {
                const TrackGroup &g = m_groups.value(key);
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
            }
        }
        m_flashHeld.clear();
    }
}

void TrackEngine::ensureColourScenes()
{
    // A palette colour a group has no scene of is made from its fixtures'
    // colour channels: hidden, only the RGB(W) values, so it obeys the
    // group's parts like any other colour. "The same colour on every lamp"
    // must not fail on a missing scene.
    struct Swatch { const char *name; int r, g, b, w; };
    static const Swatch table[] = {
        { "red", 255, 0, 0, 0 },       { "green", 0, 255, 0, 0 },     { "blue", 0, 0, 255, 0 },
        // cyan and magenta at 75 %: two channels read brighter than one on
        // RGB LEDs (Tobias, 2026-09-26: "meget lyse ... skrues en smule ned")
        { "cyan", 0, 192, 192, 0 },    { "magenta", 192, 0, 192, 0 }, { "yellow", 255, 255, 0, 0 },
        { "white", 255, 255, 255, 255 }, { "orange", 255, 90, 0, 0 },  { "pink", 255, 60, 120, 0 },
        { "purple", 140, 0, 255, 0 },  { "amber", 255, 160, 0, 0 },   { "uv", 90, 0, 255, 0 } };

    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_COLOUR_PREFIX))
            existing.insert(func->name().mid(ENGINE_COLOUR_PREFIX.length()), func->id());
    }

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.patternDevice)
            continue;

        // THE WHITE LAMP, LEARNED FROM THE OPERATOR'S OWN WHITE SCENES.
        //
        // Three of this rig's six strobes have a white lamp and three do not
        // (Tobias, 2026-09-22), and the fixture definitions for both models
        // are not in this tree - so neither the White tag nor the channel
        // name can be checked here. What CAN be checked is what the
        // operator's own white scenes drive, and he confirmed those are the
        // ones that have always blinked correctly: "de 3 lamper med hvid
        // kanal virkede allerede (blinkede) foer, saa hvad end der hele
        // tiden har virket som kanal er det rigtige."
        //
        // The test is: a channel that ONLY the white scenes touch, and that
        // is neither the dimmer nor one of this fixture's red, green or
        // blue. "Only white" is what makes it a lamp rather than part of the
        // look - a pan or a zoom is carried by every colour's scene alike.
        //
        // The first version of this left out "only white" and simply took
        // everything a white scene wrote that was not the dimmer or RGB.
        // Simulated against the show file before it went anywhere, that
        // learned PAN, TILT, STROBE and ZOOM on the seven wash heads as
        // "white lamps" and would have driven them to 70 % on every white.
        // With the rule as it stands the simulation returns exactly three
        // channels in the whole rig: channel 8 on the three 8+8 strobes.
        //
        // This is NOT g.colourValue - that only learns a channel when at
        // least two different COLOURS write it differently, and channel 8 is
        // written by the white scenes and by nothing else. It would always
        // come back empty; I checked before building on it.
        QHash<quint32, QSet<quint32> > whiteCh, otherCh;
        for (QHash<quint32, TrackFuncInfo>::const_iterator wf = m_funcs.constBegin();
             wf != m_funcs.constEnd(); ++wf)
        {
            if (wf.value().generated || wf.value().colour.isEmpty()
                || wf.value().groups.contains(key) == false)
                continue;
            Scene *ws = qobject_cast<Scene *>(m_doc->function(wf.key()));
            if (ws == nullptr)
                continue;
            bool isWhiteScene = wf.value().colour == QStringLiteral("white");
            foreach (const SceneValue &sv, ws->values())
            {
                if (sv.value == 0 || g.fixtures.contains(sv.fxi) == false)
                    continue;
                if (isWhiteScene)
                    whiteCh[sv.fxi].insert(sv.channel);
                else
                    otherCh[sv.fxi].insert(sv.channel);
            }
        }
        QHash<quint32, QList<quint32> > whiteLamps;
        for (QHash<quint32, QSet<quint32> >::const_iterator wc = whiteCh.constBegin();
             wc != whiteCh.constEnd(); ++wc)
        {
            Fixture *wfx = m_doc->fixture(wc.key());
            if (wfx == nullptr)
                continue;
            quint32 wdim = dimmerChannel(wfx);
            foreach (quint32 ch, wc.value())
            {
                if (ch == wdim || otherCh.value(wc.key()).contains(ch))
                    continue;
                const QLCChannel *wch = wfx->channel(ch);
                if (wch == nullptr || wch->colour() == QLCChannel::Red
                    || wch->colour() == QLCChannel::Green || wch->colour() == QLCChannel::Blue)
                    continue;
                // and never something that MOVES or focuses the fixture. The
                // rule above already leaves only channel 8 on this rig, but
                // the failure this guard prevents is a head swinging on every
                // white, and it costs one comparison.
                if (wch->group() == QLCChannel::Pan || wch->group() == QLCChannel::Tilt
                    || wch->group() == QLCChannel::Speed || wch->group() == QLCChannel::Beam)
                    continue;
                whiteLamps[wc.key()].append(ch);
            }
        }

        QStringList wanted = m_palette;
        if (wanted.contains("white") == false)
            wanted.append("white");                 // the flash
        foreach (const QString &colour, wanted)
        {
            bool have = false;
            for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
                if (it.value().role == ENGINE_ROLE_COLOR && it.value().colour == colour && it.value().generated == false
                    && it.value().groups.count() == 1 && it.value().groups.contains(key))
                    have = true;
            // a gap is always filled; in FULL AUTO every colour is ours
            if (have && m_fullAuto == false)
                continue;

            const Swatch *sw = nullptr;
            for (uint i = 0; i < sizeof(table) / sizeof(table[0]); i++)
                if (colour == QLatin1String(table[i].name))
                    sw = &table[i];

            QList<SceneValue> values;
            int touched = 0;
            foreach (quint32 fid, g.fixtures)
            {
                Fixture *fxi = m_doc->fixture(fid);
                if (fxi == nullptr)
                    continue;
                // EVERY cell: a four-cell bar is one fixture with four reds,
                // and writing the first of them lit one pixel of the bar
                QList<quint32> rcs, gcs, bcs, wcs;
                for (quint32 i = 0; i < fxi->channels(); i++)
                {
                    const QLCChannel *qch = fxi->channel(i);
                    if (qch == nullptr || qch->group() != QLCChannel::Intensity)
                        continue;
                    if (qch->colour() == QLCChannel::Red)   rcs.append(i);
                    if (qch->colour() == QLCChannel::Green) gcs.append(i);
                    if (qch->colour() == QLCChannel::Blue)  bcs.append(i);
                    if (qch->colour() == QLCChannel::White) wcs.append(i);
                }

                bool coloured = false;
                bool isWhite = colour == QStringLiteral("white");

                // THE FIXTURE'S WHITE LAMP (Tobias, 2026-09-22: "3 af
                // stroberne har altsaa en hvid lampe. Naar de er hvide, skal
                // de 3 bruge den hvide lampe, hvor resten skal bruge RGB
                // kanalerne for hvid").
                //
                // Measured on the show file: the three 8+8 strobes' white
                // scenes write red, green, blue AND channel 8; the three
                // 80-segment ones write red, green and blue only. Channel 8
                // is the lamp, and those three should use it alone.
                //
                // Three sources, in order of how much they prove: the
                // definition's own White tag, what the operator's white
                // scenes drive (whiteLamps, learned above the colour loop -
                // this is the one that finds channel 8 on the 8+8 strobes),
                // and last the channel's NAME, the same trick this file
                // already uses on the shutter channels.
                QList<quint32> whiteLamp = wcs;
                foreach (quint32 c, whiteLamps.value(fid))
                {
                    if (rcs.contains(c) == false && gcs.contains(c) == false
                        && bcs.contains(c) == false && whiteLamp.contains(c) == false)
                        whiteLamp.append(c);
                }
                if (isWhite)
                {
                    for (quint32 i = 0; i < fxi->channels(); i++)
                    {
                        const QLCChannel *qch = fxi->channel(i);
                        if (qch == nullptr || qch->group() != QLCChannel::Intensity)
                            continue;
                        if (qch->name().contains(QStringLiteral("white"), Qt::CaseInsensitive) == false)
                            continue;
                        if (rcs.contains(i) || gcs.contains(i) || bcs.contains(i)
                            || whiteLamp.contains(i))
                            continue;
                        whiteLamp.append(i);
                    }
                }

                if (isWhite && whiteLamp.isEmpty() == false)
                {
                    // A real white channel: use it alone. R+G+B on top only
                    // makes it colder and dirtier.
                    // And on a strobe it is capped: those three lamps at a
                    // full white channel are painful to stand in front of.
                    foreach (quint32 c, whiteLamp) values.append(SceneValue(fid, c, uchar(g.strobes ? ENGINE_STROBE_WHITE : 255)));
                    foreach (quint32 c, rcs) values.append(SceneValue(fid, c, uchar(0)));
                    foreach (quint32 c, gcs) values.append(SceneValue(fid, c, uchar(0)));
                    foreach (quint32 c, bcs) values.append(SceneValue(fid, c, uchar(0)));
                    coloured = true;
                }
                else if (isWhite && g.colourValue.contains(fid) == false
                         && (rcs.isEmpty() || gcs.isEmpty() || bcs.isEmpty()))
                {
                    // Nothing to make a white out of: no white channel, no
                    // full red-green-blue, and nothing learned from a scene
                    // of the operator's. This fixture sits the white out.
                    //
                    // Until 2026-09-22 this branch also caught the fixtures
                    // that DO have red, green and blue - on the reasoning
                    // that faking a white from them is colder and dirtier
                    // than a real white channel. True for a wash; wrong for
                    // a strobe, which is where white belongs. Both strobe
                    // models in this rig are RGB-only, so every white they
                    // were ever asked for - including the manual FLASH -
                    // simply did not reach them. Tobias: "RGB = hvid maa
                    // gerne findes til strob/hurtige-blink ... flash maa
                    // gerne ramme alle RGB paa de strobelamper der ikke har
                    // den hvide kanal."
                    continue;
                }
                else if (sw != nullptr && rcs.isEmpty() == false && gcs.isEmpty() == false && bcs.isEmpty() == false)
                {
                    // A white made from red, green and blue is capped on a
                    // strobe exactly as a real white channel is - 70 %. The
                    // other colours are not touched: the cap is about how
                    // hard a white strobe hits, not about the group's level,
                    // which is the trim's job.
                    int vr = sw->r, vg = sw->g, vb = sw->b, vw = sw->w;
                    if (isWhite && g.strobes)
                    {
                        if (vr > ENGINE_STROBE_WHITE) vr = ENGINE_STROBE_WHITE;
                        if (vg > ENGINE_STROBE_WHITE) vg = ENGINE_STROBE_WHITE;
                        if (vb > ENGINE_STROBE_WHITE) vb = ENGINE_STROBE_WHITE;
                        if (vw > ENGINE_STROBE_WHITE) vw = ENGINE_STROBE_WHITE;
                    }
                    foreach (quint32 c, rcs) values.append(SceneValue(fid, c, uchar(vr)));
                    foreach (quint32 c, gcs) values.append(SceneValue(fid, c, uchar(vg)));
                    foreach (quint32 c, bcs) values.append(SceneValue(fid, c, uchar(vb)));
                    foreach (quint32 c, wcs) values.append(SceneValue(fid, c, uchar(vw)));
                    coloured = true;
                }
                else if (g.colourValue.contains(fid))
                {
                    // the values the user's own scene of this colour taught us
                    const QMap<quint32, QMap<QString, uchar> > &chans = g.colourValue.value(fid);
                    for (QMap<quint32, QMap<QString, uchar> >::const_iterator cit = chans.constBegin(); cit != chans.constEnd(); ++cit)
                    {
                        if (cit.value().contains(colour) == false)
                            continue;
                        values.append(SceneValue(fid, cit.key(), cit.value().value(colour)));
                        coloured = true;
                    }
                }
                if (coloured == false)
                    continue;

                // the fixture's base: what every colour scene of it sets alike
                const QMap<quint32, uchar> base = g.baseValue.value(fid);
                for (QMap<quint32, uchar>::const_iterator bit = base.constBegin(); bit != base.constEnd(); ++bit)
                    values.append(SceneValue(fid, bit.key(), bit.value()));

                // ... AND ON EVERY OTHER COLOUR THE WHITE LAMP GOES OUT.
                //
                // The white branch above drives the learned white channel to
                // 70 % and takes red, green and blue down to nought. Nothing
                // did the reverse: DMX is LTP, so once channel 8 on the three
                // 8+8 strobes had been lifted - by that white scene, or by one
                // of the operator's own white flashes, which put 180 to 255 on
                // it ("Flash Strobes WHITE", "Strob 5 white 100 %", "Strob 3
                // white 80 %") - it stayed there. From the first white of the
                // night those three strobes blinked WHITE under every colour
                // the room asked for, while the three 80-segment ones blinked
                // in the colour. Six lamps in a row, three of them wrong.
                //
                // Found 2026-09-22 (runde 161) by asking, for every lamp on
                // the rig, which of its channels anything ever writes: channel
                // 8 on the 8+8 strobes was written by five hand scenes and by
                // the engine's white, and by nothing else, ever.
                //
                // Last, deliberately: scene->setValue takes the last write, so
                // this cannot be undone by the base above.
                // (braces: Qt's foreach expands to nested loops and an if,
                // and an unbraced outer if is -Wdangling-else on MinGW, which
                // is an error in CI. verify_track_r126 caught this one.)
                if (isWhite == false)
                {
                    foreach (quint32 c, whiteLamp)
                        values.append(SceneValue(fid, c, uchar(0)));
                }
                // The fixture's own effect engine, at 0. A laser bar has
                // "Effect", "Effect Speed", "Movement Effect" and "Movement
                // Effect Speed" channels; put a value on one and the bar runs
                // its own built-in pattern at its own pace and stops obeying
                // the colour and dimmer channels. One press of a Virtual
                // Console button that sets them - LaserWiggle writes 146 to
                // Movement Effect - and every look the engine sends afterwards
                // lands on a fixture that is not listening. The colour scene
                // is what runs on every group in the cast every section, so
                // holding the effect channels at zero here is what takes the
                // bars back. (Tobias, 2026-09-15: "de bevaeger sig op og ned
                // paa ALLE programmerne ... derudover taender de fortsat
                // ikke" - one cause, both symptoms.)
                if (g.lasers)
                {
                    static const QRegularExpression fxRx(QStringLiteral("effect"),
                                                         QRegularExpression::CaseInsensitiveOption);
                    for (quint32 i = 0; i < fxi->channels(); i++)
                    {
                        const QLCChannel *qch = fxi->channel(i);
                        if (qch == nullptr || base.contains(i) || qch->name().contains(fxRx) == false)
                            continue;
                        bool written = false;
                        foreach (const SceneValue &sv, values)
                            if (sv.fxi == fid && sv.channel == i)
                                written = true;
                        if (written == false)
                            values.append(SceneValue(fid, i, uchar(0)));
                    }
                }
                // The per-eye channels, at 0. On a laser bar an eye is lit by
                // putting a COLOUR VALUE on its own channel, the master dimmer
                // dims all eight together, and nothing turns an eye off but a
                // 0 written back to it (Tobias, 2026-09-09). The operator's own
                // colour scenes write channel 4 alone (checked in PSMAIN: seven
                // of them, eyes-set=0), so the colour scenes learned from them
                // did the same - and when an eye chase on mot: stopped, its
                // last step's eye stayed burning under whatever colour came
                // next. A running colour scene that holds the eyes at 0 takes
                // them back the moment the chase lets go; while the chase runs
                // it started later and wins the LTP, exactly as it already
                // does for channel 4.
                if (g.perEye)
                {
                    static const QRegularExpression eyeRx(QStringLiteral("(colou?r|eye)\\s*\\d+"),
                                                          QRegularExpression::CaseInsensitiveOption);
                    for (quint32 i = 0; i < fxi->channels(); i++)
                    {
                        const QLCChannel *qch = fxi->channel(i);
                        if (qch == nullptr || base.contains(i) || qch->name().contains(eyeRx) == false)
                            continue;
                        bool written = false;
                        foreach (const SceneValue &sv, values)
                            if (sv.fxi == fid && sv.channel == i)
                                written = true;
                        if (written == false)
                            values.append(SceneValue(fid, i, uchar(0)));
                    }
                }
                // and an open shutter, when the definition says which value that is
                for (quint32 i = 0; i < fxi->channels(); i++)
                {
                    const QLCChannel *qch = fxi->channel(i);
                    if (qch == nullptr || qch->group() != QLCChannel::Shutter || base.contains(i))
                        continue;
                    bool open = false;
                    foreach (QLCCapability *cap, qch->capabilities())
                    {
                        if (cap != nullptr && cap->preset() == QLCCapability::ShutterOpen)
                        {
                            values.append(SceneValue(fid, i, uchar(cap->min())));
                            open = true;
                            break;
                        }
                    }
                    // Runde 219: a channel that only names the "Strobe slow to
                    // fast" preset (the Yuer strobes and the 8 eyes laser) has
                    // no ShutterOpen, so nothing wrote it back after a burst:
                    // stopSlot() lets go of the str: scene, the channel is not
                    // an intensity channel, and it held 162-209 - the lamps
                    // strobed under 97 % of the lit beats of 09-20 ("meget
                    // hurtige"). 0 is "no strobe": every one of Tobias' own
                    // scenes writes 0 there. Fast-to-slow is left alone - its 0
                    // could be the fastest.
                    if (open == false && qch->preset() == QLCChannel::ShutterStrobeSlowFast)
                        values.append(SceneValue(fid, i, uchar(0)));
                }
                touched++;
            }
            if (touched == 0)
                continue;

            QString name = QString("%1 %2").arg(key).arg(colour);
            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                foreach (SceneValue old, scene->values())
                    scene->unsetValue(old.fxi, old.channel);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
            }
            else
            {
                scene = new Scene(m_doc);
                scene->setName(ENGINE_COLOUR_PREFIX + name);
                scene->setVisible(false);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
                if (m_doc->addFunction(scene) == false)
                {
                    delete scene;
                    continue;
                }
            }

            TrackFuncInfo info;
            info.id = scene->id();
            info.name = scene->name();
            info.scatter = nameScatter(info.name);
            info.type = int(Function::SceneType);
            info.role = ENGINE_ROLE_COLOR;
            info.guess = ENGINE_ROLE_COLOR;
            info.groups.insert(key);
            info.colour = colour;
            info.generated = true;
            info.stars = 1;
            info.starsGuess = 1;
            info.fixtureCount = touched;
            m_funcs.insert(info.id, info);
        }
    }
}

void TrackEngine::ensureStrobeScenes()
{
    // Every fixture in this rig carries a shutter channel that strobes in
    // hardware - which is a different animal from blinking the dimmer, and
    // it is the thing that was missing. One hidden scene per group per rate;
    // it sets the shutter and NOTHING else, so the colour and the level
    // underneath still decide what the strobe looks like.
    // Where in the shutter channel's strobe band these sit. NOT the whole
    // band: an LED at the top of it flickers so fast it reads as a slightly
    // dim steady light rather than a strobe, and the bottom is a slow
    // heartbeat that fights the beat. Everything usable is in the upper
    // middle, so all six rates live between 68 % and 90 % and the engine
    // picks among them rather than climbing to the ceiling.
    static const qreal rates[] = { 0.68, 0.72, 0.77, 0.81, 0.86, 0.90 };
    const int rateCount = int(sizeof(rates) / sizeof(rates[0]));
    Q_ASSERT(rateCount == ENGINE_STROBE_RATES);

    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_STROBE_PREFIX))
            existing.insert(func->name().mid(ENGINE_STROBE_PREFIX.length()), func->id());
    }

    m_strobeScenes.clear();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        QList<quint32> ids;
        for (int r = 0; r < rateCount; r++)
        {
            QList<SceneValue> values;
            foreach (quint32 fid, g.fixtures)
            {
                Fixture *fxi = m_doc->fixture(fid);
                if (fxi == nullptr)
                    continue;
                for (quint32 i = 0; i < fxi->channels(); i++)
                {
                    const QLCChannel *qch = fxi->channel(i);
                    if (qch == nullptr || qch->group() != QLCChannel::Shutter)
                        continue;
                    // the white strobe channel is left alone on purpose: it
                    // is the one that made everything read white
                    if (qch->name().contains(QStringLiteral("white"), Qt::CaseInsensitive))
                        continue;
                    int lo = -1, hi = -1;
                    bool invert = false;
                    // The preset is the honest answer; the name is a guess.
                    // Matching "strob" in the text also matches "No strobe"
                    // and "Strobe off", and a definition that wrote the widest
                    // band as the OFF band would then have set the shutter to
                    // not strobing.
                    foreach (QLCCapability *cap, qch->capabilities())
                    {
                        if (cap == nullptr)
                            continue;
                        int p = int(cap->preset());
                        bool slowFast = p == int(QLCCapability::StrobeSlowToFast)
                                     || p == int(QLCCapability::StrobeFreqRange)
                                     || p == int(QLCCapability::StrobeFrequency);
                        bool fastSlow = p == int(QLCCapability::StrobeFastToSlow);
                        if (slowFast == false && fastSlow == false)
                            continue;
                        if (int(cap->max()) - int(cap->min()) < hi - lo)
                            continue;               // the widest strobe band wins
                        lo = int(cap->min());
                        hi = int(cap->max());
                        invert = fastSlow;
                    }
                    if (lo < 0 && qch->preset() == QLCChannel::ShutterStrobeSlowFast)
                    {
                        lo = 16;                    // a definition that only names the preset
                        hi = 230;
                    }
                    else if (lo < 0 && qch->preset() == QLCChannel::ShutterStrobeFastSlow)
                    {
                        lo = 16;
                        hi = 230;
                        invert = true;
                    }
                    if (lo < 0 || hi <= lo)
                        continue;
                    qreal f = invert ? 1.0 - rates[r] : rates[r];
                    int v = lo + int(qRound(qreal(hi - lo) * f));
                    values.append(SceneValue(fid, i, uchar(qBound(0, v, 255))));
                }
            }
            if (values.isEmpty())
                continue;

            QString name = QString("%1 %2").arg(key).arg(r);
            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                scene->setVisible(false);        // in case a hand un-hid it
                foreach (SceneValue old, scene->values())
                    scene->unsetValue(old.fxi, old.channel);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
            }
            else
            {
                scene = new Scene(m_doc);
                scene->setName(ENGINE_STROBE_PREFIX + name);
                scene->setVisible(false);
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
                if (m_doc->addFunction(scene) == false)
                {
                    delete scene;
                    continue;
                }
            }
            while (ids.count() < r)
                ids.append(Function::invalidId());
            ids.append(scene->id());
        }
        if (ids.isEmpty() == false)
            m_strobeScenes.insert(key, ids);
    }
}

void TrackEngine::ensureOffScenes()
{
    // Switching a group OFF has to switch the whole group off, not just the
    // parts the engine happens to be driving. An animation laser's light
    // lives on its effect channels, a laser bar's on a colour channel, and a
    // scene of the operator's - or a whole-room look running for a DIFFERENT
    // group - can be holding any of them up. So: one scene per group that
    // sets every channel to zero and blends with Replace, which writes the
    // exact value AFTER the ordinary playback layer.
    // Where this sits in the layer order (GenericFader::playbackOrder):
    //   0  ordinary playback - scenes, chasers, EFX, AND a plain VC slider or
    //      a plain flash button, because those ask for Universe::Auto
    //   1  us, and the per-fixture dimmer scenes
    //   3  a VC slider with Monitor on (Universe::Override)
    //   6  a flash button with Override="1" (Universe::Flashing)
    //   9  Simple Desk
    // So this beats ordinary playback and a PLAIN fader - which is the point
    // while AUTO runs, but it is worth knowing: to busk over a running AUTO a
    // widget has to be at 3 or above. With AUTO off everything here is
    // released within a second and the faders are the operator's again.
    //
    // Pan, tilt and the speed channels are left alone on purpose: an off
    // group should go dark where it stands, not swing to a corner first.
    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_OFF_PREFIX))
            existing.insert(func->name().mid(ENGINE_OFF_PREFIX.length()), func->id());
    }

    m_offScenes.clear();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        QList<SceneValue> values;
        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            for (quint32 i = 0; i < fxi->channels(); i++)
            {
                const QLCChannel *qch = fxi->channel(i);
                if (qch == nullptr)
                    continue;
                if (qch->group() == QLCChannel::Pan || qch->group() == QLCChannel::Tilt
                    || qch->group() == QLCChannel::Speed)
                    continue;
                values.append(SceneValue(fid, i, uchar(0)));
            }
        }
        if (values.isEmpty())
            continue;

        Scene *scene = nullptr;
        if (existing.contains(key))
            scene = qobject_cast<Scene *>(m_doc->function(existing.value(key)));
        if (scene != nullptr)
        {
            scene->setVisible(false);
            foreach (SceneValue old, scene->values())
                scene->unsetValue(old.fxi, old.channel);
            foreach (SceneValue sv, values)
                scene->setValue(sv);
        }
        else
        {
            scene = new Scene(m_doc);
            scene->setName(ENGINE_OFF_PREFIX + key);
            scene->setVisible(false);
            foreach (SceneValue sv, values)
                scene->setValue(sv);
            if (m_doc->addFunction(scene) == false)
            {
                delete scene;
                continue;
            }
        }
        scene->setBlendMode(Universe::ReplaceBlend);
        m_offScenes.insert(key, scene->id());
    }

    // Runde 356: the music's own dark needs a mask of its own. The off mask
    // zeroes every channel but pan, tilt and speed - a moving head's zoom and
    // colour too - and a stab every beat would send those motors to nought
    // and back eight times a second. The intensity override alone did not
    // reach the heads in the harness (their dimmers are held by more than the
    // engine's dim: parts). So: a second scene per group with a dimmer, only
    // its Intensity channels (the dimmer, and the red/green/blue/white a
    // fixture mixes its light from), at zero, Replace. A group without a
    // dimmer (a laser bar's light is its colour channel) takes the off mask.
    // ... and they are FLASHED, not run (setAutoDark): a flash sits on the
    // Flashing layer with Replace and forced LTP, above every playback - in
    // the harness a run mask lost to looks fading out and to steps started
    // after it on the same layer (one stab in five stayed lit). A group
    // without a dimmer gets the off mask's channels in a scene of its own: a
    // flash and a run share one fader map, so the off scene itself must never
    // be flashed.
    foreach (quint32 dfid, m_darkScenes)
    {
        Function *df = m_doc->function(dfid);
        if (df != nullptr && df->flashing())
            df->unFlash(m_doc->masterTimer());
    }
    m_darkScenes.clear();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        QList<SceneValue> values;
        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            // R400_DARK_DIMMER: g.hasDimmer is not known yet here -
            // ensureDimmerScenes() sets it after this, on groups rebuilt from
            // nothing - so every dark mask was the full off mask, and each dark
            // beat sent the heads' zoom to nought and back (harness: 168 -> 0
            // -> 168 in 120 ms, every beat of a break). The fixture's own
            // dimmer decides, by the test that makes it a dimmer part.
            const bool fxDimmer = dimmerChannel(fxi) != QLCChannel::invalid();
            for (quint32 i = 0; i < fxi->channels(); i++)
            {
                const QLCChannel *qch = fxi->channel(i);
                if (qch == nullptr)
                    continue;
                if (fxDimmer ? qch->group() == QLCChannel::Intensity
                             : (qch->group() != QLCChannel::Pan && qch->group() != QLCChannel::Tilt
                                && qch->group() != QLCChannel::Speed))
                    values.append(SceneValue(fid, i, uchar(0)));
            }
        }
        if (values.isEmpty())
            continue;
        const QString dk = key + QStringLiteral(" (dark)");
        Scene *scene = existing.contains(dk) ? qobject_cast<Scene *>(m_doc->function(existing.value(dk))) : nullptr;
        if (scene != nullptr)
        {
            scene->setVisible(false);
            foreach (SceneValue old, scene->values())
                scene->unsetValue(old.fxi, old.channel);
            foreach (SceneValue sv, values)
                scene->setValue(sv);
        }
        else
        {
            scene = new Scene(m_doc);
            scene->setName(ENGINE_OFF_PREFIX + dk);
            scene->setVisible(false);
            foreach (SceneValue sv, values)
                scene->setValue(sv);
            if (m_doc->addFunction(scene) == false)
            {
                delete scene;
                continue;
            }
        }
        scene->setBlendMode(Universe::ReplaceBlend);
        m_darkScenes.insert(key, scene->id());
    }
}

void TrackEngine::applyGroupOff()
{
    // called from the tick and the moment the switch is thrown, so the group
    // goes out under the operator's finger rather than on the next beat
    if (m_doc == nullptr)
        return;
    foreach (const QString &key, m_groupOrder)
    {
        quint32 fid = m_offScenes.value(key, Function::invalidId());

        // BLACKOUT rides the same mask. The intensity attribute only scales
        // channels QLC+ flags as Intensity, so on a laser bar (colour on a
        // colour channel) or an animation laser (light on effect channels)
        // turning the level to zero did nothing at all - the masks are the
        // only thing that actually blacks those out. Driven from here, so a
        // teardown that drops them is corrected on the next beat instead of
        // leaving the rig lit for the rest of the night.
        // ... and so does the closing sequence at nought (runde 213)
        QString black = "black:" + key;
        if ((m_blackout || m_closingDim <= 0.0) && fid != Function::invalidId())
            run(black, fid, 1.0, 0, true);
        else if (m_active.contains(black))
            stopSlot(black, true);

        QString slot = "off:" + key;
        if (m_groupOff.contains(key) && fid != Function::invalidId())
        {
            run(slot, fid, 1.0, 0, true);
            continue;
        }
        if (m_active.contains(slot))
            stopSlot(slot, true);
        // slotDocChanged() empties m_active without stopping anything, so a
        // mask can be running with nobody holding its slot. Switching the
        // group back on would then never turn it off again and the group
        // would stay dark for the rest of the night.
        //
        // ... unless BLACKOUT holds it: "black:" and "off:" run the SAME scene,
        // and without this the mask BLACKOUT had just started was stopped here
        // on the next call, restarted the call after - the laser bars and the
        // animation lasers blinking every other beat through a blackout
        // instead of going dark (runde 168).
        Function *func = fid != Function::invalidId() ? m_doc->function(fid) : nullptr;
        if (func != nullptr && func->isRunning() && m_active.contains(black) == false)
            func->stop(FunctionParent::master());
    }

    // A slot whose group no longer exists - renamed, emptied, or the largest-
    // group rule picked a different name - can never be reached by the loops
    // that would otherwise stop it, because they all walk the CURRENT groups.
    // A hardware strobe or an EFX sweep left like that runs until QLC+ exits.
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("off:"))
        {
            if (m_groupOff.contains(slot.mid(4)) == false)
                stopSlot(slot, true);
            continue;
        }
        static const QStringList owned = { "str:", "efx:", "zoom:", "col:",
                                           "mot:", "pos:", "black:" };
        bool mine = false;
        foreach (const QString &p, owned)
        {
            if (slot.startsWith(p))
                mine = true;
        }
        if (mine && m_groups.contains(slotGroup(slot)) == false)
            stopSlot(slot, true);
    }
}

bool TrackEngine::barsWide(const QString &group) const
{
    // Tobias (runde 346): the bars "strobede ... specielt paa de enkeltoejede
    // chases". The shutter from 90 % (runde 352) is for a look with more than
    // one eye lit: not a bare chase or ping-pong walking a single eye.
    if (m_moves.contains(group) == false)
        return false;
    const TrackMove &mv = m_moves.value(group);
    const bool walk = mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG
                   || mv.pattern == ENGINE_PAT_SPARKLE;
    return (walk && mv.bare && mv.width <= 1) == false;
}

/* ---- runde 360 ---- */
// R360_DROP_SIZE: how far the music jumps where a drop arrives - the
// waveform and the kick over its first eight beats against the eight before
// its last two (the silence and the black beat). In 128 drop arrivals of the
// library the jump runs 0.30-0.74 (tenth to ninetieth), median 0.56, and it
// follows the bass's own jump (r 0.82). 0.30 and under is 0, 0.70 and over 1.
qreal TrackEngine::dropSizeAt(int beat) const
{
    if (m_audLevel.isEmpty() || beat < 12 || beat + 7 > m_audLevel.count())
        return 1.0;
    auto mean = [&](const QVector<quint8> &v, int a, int b) {
        qreal sum = 0.0;
        for (int i = a; i <= b; i++)
            sum += v.at(i - 1) / 255.0;
        return sum / qreal(b - a + 1);
    };
    qreal size = mean(m_audLevel, beat, beat + 7) - mean(m_audLevel, beat - 10, beat - 3);
    if (m_audKick.count() == m_audLevel.count())
        size = 0.5 * size + 0.5 * (mean(m_audKick, beat, beat + 7) - mean(m_audKick, beat - 10, beat - 3));
    return qBound(0.0, (size - 0.30) / 0.40, 1.0);
}

/* ---- runde 359 ---- */
void TrackEngine::setTrackPump(const QVariantList &pumpRaw)
{
    m_audPump.clear();
    if (m_audLevel.isEmpty() == false && pumpRaw.count() == m_audLevel.count())
    {
        for (const QVariant &v : pumpRaw)
            m_audPump.append(quint8(qBound(0, v.toInt(), 255)));
    }
}

// R359_RISER: how far a build has climbed at beat `at` - the highs and the
// waveform over four beats, from the build's start to the riser's own peak
// before `to`, the highest point reached so far (a riser that flattens holds,
// one that dips is not followed down). -1 where the music does not climb at
// all (12 % or more) or BLT sent no curves.
qreal TrackEngine::riserAt(int at, int from, int to) const
{
    if (m_audHigh.isEmpty() || m_audHigh.count() != m_audLevel.count() || to - from < 8)
        return -1.0;
    auto rise = [&](int b) {
        qreal sum = 0.0;
        int n = 0;
        for (int i = b - 3; i <= b; i++)
        {
            if (i >= 1 && i <= m_audHigh.count())
            {
                sum += 0.5 * m_audHigh.at(i - 1) / 255.0 + 0.5 * m_audLevel.at(i - 1) / 255.0;
                n++;
            }
        }
        return n > 0 ? sum / n : 0.0;
    };
    // the top is the riser's own peak (the last bar before a drop is often the
    // quiet one - the silence and the black beat take it)
    const qreal h0 = rise(from + 3);
    qreal h1 = h0, sofar = h0;
    for (int b = from + 3; b < to; b++)
    {
        h1 = qMax(h1, rise(b));
        if (b <= at)
            sofar = qMax(sofar, rise(b));
    }
    if (h1 - h0 >= 0.12)
        return qBound(0.0, (sofar - h0) / (h1 - h0), 1.0);
    return -1.0;
}

/* ---- runde 358 ---- */
void TrackEngine::setTrackRhythm(const QVariantList &midRaw, const QVariantList &onsetHigh)
{
    m_audMid.clear();
    m_onsetHigh.clear();
    // beat for beat with the waveform, or not at all
    if (m_audLevel.isEmpty() == false && midRaw.count() == m_audLevel.count())
    {
        for (const QVariant &v : midRaw)
            m_audMid.append(quint8(qBound(0, v.toInt(), 255)));
    }
    if (m_audLevel.isEmpty() == false && onsetHigh.count() == m_audLevel.count())
    {
        for (const QVariant &v : onsetHigh)
            m_onsetHigh.append(quint16(qBound(0, v.toInt(), 65535)));
    }
}

int TrackEngine::onsetHigh(int beat, int quarter) const
{
    if (beat < 1 || beat > m_onsetHigh.count() || quarter < 0 || quarter > 3)
        return -1;
    return (m_onsetHigh.at(beat - 1) >> (4 * quarter)) & 0xf;
}

qreal TrackEngine::audMid(int beat) const
{
    if (beat < 1 || beat > m_audMid.count())
        return -1.0;
    return m_audMid.at(beat - 1) / 255.0;
}

void TrackEngine::newTrackMemory(const QString &title)
{
    clearDjLoop();
    m_landOwed = true;                   // runde 360: it may come in inside its drop
    m_miniLandUntil = -1;                // runde 362
    m_miniLandLast = -100;
    m_nextDropSize = 1.0;
    m_dropGrow = false;
    m_dropGrowAt = -1;
    m_dropGrowLine = -1;                 // runde 367
    m_dropSize = 1.0;
    if (title != m_trackTitle)          // the same track sent again keeps what its drops looked like
    {
        m_lookMemory.clear();
        m_lookRepeat = 0;
    }
    m_lookPending.clear();
    m_lookActive.clear();
    m_reuseColour.clear();
    m_reuseStyle = -1;
    m_reuseMotion.clear();
    m_halfTime = false;
}

void TrackEngine::clearDjLoop()
{
    m_loopFrom = m_loopTo = -1;
    m_loopPasses = 0;
    m_loopLen = 0;
    m_loopJumpMs = -1;
}

// what a section sounds like over its first beats: the waveform, the kick,
// the highs and the mids (whatever BLT sent), 0-1 each
QVector<qreal> TrackEngine::soundSig(int from, int beats) const
{
    QVector<qreal> out;
    auto mean = [&](const QVector<quint8> &v) {
        qreal sum = 0.0;
        int n = 0;
        for (int b = from; b < from + beats; b++)
        {
            if (b >= 1 && b <= v.count())
            {
                sum += v.at(b - 1) / 255.0;
                n++;
            }
        }
        return n > 0 ? sum / n : -1.0;
    };
    out << mean(m_audLevel) << mean(m_audKick) << mean(m_audHigh) << mean(m_audMid);
    return out;
}

// R358_BRAKE. The tempo the deck has been playing at is the reference - a
// pitch ride moves it with the deck, a brake falls away from it inside a
// second. Under 85 % of it the room goes down with the deck (brakeDim:
// dark at a quarter of the speed); back over 90 % and it is the room again.
void TrackEngine::setDeckTempo(qreal bpm)
{
    if (bpm <= 20.0 || bpm >= 400.0)
        return;
    if (m_tempoRef <= 0.0)
        m_tempoRef = bpm;
    const qreal ratio = bpm / m_tempoRef;
    if (ratio >= 0.90 && ratio <= 1.10)
        m_tempoRef = 0.8 * m_tempoRef + 0.2 * bpm;
    qreal speed = ratio >= 0.90 ? 1.0 : qBound(0.0, ratio, 1.0);
    // a brake stops the deck inside two seconds; a tempo that stays down that
    // long is the deck's new tempo (SYNC to a slower master, a pitch pulled
    // hard), and the room comes back on it
    const qint64 nowMs = m_clock.elapsed();
    if (speed < 0.90)
    {
        if (m_brakeMs < 0)
            m_brakeMs = nowMs;
        else if (nowMs - m_brakeMs > 2000)
        {
            m_tempoRef = bpm;
            speed = 1.0;
            m_brakeMs = -1;
        }
    }
    else
        m_brakeMs = -1;
    if (qAbs(speed - m_deckSpeed) >= 0.02)
    {
        const bool was = m_deckSpeed < 0.85;
        m_deckSpeed = speed;
        if (was != (m_deckSpeed < 0.85))
            qDebug() << "[TrackEngine] brake" << (m_deckSpeed < 0.85 ? "on" : "off") << bpm << m_tempoRef;
        reapplyLevels();
    }
}

/* ---- runde 357 ---- */
void TrackEngine::setTrackFills(const QVariantList &fills)
{
    m_fills.clear();
    for (const QVariant &v : fills)
    {
        const QVariantList p = v.toList();
        if (p.count() >= 2 && p.at(0).toInt() > 0 && p.at(1).toInt() > p.at(0).toInt())
            m_fills.append(qMakePair(p.at(0).toInt(), p.at(1).toInt()));
    }
}

bool TrackEngine::inFill(int beat) const
{
    for (const QPair<int, int> &f : m_fills)
    {
        if (beat >= f.first && beat < f.second)
            return true;
    }
    return false;
}

qreal TrackEngine::colourTaste(const QString &colour) const
{
    if (m_tasteLoaded == false)
    {
        m_tasteLoaded = true;
        const QString raw = QSettings().value(QStringLiteral("trackengine/colour-taste")).toString();
        if (raw.isEmpty())
        {
            // seeded from his own taps on 10-02/03/04: red 48 % of 161 picks
            // against 21 % of the time on stage, and the colours he most often
            // tapped AWAY from (cyan, green) - the square root of picked over
            // shown, so the seed leans and the nights decide
            m_colourTaste = { { "red", 1.50 }, { "magenta", 0.96 }, { "blue", 0.66 },
                              { "cyan", 0.74 }, { "orange", 0.79 }, { "green", 0.75 } };
        }
        else
        {
            foreach (const QString &kv, raw.split(';', Qt::SkipEmptyParts))
            {
                const QStringList p = kv.split(':');
                if (p.count() == 2)
                    m_colourTaste.insert(p.at(0), qBound(0.4, p.at(1).toDouble(), 2.5));
            }
        }
    }
    return m_colourTaste.value(colour, 1.0);
}

void TrackEngine::learnColour(const QString &picked, const QString &shown)
{
    colourTaste(picked);                 // loaded
    m_colourTaste.insert(picked, qMin(2.5, m_colourTaste.value(picked, 1.0) * 1.08));
    // the colour he tapped away from, unless he keeps it in the mix
    if (shown.isEmpty() == false && shown != picked && m_overrideSet.contains(shown) == false
        && shown != QStringLiteral("white"))
        m_colourTaste.insert(shown, qMax(0.4, m_colourTaste.value(shown, 1.0) * 0.96));
    QStringList out;
    for (auto it = m_colourTaste.constBegin(); it != m_colourTaste.constEnd(); ++it)
        out << QString("%1:%2").arg(it.key()).arg(it.value(), 0, 'f', 3);
    out.sort();
    QSettings().setValue(QStringLiteral("trackengine/colour-taste"), out.join(';'));
}

/* ---- runde 356: the music's own dark (R356_MUSIC_DARK) ---- */
void TrackEngine::setTrackAudio(const QVariantList &level, const QVariantList &kickRaw, const QVariantList &highRaw)
{
    m_audLevel.clear();
    m_audKick.clear();
    m_audHigh.clear();
    for (const QVariant &v : level)
        m_audLevel.append(quint8(qBound(0, v.toInt(), 255)));
    // the raw kick and highs count only when they cover the track beat for beat
    if (kickRaw.count() == level.count() && highRaw.count() == level.count())
    {
        for (const QVariant &v : kickRaw)
            m_audKick.append(quint8(qBound(0, v.toInt(), 255)));
        for (const QVariant &v : highRaw)
            m_audHigh.append(quint8(qBound(0, v.toInt(), 255)));
    }
}

qreal TrackEngine::audLevel(int beat) const
{
    if (beat < 1 || beat > m_audLevel.count())
        return 1.0;
    return m_audLevel.at(beat - 1) / 255.0;
}

bool TrackEngine::silentBeat(int beat) const
{
    // under a fifth of the track's own top (BLT's display curve is the level
    // over its 95th percentile), and neither kick nor highs on the beat when
    // BLT measured those - a whispered vocal over a pad is not silence
    if (beat < 1 || beat > m_audLevel.count())
        return false;
    if (m_audLevel.at(beat - 1) >= 51)
        return false;
    if (m_audKick.isEmpty() == false
        && (m_audKick.at(beat - 1) >= 51 || m_audHigh.at(beat - 1) >= 51))
        return false;
    return true;
}

bool TrackEngine::silentish(int beat) const
{
    // a quiet beat (under 30 % of the top) next to a silent one belongs to
    // the silence: a single hat or breath in the gap flipped it back and
    // forth in the harness (Clean Bandit 61-64: lit, dark, lit, dark before
    // the drop; Frank Ocean's 60-beat gap restarted its 8 black beats)
    if (silentBeat(beat))
        return true;
    if (beat < 1 || beat > m_audLevel.count() || m_audLevel.at(beat - 1) >= 77)
        return false;
    return silentBeat(beat - 1) || silentBeat(beat + 1);
}

int TrackEngine::silentRunStart(int beat) const
{
    int b = beat;
    while (b > 1 && silentish(b - 1))
        b--;
    return b;
}

void TrackEngine::setAutoDark(bool on)
{
    if (on == m_autoDark)
        return;
    m_autoDark = on;
    if (m_doc == nullptr)
        return;
    // the dark masks, flashed (see ensureOffScenes); under BLACKOUT the
    // operator's masks already hold everything
    foreach (quint32 dfid, m_darkScenes)
    {
        Function *df = m_doc->function(dfid);
        if (df == nullptr)
            continue;
        if (on && m_blackout == false)
            df->flash(m_doc->masterTimer(), true, true);
        else
            df->unFlash(m_doc->masterTimer());
    }
    reapplyLevels();                     // every intensity, as BLACKOUT does
    foreach (quint32 fid, m_fadeAttr.keys())
    {
        Function *func = m_doc->function(fid);
        if (func != nullptr)
            func->adjustAttribute(lightsOut() ? 0.0 : m_fadeLevel.value(fid, 0.0), m_fadeAttr.value(fid));
    }
}

void TrackEngine::clearMusicDark()
{
    m_landBurstUntil = -1;               // runde 357
    m_deckSpeed = 1.0;                   // runde 358: a stopped or new deck has no brake
    m_tempoRef = -1.0;
    m_brakeMs = -1;
    m_vocalNow = false;
    m_vocalRun = 0;
    m_grooveSlots = 0;
    m_backbeat = false;
    m_pumpNow = -1.0;                    // runde 359
    m_density = 0;
    m_chopPlan.clear();
    m_chopTimer.stop();
    m_silenceDark = false;
    m_musicDarkEvent.clear();
    setAutoDark(false);
}

void TrackEngine::slotChopTimer()
{
    // dark inside a planned stab, or for the whole beat the music is silent;
    // the timer wakes at the next edge. The stab ends on the beat the plan
    // expected - a late beat does not stretch it (the plan's own end), an
    // early one cuts it (tick() plans afresh).
    const qint64 now = m_clock.elapsed();
    bool inStab = false;
    qint64 next = -1;
    for (const QPair<qint64, qint64> &w : std::as_const(m_chopPlan))
    {
        if (now >= w.first && now < w.second)
            inStab = true;
        if (w.first > now && (next < 0 || w.first < next))
            next = w.first;
        if (w.second > now && (next < 0 || w.second < next))
            next = w.second;
    }
    if (m_flash)
        inStab = false;
    setAutoDark(m_silenceDark || inStab);
    if (next > now)
        m_chopTimer.start(int(qMax(qint64(1), next - now)));
    else
        m_chopTimer.stop();
}

void TrackEngine::driveStrobe(const QSet<QString> &cast, int beat, qreal energy, bool isDrop,
                              bool isBuild, qreal prog, int bar, int beatInBar, bool quiet)
{
    const int rateCount = ENGINE_STROBE_RATES;
    // When the hardware strobe comes on, how fast it runs, and who joins in.
    // Blinking a dimmer is a pulse; THIS is a strobe, and it is deliberately
    // rare below three-quarters of the fader and everywhere at the top.
    QRandomGenerator *rng = QRandomGenerator::global();
    auto roll = [rng](qreal p) { return rng->bounded(1000) < int(qBound(0.0, p, 1.0) * 1000.0); };
    qreal e = qBound(0.0, energy, 1.0);
    // 0 a third of the way up, 1 at the stop - and every use of it below is a
    // ramp rather than a threshold, so 55 % and 65 % are different, and so
    // are 90 % and 100 %.
    qreal w = qBound(0.0, (e - 0.33) / 0.67, 1.0);
    // RUNDE 346 (Tobias, after 10-02: "hold de vilde strobs til naar Energien
    // er 100%. 100% er ment til fuldstaendigt amok. Hvor der saa langsomt
    // skalerer baglaens med energi-slideren"). How OFTEN, how LONG and how
    // FAST a burst is now ride on w squared: the same ramp, bent down, so it
    // is still nought at a third and one at the stop, but 70 % gives 0.30
    // (was 0.55) and 85 % 0.60 (was 0.78). The top is unchanged - the
    // bottom half of the strobes' life is calmer, and the last 15 % on the
    // slider is where they go wild. On Friday the bursts ran 4-7 % of the
    // beats at 70-89 % and 14 % above 90 %.
    const qreal w2 = w * w;
    // the stop's own last stretch: nought at 85 % on the slider, one at 100
    const qreal amok = qBound(0.0, (m_faderNow - 0.85) / 0.15, 1.0);

    // The DJ scrubbed backwards. A burst is at most four beats, so any
    // backwards jump can leave an end beat in the future - and then
    // "beat <= m_strobeUntil" stays true for the whole length of the jump and
    // the strobe runs solid. It cannot be checked against m_lastBeat: tick()
    // has already set that to this beat by the time we get here.
    // ... and the budget window goes back with it (runde 265): it counts
    // beats PLAYED, like m_calmUntil. A DJ loop shorter than 64 beats (56-71
    // with the window started at 40) neither reached 64 beats past the start
    // nor went back past it, so the window never restarted - ten beats spent
    // and the budgeted bursts stopped for as long as the loop ran.
    if (m_strobeSeen >= 0 && beat < m_strobeSeen)
    {
        m_strobeUntil = -1;
        if (m_strobeWindow >= 0)
            m_strobeWindow -= m_strobeSeen - beat;
    }
    m_strobeSeen = beat;

    // the budget window: 64 beats, restarted when it runs out or the track
    // scrubs backwards past its start
    if (m_strobeWindow < 0 || beat < m_strobeWindow || beat - m_strobeWindow >= ENGINE_STROBE_WINDOW)
    {
        m_strobeWindow = beat;
        m_strobeSpent = 0;
    }
    int budget = ENGINE_STROBE_BUDGET_LOW
                 + int(qRound(w2 * qreal(ENGINE_STROBE_BUDGET_HIGH - ENGINE_STROBE_BUDGET_LOW)));   // w2: runde 346
    auto affordable = [this, budget](int beats) { return m_strobeSpent + beats <= budget; };

    if (quiet)
    {
        m_strobeUntil = -1;
    }
    else if (beat > m_strobeUntil)
    {
        int want = -1, beats = 2;
        bool landing = false;            // runde 357
        // Which rate is mostly a DRAW, not a function of the energy: every one
        // of them is inside the usable band, so what the energy buys is how
        // often the strobe comes and how long it stays, not how fast it runs.
        // Drawing it means two bursts in a row are never quite the same.
        // The RATE follows the fader too, not only how often: at a third of
        // the fader the draw stays in the slow half of the six rates, at the
        // top it has them all. A slow hardware strobe at low energy is a
        // flicker; the fast one is the club (Tobias, 2026-09-18).
        int drawn = qBound(0, int(qRound(qreal(rng->bounded(rateCount)) * (0.35 + 0.65 * w2))), rateCount - 1);
        // runde 338: at the top of the fader the slow end of the band is a
        // flicker, not the club - the draw starts two rates up from w 1
        // (runde 346: from the last 15 % of the slider, the amok)
        drawn = qMax(drawn, qMin(rateCount - 1, int(qRound(2.0 * amok))));
        // The riser starts earlier in the build the higher the fader is: at a
        // quarter it only arrives in the last eighth, at the top it runs the
        // last third of the build - and there it does climb, because a riser
        // that speeds up is the whole point of a riser.
        // (runde 346: on w2, and the riser's TOP rides on it too - it climbed
        // to the fastest rate at any fader, 60 % as well as the stop: at 80 %
        // it now tops out a rate under, at 60 % at the middle of the band)
        qreal riserFrom = 0.90 - 0.30 * w2;
        if (isBuild && prog > riserFrom && e > 0.18)
        {
            qreal into = qBound(0.0, (prog - riserFrom) / qMax(0.02, 1.0 - riserFrom), 1.0);
            want = int(qRound(into * qreal(rateCount - 1) * (0.45 + 0.55 * w2)
                              * (0.55 + 0.45 * m_nextDropSize)));   // R362_BUILD_SIZE
            beats = 1;
        }
        // R365_LAND_ONCE: only where the drop ARRIVES (or lands late, runde
        // 360) - as the hit (runde 283) and the white landing. A DJ loop over
        // the drop's first bar landed again on every pass (the harness: four
        // `strobe-land` in a 4-beat loop, the strobe lamps over MASTER each
        // time), and so did a second drop flag inside a running drop.
        else if (isDrop && bar == 0 && ((beatInBar == 0 && m_dropArrivedNow) || m_lateLandNow) && m_faderNow >= ENGINE_DROP_SHOW)   // runde 360: or late
        {
            want = drawn;                                // the drop lands
            beats = 1 + int(qRound(3.0 * w2 * (0.40 + 0.60 * m_dropSize)));   // a whole bar at the top (runde 338; w2 runde 346; the drop's size runde 360)
            landing = true;
        }
        // bar >= 0 too: a drop whose kick has not arrived yet is handed a
        // NEGATIVE bar (see m_dropLand in tick). The landing above already
        // waits for it; without this the "and again" bursts fired straight
        // through the delay, which is the fake drop given away by the strobes.
        else if (isDrop && bar >= 0 && w > 0.0 && beatInBar == 0 && roll(0.05 + 0.70 * w2)
                 && affordable(1 + int(qRound(3.0 * w2))))
        {
            want = drawn;                                // and again, more of it
            beats = 1 + int(qRound(3.0 * w2));
        }
        else if (isDrop == false && isBuild == false && w > 0.0
                 && (bar % qMax(2, 10 - int(qRound(8.0 * w)))) == 0
                 && beatInBar == 3 && roll(0.10 + 0.50 * w2) && affordable(1))
        {
            want = drawn;                                // a groove gets a taste
            beats = 1;
        }
        // Runde 313: the strobes are on from 30 % now (ENGINE_STROBE_ON), for
        // their chases and their walk - the HARDWARE strobe keeps the line it
        // has always had in practice, 55 %: under it the strobes were out of
        // the cast, so no burst ever reached them. Tobias asked for the chase
        // pace, not for bursts at a bar's level.
        if (want >= 0 && m_faderNow < ENGINE_HWSTROBE_ON)
            want = -1;
        bool headsOnly = false;
        // THE HEADS' OWN (runde 346, Tobias: "Movingheads kunne godt strobe
        // lidt mere i drops"). Between the drop's bursts, on the first beat of
        // a bar, the heads alone take one beat of their shutter (a bar in five
        // at 60 %, two in three at the top) -
        // slow rates at 60 %, the top of the band at the stop, and more often
        // the higher the slider. Outside the strobe budget: it is the heads'
        // accent, not the strobes'.
        const qreal hh = qBound(0.0, (m_faderNow - 0.60) / 0.40, 1.0);
        if (want < 0 && isDrop && bar >= 0 && beatInBar == 0
            && m_faderNow >= 0.60 && roll(0.20 + 0.45 * hh))
        {
            want = qBound(0, int(qRound(hh * qreal(rateCount - 1))) - int(rng->bounded(2)), rateCount - 1);
            beats = 1;
            headsOnly = true;
        }
        if (want >= 0)
        {
            m_strobeRate = want;
            m_strobeUntil = beat + beats - 1;
            m_strobeHeadsOnly = headsOnly;
            // runde 357: a new burst that is not the landing ends the landing's
            // freedom; the landing sets it
            m_landBurstUntil = landing ? m_strobeUntil : -1;
            m_landBurstFrom = landing ? beat : -1;     // runde 363
            if (landing)
                reapplyLevels();         // the strobes at the landing's own level, this beat
        }
    }

    bool on = beat <= m_strobeUntil;
    // every burst counts, budgeted or not - but only a beat something actually
    // strobed on (runde 267). It counted every burst beat, and a burst with the
    // strobes out of the cast (resting, a settled drop, a fader under 55 %)
    // and the rest of the rig below the abefest joins nobody: at 90 % in a
    // drop the "and again" rolls spent the whole ten-beat window in three or
    // four bars nobody saw, and the strobes came back on stage to no budget.
    bool lit = false;
    foreach (const QString &key, m_groupOrder)
    {
        QString slot = "str:" + key;
        const TrackGroup &g = m_groups.value(key);
        const QList<quint32> &ids = m_strobeScenes.value(key);
        // Only a group that is in the cast: its colour scene is what writes
        // ShutterOpen every cycle, and that is what puts the shutter back
        // when the burst ends. Strobing a group with nothing else running
        // leaves the fixtures strobing for ever.
        // The strobe group joins whenever it is lit; everyone else only once
        // the room is at the top of the fader - that is the "abefest".
        // runde 346: the laser bars only in the amok - a drop from 95 % on the
        // slider (they strobed 11 % of the beats above 90 % on Friday, and the
        // shutter on a single walking eye is the "strobing" Tobias saw); the
        // heads in every drop from 60 %; the rest from w2 0.30 (w 0.55, as
        // before). A heads-only burst is the heads'.
        bool joins = cast.contains(key)
                     && (m_strobeHeadsOnly ? g.heads
                         // runde 357 (R357_STROBE_LAND, Tobias 10-05: "Stroberne skal
                         // ikke koere 100% lysstyrke paa andre tidspunkt end lige efter
                         // droppet ... Kun Strob/blink lige efter drop maa overgaa
                         // master/gruppedimmer"): the strobe lamps' hardware strobe
                         // flashes at the lamp's own full, whatever MASTER and the trim
                         // say - in the harness the dimmer stayed at 23 of 255 at
                         // MASTER 0.30 x trim 0.30 while the shutter ran 9.8 % of the
                         // time. On the strobe lamps it is the landing's alone now;
                         // everywhere else they pulse on their dimmer, under MASTER.
                         // runde 359 (R359_LAND_16): the strobe lamps' landing is
                         // blinked on their dimmer, on the beat's sixteenths
                         // (pulseFactor) - the shutter's own rate is a guess with no
                         // Hz in the fixture definition, the beat clock is not
                         : (g.strobes ? beat <= m_landBurstUntil && m_landSixteenths == false
                            : (g.lasers ? (isDrop && (m_faderNow >= ENGINE_BARS_AMOK                // runde 348
                                                         || (m_faderNow >= 0.90 && barsWide(key))))    // runde 352
                               : (g.heads ? ((isDrop && m_faderNow >= 0.60) || w2 > 0.30)
                                  : w2 > 0.30))));
        if (on == false || joins == false || ids.isEmpty()
            || m_groupOff.contains(key) || g.generatable() == false)
        {
            stopSlot(slot, true);
            continue;
        }
        // int(): QList::count() is qsizetype under Qt 6 and qBound would
        // not deduce a common type
        // everyone but the strobe group runs a notch slower, so the strobes
        // still lead when the whole rig joins in at the top of the fader
        int r = qBound(0, (g.strobes || m_strobeHeadsOnly) ? m_strobeRate : qMax(0, m_strobeRate - 2),
                       int(ids.count()) - 1);
        if (ids.at(r) == Function::invalidId())
        {
            stopSlot(slot, true);            // a hole in the rates: nothing, not the old one
            continue;
        }
        // Restarted every beat on purpose. The shutter is an LTP channel and
        // the last fader started wins - and at the top of the fader a colour
        // scene restarts every bar, which would otherwise put ShutterOpen
        // back on top and silently kill the strobe mid-burst.
        stopSlot(slot, true);
        run(slot, ids.at(r), 1.0, 0, true);
        lit = true;
    }
    if (lit && m_strobeHeadsOnly == false)
        m_strobeSpent++;
}

bool TrackEngine::setsColourOf(Function *func) const
{
    if (func == nullptr || m_doc == nullptr)
        return true;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return true;                 // cannot tell: assume it does
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            // A ZERO does not impose a colour, it clears one. The laser bars'
            // dimmer chases write nought to all eight eye channels in every
            // step (see dim() in gen_programs.py, and the LTP trap it is
            // there for) - that is housekeeping, not a red programme.
            //
            // ... unless nought IS a colour on that channel: the animation
            // laser's white is ch 1 = 0 ("AniWhite"). Read as "no colour",
            // its unnamed white patterns sat in every colour's pool and could
            // paint white over a red room (LTP) (runde 176).
            //
            // DORMANT (runde 179): this runs when the table is built, BEFORE
            // learnGroups() fills colourValue, so it - and the older "channel
            // this group's colour scenes move" branch below - never sees a
            // learned channel. Fixing the order is not safe a day before the
            // rig: the laser bars' ch 6 "Effect" is 79 in one colour scene of
            // the operator's and 0 in the rest, and with the order fixed every
            // bar programme holding ch 6 at 0 would read as colour-painting and
            // leave the bars' pools empty. So: pattern devices only, where the
            // colour IS the channel, for the day the order is put right.
            if (sv.value == 0)
            {
                bool zeroIsColour = false;
                foreach (const TrackGroup &zg, m_groups)
                {
                    if (zg.patternDevice == false)
                        continue;
                    foreach (uchar cv, zg.colourValue.value(sv.fxi).value(sv.channel))
                    {
                        if (cv == 0)
                        {
                            zeroIsColour = true;
                            break;
                        }
                    }
                    if (zeroIsColour)
                        break;
                }
                if (zeroIsColour)
                    return true;
                continue;
            }
            Fixture *fxi = m_doc->fixture(sv.fxi);
            const QLCChannel *qch = fxi != nullptr ? fxi->channel(sv.channel) : nullptr;
            if (qch == nullptr)
                continue;
            if (qch->colour() != QLCChannel::NoColour)
                return true;
            if (qch->group() == QLCChannel::Colour)
                return true;             // a colour wheel
            // a per-eye colour channel the definition leaves as NoFunction -
            // the laser bars' "Laser Color 1..8", the same names
            // splitColourFunction() reads. Seven AUTO bar programmes (Eyes
            // Rainbow, Eyes Split, Eyes Chase, Eyes Pairs) paint these and
            // counted as colourless, so they sat in every colour's pool: red
            // beside green on neighbouring eyes (runde 198)
            {
                static const QRegularExpression eyeColour(QStringLiteral("(colou?r|eye)\\s*\\d+"),
                                                          QRegularExpression::CaseInsensitiveOption);
                if (qch->name().contains(eyeColour))
                    return true;
            }
            // a channel this group's colour scenes are known to move
            foreach (const TrackGroup &g, m_groups)
            {
                if (g.colourValue.value(sv.fxi).contains(sv.channel))
                    return true;
            }
        }
    }
    return false;
}

bool TrackEngine::aimsOf(Function *func) const
{
    // Does the programme steer the heads itself? Only then does the engine's
    // own sweep have to step aside. The test used to be "is it a chaser" -
    // and every AUTO dimmer walk is a chaser, so for as long as a programme
    // ran on the heads (70 % of every groove and drop on the base) the sweep
    // was OFF and the heads stood still on their aim. Measured in the
    // tracklog of 2026-09-17: in drops the wash had a chase and no EFX on 592
    // beats against 144 with one; in grooves 379 against 428. That is the
    // heads not moving for most of the night, and "energien gaar ikke
    // igennem rummet" (Tobias, 2026-09-18) has that as its first cause.
    if (func == nullptr || m_doc == nullptr)
        return false;
    if (func->type() == Function::EFXType)
        return true;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    foreach (quint32 sid, steps)
    {
        Function *sf = m_doc->function(sid);
        if (sf == nullptr)
            continue;
        if (sf->type() == Function::EFXType)
            return true;
        Scene *scene = qobject_cast<Scene *>(sf);
        if (scene == nullptr)
            continue;
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            Fixture *fxi = m_doc->fixture(sv.fxi);
            const QLCChannel *qch = fxi != nullptr ? fxi->channel(sv.channel) : nullptr;
            if (qch == nullptr)
                continue;
            if (qch->group() == QLCChannel::Pan || qch->group() == QLCChannel::Tilt)
                return true;
        }
    }
    return false;
}

bool TrackEngine::coversColourOf(Function *func, const QSet<QString> &groups) const
{
    // Does this programme paint a colour on EVERY fixture of its group, in
    // every step?
    //
    // It matters because the engine runs two layers at once: the group's
    // colour scene in "col:" and the programme in "mot:". Colour channels
    // sit in QLCChannel::Intensity, which QLC+ blends HTP - universe.cpp
    // refuses a value lower than the one already there, per channel. So a
    // room standing in magenta under a programme that paints cyan on half
    // the lamps does not show magenta and cyan: the cyan lamps get
    // max(255,0), max(0,255), max(255,255) and come out WHITE. Measured
    // 2026-09-16 across the twelve pair directions: four survived, four
    // turned the partner white, two lost the partner altogether.
    //
    // When the programme covers the whole group, the colour scene underneath
    // has nothing left to contribute and the tick stops it. The test is
    // deliberately strict - EVERY fixture, EVERY step - because a programme
    // that paints only some lamps still needs the scene for the rest, or a
    // lamp would stand with its dimmer up and no colour behind it.
    if (func == nullptr || m_doc == nullptr)
        return false;
    if (groups.count() != 1)
        return false;
    const TrackGroup &g = m_groups.value(*groups.constBegin());
    if (g.fixtures.isEmpty())
        return false;

    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    if (steps.isEmpty())
        return false;

    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return false;
        QSet<quint32> painted;
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            if (g.fixtures.contains(sv.fxi) == false)
                continue;
            Fixture *fxi = m_doc->fixture(sv.fxi);
            const QLCChannel *qch = fxi != nullptr ? fxi->channel(sv.channel) : nullptr;
            if (qch == nullptr)
                continue;
            // A lamp this step holds at nought needs no colour behind it -
            // it is off. Without this a pulse (lit step, dark step) would
            // never qualify, and the pulse is exactly where the two-colour
            // programmes live.
            if (sv.value == 0 && sv.channel == dimmerChannel(fxi))
            {
                painted.insert(sv.fxi);
                continue;
            }
            if (sv.value == 0)
                continue;                // a zero clears a colour, it does not set one
            if (qch->colour() != QLCChannel::NoColour
                || qch->group() == QLCChannel::Colour
                || g.colourValue.value(sv.fxi).contains(sv.channel))
                painted.insert(sv.fxi);
        }
        foreach (quint32 fid, g.fixtures)
        {
            if (painted.contains(fid) == false)
                return false;
        }
    }
    return true;
}

qreal TrackEngine::litShareOf(Function *func, const QSet<quint32> &touched) const
{
    if (func == nullptr || touched.isEmpty() || m_doc == nullptr)
        return 1.0;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    if (steps.isEmpty())
        return 1.0;

    qreal sum = 0.0;
    int counted = 0;
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return 1.0;                  // not something we can measure: no claim
        // The MASTER DIMMER decides, where there is one. Red, green, blue and
        // white are Intensity channels too, and a generated step scene writes
        // the colour to every fixture and only varies the dimmer - so
        // "any Intensity channel is non-zero" said every lamp was lit in every
        // step, and the measure was worthless (caught in the same round it was
        // written). A fixture with no master dimmer falls back to its colour
        // channels; a scene that does not write the dimmer at all is not
        // turning the lamp off, so it counts as lit.
        QSet<quint32> lit;
        QSet<quint32> dimmed;
        quint32 runFid = Fixture::invalidId();         // B23 H5: the same answers for
        bool runTouched = false;                       // every value of one fixture
        Fixture *runFxi = nullptr;
        quint32 runDch = QLCChannel::invalid();
        for (const SceneValue &sv : valuesOf(scene))
        {
            if (sv.fxi != runFid)
            {
                runFid = sv.fxi;
                runTouched = touched.contains(sv.fxi);
                runFxi = runTouched ? m_doc->fixture(sv.fxi) : nullptr;
                runDch = runFxi != nullptr ? dimmerChannel(runFxi) : QLCChannel::invalid();
            }
            if (runTouched == false)
                continue;
            Fixture *fxi = runFxi;
            if (fxi == nullptr)
                continue;
            quint32 dch = runDch;
            if (dch != QLCChannel::invalid() && sv.channel == dch)
            {
                dimmed.insert(sv.fxi);
                if (sv.value > 0)
                    lit.insert(sv.fxi);
                continue;
            }
            if (dch != QLCChannel::invalid())
                continue;                // this fixture is judged by its dimmer
            const QLCChannel *qch = fxi->channel(sv.channel);
            if (sv.value > 0 && qch != nullptr && qch->group() == QLCChannel::Intensity)
                lit.insert(sv.fxi);
        }
        foreach (quint32 fid, touched)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi != nullptr && dimmerChannel(fxi) != QLCChannel::invalid()
                && dimmed.contains(fid) == false)
                lit.insert(fid);         // the step says nothing about it: still lit
        }
        sum += qreal(lit.count()) / qreal(touched.count());
        counted++;
    }
    return counted > 0 ? sum / counted : 1.0;
}

qreal TrackEngine::minLitOf(Function *func, const QSet<quint32> &touched) const
{
    // The lowest master dimmer any step leaves any lamp at, 0..1. A lamp with
    // a dimmer that a step does not write is a lamp this programme makes no
    // promise about: 0. Lamps without a master dimmer are not judged here.
    if (func == nullptr || touched.isEmpty() || m_doc == nullptr)
        return 0.0;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    if (steps.isEmpty())
        return 0.0;
    int lowest = 255;
    bool judged = false;
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return 0.0;
        const QList<SceneValue> values = valuesOf(scene);   // B23 H1: once per step, not per lamp
        foreach (quint32 fid, touched)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            const quint32 dch = dimmerChannel(fxi);
            if (dch == QLCChannel::invalid())
                continue;
            int v = -1;
            for (const SceneValue &sv : values)
            {
                if (sv.fxi == fid && sv.channel == dch)
                {
                    v = int(sv.value);
                    break;
                }
            }
            if (v < 0)
                return 0.0;              // not written: no promise
            lowest = qMin(lowest, v);
            judged = true;
        }
    }
    return judged ? qreal(lowest) / 255.0 : 0.0;
}

qreal TrackEngine::peakLitOf(Function *func, const QSet<quint32> &touched) const
{
    // runde 259: the brightest master dimmer in the step where the brightest
    // lamp is darkest, 0..1 - "at least one lamp on" in every step. A step
    // that writes no dimmer of these lamps promises nothing: 0.
    if (func == nullptr || touched.isEmpty() || m_doc == nullptr)
        return 0.0;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    if (steps.isEmpty())
        return 0.0;
    int worst = 255;
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return 0.0;
        int brightest = -1;
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            if (touched.contains(sv.fxi) == false)
                continue;
            Fixture *fxi = m_doc->fixture(sv.fxi);
            if (fxi != nullptr && sv.channel == dimmerChannel(fxi))
                brightest = qMax(brightest, int(sv.value));
        }
        if (brightest < 0)
            return 0.0;
        worst = qMin(worst, brightest);
    }
    return qreal(worst) / 255.0;
}

qreal TrackEngine::beamShareOf(Function *func, const QSet<quint32> &touched) const
{
    // Runde 338 (Tobias, 2026-10-03: "laser-bars i de hoeje energier brugte
    // for meget 'et-oeje' chases"). litShare counts FIXTURES by their master
    // dimmer, so "Bars Drop Eyes Single Cyan Fast" - every bar lit, one of its
    // eight eyes on - read 1.0, the fullest picture there is. This counts
    // beams: a fixture with per-eye channels ("Laser Color 1..8", the names
    // splitColourFunction reads) is that many beams, lit where its eye channel
    // is non-zero - or all of them when the step writes no eye at all (the
    // bar's colour channel or the group's colour scene paints them). Every
    // other fixture is one beam, lit as litShareOf reads it.
    if (func == nullptr || touched.isEmpty() || m_doc == nullptr)
        return 1.0;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    if (steps.isEmpty())
        return 1.0;
    static const QRegularExpression eyeName(QStringLiteral("(colou?r|eye)\\s*\\d+"),
                                            QRegularExpression::CaseInsensitiveOption);
    QHash<quint32, QSet<quint32> > eyes;      // fixture -> its eye channels (4 or more)
    QHash<quint32, quint32> dimmers;
    foreach (quint32 fid, touched)
    {
        if (m_dimmerCache.contains(fid) == false)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            m_dimmerCache.insert(fid, dimmerChannel(fxi));
            QSet<quint32> e;
            for (quint32 ch = 0; ch < fxi->channels(); ch++)
            {
                const QLCChannel *qch = fxi->channel(ch);
                if (qch != nullptr && qch->name().contains(eyeName))
                    e.insert(ch);
            }
            if (e.count() >= 4)
                m_eyeCache.insert(fid, e);
        }
        dimmers.insert(fid, m_dimmerCache.value(fid));
        if (m_eyeCache.contains(fid))
            eyes.insert(fid, m_eyeCache.value(fid));
    }
    qreal sum = 0.0;
    int counted = 0;
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return 1.0;                  // not something we can measure: no claim
        QHash<quint32, int> dimVal;      // -1: not written
        QHash<quint32, int> eyesOn;
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            if (touched.contains(sv.fxi) == false)
                continue;
            if (sv.channel == dimmers.value(sv.fxi, QLCChannel::invalid()))
                dimVal.insert(sv.fxi, int(sv.value));
            else if (sv.value > 0 && eyes.value(sv.fxi).contains(sv.channel))
                eyesOn[sv.fxi] += 1;
        }
        int total = 0, lit = 0;
        foreach (quint32 fid, touched)
        {
            const int beams = eyes.contains(fid) ? int(eyes.value(fid).count()) : 1;
            total += beams;
            if (dimVal.value(fid, -1) == 0)
                continue;
            if (eyes.contains(fid))
                lit += eyesOn.value(fid, 0) > 0 ? eyesOn.value(fid) : beams;
            else
                lit += 1;
        }
        if (total > 0)
        {
            sum += qreal(lit) / qreal(total);
            counted++;
        }
    }
    return counted > 0 ? sum / counted : 1.0;
}

bool TrackEngine::fullBankOf(Function *func, const QSet<quint32> &touched) const
{
    // Runde 338: a step that puts (nearly) EVERY lamp it touches at 200 or more
    // on its master dimmer, three lamps or more - on the strobes that is the whole
    // bank flashing at full, the picture Tobias wants gone ("strobe-lamperne
    // ... paa 100% dimmer (puler op et kort oejeblik)"). 432 of the show's
    // 1111 strobe programmes have one: Wide Row Pulse, Blink, Burst, the
    // two-colour Duo/Swap scenes.
    if (func == nullptr || touched.count() < 3 || m_doc == nullptr)
        return false;
    QList<quint32> steps;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
    }
    else
        steps.append(func->id());
    foreach (quint32 sid, steps)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            continue;
        int withDimmer = 0, full = 0;
        QSet<quint32> seen;
        foreach (const SceneValue &sv, valuesOf(scene))
        {
            if (touched.contains(sv.fxi) == false || seen.contains(sv.fxi))
                continue;
            // beamShareOf (called first, on the same fixtures) filled the cache
            quint32 dch = m_dimmerCache.value(sv.fxi, QLCChannel::invalid());
            if (m_dimmerCache.contains(sv.fxi) == false)
            {
                Fixture *fxi = m_doc->fixture(sv.fxi);
                dch = fxi != nullptr ? dimmerChannel(fxi) : QLCChannel::invalid();
            }
            if (sv.channel != dch)
                continue;
            seen.insert(sv.fxi);
            withDimmer++;
            if (sv.value >= 200)
                full++;
        }
        // three in four or more: five of six is the same picture ("Row Gap",
        // headless 10-03) - only the one dark lamp says it is a chase
        if (withDimmer >= 3 && full * 4 >= withDimmer * 3)
            return true;
    }
    return false;
}

bool TrackEngine::canOwnDimmers(const TrackFuncInfo &info, bool onBase) const
{
    // A dimmer chase takes the group's dimmers from the engine's own parts
    // (motionOwns) - so its dark steps show. One that lights every lamp in
    // every step never did (litShare >= 0.99: the parts kept the level), and
    // on the base nothing did, so the room never went dark under a chase
    // (runde 178). Runde 231: a programme that keeps EVERY lamp at
    // ENGINE_OWN_FLOOR or more in every step owns them too, base included -
    // its figure shows in full, and the room still cannot go dark. Before,
    // those figures (Throb, Glimmer, Pendulum, the build climbs ...) only
    // showed where they rose above the engine's own level.
    if (info.dimmer == false || info.type == int(Function::SceneType))
        return false;
    if (info.minLit >= ENGINE_OWN_FLOOR)
        return true;
    // Runde 259, Tobias: "lamper må gerne gå under 35%, det vigtigste er bare,
    // at der altid er mindst en lampe på". So: at least one lamp at
    // ENGINE_OWN_FLOOR in every step owns the dimmers too, base included -
    // the others may go as low as the figure wants.
    if (info.peakLit >= ENGINE_OWN_FLOOR)
        return true;
    // Runde 260, Tobias: "Når der er flere end en gruppe der kører samtidigt,
    // må programmer der har helt ned til 0 på alle lamper samtidigt også
    // gerne køre" - with another lamp group lit beside it, the base is just
    // one group of several and its dark steps may show like theirs
    return (onBase == false || baseCovered()) && info.litShare < 0.99;
}

bool TrackEngine::baseCovered() const
{
    // A lamp group in the cast beside the base (runde 260) - not dark this
    // beat (the pre-drop blink, a move), not switched OFF, not a laser (it
    // draws beams, it does not light the room) - the strobes DO count (runde
    // 261, Tobias: "they are bright, so they kinda do either way") - and not
    // itself running a chase with steps where every lamp is out: then the two
    // could be dark on the same beat. The group beside the base always keeps
    // its own programme; it is the base that gives way.
    foreach (const QString &key, m_baseCover)
    {
        if (m_motionDim.contains(key))
        {
            const TrackFuncInfo &o = m_funcs.value(m_active.value(QStringLiteral("mot:") + key,
                                                                   Function::invalidId()));
            if (o.minLit < ENGINE_OWN_FLOOR && o.peakLit < ENGINE_OWN_FLOOR)
                continue;
        }
        return true;
    }
    return false;
}

quint32 TrackEngine::dimmerChannel(Fixture *fxi) const
{
    // QLC's own answer first - but it gives up on definitions that declare
    // heads (the animation lasers, the mini pars), so fall back to reading
    // the channel list: a plain white Intensity channel, master dimmer
    // preset preferred.
    if (fxi == nullptr)
        return QLCChannel::invalid();

    quint32 ch = fxi->masterIntensityChannel();
    if (ch != QLCChannel::invalid())
        return ch;

    quint32 plain = QLCChannel::invalid();
    for (quint32 i = 0; i < fxi->channels(); i++)
    {
        const QLCChannel *qch = fxi->channel(i);
        if (qch == nullptr || qch->group() != QLCChannel::Intensity
            || qch->colour() != QLCChannel::NoColour)
            continue;
        if (qch->preset() == QLCChannel::IntensityMasterDimmer)
            return i;
        if (plain == QLCChannel::invalid())
            plain = i;
    }
    return plain;
}

void TrackEngine::ensureDimmerScenes()
{
    // one hidden scene per FIXTURE, holding its master dimmer at full; the
    // scene's intensity attribute is that fixture's level. A group's level is
    // all of its parts, a chase across the group is the parts in turn.
    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name().startsWith(ENGINE_DIMMER_PREFIX))
            existing.insert(func->name().mid(ENGINE_DIMMER_PREFIX.length()), func->id());
    }

    for (QMap<QString, TrackGroup>::iterator it = m_groups.begin(); it != m_groups.end(); ++it)
    {
        TrackGroup &g = it.value();
        g.parts.clear();
        g.hasDimmer = false;

        for (int i = 0; i < g.fixtures.count(); i++)
        {
            Fixture *fxi = m_doc->fixture(g.fixtures.at(i));
            quint32 ch = fxi == nullptr ? QLCChannel::invalid() : dimmerChannel(fxi);
            if (ch == QLCChannel::invalid())
                continue;            // no dimmer: not a part, or the pattern goes dark on its step
            SceneValue sv(fxi->id(), ch, 255);
            QString name = QString("%1 #%2").arg(g.key).arg(i + 1);

            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                // the fixture behind this part may have changed: hold only it
                foreach (SceneValue old, scene->values())
                {
                    if (old.fxi != sv.fxi || old.channel != sv.channel)
                        scene->unsetValue(old.fxi, old.channel);
                }
                scene->setValue(sv);
            }
            else
            {
                scene = new Scene(m_doc);
                scene->setName(ENGINE_DIMMER_PREFIX + name);
                scene->setVisible(false);
                scene->setValue(sv);
                if (m_doc->addFunction(scene) == false)
                {
                    delete scene;
                    scene = nullptr;
                }
            }
            g.parts.append(scene == nullptr ? Function::invalidId() : scene->id());
            if (scene != nullptr)
            {
                // HTP zero cannot silence a dimmer in another running scene.
                // Own this channel while AUTO runs; manual override and FLASH
                // remain above this layer, and release() removes the fader.
                scene->setBlendMode(Universe::ReplaceBlend);
                g.hasDimmer = true;
            }
        }
    }
}

void TrackEngine::ensureAtmosScenes()
{
    // the hazer is not a light, so it is not in any group - but its two
    // channels get their own sliders on the Track page
    m_hazeChannels.clear();
    m_fanChannels.clear();

    foreach (Fixture *fxi, m_doc->fixtures())
    {
        if (fxi == nullptr || fxi->fixtureDef() == nullptr)
            continue;
        QString model = fxi->fixtureDef()->model().toLower();
        if (model.contains("haze") == false && model.contains("smoke") == false
            && model.contains("fog") == false && model.contains("hazer") == false)
            continue;

        for (quint32 ch = 0; ch < fxi->channels(); ch++)
        {
            const QLCChannel *qch = fxi->channel(ch);
            if (qch == nullptr)
                continue;
            QString n = qch->name().toLower();
            if (n.contains("fan") || n.contains("blower"))
                m_fanChannels.append(qMakePair(fxi->id(), ch));
            else if (n.contains("haze") || n.contains("fog") || n.contains("smoke")
                     || n.contains("output") || n.contains("pump"))
                m_hazeChannels.append(qMakePair(fxi->id(), ch));
        }
    }

    auto ensure = [this](const QString &name, quint32 &id,
                         const QList<QPair<quint32, quint32> > &channels) {
        id = Function::invalidId();
        if (channels.isEmpty())
            return;
        foreach (Function *func, m_doc->functions())
        {
            if (func != nullptr && func->name() == name)
            {
                id = func->id();
                return;
            }
        }
        Scene *scene = new Scene(m_doc);
        scene->setName(name);
        scene->setVisible(false);
        for (int i = 0; i < channels.count(); i++)
            scene->setValue(SceneValue(channels.at(i).first, channels.at(i).second, 0));
        if (m_doc->addFunction(scene))
            id = scene->id();
        else
            delete scene;
    };
    ensure(ENGINE_HAZE_SCENE, m_hazeScene, m_hazeChannels);
    ensure(ENGINE_FAN_SCENE, m_fanScene, m_fanChannels);
}

bool TrackEngine::hazeAvailable()
{
    ensureTable();
    return m_hazeChannels.isEmpty() == false || m_fanChannels.isEmpty() == false;
}

qreal TrackEngine::haze() const { return m_haze; }
qreal TrackEngine::fan() const { return m_fan; }

void TrackEngine::applyAtmos(quint32 sceneId, const QList<QPair<quint32, quint32> > &channels, qreal level)
{
    Scene *scene = qobject_cast<Scene *>(m_doc ? m_doc->function(sceneId) : nullptr);
    if (scene == nullptr)
        return;

    uchar value = uchar(qRound(qBound(0.0, level, 1.0) * 255.0));
    for (int i = 0; i < channels.count(); i++)
    {
        // checkHTP = FALSE. Scene::setValue pushes the new value into the
        // running fader with add(), and add() keeps whichever value is higher
        // - so the hazer went up and never came down again. replace() is what
        // a live slider needs, and that is what the third argument picks.
        scene->setValue(SceneValue(channels.at(i).first, channels.at(i).second, value),
                        false, false);
    }

    // NEVER stop this scene once it has run. "Haze Volume" and "Blower
    // Speed" are Effect / Speed channels - LTP - and an LTP channel KEEPS
    // its last value when the function that wrote it goes away. Stopping at
    // nought was the fault Tobias hit on 2026-09-17 ("haze-output sad fast,
    // maatte manuelt skrue ned paa kanalen"): stop() is deferred to the
    // MasterTimer thread, and its tick handles a stopped function with
    // postRun() INSTEAD of a final write - so the nought that setValue() had
    // just put into the fader (replace, above) never reached the universe,
    // dismissAllFaders() dropped the channel, and the hazer stood at the
    // value it had when the slider was still up. A running scene at nought
    // is the only thing that holds an LTP channel at nought: so the scene
    // starts the first time the slider is touched and stays up, writing
    // whatever the slider says - including off - on every frame. It is one
    // fader on two channels nothing else in AUTO writes; a hand scene of the
    // operator's started later still wins under LTP while it runs.
    if (scene->isRunning() == false || scene->stopped())
        scene->start(m_doc->masterTimer(), FunctionParent::track());
}

void TrackEngine::setHaze(qreal level)
{
    level = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(level + 1.0, m_haze + 1.0))
        return;
    ensureTable();
    m_haze = level;
    applyAtmos(m_hazeScene, m_hazeChannels, level);
    emit liveChanged();
}

void TrackEngine::setFan(qreal level)
{
    level = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(level + 1.0, m_fan + 1.0))
        return;
    ensureTable();
    m_fan = level;
    applyAtmos(m_fanScene, m_fanChannels, level);
    emit liveChanged();
}

void TrackEngine::loadRoles()
{
    // One key for every workspace. That is wrong the day a second show turns
    // up - its function 12 inherits this show's role - but see the note over
    // saveRoles(): the fix needs something Doc does not expose today, and a
    // half-right fix here loses every role assignment without saying so.
    QSettings settings;
    QString stored = settings.value(SETTINGS_ENGINE_ROLES, QString()).toString();
    foreach (QString entry, stored.split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 2)
            continue;
        quint32 fid = parts.at(0).toUInt();
        if (m_funcs.contains(fid))
        {
            int r = parts.at(1).toInt();
            if (r >= -1 && r < ENGINE_ROLE_COUNT)     // a file from another show
                m_funcs[fid].role = r;
        }
    }

    QString stars = settings.value(SETTINGS_ENGINE_STARS, QString()).toString();
    foreach (QString entry, stars.split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 2)
            continue;
        quint32 fid = parts.at(0).toUInt();
        if (m_funcs.contains(fid))
            m_funcs[fid].stars = qBound(1, parts.at(1).toInt(), 3);
    }

    // the verdicts: "fid:bucket:up:down", one per bucket that has anything.
    // A malformed or out-of-range entry is skipped rather than clamped - a
    // count is evidence, and inventing a plausible number out of a broken
    // line would be inventing evidence.
    QString rating = settings.value(SETTINGS_ENGINE_RATING, QString()).toString();
    foreach (QString entry, rating.split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 4)
            continue;
        quint32 fid = parts.at(0).toUInt();
        int b = parts.at(1).toInt();
        if (m_funcs.contains(fid) == false || b < 0 || b >= ENGINE_RATE_BUCKETS)
            continue;
        m_funcs[fid].up[b] = qMax(0, parts.at(2).toInt());
        m_funcs[fid].down[b] = qMax(0, parts.at(3).toInt());
    }

    foreach (QString entry, settings.value(SETTINGS_ENGINE_SEEN, QString())
                                    .toString().split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 3)
            continue;
        quint32 fid = parts.at(0).toUInt();
        int b = parts.at(1).toInt();
        if (m_funcs.contains(fid) == false || b < 0 || b >= ENGINE_RATE_BUCKETS)
            continue;
        m_funcs[fid].seen[b] = qMax(0, parts.at(2).toInt());
    }

    foreach (QString entry, settings.value(SETTINGS_ENGINE_BANNED, QString())
                                    .toString().split(';', Qt::SkipEmptyParts))
    {
        quint32 fid = entry.toUInt();
        if (m_funcs.contains(fid))
            m_funcs[fid].banned = true;
    }

    m_autoRatings.clear();
    QJsonObject recipes = QJsonDocument::fromJson(settings.value(SETTINGS_ENGINE_AUTORATING)
                                                .toString().toUtf8()).object();
    for (auto it = recipes.constBegin(); it != recipes.constEnd(); ++it)
    {
        QJsonObject votes = it.value().toObject();
        int up = votes.value("up").toInt(-1), down = votes.value("down").toInt(-1);
        if (it.key().startsWith("v1|") && it.key().size() <= 8192 && up >= 0 && down >= 0 && up <= 1000000 && down <= 1000000)
            m_autoRatings.insert(it.key(), qMakePair(up, down));
    }
    m_ratingOn = settings.value(SETTINGS_ENGINE_RATINGON, false).toBool();
}

// NOTE - roles, stars and the group switches are keyed on FUNCTION ID under
// one settings key shared by every workspace. With a second show its function
// 12 inherits this show's role, and the first save from that show overwrites
// this one's. It is on the backlog, NOT fixed, and the obvious fix does not
// work: Doc has setWorkspacePath()/workspacePath() only, and app.cpp feeds it
// QFileInfo(fileName).absolutePath() - the DIRECTORY. Two shows in the same
// folder get the same fingerprint, so keying on it buys nothing while the
// silent-loss path (loadRoles falls back to the shared key, saveRoles only
// ever writes the new one) is real. Doing it properly means giving Doc the
// file name, and that is engine core - bane B.
void TrackEngine::saveRoles()
{
    // An empty table is "not built yet", not "nothing is worth keeping".
    // trackLoaded() saves unconditionally, and BLT sends the current track
    // the moment QLC+ connects - before the Track page has been opened and
    // before the first beat, so before anything has called ensureTable().
    // Writing this state would replace every role, star, verdict, stage
    // count and ban with an empty string.
    if (m_funcs.isEmpty())
        return;

    QStringList entries;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        if (it.value().role != it.value().guess || it.value().step)
            entries << QString("%1:%2").arg(it.key()).arg(it.value().role);
    QSettings().setValue(SETTINGS_ENGINE_ROLES, entries.join(';'));
    QSettings().setValue(SETTINGS_ENGINE_GROUPOFF,
                         QStringList(m_groupOff.values()).join(';'));

    QStringList stars;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        if (it.value().stars != it.value().starsGuess && it.value().generated == false)
            stars << QString("%1:%2").arg(it.key()).arg(it.value().stars);
    QSettings().setValue(SETTINGS_ENGINE_STARS, stars.join(';'));

    QStringList rating, bans, exposure;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.generated)
            continue;                  // rebuilt every table: nothing to keep
        if (info.banned)
            bans << QString::number(it.key());
        for (int b = 0; b < ENGINE_RATE_BUCKETS; b++)
        {
            if (info.up[b] == 0 && info.down[b] == 0)
                continue;
            rating << QString("%1:%2:%3:%4").arg(it.key()).arg(b).arg(info.up[b]).arg(info.down[b]);
        }
        for (int b = 0; b < ENGINE_RATE_BUCKETS; b++)
        {
            if (info.seen[b] > 0)
                exposure << QString("%1:%2:%3").arg(it.key()).arg(b).arg(info.seen[b]);
        }
    }
    QSettings().setValue(SETTINGS_ENGINE_RATING, rating.join(';'));
    QSettings().setValue(SETTINGS_ENGINE_BANNED, bans.join(';'));
    QSettings().setValue(SETTINGS_ENGINE_SEEN, exposure.join(';'));
    QJsonObject recipes;
    for (auto it = m_autoRatings.constBegin(); it != m_autoRatings.constEnd(); ++it)
    {
        QJsonObject votes;
        votes.insert("up", it.value().first);
        votes.insert("down", it.value().second);
        recipes.insert(it.key(), votes);
    }
    QSettings().setValue(SETTINGS_ENGINE_AUTORATING,
                         QString::fromUtf8(QJsonDocument(recipes).toJson(QJsonDocument::Compact)));
}

void TrackEngine::rebuild()
{
    saveRoles();               // the counts since the last save, see setFullAuto()
    m_dirty = true;
    ensureTable();
    emit tableChanged();
}

QVariantList TrackEngine::table()
{
    ensureTable();

    QVariantList list;
    // The sort keys once per row: the comparator built and sorted two group
    // lists and lower-cased two names on each of ~160,000 comparisons -
    // ~240 ms per tableChanged with SETUP open (measured, 11,500 rows).
    struct RowKey { bool tail; QString group; QString name; const TrackFuncInfo *info; };
    QList<RowKey> keys;
    keys.reserve(m_funcs.count());
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &f = it.value();
        // the same group name the row shows: a QSet iterates in hash order
        QStringList gl = f.groups.values();
        gl.sort();
        keys.append({ f.junk || f.step || f.groups.isEmpty(),
                      gl.isEmpty() ? QString() : gl.first(), f.name.toLower(), &f });
    }
    std::sort(keys.begin(), keys.end(), [](const RowKey &a, const RowKey &b) {
        if (a.tail != b.tail) return b.tail;     // the usable looks first
        if (a.group != b.group) return a.group < b.group;
        return a.name < b.name;
    });
    QList<TrackFuncInfo> rows;
    rows.reserve(keys.count());
    for (const RowKey &k : keys)
        rows.append(*k.info);

    // Everything the operator actually reaches sits on a Virtual Console
    // widget; the rest is show-file archaeology that only made this list
    // longer to scroll. The console answers for itself - usageList() knows
    // every widget type that can hold a function, and keeps knowing when
    // QLC+ adds one - so nothing here walks the widget tree by hand.
    //
    // Fails OPEN on purpose, twice over: no console at all, or a console
    // with no widgets yet because it has not finished loading, leaves every
    // row visible. A SETUP list that had quietly emptied itself would look
    // exactly like a lost show file, and that is not a thing to discover
    // during a gig.
    //
    // This is the DISPLAY filter and nothing else. m_funcs is untouched, so
    // what the engine may pick is unchanged, and a VC button still starts
    // its function whatever this decides. Busking cannot notice it.
    // SHOW ALL brings the rows back, and a row that already has a role is
    // never hidden by any of this.
    VirtualConsole *vc = (m_doc != nullptr && m_doc->parent() != nullptr)
                         ? m_doc->parent()->findChild<VirtualConsole *>()
                         : nullptr;
    if (vc != nullptr && vc->widgetsList().isEmpty())
        vc = nullptr;

    foreach (const TrackFuncInfo &info, rows)
    {
        bool hidden = info.junk || info.step || info.groups.isEmpty() || info.generated
                      || (vc != nullptr && vc->usageList(info.id).isEmpty());
        // "A row that has been given a role is never hidden" - given by the
        // OPERATOR, which is role != guess, the same test saveRoles() uses to
        // decide what is his. The first version tested role < 0, and every
        // classified look has role == guess >= 0 by default, so of the ~730
        // rows not on a VC widget the filter hid almost none (PSMAIN: 573
        // visible own functions, 237 on widgets; 396 AUTO, none on widgets).
        // role < 0 stays in the test so junk and step rows hide as before.
        if (hidden && m_showAll == false && (info.role < 0 || info.role == info.guess))
            continue;

        QStringList groups = info.groups.values();
        groups.sort();

        QVariantMap row;
        row.insert("id", QVariant::fromValue(info.id));
        row.insert("name", info.name);
        row.insert("path", info.path);
        row.insert("role", info.role);
        row.insert("guess", info.guess);
        row.insert("group", groups.join(" + "));
        row.insert("colour", info.colour);
        row.insert("hidden", hidden);
        row.insert("stars", info.stars);
        row.insert("starsGuess", info.starsGuess);
        row.insert("generated", info.generated);
        row.insert("banned", info.banned);
        // the verdicts, summed over the four buckets - enough for the row to
        // show why something is favoured. A binding on a Q_INVOKABLE would
        // only refresh because the model happens to be rebuilt; data in the
        // row refreshes because it IS the row.
        int rup = 0, rdown = 0;
        for (int b = 0; b < ENGINE_RATE_BUCKETS; b++)
        {
            rup += info.up[b];
            rdown += info.down[b];
        }
        row.insert("rateUp", rup);
        row.insert("rateDown", rdown);
        list.append(row);
    }
    return list;
}

void TrackEngine::assignRole(quint32 fid, int role)
{
    ensureTable();
    // a generated function's id belongs to whatever is generated there after
    // the next rebuild - as setBanned() already refuses (runde 196)
    if (m_funcs.contains(fid) == false || m_funcs.value(fid).generated)
        return;
    m_funcs[fid].role = role;
    saveRoles();
    m_dirty = true;            // palette may have changed
    ensureTable();
    emit tableChanged();
}

void TrackEngine::setStars(quint32 fid, int stars)
{
    ensureTable();
    if (m_funcs.contains(fid) == false || m_funcs.value(fid).generated)   // never saved: runde 196
        return;
    m_funcs[fid].stars = qBound(1, stars, 3);
    saveRoles();
    m_moves.clear();
    emit tableChanged();
}

void TrackEngine::autoAssign(bool force)
{
    bool rebuilt = m_dirty;
    ensureTable();
    if (force)
    {
        QSettings().remove(SETTINGS_ENGINE_ROLES);
        for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
            it.value().role = it.value().step ? -1 : it.value().guess;
        saveRoles();
        m_dirty = true;
        ensureTable();
        rebuilt = true;
    }
    // SETUP calls autoAssign(false) every time it opens: rebuilding the whole
    // table for that tore the live page's cast and colour tiles down under
    // the operator's finger, twice. Nothing changed - nothing to tell.
    if (rebuilt)
        emit tableChanged();
}

QVariantList TrackEngine::groups()
{
    ensureTable();
    QVariantList list;
    // ONE walk for every group's counts (it was one per group, each a QSet
    // lookup on all 11,500 rows - ~16 ms per tableChanged, read twice)
    QHash<QString, QPair<int, int> > counts;          // group -> colours, motions
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const int role = it.value().role;
        if (role != ENGINE_ROLE_COLOR && role != ENGINE_ROLE_MOTION)
            continue;
        foreach (const QString &gk, it.value().groups)
        {
            QPair<int, int> &c = counts[gk];
            if (role == ENGINE_ROLE_COLOR) c.first++; else c.second++;
        }
    }
    const QString base = baseGroup();
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        const int colours = counts.value(key).first;
        const int motions = counts.value(key).second;
        QVariantMap row;
        row.insert("key", key);
        row.insert("fixtures", g.fixtures.count());
        row.insert("dimmer", g.hasDimmer);
        row.insert("enabled", m_groupOff.contains(key) == false);
        row.insert("colours", colours);
        row.insert("motions", motions);
        row.insert("strobes", g.strobes);
        row.insert("lasers", g.lasers);
        // An animation laser is on or it is off - its "dimmer" is a switch, so
        // a fader for it is a lie. The Track page gives those a button.
        row.insert("switchOnly", g.patternDevice || g.hasDimmer == false);
        row.insert("heads", g.heads);
        row.insert("base", key == base);
        list.append(row);
    }
    return list;
}

void TrackEngine::setGroupEnabled(QString key, bool enable)
{
    // Already precise: it names the group, so it blames exactly the program
    // that was on it. No tapping required.
    const bool changed = enable != (m_groupOff.contains(key) == false);
    if (enable) m_groupOff.remove(key); else m_groupOff.insert(key);
    if (enable == false && key == m_echoKey)
        stopEcho();
    if (changed)
        logSignal((enable ? QStringLiteral("sig:group-on:") : QStringLiteral("sig:group-off:")) + key);
    // right now, not on the next beat - and stop whatever of ours is on it
    if (m_doc != nullptr)
    {
        ensureTable();
        if (enable == false)
        {
            foreach (const QString &slot, m_active.keys())
            {
                if (slot.startsWith("off:"))
                    continue;                    // that one is the mask itself
                if (slot.endsWith(":" + key) || slot == "col:" + key || slot == "mot:" + key)
                    stopSlot(slot, true);
            }
            const TrackGroup &g = m_groups.value(key);
            for (int i = 0; i < g.parts.count(); i++)
                stopSlot(partSlot(key, i), true);
            // out of the cast as well, or the pulse timer keeps working on it
            m_cast.remove(key);
            m_liveMove.remove(key);
            m_patterned.remove(key);
            m_motionDim.remove(key);
            m_pulseDepth.remove(key);
            m_breathe.remove(key);
        }
        applyGroupOff();
    }
    // after ensureTable() (fejljagt 09-27): saveRoles() keeps an unbuilt
    // table untouched, so called first the switch never reached GROUPOFF
    // when the Track page had not built the table yet
    saveRoles();
    if (m_startScene)
        startLook();                     // the opening picture follows the switches
    emit tableChanged();
    emit liveChanged();          // the cast panel's lit border follows at once (r199)
}

bool TrackEngine::groupEnabled(QString key) const { return m_groupOff.contains(key) == false; }

QVariantMap TrackEngine::trims() const
{
    QVariantMap map;
    foreach (const QString &key, m_groupOrder)
        map.insert(key, m_groupTrim.value(key, 1.0));
    return map;
}

qreal TrackEngine::groupTrim(QString key) const { return m_groupTrim.value(key, 1.0); }

void TrackEngine::setGroupTrim(QString key, qreal level)
{
    level = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(level + 1.0, m_groupTrim.value(key, 1.0) + 1.0))
        return;
    m_groupTrim.insert(key, level); // the signal's settings snapshot must contain the new trim
    saveNight();                    // runde 184: the same night gets it back after a restart
    // A fader is dragged, not tapped, so this would write a line per frame.
    // One per group per logged beat is enough to see the gesture and where it
    // ended, and the beat lines around it carry the rest.
    if (m_trimLogged.value(key, -1) != m_logBeatNo)
    {
        m_trimLogged.insert(key, m_logBeatNo);
        logSignal(QStringLiteral("sig:trim:") + key
                  + QLatin1Char('=') + QString::number(level, 'f', 2));
    }

    // straight onto everything that is lit for this group - the dimmer parts
    // and the colour scene alike - without waiting for the beat
    reapplyLevels();
    emit liveChanged();
}

QString TrackEngine::baseGroup() const
{
    // never a strobe group: the base is in the cast at every fader, so a
    // strobe base lit the strobes at 0 % and between tracks, past the 55 %
    // rule - and one tap on its SETUP tile made it so (runde 190)
    // ... nor the laser bars (runde 213): the base skips the dark hold while
    // the bars re-aim, and idle() lights it between tracks with the tilt
    // unknown - lit beams below home at any fader
    if (m_base.isEmpty() == false && m_groups.contains(m_base) && m_groupOff.contains(m_base) == false
        && m_groups.value(m_base).strobes == false && m_groups.value(m_base).lasers == false)
        return m_base;
    // automatic: the moving heads, if there are any with a colour to show
    foreach (const QString &key, m_groupOrder)
    {
        if (m_groups.value(key).heads && m_groupOff.contains(key) == false
            && candidates(ENGINE_ROLE_COLOR, key).isEmpty() == false)
            return key;
    }
    return QString();
}

void TrackEngine::cycleGroup(QString key)
{
    // Runde 234: a TAP is ON <-> OFF, nothing else. It used to cycle
    // ON -> BASE -> OFF, and one tap on the Minis in SETUP (01:42, 26 Sep)
    // made them the base for the rest of the night - and saved it - without
    // Tobias meaning to ("Jeg har altså ikke ændret mini til at være base").
    // BASE is a press-and-hold now (toggleBase).
    ensureTable();
    const bool turnOn = m_groupOff.contains(key);
    if (turnOn == false && m_base == key)
    {
        m_base.clear();                         // a base switched off is no base
        QSettings().setValue(SETTINGS_ENGINE_BASE, m_base);
    }
    setGroupEnabled(key, turnOn);               // saves and tells the page
}

void TrackEngine::toggleBase(QString key)
{
    // press-and-hold on a group in SETUP: make it the base, or give the base
    // back to the automatic pick. Strobes and lasers are never the base
    // (runde 190, 213). A group that is OFF is switched on to be the base.
    ensureTable();
    if (m_base == key)
        m_base.clear();
    else if (m_groups.value(key).strobes || m_groups.value(key).lasers)
        return;
    else
        m_base = key;
    QSettings().setValue(SETTINGS_ENGINE_BASE, m_base);
    if (m_base == key && m_groupOff.contains(key))
    {
        setGroupEnabled(key, true);             // saves and tells the page
        return;
    }
    saveRoles();
    emit tableChanged();
}

QVariantList TrackEngine::palette()
{
    ensureTable();
    QVariantList list;
    foreach (const QString &c, m_palette)
        list.append(c);
    return list;
}

bool TrackEngine::showAll() const { return m_showAll; }
void TrackEngine::setShowAll(bool on) { if (on != m_showAll) { m_showAll = on; emit tableChanged(); } }
bool TrackEngine::fullAuto() const { return m_fullAuto; }

void TrackEngine::setFullAuto(bool on)
{
    if (on == m_fullAuto)
        return;
    m_fullAuto = on;
    invalidateCandidates();          // userAllowed() reads m_fullAuto
    QSettings().setValue(SETTINGS_ENGINE_FULLAUTO, m_fullAuto);
    // a bar FIGURE stopping is a move, and an EFX has no fade-out: dark at
    // once, as release() and idle() do (runde 220)
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &lg = m_groups.value(key);
        if (lg.lasers == false || lg.patternDevice || m_active.contains("efx:" + key) == false)
            continue;
        stopSlot("col:" + key, true);
        for (int i = 0; i < lg.parts.count(); i++)
            stopSlot(partSlot(key, i), true);
        // B22: a dimmer chase (mot:) is the only writer when it runs on the
        // bars, and the echo lights them too - both went soft further down,
        // so the beams jumped back to the aim still lit for up to 2 s
        stopSlot("mot:" + key, true);
        stopSlot("echo:" + key, true);
    }
    // whatever runs now may be a function that is no longer allowed
    foreach (const QString &slot, m_active.keys())
    {
        // ... but not the OFF and BLACKOUT masks: between tracks no beat puts
        // them back, and an OFF group lit up under the start look (runde 196)
        // the hardware strobe is cut, as release() and idle() cut it: a
        // shutter-only scene has nothing for a soft stop's fade to scale,
        // and it strobed on at full for the fade (runde 215)
        if (slot.startsWith("str:"))
        {
            stopSlot(slot, true);
            continue;
        }
        if (slot.startsWith("idle:") == false && slot.startsWith("off:") == false
            && slot.startsWith("black:") == false)
            stopSlot(slot, false);
    }
    m_position.clear();
    m_moves.clear();
    m_sectionMotion.clear();      // a held pick was chosen under the other mode's rules
    // ensureTable() builds every TrackFuncInfo afresh and reads the verdicts
    // and stage counts back from QSettings - so whatever tick() has counted
    // since the last save is gone unless it is written first
    saveRoles();
    m_dirty = true;
    // the soft stops are stepped down by the fade timer, and no beat may come
    if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
        m_fadeTimer.start();
    // the opening picture was among what just stopped: tick() will not bring
    // it back while it is up, so it is put back here (runde 185)
    if (m_startScene)
        startLook();
    emit tableChanged();
    emit liveChanged();
}

bool TrackEngine::accent() const { return m_accent; }
void TrackEngine::setAccent(bool on)
{
    m_accent = on;
    QSettings().setValue(SETTINGS_ENGINE_ACCENT, on);
    emit tableChanged();
}
int TrackEngine::holdBars() const { return m_holdBars; }
void TrackEngine::setHoldBars(int bars)
{
    m_holdBars = qBound(4, bars, 128);
    QSettings().setValue(SETTINGS_ENGINE_HOLDBARS, m_holdBars);
    // a tile is a fixed hold: the fader lets go of it (runde 189)
    m_holdAuto = false;
    QSettings().setValue(SETTINGS_ENGINE_HOLDAUTO, m_holdAuto);
    emit tableChanged();
}
bool TrackEngine::holdAuto() const { return m_holdAuto; }
void TrackEngine::setHoldAuto(bool on)
{
    if (on == m_holdAuto)
        return;
    m_holdAuto = on;
    QSettings().setValue(SETTINGS_ENGINE_HOLDAUTO, m_holdAuto);
    emit tableChanged();
}

/*********************************************************************
 * Live controls
 *********************************************************************/

QString TrackEngine::colourOverride() const { return m_override; }

void TrackEngine::setColourOverride(QString colour)
{
    // the house rule holds for the palette tiles too
    if (engineBannedColour(colour))
        colour.clear();
    // one tile (or AUTO): the set is that one (runde 304)
    const QStringList one = colour.isEmpty() ? QStringList() : QStringList(colour);
    if (colour == m_override && m_overrideSet == one)
        return;
    const bool setChanged = m_overrideSet != one;
    // R370: one tile or AUTO is not a fade or a chase
    const bool modeChanged = m_colourMode != 0;
    m_colourMode = 0;
    m_overrideSet = one;
    m_overrideIdx = 0;
    const QString was = m_override;
    applyOverride(colour);
    // the lead did not change, the set did (a tile taken out down to the one
    // leading): applyOverride() returned early - say so (review 305)
    if (setChanged && was == colour)
    {
        logSignal(colour.isEmpty() ? QStringLiteral("sig:colour-auto")
                                   : QStringLiteral("sig:colour:") + colour);
        emit liveChanged();
    }
    else if (modeChanged)
        emit liveChanged();
}

QStringList TrackEngine::colourOverrides() const { return m_overrideSet; }

void TrackEngine::toggleColourOverride(const QString &colour)
{
    // Runde 304 (Tobias, 2026-09-29: "Når man vælger farve den skal bruge
    // selv, så kan man aktivere mere end én. Så f.eks. trykker man på blå og
    // lilla og så bruger den begge 2 i mix"). The room LEADS in one of them
    // and the next is the look's partner: two-colour programmes pair them,
    // the accent group wears it in every section, the bars' echo answers in
    // it - and at every colour change (the hold clock, a break or a drop,
    // NEXT) the lead turns to the next one. One tile is the old single lock.
    if (colour.isEmpty())
        return;
    // Runde 313 (B27, Tobias: "Det er hele pointen med manuel farveskift, at
    // den godt må override så man kan blande farver selv") - any mix of
    // tiles is the DJ's. Only a colour the engine CANNOT use is refused, and
    // the tile says so: it blinks, so the press is seen to have arrived.
    if (engineBannedColour(colour) || m_palette.contains(colour) == false)
    {
        emit colourRejected(colour);
        return;
    }
    // the start scene's red is not the DJ's (review 305): a tile tapped while
    // it is up starts a set of its own, and outlives it (m_startColour)
    // R378_START_LAYER (Tobias 10-06: "hvis man f.eks. tilfoejer en eller flere
    // farver"): a tile tapped on the opening picture ADDS to its red - the red
    // is his from then on; tapping red off changes it altogether
    QStringList set = m_overrideSet;
    // ... and that is so even when the tap lands on the start scene's own
    // colour: setColourOverride() then returns early (nothing changed) and
    // m_startColour stayed true, so the DJ's red was cleared with the start
    // scene (B27 point 4)
    m_startColour = false;
    const int wasAt = int(set.indexOf(m_override));
    if (set.contains(colour) == false)
        learnColour(colour, m_colour);   // runde 357: a tile tapped on is his taste
    if (set.contains(colour))
        set.removeAll(colour);
    else
        set.append(colour);
    // R370: FADE and CHASE keep two tiles at least - the last tap but one is
    // refused (the tile blinks); AUTO lets them all go
    if (m_colourMode != 0 && set.count() < 2)
    {
        emit colourRejected(colour);
        return;
    }
    if (set.count() <= 1)
    {
        setColourOverride(set.isEmpty() ? QString() : set.first());
        return;
    }
    // the lead stays; taken out, the NEXT tile in order leads (review 305:
    // it jumped back to the first)
    QString lead = set.contains(m_override) ? m_override
                 : set.at(qBound(0, wasAt, int(set.count()) - 1));
    // white is punctuation (REGLER: "en hvid base ser ud som arbejdslys"): in a
    // set it is worn by the other groups, never the base's lead (review 307)
    if (lead == QStringLiteral("white"))
    {
        foreach (const QString &c, set)
        {
            if (c != QStringLiteral("white"))
            {
                lead = c;
                break;
            }
        }
    }
    m_overrideSet = set;
    m_overrideIdx = int(set.indexOf(lead));
    logSignal(QStringLiteral("sig:colour:") + set.join('+'));   // every change of the set is on the log
    // the spread moves with the set even when the lead stays: a pattern device
    // lets go of the scene it holds, as a lead change makes it (review 307)
    foreach (const QString &pk, m_groupOrder)
    {
        if (m_groups.value(pk).patternDevice)
            m_sectionMotion.remove(pk);
    }
    applyOverride(lead);
    // applyOverride() returns early when the lead did not change: the set did
    if (m_startScene)
        startLook();                     // R378_START_LAYER: the picture fades/chases the set
    emit liveChanged();
}

QString TrackEngine::setPartnerOf(int leadIdx) const
{
    // The look's partner from the tiles (runde 304): the next one after the
    // lead - but with three tiles or more, the first after it that the colour
    // rules pair with the lead (HARMONY's mixesWith), so [red, blue, green]
    // does not stand green by red (review 305). None pairs: the next one.
    const int n = int(m_overrideSet.count());
    if (n < 2)
        return QString();
    const QString lead = m_overrideSet.at(((leadIdx % n) + n) % n);
    if (n > 2)
    {
        for (int k = 1; k < n; k++)
        {
            const QString c = m_overrideSet.at((leadIdx + k) % n);
            if (engineColoursPair(lead, c))
                return c;
        }
    }
    return m_overrideSet.at((leadIdx + 1) % n);
}

void TrackEngine::setColourMode(int mode)
{
    // R370_COLOUR_MODE (Tobias 10-06: "fade og chase ... naar man trykker paa
    // fade/chase, kan man vaelge flere farver (maaske med at den standard
    // vaelger to farver for en, saa man ikke trykker og der ikke sker noget)")
    mode = qBound(0, mode, 2);
    if (mode == m_colourMode)
        return;
    if (mode != 0 && m_overrideSet.count() < 2)
    {
        // the colour leading now and the engine's own partner for it - the
        // tiles light, and he can change them from there
        QString lead = m_override.isEmpty() ? m_colour : m_override;
        if (lead.isEmpty() || lead == QStringLiteral("white") || engineBannedColour(lead)
            || m_palette.contains(lead) == false)
            lead = firstRoomColour();
        QString partner = accentFor(lead, false);
        if (partner.isEmpty() || partner == lead)
        {
            foreach (const QString &c, m_palette)
            {
                if (c != lead && c != QStringLiteral("white") && engineBannedColour(c) == false)
                {
                    partner = c;
                    break;
                }
            }
        }
        if (lead.isEmpty() || partner.isEmpty())
            return;
        m_startColour = false;
        m_overrideSet = QStringList() << lead << partner;
        m_overrideIdx = 0;
        m_colourMode = mode;
        if (lead != m_override)
            applyOverride(lead);         // logs the set
        else
            logSignal(QStringLiteral("sig:colour:") + m_overrideSet.join('+'));
    }
    else
        m_colourMode = mode;
    logSignal(QStringLiteral("sig:colour-mode:")
              + (mode == 1 ? QStringLiteral("fade") : mode == 2 ? QStringLiteral("chase") : QStringLiteral("auto")));
    if (m_startScene)
        startLook();                     // R378_START_LAYER
    emit liveChanged();
}

void TrackEngine::updateColourLayer(int beat, const QString &base, bool isBreak, bool isBuild,
                                    bool isDrop, qreal prog, qreal fader, bool frozen, bool jump, qreal kick)
{
    m_layerOwned.clear();
    m_layerBaseKey = base;
    // a set that fell under two by other means (the palette lost a colour, a
    // project load): FADE/CHASE have nothing to turn - back to AUTO
    if (m_colourMode != 0 && m_overrideSet.count() < 2)
    {
        m_colourMode = 0;
        emit liveChanged();
    }
    int style = 0;
    if (m_overrideSet.count() >= 2 && m_startScene == false)
    {
        if (m_colourMode == 1)
            style = 1;
        else if (m_colourMode == 2)
            style = 2;
        // AUTO with two tiles or more: the engine's choice. A break glides, a
        // build walks; a groove glides under 60 % and walks above; a drop the
        // same - but the wide and the heavy drops always glide.
        else if (isBreak)
            style = 1;
        else if (isBuild)
            style = 2;
        else if (isDrop)
            style = (fader < 0.60 || m_dropStyle == 2 || m_dropStyle == 4) ? 1 : 2;
        else
            style = fader < 0.60 ? 1 : 2;
    }
    const int n = int(m_overrideSet.count());
    if (style != m_layerStyle)
    {
        if (m_layerStyle == 0)
            m_layerPos = qreal(m_overrideIdx);   // on from the colour leading now
        m_layerStyle = style;
        emit liveChanged();
    }
    if (style == 0)
    {
        if (m_mixGlide == false)         // runde 374: the mix's glide runs on it too
            m_layerTimer.stop();
        m_layerBeat = beat;
        m_layerRate = 0.0;
        return;
    }
    // THE PACE IS THE SLIDER'S (Tobias: "Ideen skal maaske endda virke hele
    // vejen igennem energi-slideren"). A fade: one colour into the next over
    // 16 bars at the bottom, 8 from 30 %, 4 from 60 %, 2 from 85 %. A chase:
    // the whole group every four bars at the bottom, the halves trading every
    // bar from 30 %, a walk down the row every two beats from 60 % and every
    // beat from 75 %. A break at half the pace, the second half of a build at
    // twice.
    qreal stepBeats;
    if (style == 1)
        stepBeats = 4.0 * (fader < 0.30 ? 16 : fader < 0.60 ? 8 : fader < 0.85 ? 4 : 2);
    else
    {
        m_layerPattern = fader < 0.30 ? 0 : fader < 0.60 ? 1 : 2;
        stepBeats = fader < 0.30 ? 16 : fader < 0.60 ? 4 : fader < 0.75 ? 2 : 1;
    }
    if (isBreak)
        stepBeats *= 2.0;
    else if (isBuild && prog > 0.5)
        stepBeats = qMax(1.0, stepBeats / 2.0);
    // R374_LAYER_SPEED (Tobias: "den skal vel bare bruge den speed knap vi
    // allerede har"): 1/2x a step takes twice as long, 2x half - a chase
    // never faster than the beat
    if (m_speed < 0)
        stepBeats *= 2.0;
    else if (m_speed > 0)
        stepBeats = style == 2 ? qMax(1.0, stepBeats / 2.0) : stepBeats / 2.0;
    // R374_CHASE_KICK: from 60 % a chase steps on the kick - a beat with no
    // kick (a breakdown, a vocal) holds the colours where they are. Not in a
    // build: its chase is the climb, kick or no kick.
    const bool kickHold = style == 2 && fader >= 0.60 && isBuild == false
                       && kick >= 0.0 && kick < 0.20;
    if (beat != m_layerBeat)
    {
        // the beat that ended, at the pace it had (a slider move changes the
        // pace from here, never the colour already showing)
        if (m_layerBeat >= 0 && kickHold == false)
            m_layerPos += m_layerRate;
        m_layerBeat = beat;
        m_layerLeadAge++;
        // the drop's landing and NEXT: straight to the next colour - a cut
        if (jump && frozen == false)     // R373_HOLD_JUMP
        {
            m_layerPos = std::floor(m_layerPos) + 1.0;
            m_layerLeadAge = 16;
        }
    }
    m_layerRate = frozen ? 0.0 : 1.0 / stepBeats;   // HOLD, CALM, ENERGY 0: the colour stands
    if (m_layerPos >= 1000.0 * n)
        m_layerPos -= 1000.0 * n;
    // the lead - the lasers, the accent, the log - is the colour the base
    // shows most of now. Under a CHASE it turns every four bars at most: the
    // laser bars change colour as they always have (Tobias: "lasere skifter
    // bare farver som nu"), not on every step of the walk
    int lead = int(std::floor(m_layerPos + (style == 1 ? 0.5 : 0.0))) % n;
    if (m_overrideSet.at(lead) == QStringLiteral("white"))
        lead = (lead + 1) % n;
    if (m_overrideSet.at(lead) != m_override && (style == 1 || m_layerLeadAge >= 16))
    {
        m_layerLeadAge = 0;
        m_overrideIdx = lead;
        m_override = m_overrideSet.at(lead);
        m_colour = m_override;
        m_accentPick.clear();
        foreach (const QString &pk, m_groupOrder)
        {
            if (m_groups.value(pk).patternDevice)
                m_sectionMotion.remove(pk);
        }
        emit liveChanged();
    }
    if (style == 1)
    {
        if (m_layerTimer.isActive() == false)
            m_layerTimer.start();
    }
    else if (m_mixGlide == false)        // runde 374
        m_layerTimer.stop();
    emit colourFadeChanged();
}

// R374_FADE_SHOWN: the base's fade, as the FADE tile draws it
qreal TrackEngine::colourFadeT() const
{
    if (m_layerStyle != 1)
        return 0.0;
    qreal frac = 0.0;
    if (m_beatMs > 0.0)
        frac = qBound(0.0, qreal(m_clock.elapsed() - m_beatStartMs) / m_beatMs, 1.0);
    const qreal pos = m_layerPos + frac * m_layerRate;
    return qBound(0.0, pos - std::floor(pos), 1.0);
}

QString TrackEngine::colourFadeFrom() const
{
    const QStringList seq = layerColours(m_layerBaseKey);
    if (m_layerStyle != 1 || seq.count() < 2)
        return QString();
    const int n = int(seq.count()), i = int(std::floor(m_layerPos));
    return seq.at(((i % n) + n) % n);
}

QString TrackEngine::colourFadeTo() const
{
    const QStringList seq = layerColours(m_layerBaseKey);
    if (m_layerStyle != 1 || seq.count() < 2)
        return QString();
    const int n = int(seq.count()), i = int(std::floor(m_layerPos)) + 1;
    return seq.at(((i % n) + n) % n);
}

void TrackEngine::applyMixGlide(bool frame)
{
    // R374_MIX_GLIDE: the room's colour into the next track's, at equal power
    const QString key = m_mixGlideKey;
    if (key.isEmpty())
        return;
    qreal p = m_mixGlideP;
    if (frame && m_beatMs > 0.0)
        p += qBound(0.0, qreal(m_clock.elapsed() - m_beatStartMs) / m_beatMs, 1.0) * m_mixGlideRate;
    p = qBound(0.0, p, 1.0);
    const qreal lvl = m_layerLevel.value(key, 1.0);
    const quint32 fa = colourFunction(key, colourForGroup(key, m_mixGlideFrom));
    const quint32 fb = colourFunction(key, colourForGroup(key, m_mixGlideTo));
    if (fb == Function::invalidId() || fb == fa)
    {
        // R376_GLIDE_SAME: one scene for both (a wheel's stand-in), or none for
        // the next colour: the cosine took the base dark - it stays at full
        run(QStringLiteral("col:") + key, fa, m_funcs.value(fa).dimmer ? lvl : 1.0, 0, true);
        stopSlot(QStringLiteral("colx:") + key, true);
        return;
    }
    const qreal wa = std::cos(p * M_PI / 2.0), wb = std::sin(p * M_PI / 2.0);
    run(QStringLiteral("col:") + key, fa, wa * (m_funcs.value(fa).dimmer ? lvl : 1.0), 0, true);
    if (fb != Function::invalidId() && fb != fa)
        run(QStringLiteral("colx:") + key, fb, wb * (m_funcs.value(fb).dimmer ? lvl : 1.0), 0, true);
    else
        stopSlot(QStringLiteral("colx:") + key, true);
}

QStringList TrackEngine::layerColours(const QString &key) const
{
    // white is punctuation: never the base's (REGLER), in the others' turns
    QStringList seq;
    foreach (const QString &c, m_overrideSet)
    {
        if (key == m_layerBaseKey && c == QStringLiteral("white"))
            continue;
        if (engineBannedColour(c))
            continue;
        seq << c;
    }
    return seq;
}

bool TrackEngine::layerGroup(const QString &key) const
{
    // the RGB groups: the washes, the heads, the strobes. The laser bars and
    // the animation lasers change colour as they always have (Tobias 10-06:
    // "lasere skifter bare farver som nu").
    if (m_layerStyle == 0)
        return false;
    const TrackGroup &g = m_groups.value(key);
    if (g.lasers || g.patternDevice || g.rgb == false)
        return false;
    const QStringList seq = layerColours(key);
    if (seq.count() < 2)
        return false;
    foreach (const QString &c, seq)
    {
        if (colourFunction(key, c) == Function::invalidId())
            return false;
    }
    return true;
}

void TrackEngine::applyColourLayer(const QString &key)
{
    applyColourLayer(key, false);
}

void TrackEngine::applyColourLayer(const QString &key, bool frame)
{
    const QStringList seq = layerColours(key);
    const int n = int(seq.count());
    if (n < 2)
        return;
    // R371_LAYER_FIDS: the colour scenes are looked up on the beat; a frame
    // between two beats uses what the beat found
    if (frame == false || m_layerFids.value(key).count() != n)
    {
        QList<quint32> fids;
        foreach (const QString &c, seq)
            fids << colourFunction(key, c);
        m_layerFids.insert(key, fids);
    }
    const QList<quint32> fidList = m_layerFids.value(key);
    const TrackGroup &g = m_groups.value(key);
    const qreal lvl = m_layerLevel.value(key, 1.0);
    auto levelOf = [&](quint32 fid, qreal w) { return w * (m_funcs.value(fid).dimmer ? lvl : 1.0); };
    if (m_layerStyle == 1)
    {
        // FADE: the two colours crossfade at equal power (both at 0.71 in
        // the middle, so the mix is not a dip) - or through dark where the
        // mix would be yellow
        qreal frac = 0.0;
        if (m_beatMs > 0.0)
            frac = qBound(0.0, qreal(m_clock.elapsed() - m_beatStartMs) / m_beatMs, 1.0);
        qreal pos = m_layerPos + frac * m_layerRate;
        // from 30 % the other groups run half a colour ahead of the base:
        // one arriving while the other leaves
        if (key != m_layerBaseKey && m_faderNow >= 0.30)
            pos += 0.5;
        const int i = int(std::floor(pos));
        const qreal t = pos - qreal(i);
        const int ia = ((i % n) + n) % n, ib = (((i + 1) % n) + n) % n;
        const QString a = seq.at(ia);
        const QString b = seq.at(ib);
        // R373_FADE_FREE (Tobias 10-06): the colours he chose may pass any
        // colour on the way - red -> green through yellow too - only the
        // tiles themselves are held to the palette
        const qreal wa = std::cos(t * M_PI / 2.0), wb = std::sin(t * M_PI / 2.0);
        const quint32 fa = fidList.at(ia), fb = fidList.at(ib);   // R371
        run(QStringLiteral("col:") + key, fa, levelOf(fa, wa), 0, true);
        if (fb != Function::invalidId() && b != a)    // R372_COLX_READY: at nought until its turn
            run(QStringLiteral("colx:") + key, fb, levelOf(fb, wb), 0, true);
        else
            stopSlot(QStringLiteral("colx:") + key, true);
        return;
    }
    // CHASE: on the beat, hard - each group a step on from the last, so the
    // rig is never one colour; on a row of lamps the colours walk it
    const int k = int(std::floor(m_layerPos)) + qMax(0, int(m_groupOrder.indexOf(key)));
    const int lamps = int(g.fixtures.count());
    quint32 f = Function::invalidId();
    if (m_layerPattern > 0 && lamps >= 2)
    {
        QStringList perLamp;
        for (int i = 0; i < lamps; i++)
        {
            // halves: left against right; the walk: pairs of lamps, a lamp on each step
            const int idx = m_layerPattern == 1 ? (i * 2 / lamps) + k : (i + k) / 2;
            perLamp << seq.at(((idx % n) + n) % n);
        }
        f = chaseColourFunction(key, perLamp);
    }
    if (f == Function::invalidId())
        f = fidList.at(((k % n) + n) % n);   // R371
    run(QStringLiteral("col:") + key, f, levelOf(f, 1.0), 0, true);
    stopSlot(QStringLiteral("colx:") + key, true);
}

quint32 TrackEngine::chaseColourFunction(const QString &group, const QStringList &perLamp)
{
    // one colour per lamp, from the group's own colour scenes - every channel
    // of that lamp but its master dimmer (the parts own that), so the shutter
    // and the modes stay as each colour scene sets them. Hidden, made once.
    // R371_CHASE_PAIR: two scenes per group, not one per combination - the
    // one showing keeps its values, the other is written and shown next
    const QString want = perLamp.join(QLatin1Char(','));
    const int side = m_chaseSide.value(group, 0);
    const QString showKey = group + QLatin1Char('|') + QString::number(side);
    if (m_chaseShown.value(showKey) == want && m_chaseScenes.contains(showKey)
        && m_doc->function(m_chaseScenes.value(showKey)) != nullptr)
        return m_chaseScenes.value(showKey);
    const int other = 1 - side;
    const QString ckey = group + QLatin1Char('|') + QString::number(other);
    const TrackGroup &g = m_groups.value(group);
    if (perLamp.count() != g.fixtures.count())
        return Function::invalidId();
    QList<SceneValue> values;
    for (int i = 0; i < int(g.fixtures.count()); i++)
    {
        const quint32 fxid = g.fixtures.at(i);
        Fixture *fxi = m_doc->fixture(fxid);
        Scene *src = qobject_cast<Scene *>(m_doc->function(colourFunction(group, perLamp.at(i))));
        if (fxi == nullptr || src == nullptr)
            return Function::invalidId();
        const quint32 dim = dimmerChannel(fxi);
        foreach (const SceneValue &sv, src->values())
        {
            if (sv.fxi == fxid && sv.channel != dim)
                values.append(sv);
        }
    }
    if (values.isEmpty())
        return Function::invalidId();
    const QString name = ENGINE_COLOUR_PREFIX + QStringLiteral("%1 chase %2").arg(group, other == 0 ? QStringLiteral("A") : QStringLiteral("B"));
    Scene *scene = m_chaseScenes.contains(ckey) ? qobject_cast<Scene *>(m_doc->function(m_chaseScenes.value(ckey))) : nullptr;
    if (scene == nullptr)
    {
        foreach (Function *func, m_doc->functions())
        {
            if (func != nullptr && func->name() == name)
                scene = qobject_cast<Scene *>(func);
        }
    }
    if (scene != nullptr)
    {
        // QLC+ saves a hidden scene's values as nought: written again
        foreach (SceneValue old, scene->values())
            scene->unsetValue(old.fxi, old.channel);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
    }
    else
    {
        scene = new Scene(m_doc);
        scene->setName(name);
        scene->setVisible(false);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
        if (m_doc->addFunction(scene) == false)
        {
            delete scene;
            return Function::invalidId();
        }
    }
    m_chaseScenes.insert(ckey, scene->id());
    m_chaseShown.insert(ckey, want);
    m_chaseSide.insert(group, other);
    return scene->id();
}

void TrackEngine::slotLayerTimer()
{
    // R378_START_LAYER: the opening picture's fade or chase, on a clock - a
    // colour per 32 s, a chase step per 8 s (16 and 4 bars at 120); SPEED
    // halves or doubles it
    if (m_startScene)
    {
        if (m_layerStyle == 0 || m_layerOwned.isEmpty() || m_docTimer.isActive() || m_building)
        {
            m_layerTimer.stop();
            return;
        }
        const qint64 now = m_clock.elapsed();
        qreal per = m_layerStyle == 1 ? 32000.0 : 8000.0;
        if (m_speed < 0)
            per *= 2.0;
        else if (m_speed > 0)
            per /= 2.0;
        m_layerPos += qreal(now - m_startLayerMs) / per;
        m_startLayerMs = now;
        const int n = int(m_overrideSet.count());
        if (n > 0 && m_layerPos >= 1000.0 * n)
            m_layerPos -= 1000.0 * n;
        foreach (const QString &key, m_layerOwned)
        {
            if (m_active.contains(QStringLiteral("col:") + key) == false)
                continue;
            if (m_flash && m_flashHeld.contains(key))
                continue;
            applyColourLayer(key, true);
        }
        emit colourFadeChanged();
        return;
    }
    // the fade between two beats; tick() decides each beat which groups it paints
    // R371: ... and when the beats stop (a paused deck, SHOW OFF): tick()
    // starts it again with the next beat
    const bool stale = m_beatMs <= 0.0
                    || qreal(m_clock.elapsed() - m_beatStartMs) > qMax(2000.0, 4.0 * m_beatMs);
    if ((m_layerStyle != 1 && m_mixGlide == false) || m_docTimer.isActive() || m_building || stale || m_startScene)
    {
        m_layerTimer.stop();
        return;
    }
    if (m_layerStyle == 1)
    {
        foreach (const QString &key, m_layerOwned)
        {
            if (m_active.contains(QStringLiteral("col:") + key) == false)
                continue;                // stopped since the beat (a flash, the cast)
            if (m_flash && m_flashHeld.contains(key))
                continue;
            applyColourLayer(key, true); // R371: the scene ids the beat found
        }
        emit colourFadeChanged();        // runde 374: the FADE tile's bar
    }
    // R374_MIX_GLIDE: the base between two beats
    if (m_mixGlide && m_mixGlideKey.isEmpty() == false
        && m_active.contains(QStringLiteral("col:") + m_mixGlideKey)
        && (m_flash && m_flashHeld.contains(m_mixGlideKey)) == false)
        applyMixGlide(true);
}

void TrackEngine::applyOverride(const QString &colour)
{
    if (colour == m_override)
        return;
    m_override = colour;
    // the accent was drawn to go with the colour before the tile; a tile, or
    // letting one go, is a new room colour (runde 171)
    m_accentPick.clear();
    // ... and so was the next track's colour in a mix: drawn against the
    // room colour the tile has just replaced (runde 198)
    m_nextColour.clear();
    // ... and a pattern device lets go of its held scene: it keeps a scene in
    // a partner colour through the section (runde 171), and only a ROOM
    // colour change (changeColour) released it - a tile sets m_colour here,
    // so the red pattern ran on in the blue room until the next section
    // (runde 179)
    foreach (const QString &pk, m_groupOrder)
    {
        if (m_groups.value(pk).patternDevice)
            m_sectionMotion.remove(pk);
    }
    logSignal(colour.isEmpty() ? QStringLiteral("sig:colour-auto")
                               : QStringLiteral("sig:colour:") + m_overrideSet.join('+'));
    m_startColour = false;               // a tile the DJ tapped is theirs
    if (colour.isEmpty() == false)
        m_colour = colour;
    // AUTO after the WHITE tile: white is punctuation, never the room's own
    // colour - the room stood white until the next change, at a low fader
    // several minutes (runde 205)
    else if (engineBannedColour(m_colour) || m_colour == QStringLiteral("white"))
        m_colour = firstRoomColour();
    if (m_startScene)
        startLook();                     // the opening picture follows the tiles
    else
        emit liveChanged();
}

QString TrackEngine::currentColour() const { return m_colour; }
QStringList TrackEngine::cast() const
{
    QStringList list = m_cast.values();
    list.sort();
    return list;
}

qreal TrackEngine::master() const { return m_master; }

// MASTER as it reaches the light: the operator's fader times the closing lid
// (runde 211). The held FLASH button stays at full - it is the operator's hand.
qreal TrackEngine::masterOut() const { return m_master * m_closingDim * brakeDim(); }   // runde 358: and a brake

void TrackEngine::setMaster(qreal level)
{
    level = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(level + 1.0, m_master + 1.0))
        return;
    m_master = level;

    // everything that is lit, not just the hidden dimmer scenes: a group whose
    // colour scene carries the intensity, or that has no dimmer channel at
    // all, used to ignore this fader until the next beat - or for ever
    reapplyLevels();
    emit liveChanged();
}

bool TrackEngine::blackout() const { return m_blackout; }

void TrackEngine::setBlackout(bool on)
{
    if (on == m_blackout)
        return;
    m_blackout = on;
    logSignal(on ? QStringLiteral("sig:blackout") : QStringLiteral("sig:blackout-off"));
    if (on)
        stopEcho();
    if (on && m_testTimer.isActive())   // a test under BLACKOUT tests nothing (runde 202)
        selfTest();
    if (on)
        labShutdown(false);             // R410_LAB

    applyGroupOff();          // it owns both masks: the off ones and the black ones

    // and the levels: reapplyLevels() knows about MASTER, the group trim, the
    // pulse AND the blackout clamp. The hand-rolled loop that used to be here
    // only handled the dim: slots, so a colour scene that carried its own
    // dimmer came back at full when blackout was released - at MASTER 50 %
    // the room jumped to full, and during a START scene it stayed there.
    reapplyLevels();
    // and onto what is fading out
    foreach (quint32 fid, m_fadeAttr.keys())
    {
        Function *func = m_doc->function(fid);
        if (func != nullptr)
            func->adjustAttribute(m_blackout ? 0.0 : m_fadeLevel.value(fid, 0.0), m_fadeAttr.value(fid));
    }
    emit liveChanged();
}

bool TrackEngine::mixing() const { return m_mixing; }

void TrackEngine::setMixing(bool on)
{
    if (on == m_mixing)
        return;
    m_mixing = on;
    // A mix that begins while a track plays is this track going OUT: from
    // here the colour is frozen and, from bar 4-8 of the mix, the base turns
    // to the next track's colour (tick()). The cast is not faded out - the
    // section and ENERGY decide it, as in the rest of the track (runde 270:
    // this used to promise a sixteen-bar fade that no longer exists). A mix that is on when the next track loads is that
    // track coming IN, and trackLoaded() clears the mark - an intro is not
    // faded out.
    m_mixBeat = (on && m_lastState.isEmpty() == false) ? m_lastBeat : -1;
    emit liveChanged();
}

static QString engineSettingsPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                  + QDir::separator() + "QLC+";
    QDir().mkpath(dir);
    return dir + QDir::separator() + "track-settings.json";
}

QString TrackEngine::exportSettings()
{
    // roles, stars, thresholds, banned colours, FULL AUTO, hold, accent ...
    // everything under trackengine/ and trackmanager/, as one JSON file
    // ... with tonight's table in it (fejljagt 09-27): the stage counts tick()
    // adds are only in memory until the next trackLoaded()/rate(), and this
    // file is the only backup. saveRoles() keeps an unbuilt table untouched.
    saveRoles();
    QSettings settings;
    QJsonObject obj;
    foreach (const QString &key, settings.allKeys())
    {
        if (key.startsWith(QStringLiteral("trackengine/")) == false && key.startsWith(QStringLiteral("trackmanager/")) == false)
            continue;
        // tonight's state is not a setting: carried to another night, it
        // would only be thrown away by its own date stamp (runde 184)
        if (key == SETTINGS_ENGINE_NIGHT || key == SETTINGS_ENGINE_ROOMAUTO || key == SETTINGS_ENGINE_GROUPTRIM
            || key == QStringLiteral("trackmanager/showran"))
            continue;
        QVariant v = settings.value(key);
        switch (v.typeId())
        {
            case QMetaType::Bool:   obj.insert(key, v.toBool()); break;
            case QMetaType::Int:    obj.insert(key, v.toInt()); break;
            case QMetaType::Double: obj.insert(key, v.toDouble()); break;
            default:                obj.insert(key, v.toString()); break;
        }
    }
    // QSaveFile: written beside the real one and moved into place, so a crash
    // halfway leaves the previous export - the only backup of the ratings,
    // roles and bans - rather than an empty file (runde 176)
    QSaveFile file(engineSettingsPath());
    if (file.open(QIODevice::WriteOnly) == false)
        return tr("could not write %1").arg(file.fileName());
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (file.commit() == false)
        return tr("could not write %1").arg(file.fileName());
    return tr("saved %1 settings to %2").arg(obj.count()).arg(file.fileName());
}

QString TrackEngine::importSettings()
{
    QFile file(engineSettingsPath());
    if (file.open(QIODevice::ReadOnly) == false)
        return tr("no %1").arg(file.fileName());
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();
    if (err.error != QJsonParseError::NoError || doc.isObject() == false)
        return tr("not a settings file: %1").arg(err.errorString());

    QSettings settings;
    QJsonObject obj = doc.object();
    int n = 0;
    foreach (const QString &key, obj.keys())
    {
        if (key.startsWith(QStringLiteral("trackengine/")) == false && key.startsWith(QStringLiteral("trackmanager/")) == false)
            continue;
        QVariant v = obj.value(key).toVariant();
        if (v.isValid() == false)
            continue;                      // a null in the file would read back as 0
        if (key == SETTINGS_TRACK_PORT && (v.toInt() <= 0 || v.toInt() > 65535))
            continue;
        if (key == SETTINGS_TRACK_TRIM && (v.toInt() < 0 || v.toInt() > 200))
            continue;
        if ((key == SETTINGS_TRACK_BPMLOW || key == SETTINGS_TRACK_BPMHIGH)
            && (v.toInt() <= 0 || v.toInt() > 300))
            continue;
        if (key == SETTINGS_ENGINE_HOLDBARS)
            v = qBound(4, v.toInt(), 128);
        // runde 196: tonight's state is not a setting (export leaves it out,
        // so does import); the role-mode switch has no control to undo it;
        // quantise and the kick thresholds get the bounds they have elsewhere
        if (key == SETTINGS_ENGINE_NIGHT || key == SETTINGS_ENGINE_ROOMAUTO || key == SETTINGS_ENGINE_GROUPTRIM
            || key == QStringLiteral("trackmanager/showran") || key == QStringLiteral("trackmanager/rolemode"))
            continue;
        if (key == QStringLiteral("trackmanager/quantize"))
            v = qBound(1, v.toInt(), 32);
        if (key == QStringLiteral("trackmanager/dropkick"))
            v = qBound(0.30, v.toDouble(), 0.90);
        if (key == QStringLiteral("trackmanager/breakkick"))
            v = qBound(0.05, v.toDouble(), 0.50);      // the learner's ceiling (runde 197)
        settings.setValue(key, v);
        n++;
    }
    // take them on board: roles, stars and options are read in ensureTable
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = qBound(4, settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt(), 128);   // as setHoldBars (runde 220)
    m_holdAuto = settings.value(SETTINGS_ENGINE_HOLDAUTO, true).toBool();
    loadClockCurve(settings);
    m_closingOn = settings.value(SETTINGS_ENGINE_CLOSING, true).toBool();
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_logEnabled = settings.value(SETTINGS_ENGINE_LOG, true).toBool();
    m_groupOff.clear();
    foreach (QString key, settings.value(SETTINGS_ENGINE_GROUPOFF, QString()).toString().split(';', Qt::SkipEmptyParts))
        m_groupOff.insert(key);
    m_dirty = true;
    // the file's roles REPLACE the ones in memory: ensureTable() carries a
    // function's in-memory role over, and loadRoles() only overrides the ids
    // the file names - a hand-set role the file did not have survived the
    // import and was saved again (runde 196)
    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
        it.value().role = -2;
    // rebuild NOW, not on the next table() call: setFullAuto(), rebuild() and
    // a doc change all save the in-memory table before they rebuild, and
    // until this rebuild has run the in-memory table is the OLD verdicts -
    // one FULL AUTO toggle in that window would write them over the import.
    // And the first such toggle is the one four lines down: setFullAuto()
    // saves, so it has to run AFTER the table holds the imported values, or
    // it writes the old roles, stars, verdicts, stage counts and bans back
    // over the file that was just read - while reporting "loaded N".
    ensureTable();
    bool wantAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
    if (wantAuto != m_fullAuto)
    {
        m_fullAuto = !wantAuto;            // let the setter do its teardown
        setFullAuto(wantAuto);
    }
    m_moves.clear();
    // the imported group switches, on stage now - between tracks no beat
    // would apply them, and the tiles said the opposite (runde 196)
    applyGroupOff();
    if (m_startScene)
        startLook();
    // an imported clock curve moves ENERGY now, not on the next beat (runde
    // 209) - and LAST (runde 210): roomChanged can take the start scene down
    // and put the idle look up, which must be built from the imported base,
    // switches and roles, not the old ones
    m_roomSent = -1;
    announceRoom();
    emit tableChanged();
    emit liveChanged();
    return tr("loaded %1 settings - restart QLC+ for the Track page's own values").arg(n);
}

int TrackEngine::speed() const { return m_speed; }

void TrackEngine::setSpeed(int speed)
{
    speed = qBound(-1, speed, 1);
    if (speed == m_speed)
        return;
    m_speed = speed;
    // running chases keep their old step until restarted: restart them
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("mot:"))
        {
            stopSlot(slot, true);
            // a group whose chase owned the dimmers had its parts stopped:
            // it stood dark until the next beat (runde 174). The parts take
            // over again until the chase is back.
            const QString group = slotGroup(slot);
            if (m_motionDim.remove(group))
                setDimmer(group, m_moveLevel.value(group, 0.0));
        }
    }
    emit liveChanged();
}

bool TrackEngine::flashing() const { return m_flash; }

void TrackEngine::setFlash(bool pressed)
{
    if (pressed == m_flash)
        return;
    m_flash = pressed;

    if (pressed)
    {
        ensureTable();
        // the manual flash is the strobes in WHITE, whatever the cast and
        // the palette are doing
        QSet<QString> strobeGroups, allOn;
        foreach (const QString &key, m_groupOrder)
        {
            if (m_groupOff.contains(key))
                continue;
            allOn.insert(key);
            if (m_groups.value(key).strobes)
                strobeGroups.insert(key);
        }
        // an automatic hit that is still up (a red one, say) goes first: the
        // button's white must not be laid over it, now that the button is at
        // full and the hit would be too on the next fader move (runde 201)
        stopSlot("flash", true);
        // ... and a hardware strobe burst that is running: driveStrobe() is
        // quiet under FLASH from the next beat, but the burst's scene kept the
        // lamps strobing under the held white until then (runde 221)
        foreach (const QString &key, m_groupOrder)
            stopSlot("str:" + key, true);
        m_strobeUntil = -1;
        quint32 fid = flashFunction(strobeGroups, "white");
        if (fid == Function::invalidId())
            fid = flashFunction(allOn, "white");
        // the same colour guard as the automatic hit: the ranking may hand
        // back a RED scene when there is no white one, and that must not be
        // laid over the generated white below
        if (fid != Function::invalidId())
        {
            const QString fc = m_funcs.value(fid).colour;
            if (fc.isEmpty() == false && fc != QStringLiteral("white"))
                fid = Function::invalidId();
        }
        if (fid != Function::invalidId())
            run("flash", fid, 1.0, 0, true);
        // ... and the generated white ALWAYS, not only when no scene of the
        // operator's was found (Tobias, 2026-09-22).
        //
        // "Flash Strobes WHITE" is the scene that wins the ranking above, and
        // measured on the show file it writes dimmer 255 and one channel of
        // its own on the three 8+8 strobes - and NO red, green or blue, on
        // any strobe. The three 80-segment strobes it does not touch at all.
        // So the button raised dimmers over whatever colour happened to be
        // running and called it a white flash.
        //
        // genFlash() is what puts the white on: the generated white scene
        // (red, green and blue at 70 % on a strobe - they have no white
        // channel), every strobe group, and the dimmer at full with the
        // group's trim stepped over. The operator's scene still runs on top
        // of it; the two do not fight, they layer.
        genFlash(true);
    }
    else
    {
        const QSet<QString> released = m_flashHeld;
        stopSlot("flash", true);
        genFlash(false);
        // The strobes on stage go back to this beat's picture NOW, not on the
        // next beat: genFlash(false) only cuts the groups outside the cast,
        // so the rest stood at full - lit, static, nothing happening - until
        // the beat came round, and for good if the link had gone quiet
        // (runde 181). A group whose chase owns the dimmers hands them back
        // to it; the others take the level the beat gave them, with the pulse.
        foreach (const QString &key, released)
        {
            if (m_cast.contains(key) == false)
                continue;                        // genFlash(false) cut those
            const TrackGroup &g = m_groups.value(key);
            if (m_motionDim.contains(key))
            {
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
            }
            else if (g.hasDimmer)
                setDimmer(key, m_moveLevel.value(key, 0.0));
        }
        // a white room: the flash ran on the very scene "col:" holds, and its
        // level override stayed at full - MASTER and the trim back on now,
        // not on the next beat (runde 215)
        reapplyLevels();
    }
    emit liveChanged();
}

void TrackEngine::slotBeatWatch()
{
    // No tick for a beat and a half (runde 302): the one-beat events end as
    // the next beat would have ended them. The held FLASH button is the
    // operator's and stays; its own release does this (setFlash).
    bool changed = false;
    if (m_flash == false)
    {
        const QSet<QString> released = m_flashHeld;
        if (m_active.contains(QStringLiteral("flash")) || released.isEmpty() == false)
            changed = true;
        stopSlot(QStringLiteral("flash"), true);
        if (released.isEmpty() == false)
            genFlash(false);
        // the strobes on stage go back to their level now, as setFlash(false)
        foreach (const QString &key, released)
        {
            if (m_cast.contains(key) == false)
                continue;                        // genFlash(false) cut those
            const TrackGroup &g = m_groups.value(key);
            if (m_motionDim.contains(key))
            {
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
            }
            else if (g.hasDimmer)
                setDimmer(key, m_moveLevel.value(key, 0.0));
        }
    }
    foreach (const QString &key, m_groupOrder)
    {
        const QString slot = QStringLiteral("str:") + key;
        if (m_active.contains(slot))
        {
            stopSlot(slot, true);
            changed = true;
        }
    }
    m_strobeUntil = -1;
    if (changed)
    {
        reapplyLevels();
        emit liveChanged();
    }
}

QString TrackEngine::report() const { return m_report; }

/*********************************************************************
 * Choosing
 *********************************************************************/

int TrackEngine::candidateGate() const
{
    // The only inputs of candidates() that move between two rebuilds of the
    // table: the fader for "dryp", the build's length and SPEED for "climb"
    // (the same arithmetic buildCandidates() used to run per entry).
    int gate = m_faderNow < 0.995 ? 1 : 0;
    if (m_speed == 0 && (m_buildLen == 16 || m_buildLen == 32))
        gate |= 2;
    if (m_speed == 0 && m_buildLen == 32)
        gate |= 4;
    return gate;
}

void TrackEngine::invalidateCandidates()
{
    m_candIndex.clear();
    m_homeCache.clear();
    m_sharedLooks = -1;
}

QList<TrackFuncInfo *> TrackEngine::candidates(int role, const QString &group) const
{
    // The walk over the whole table (0.5-4.5 ms a call at 11,500 functions,
    // 30-45 calls a beat) is done once per (role, group) per table now.
    // Mid-rebuild m_funcs is being refilled: nothing is kept then.
    QList<CandEntry> fresh;
    const QList<CandEntry> *entries = &fresh;
    if (m_building)
        fresh = buildCandidates(role, group);
    else
    {
        const QString key = QString::number(role) + QLatin1Char('|') + group;
        QHash<QString, QList<CandEntry> >::const_iterator ci = m_candIndex.constFind(key);
        if (ci == m_candIndex.constEnd())
            ci = m_candIndex.insert(key, buildCandidates(role, group));
        entries = &ci.value();
    }
    const int gate = candidateGate();
    QList<TrackFuncInfo *> out;
    out.reserve(entries->count());
    for (const CandEntry &e : *entries)
    {
        // "DrypDryp" only at a full fader; a climb only where it fits
        if ((gate & 1) && e.dryp)
            continue;
        if (e.climb && (gate & (e.climbLong ? 4 : 2)) == 0)
            continue;
        out.append(e.info);
    }
    return out;
}

QList<TrackEngine::CandEntry> TrackEngine::buildCandidates(int role, const QString &group) const
{
    QList<CandEntry> out;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.role != role)
            continue;
        // Hard, and hard on purpose: no weighting, no floor, no "unless
        // nothing else is left". The operator said never.
        if (info.banned)
            continue;
        if (info.frozen)
            continue;                    // it can never step: nothing to follow the music with
        // "DrypDryp" (the animation lasers) only at a full fader. Tobias,
        // 2026-09-25: "skal kun bruges paa 100% energi. Den er alt for vild
        // til alt andet." By name, as the break's "vifte" is (tierOf).
        // (Per call, in candidates(): CandEntry::dryp and candidateGate().)
        // A BUILD PROGRAMME ("... Climb", runde 227) rises from sparse to
        // full over its loop - 16 beats, or 32 for "Long". It belongs in a
        // build and nowhere else, and only with room left to reach the top
        // before the drop: m_buildLen is the beats still to go.
        // Its top lands on the drop only when the beats left are whole
        // passes, and only at the drawn tempo - the SPEED tiles stretch or
        // halve it (runde 230, review).
        // runde 287 (Tobias: "du bestemmer hvad der er bedst"): at most
        // the last 32 beats. A 16-beat Climb started over eight times in
        // a 128-beat build - the sawtooth runde 237 took out of the level.
        // A long build opens static, chases from its middle, and the
        // climb joins where 32 (or 16) are left, as an odd build does.
        // (Per call, in candidates(): CandEntry::climb and candidateGate().)
        // Per-group slots must never start a whole-room snapshot. Its other
        // groups would bypass cast, colour and intensity decisions. Such
        // looks remain available as START scenes and on the Virtual Console.
        // (Runde 218: tested BEFORE the laser block - both are pure
        // `continue` tests, so the order cannot change the result, but this
        // one is cheap and throws away most of the table.)
        if (group.isEmpty() == false
            && (info.groups.contains(group) == false || info.groups.count() != 1))
            continue;
        // LASER SAFETY (runde 190, Tobias: "ja, lav vagten"). A programme
        // that steers pan/tilt on the laser bars runs only as a POSITION,
        // where the 40/60 % rules are measured every beat. Roles are kept per
        // function id and win over the guess, so a renumbered show or a slip
        // in SETUP could hand a tilt dive to MOTION, FLASH or IDLE - where
        // nothing measures it. The animation lasers (pattern devices) are
        // not bars.
        if (role != ENGINE_ROLE_POSITION && info.aims)
        {
            bool bars = false;
            foreach (const QString &lg, info.groups)
            {
                // constFind: value() copied the whole TrackGroup (runde 218)
                QMap<QString, TrackGroup>::const_iterator gi = m_groups.constFind(lg);
                if (gi != m_groups.constEnd() && gi->lasers && gi->patternDevice == false)
                {
                    bars = true;
                    break;
                }
            }
            if (bars)
                continue;
        }
        if (m_doc->function(info.id) == nullptr)
            continue;
        if (userAllowed(info, group) == false)
            continue;
        // the laser bars' MOTION never switches on the bar's own effects
        // (runde 303, ownEffectOf) - the operator's programmes only: ours
        // hold those channels at nought (gen_programs bar_lit / bar_tilt)
        if (role == ENGINE_ROLE_MOTION && group.isEmpty() == false && info.generated == false
            && info.path.startsWith(ENGINE_AUTO_PATH) == false)
        {
            QMap<QString, TrackGroup>::const_iterator bg = m_groups.constFind(group);
            if (bg != m_groups.constEnd() && bg->lasers && bg->patternDevice == false
                && ownEffectOf(info.id, group))
                continue;
        }
        CandEntry e;
        e.info = const_cast<TrackFuncInfo *>(&info);
        e.dryp = info.name.contains(QStringLiteral("dryp"), Qt::CaseInsensitive);
        e.climb = info.name.contains(QStringLiteral("climb"), Qt::CaseInsensitive);
        e.climbLong = e.climb && info.name.contains(QStringLiteral("long"), Qt::CaseInsensitive);
        out.append(e);
    }
    // NOT by id. The order of this list is the order the cursor walks, and
    // the cursor moves ONE TO THREE PLACES per section (m_motionCursor).
    // Sorted by id, the list is the order the generator wrote the file in -
    // and the generator writes a figure's variants next to each other:
    // "Wash Wave", "Wash Wave Back", "Wash Wave Fade", then "Wash Colour
    // Cycle", "Wash Colour Cycle Fade", "Wash Colour Random" ... So a step of
    // one to three lands on a VARIANT OF THE SAME FIGURE. Measured on the
    // show file of 2026-09-17: in the wash's groove pool, 48 % of the places
    // one to three ahead carry the same family; in the drop pool 56 %, on the
    // strobes 46-49 %. That is the report, with arithmetic under it - "de
    // samme med forskellige farver" and "strobe-lysene ... blinker naesten
    // altid ens".
    //
    // The family rule in motionFor() cannot save it: it drops the family that
    // just ran, and `cursor % other.count()` then lands in the SAME
    // neighbourhood - the next figure in file order, whose variants are what
    // the following sections walk through.
    //
    // So neighbours in the list must not be neighbours in the file. Sorted by
    // a hash of the name, a step of one to three is a step to an unrelated
    // figure: the same measurement gives 4-15 %. The hash is our own and
    // fixed, not qHash(): Qt 6 seeds qHash per process, and an order that
    // changes on every launch cannot be reproduced when a night goes wrong.
    // Id breaks the tie, so the order is total and stable.
    // (sorted before candidates() filters: dropping entries keeps the order)
    std::sort(out.begin(), out.end(), [](const CandEntry &a, const CandEntry &b) {
        return a.info->scatter != b.info->scatter ? a.info->scatter < b.info->scatter
                                                  : a.info->id < b.info->id;
    });
    return out;
}

bool TrackEngine::lightsGroup(quint32 fid, const QString &group) const
{
    // Does this scene actually put light on this group? A scene named for a
    // colour that sets nothing here - the wrong fixtures, or all zeroes -
    // used to be picked anyway, and the group simply went black. That is what
    // "cyan does not work, it is just black" was.
    Scene *scene = qobject_cast<Scene *>(m_doc ? m_doc->function(fid) : nullptr);
    if (scene == nullptr)
        return true;                     // a chase or an EFX: cannot tell from here
    // our own: built from this group's fixtures and only kept when at least
    // one of them took the colour (ensureColourScenes: touched > 0). A
    // macro-colour group's generated scene has no Intensity channel in it at
    // all - the dimmer is the parts' job - so the test below refused every
    // colour the engine made for the laser bars, and they ran uncoloured
    if (m_funcs.value(fid).generated)
        return true;
    const TrackGroup &g = m_groups.value(group);
    foreach (SceneValue sv, scene->values())
    {
        if (sv.value == 0 || g.fixtures.contains(sv.fxi) == false)
            continue;
        Fixture *fxi = m_doc->fixture(sv.fxi);
        const QLCChannel *ch = fxi != nullptr ? fxi->channel(sv.channel) : nullptr;
        if (ch != nullptr && ch->group() == QLCChannel::Intensity)
            return true;
        // A COLOUR WHEEL at a non-zero value is light on a fixture like the
        // laser bars: nought on that channel means no beam, so anything else
        // means a beam. Their colour scenes carry nothing else since the
        // dimmer came out of them (gen_programs.py, runde 71) - and without
        // this line the scenes would depend entirely on the wheel having been
        // LEARNED as a colour channel below. That learning works today, but
        // it is one step in a chain, and a broken link there means the bars
        // stand dark all night with nothing in the log to say why.
        if (ch != nullptr && ch->group() == QLCChannel::Colour)
            return true;
        // a learned colour channel is this group's colour, whatever the
        // definition calls it
        if (g.colourValue.value(sv.fxi).contains(sv.channel))
            return true;
    }
    return false;
}

bool TrackEngine::groupHasColour(const QString &group, const QString &colour) const
{
    if (colour.isEmpty())
        return false;
    foreach (TrackFuncInfo *info, candidates(ENGINE_ROLE_COLOR, group))
    {
        if (info->colour == colour && lightsGroup(info->id, group))
            return true;
    }
    return false;
}

QString TrackEngine::colourForGroup(const QString &group, const QString &colour) const
{
    // A lamp with a colour WHEEL has the colours the wheel has - seven on the
    // laser bars, four on the animation lasers - while the palette is built
    // from every colour any group can do. Ask a wheel for orange and nothing
    // comes back: colourFunction() returns nothing, the colour slot is
    // stopped, and a group that is in the cast stands dark. That is one more
    // reason the bars "do not light" (found 2026-09-16), and it is silent -
    // the group is eligible, it just cannot be that colour tonight.
    //
    // So: the room's colour if the group has it, otherwise the nearest one it
    // does have. Neighbours round the wheel, warm to warm and cold to cold;
    // white last, because white is punctuation rather than a colour.
    // (The table is called `neighbours`, not `near`: `near` and `far` are
    // legacy Windows macros from windef.h and MinGW rejects them as names.
    // CI caught it, 2026-09-16.)
    if (colour.isEmpty())
        return colour;
    // the colours this group has, from ONE scan of the candidates: it was one
    // scan per colour asked - up to six a group a beat for a wheel without the
    // room's colour, over some 5,500 functions (runde 204). Same test as
    // groupHasColour().
    const QList<TrackFuncInfo *> own = candidates(ENGINE_ROLE_COLOR, group);
    QSet<QString> has;
    foreach (TrackFuncInfo *info, own)
    {
        if (info->colour.isEmpty() == false && has.contains(info->colour) == false
            && lightsGroup(info->id, group))
            has.insert(info->colour);
    }
    if (has.contains(colour))
        return colour;
    static const QMap<QString, QStringList> neighbours =
    {
        { "amber",   { "yellow", "orange", "red", "white" } },
        { "orange",  { "amber", "yellow", "red", "white" } },
        { "yellow",  { "amber", "orange", "green", "white" } },
        { "purple",  { "magenta", "uv", "blue", "white" } },
        { "uv",      { "purple", "blue", "magenta", "white" } },
        { "pink",    { "magenta", "red", "white" } },
        { "magenta", { "pink", "purple", "red", "blue" } },
        { "red",     { "amber", "orange", "magenta", "white" } },
        { "green",   { "cyan", "yellow", "white" } },
        { "cyan",    { "blue", "green", "white" } },
        { "blue",    { "cyan", "purple", "uv", "white" } },
        { "white",   { "cyan", "yellow", "blue" } },
    };
    // A banned colour is never a substitute. Yellow is banned
    // (engineBannedColour) because Tobias will not have it as a colour a lamp
    // stands in - "vi skal aldrig bruge gul som stand-alone farve, det er
    // simpelthen bare en grim farve" (2026-09-16) - and the first version of
    // this table handed it to the bars for every orange and amber. The ban is
    // about a lamp standing in the colour; the yellow inside a two-colour or
    // per-eye programme is not this, and is untouched.
    foreach (const QString &c, neighbours.value(colour))
    {
        if (engineBannedColour(c) == false && has.contains(c))
            return c;
    }
    // Nothing near it either: any colour of its own, rather than a group that
    // is in the cast and contributing nothing.
    foreach (TrackFuncInfo *info, own)
    {
        if (info->groups.count() == 1 && info->colour.isEmpty() == false
            && engineBannedColour(info->colour) == false
            && lightsGroup(info->id, group))
            return info->colour;
    }
    return colour;
}

quint32 TrackEngine::colourFunction(const QString &group, const QString &colour) const
{
    QList<TrackFuncInfo *> list = candidates(ENGINE_ROLE_COLOR, group);

    // exactly this group, exactly this colour - and of those, the one that
    // lights the most fixtures ("LaserCyan", not "1onCYAN")
    TrackFuncInfo *best = nullptr;
    foreach (TrackFuncInfo *info, list)
    {
        if (info->groups.count() == 1 && info->colour == colour
            && lightsGroup(info->id, group)
            && (best == nullptr || info->fixtureCount > best->fixtureCount
                || (info->fixtureCount == best->fixtureCount && best->generated && info->generated == false)))
            best = info;
    }
    if (best != nullptr)
        return best->id;
    // anything of this colour that includes the group (a collection)
    foreach (TrackFuncInfo *info, list)
    {
        if (info->colour == colour && lightsGroup(info->id, group))
            return info->id;
    }
    // a colourless look for this group (the group has no named colours) -
    // one that LIGHTS it: "AUTO Bars Eyes Clear" is all noughts, a COLOR
    // candidate, and would have blacked the bars out (runde 176)
    foreach (TrackFuncInfo *info, list)
    {
        if (info->groups.count() == 1 && info->colour.isEmpty() && lightsGroup(info->id, group))
            return info->id;
    }
    return Function::invalidId();
}

quint32 TrackEngine::motionFunction(const QString &group, const QString &colour,
                                    const QSet<QString> &cast, int cursor) const
{
    // kept for the header's sake; the engine calls the tiered version below
    return motionFor(group, colour, cast, cursor, -1, 0.0, 1, false, 3);
}

// ONE partner colour per look (runde 243). "farverne ... skal passe sammen,
// altid" (Tobias, 2026-09-22) - and with the two-colour library of runde
// 238-241 every group drew its own partner: a blue room could stand in blue,
// cyan, magenta and white at once, plus the accent (modelled: 3+ colours in
// 23 % of hot drops with an accent, up to five). A two-colour programme fits
// when both its colours are the room's or the look's partner.
static bool pairFits(const TrackFuncInfo &info, const QString &room, const QString &partner)
{
    if (info.partner.isEmpty())
        return true;
    return (info.colour == room || info.colour == partner)
        && (info.partner == room || info.partner == partner) && info.colour != info.partner;
}

quint32 TrackEngine::motionFor(const QString &group, const QString &colour,
                               const QSet<QString> &cast, int cursor, int tier,
                               qreal bpm, int division, bool staticOnly, int maxStars,
                               qreal litFloor) const
{
    Q_UNUSED(bpm)
    Q_UNUSED(division)
    QList<TrackFuncInfo *> all = candidates(ENGINE_ROLE_MOTION, group);
    QList<TrackFuncInfo *> ok;
    // runde 292: a pattern device follows the ceiling too, down to its own
    // calmest patterns - it was exempt, and the top-star doubling below then
    // drew its wildest patterns at 35 % twice as often as at 100 %
    const bool patternGroup = m_groups.value(group).patternDevice;
    const bool layerG = layerGroup(group) || (m_mixGlide && group == m_mixGlideKey);   // R370 (R374: the mix's glide)
    const bool aimHeld = positionHeld(group);     // R378_POSITIONS
    int calmest = 3;
    if (patternGroup)
    {
        foreach (TrackFuncInfo *info, all)
            calmest = qMin(calmest, qMax(1, info->stars));
    }
    foreach (TrackFuncInfo *info, all)
    {
        if (aimHeld && info->aims)       // R378_POSITIONS: the held aim stands
            continue;
        // R410_LAB: a kept lab look runs in the ENERGY window it was given
        if (info->lab && (m_faderNow + 1e-6 < info->labMin || m_faderNow - 1e-6 > info->labMax))
            continue;
        // R398_DOWN_NOZOOMPROG: ... and under STRAIGHT DOWN the beam is the
        // hold's - no programme that zooms by itself (runde 220's chasers)
        if (aimHeld && m_positionMode == QStringLiteral("down")
            && info->name.contains(QStringLiteral("zoom"), Qt::CaseInsensitive))
            continue;
        // a static pattern scene is a look and may show in any section; a
        // chase or EFX is movement and belongs to drops and builds
        if (staticOnly && info->type != int(Function::SceneType))
            continue;
        // A chase that lights NOTHING in any step cannot be a look, and since
        // the dimmers are handed to the chase (runde 69) it would hold the
        // group dark for the whole section. The show had one: "Bars Eyes
        // Clear", two steps of nothing but zeroes, which is housekeeping
        // rather than a programme - and it was a candidate in every colour.
        if (info->type != int(Function::SceneType) && info->litShare <= 0.0)
            continue;
        // ... nor, on the strobes, one that puts the whole bank on full (runde
        // 338, fullBankOf): "fjern det der faar strobe-lamperne til at gaa paa
        // 100% dimmer". Their accent is the walk and the hardware shutter.
        if (m_groups.value(group).strobes && info->fullBank)
            continue;
        // R370: under the tiles' fade or chase the group's colour is the
        // layer's - a programme that paints colours of its own would cover it
        if (layerG && info->setsColour)
            continue;
        // How much of the group a chase has to leave lit. This matters far
        // more since the dimmers were handed over (runde 69): before, the
        // engine's own parts held the light up and a chase that walks one
        // head of seven was simply invisible; now it is real, and on the BASE
        // that is the room going dark - the thing Tobias has reported more
        // than any other. So the base asks for a floor in every section, not
        // only in a break, and the floor follows what the engine's own
        // figures already do (patternMask keeps the heads at 0.35).
        // ... unless at least ONE lamp is on in every step (runde 259, Tobias:
        // "det vigtigste er bare, at der altid er mindst en lampe på" -
        // "chases hvor lamperne skiftevis fader fra 0-100 skal jo gerne virke
        // også"): the room is never dark under it, and that is the promise
        if (litFloor > 0.0 && info->type != int(Function::SceneType)
            && info->litShare < litFloor && info->peakLit < ENGINE_OWN_FLOOR)
            continue;
        // energy stars: a three-star chase waits for a full-energy drop.
        // A BREAK is always ceiling 1, and that is one rule too many: it also
        // threw out every break programme whose name happens to carry no cool
        // word - the operator's own quiet pattern scenes among them. The
        // animation lasers' "Fladvifteboelge" is exactly the scene Tobias
        // asked breaks to be able to show (2026-09-10, "animations laser fra
        // vores egne programmer paa flad vifte"), and it was excluded every
        // time. A programme the name already files as a BREAK programme may
        // be two stars in a break; three stays out.
        int allow = maxStars;
        if (tier == 0 && info->tier == 0)
            allow = qMax(allow, 2);
        // ... except on a PATTERN DEVICE. Its scenes are its only light: an
        // animation laser with every pattern above the ceiling is in the cast
        // and dark, which is the one thing this engine promises never to do.
        // At the bottom of the fader (ceiling 1) all of its scenes are two
        // stars, so the group had nothing to show - check_reach: "Animation
        // Laser groove ceil 1: EMPTY". It shows its calmest instead.
        if (qMax(1, info->stars) > (patternGroup ? qMax(allow, calmest) : allow))
            continue;
        // a motion that also lights groups outside the cast is not allowed
        bool inside = true;
        foreach (const QString &g, info->groups)
        {
                if (cast.contains(g) == false)
                    inside = false;
        }
        if (inside == false)
            continue;
        // A PATTERN DEVICE - an animation laser - is its scenes: the figure
        // and the colour are the same file, and the engine has no way to
        // recolour one. Holding them to the room's colour left the group with
        // nothing to show whenever the room was a colour they do not have,
        // and in a BREAK that was every time: their one break scene is
        // "ANIMATION Flad vifte (hvid default)", tagged white, and white is
        // never the room's colour. So the scene Tobias asked breaks to be
        // able to show (2026-09-10, "animations laser ... paa flad vifte")
        // has never once run. The exact-colour preference below still puts
        // the right colour first where there is one.
        if (info->colour.isEmpty() || info->colour == colour
            || m_groups.value(group).patternDevice)
            ok.append(info);
    }
    // R410_LAB_ONLY: six kept looks or more, and the animation laser plays his
    // own from the lab - the busking scenes it borrowed step aside. Only when
    // one passes here: it is never dark for it.
    if (patternGroup && m_labOnlyGroups.contains(group))
    {
        QList<TrackFuncInfo *> labOwn;
        foreach (TrackFuncInfo *info, ok)
        {
            if (info->lab)
                labOwn.append(info);
        }
        if (labOwn.isEmpty() == false)
            ok = labOwn;
    }
    // a pattern device is never in the cast and dark (review 294): when the
    // ceiling left it nothing that passes the other filters, it may show
    // any of its patterns - its calmest were counted over all candidates
    if (ok.isEmpty() && patternGroup && maxStars < 3)
        return motionFor(group, colour, cast, cursor, tier, bpm, division, staticOnly, 3, litFloor);
    if (ok.isEmpty())
        return Function::invalidId();

    // the pattern made in this colour beats the colourless one, which would
    // otherwise overwrite the palette with its own colour channel
    // A programme in this colour, or one that has no colour of its own to
    // impose: a chase that only moves dimmers takes whatever the room is
    // wearing, so it belongs in every colour's pool. That is worth a great
    // deal of file: one dimmer chase does the work of seven coloured ones.
    // What this still keeps out is the old trap - a chase that writes its own
    // red over the room's blue without saying "red" in its name.
    // `colour` is ALREADY the colour this group can show: tick() runs it
    // through colourForGroup() before calling here, so a wheel that has no
    // orange has been handed red. Do not substitute again - a second lookup
    // here was added on 2026-09-20 and was a pure no-op that cost a whole
    // candidates() sweep per group per beat.
    QList<TrackFuncInfo *> exact;
    foreach (TrackFuncInfo *info, ok)
    {
        if (info->colour == colour
            || (info->colour.isEmpty() && info->setsColour == false))
            exact.append(info);
    }
    if (exact.isEmpty() == false)
        ok = exact;
    // What is left when `exact` is empty is not another colour - the loop
    // above only ever let in this colour and the colourless - it is the
    // programmes with NO colour word in the name that paint colours of their
    // own anyway: "Bars Colour Cycle", "Bars Eyes Rainbow", "Bars Colour
    // Random". On a group whose colour IS a channel value those are LTP and
    // overwrite the group's colour scene outright, so the room asks for blue
    // and the bars run a rainbow. Better no programme at all: the group keeps
    // its colour scene and its figure, lit and moving in the right colour.
    //
    // (This is NOT what Tobias saw on 2026-09-20 - "laser-bars farver er
    // lidt for random". Measured in that night's log the bars followed the
    // room on every beat except the wheel substitution orange -> red, and
    // the 37 beats they were the accent group. Both are by design. The
    // guard stays because the rainbow case is real; the report is not
    // explained by it, and nothing was changed on the strength of it.)
    //
    // ... but NOT on a pattern device, whatever its group is called. Sixteen
    // lines above, a pattern device is deliberately admitted in every colour,
    // with the reason spelled out: holding it to the room's colour left it
    // with nothing to show whenever the room was a colour it does not have.
    // This branch then threw that away again, because `lasers` is set from
    // the GROUP NAME - `g.lasers = low.contains("laser")` - and the group is
    // called "Animation Laser". So the exemption written directly above has
    // never once taken effect.
    //
    // Measured 2026-09-22 (runde 130) on the show file: the animation
    // lasers have twelve red motion scenes, ten blue, five white, four cyan
    // - and ONE green and ONE magenta, neither of them a motion. In green or
    // magenta `exact` is therefore empty, and the group was handed nothing
    // for the whole section. In a break that is the only extra group a break
    // is allowed to light, so the break stood on the base alone.
    //
    // The guard itself is right for the laser BARS: their colour is a
    // channel value, and a rainbow programme would overwrite the colour
    // scene outright. A pattern device has no colour scene to overwrite -
    // the scene IS the colour.
    //
    // RUNDE 170 - and not only on the bars. Tobias, 2026-09-22: "som jeg
    // tidligere har sagt skal farverne vi koerer, passe sammen, altid." What
    // this branch is left with is by construction ONLY programmes that paint
    // a colour of their own without naming it - on this show the 37 rainbows
    // and colour cycles (3-6 colours each: "Wash Groove Rainbow", "Bars
    // Colour Cycle", "Wash Pulse Colour" ...) and the like. On the heads, the
    // strobes and the Minis that was still a way in: never measured to happen
    // (check_reach finds no situation where `exact` is empty), but a way in.
    // No group runs one now; the group keeps its colour scene and its figure.
    else if (m_groups.value(group).patternDevice == false)
        return Function::invalidId();
    else
    {
        // A PATTERN DEVICE with no scene in the room's colour may show
        // another - but only one that GOES WITH the room: the same pairs
        // accentFor() hands the accent group, and white. Measured on the show
        // (runde 130): red 12, blue 10, white 5, cyan 4, green 1, magenta 1,
        // so in a green room this branch could hand the animation laser a RED
        // pattern - red on green, the one pairing the colour rules leave out
        // on purpose. Every room colour still has a partner here (green ->
        // cyan, magenta -> blue/white, orange -> red, red -> magenta/blue), so
        // the group is not left dark by this on this rig. White only where
        // HARMONY allows it: under blue, magenta and cyan (B22).
        static const QMap<QString, QStringList> goesWith =
        {
            { "blue",    { "white", "cyan" } },
            { "red",     { "magenta", "blue" } },    // B22: HARMONY - white only under blue/magenta/cyan
            { "cyan",    { "magenta", "white" } },
            { "green",   { "cyan" } },
            { "magenta", { "blue", "white" } },
            { "white",   { "blue", "cyan" } },
            { "orange",  { "amber", "red" } },
            { "amber",   { "red", "blue" } },
            { "uv",      { "magenta", "blue" } },          // B26/runde 329: no white under uv
        };
        const QStringList partners = goesWith.value(colour);
        QList<TrackFuncInfo *> fits;
        foreach (TrackFuncInfo *info, ok)
        {
            // a scene with no colour word paints a colour we cannot name -
            // it is not a partner of anything
            const bool fitsLook = m_partnerPick.isEmpty() ? partners.contains(info->colour)
                                : (info->colour == m_partnerPick || info->colour == m_colour);
            if (fitsLook && engineBannedColour(info->colour) == false)
                fits.append(info);
        }
        if (fits.isEmpty())
            return Function::invalidId();
        ok = fits;
    }

    // only the look's two colours (runde 243, pairFits): single-colour
    // programmes and the two-colour ones in the room + the look's partner.
    // Nothing left: the group runs the engine's own look in the room colour.
    {
        QList<TrackFuncInfo *> fitting;
        foreach (TrackFuncInfo *info, ok)
        {
            if (pairFits(*info, m_colour, m_partnerPick))
                fitting.append(info);
        }
        if (fitting.isEmpty())
            return Function::invalidId();
        ok = fitting;
    }

    // In a build, the build's own programmes first (runde 227, Tobias: "Byg
    // 1, rigtige build programmer"). The rest of the pool stays behind them
    // for a group that has none in this colour.
    if (m_buildLen > 0)
    {
        QList<TrackFuncInfo *> climbs;
        foreach (TrackFuncInfo *info, ok)
        {
            if (info->name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
                climbs.append(info);
        }
        if (climbs.isEmpty() == false)
            ok = climbs;
    }

    // this tier's motions first
    QList<TrackFuncInfo *> tagged;
    foreach (TrackFuncInfo *info, ok)
    {
        if (info->tier == tier)
            tagged.append(info);
    }
    // An ANIMATION LASER in a break had one pattern to play (runde 252,
    // BACKLOG 92): only a name with a break word ("vifte") is tier 0, so
    // Fladviftebølge ran 1265 of 1289 break beats on 25-26 Sep and every
    // intro and outro - thirteen minutes of one picture. When the break has
    // one pattern or none, the operator's other CALM patterns join it: flat
    // fans, small waves, static waves, Fingre and Flower. Tobias (runde 252b):
    // "fingre skal hellere komme i breaks end 'wave' - wave er ret vild.
    // Flower er også OK til breaks." Never the wild ones: Wave..., moving,
    // strobish, kanoner, FY FY, DrypDryp (Flower is allowed although its name
    // says Moving).
    // RUNDE 339: ALWAYS, not only with one pattern or none. Fladviftebølge
    // still ran 79 % of the break beats on 10-02 - "Flad vifte (hvid)" and the
    // FLAT scenes counted as the break's own, so the calm ones never joined.
    // Tobias, 10-03: "jeg synes bare det er ensformigt".
    if (tier == 0 && m_groups.value(group).patternDevice)
    {
        static const QStringList calmWords = { "flat", "flad", "static", "satic", "smallwave",
                                               "bølge", "boelge", "vifte", "fingre", "flower" };
        static const QStringList wildWords = { "moving", "strobish", "fy fy", "dryp", "kanon" };
        foreach (TrackFuncInfo *info, ok)
        {
            if (tagged.contains(info))
                continue;
            // R410_LAB: a kept lab look by its CALM tag, never by its name
            if (info->lab)
            {
                if (info->labCalm)
                    tagged.append(info);
                continue;
            }
            const QString lowName = info->name.toLower();
            bool calmOne = false;
            bool wildOne = lowName.startsWith(QStringLiteral("wave"));   // WaveBlue, WaveRed ...
            foreach (const QString &w, calmWords)
                calmOne = calmOne || lowName.contains(w);
            foreach (const QString &w, wildWords)
                wildOne = wildOne || lowName.contains(w);
            if (lowName.contains(QStringLiteral("flower")))
                wildOne = false;
            if (calmOne && wildOne == false)
                tagged.append(info);
        }
    }
    if (tagged.isEmpty() == false)
        ok = tagged;
    else if (tier >= 0)
    {
        // No programme of this tier (runde 233: the Minis have no break
        // programmes, yet the operator can make them the base - then every
        // break fell through to the whole pool, and the doubled top star
        // favoured the DROP figures: Mini Wide Hammer 104 of 104 beats in
        // breaks). The nearest tier instead, and in a break the calm ones.
        // Only when something is left, so no group goes without.
        QList<TrackFuncInfo *> nearby;
        QList<TrackFuncInfo *> calm;
        foreach (TrackFuncInfo *info, ok)
        {
            if (info->tier >= 0 && qAbs(info->tier - tier) >= 2)
                continue;
            nearby.append(info);
            if (tier != 0 || qMax(1, info->stars) <= 1)
                calm.append(info);
        }
        if (calm.isEmpty() == false)
            ok = calm;
        else if (nearby.isEmpty() == false)
            ok = nearby;
    }

    // Not the same FIGURE twice in a row. The per-programme cooldown counts
    // names, and the eye counts shapes: "Row Outer In" following "Row Trade"
    // is a new name and the same picture. In the log of 2026-09-17, 88 of
    // the wash's 110 programme changes were Row to Row.
    //
    // Soft, like the cooldown it sits beside: if nothing else is left, the
    // family comes back in rather than the group standing still. And only
    // when the group HAS another figure to offer - a group with one family
    // is not improved by being denied it.
    const QString lastFam = m_lastFamily.value(group);
    if (lastFam.isEmpty() == false)
    {
        QList<TrackFuncInfo *> other;
        foreach (TrackFuncInfo *info, ok)
        {
            if (info->family != lastFam)
                other.append(info);
        }
        if (other.count() >= 2)
            ok = other;
    }

    // Composition also applies to real show programmes, not just the masks.
    // Foundation/support favour broad, slow pictures; only the rhythmic lead
    // draws freely from the full menu. An empty shortlist uses the generated
    // look, rather than introducing a second fast chase to fill a slot.
    if (m_fullAuto && m_compositionTier > 0 && group != m_rhythmLead)
    {
        QList<TrackFuncInfo *> support;
        // runde 313: a strobe group steps on the slider (strobePaceFloor) -
        // two beats from 30 %, one from 50 %, eighths from 75 % in a drop -
        // and is a rhythm instrument, not a light: no lit share asked
        const bool strobeGroup = m_groups.value(group).strobes;
        const qreal strobeFloor = strobeGroup ? strobePaceFloor(tier) : 2.0;
        foreach (TrackFuncInfo *info, ok)
        {
            if (strobeGroup)
            {
                // (runde 327: 5 % slack - a chase timed in milliseconds reads
                // 0.996 of a beat on a deck pitched a hair off its tempo, and
                // divisionFor() snaps it to the beat when it runs)
                if (info->type == int(Function::SceneType) || stepBeats(*info, bpm) >= strobeFloor * 0.95)
                    support.append(info);
                continue;
            }
            if (info->type == int(Function::SceneType)
                || ((info->litShare >= (group == m_compositionBase && baseCovered() == false ? 0.60 : 0.50)
                     || info->peakLit >= ENGINE_OWN_FLOOR)      // runde 259: one lamp always on
                    && stepBeats(*info, bpm) >= 2.0))
                support.append(info);
        }
        // runde 316: over 75 % in a drop the kick has spoken (strobePaceFloor)
        // - PREFER that pace, not only allow it: a soft kick gets the eighth
        // chases, a hard one the chases that step on every beat. The slower
        // ones stay as the fallback when the pool has none at that pace.
        if (strobeGroup && tier == 2 && m_faderNow >= 0.75 && support.isEmpty() == false)
        {
            QList<TrackFuncInfo *> atPace;
            foreach (TrackFuncInfo *info, support)
            {
                if (info->type != int(Function::SceneType)
                    && qAbs(stepBeats(*info, bpm) - strobeFloor) < strobeFloor * 0.05)
                    atPace.append(info);
            }
            if (atPace.isEmpty() == false)
                support = atPace;
        }
        if (support.isEmpty())
        {
            // Pattern devices have no generated substitute: keep an eligible
            // pattern rather than turning a selected group dark.
            if (m_groups.value(group).patternDevice == false)
                return Function::invalidId();
        }
        else
            ok = support;
    }

    // RUNDE 338 (Tobias, after the night of 10-02: "laser-bars i de hoeje
    // energier brugte for meget 'et-oeje' chases"). Measured on that night's
    // log: from 80 % on the slider the bars stood with a sixth of their beams
    // or less - one bar, or one eye per bar - 47 % of the time, half of it the
    // show's own "Eyes Single", "Row Run", "Comet" programmes. From 60 % in a
    // groove or a drop the programmes that keep a third of the beams lit or
    // more come first (beamShareOf); the thin ones stay as the fallback when
    // nothing else is left, so the bars are never left without a programme.
    {
        const TrackGroup &lg = m_groups.value(group);
        if (lg.lasers && lg.patternDevice == false && tier >= 1 && m_faderNow >= 0.60)
        {
            QList<TrackFuncInfo *> broad;
            foreach (TrackFuncInfo *info, ok)
            {
                if (info->type == int(Function::SceneType) || info->beamShare >= 0.30)
                    broad.append(info);
            }
            if (broad.isEmpty() == false)
                ok = broad;
        }
    }

    // Of what is allowed, the hottest comes up most often - but it does not
    // take the whole draw. This used to DISCARD everything below the top
    // star, and one programme was then enough to empty a pool: the show's own
    // "ROED CHASE Loop Dobbelt hastighed" is three stars (the word "dobbelt")
    // and files as a groove, so at the top of the fader a red groove on the
    // strobes could choose it and nothing else - one chase instead of
    // forty-four, all night. That is the "it is the same programmes again"
    // report, from 2026-09-15, with a name on it.
    // The top star goes in TWICE instead: the same idiom as the tier pool
    // above, and the same effect where the pools are healthy (219 hot drop
    // programmes against 42 calm ones is still nine drops in ten).
    int top = 0;
    foreach (TrackFuncInfo *info, ok)
        top = qMax(top, qMax(1, info->stars));
    // Runde 290 (Tobias: "alt skal skalere efter energi-slideren ...
    // programmer"): above 60 % the top star goes in once more for every
    // fifth of the slider (1 extra copy as before up to 60 %, 3 at 100 %) -
    // with the ceiling at three stars from ~75 % in a drop, the pool was the
    // same from there to the top. And the PACE: a programme whose step is
    // near the slider's target (4 beats at 30 % down to 1 at 100 %, within
    // half an octave) goes in once more. AUTO chasers keep their own tempo -
    // the slider picks the ones that fit.
    const qreal pf = qBound(0.0, m_faderNow, 1.0);
    const int topCopies = 1 + int(qRound(2.0 * qBound(0.0, (pf - 0.60) / 0.40, 1.0)));
    const qreal paceTarget = 4.0 - 3.0 * qBound(0.0, (pf - 0.30) / 0.70, 1.0);
    QList<TrackFuncInfo *> pool = ok;
    foreach (TrackFuncInfo *info, ok)
    {
        if (qMax(1, info->stars) == top)
        {
            for (int c = 0; c < topCopies; c++)
                pool.append(info);
        }
        const qreal sb = info->type == int(Function::SceneType) ? 0.0 : stepBeats(*info, bpm);
        if (sb > 0.0 && qAbs(std::log2(sb / paceTarget)) <= 0.5)
            pool.append(info);
    }

    return pickWeighted(pool, cursor);
}

QString TrackEngine::firstRoomColour() const
{
    // The palette's first entry, but never WHITE. White is punctuation, not
    // a colour the room wears - the draw in tick() has skipped it since
    // 2026-09-10 ("hvid kun til hits og accenter; en hvid base ser ud som
    // arbejdslys"), and Tobias sharpened it on 2026-09-22: "Hvid (RGB hvid)
    // generelt skal kun bruges i drops."
    //
    // These two fallbacks did not skip it. They only fire when the current
    // colour is banned or empty, and the palette happens to come out
    // alphabetical, so white sits last and they have never landed on it -
    // but that is luck, not a rule, and on the three 80-segment strobes a
    // white base would now be red, green and blue at 70 %.
    foreach (const QString &c, m_palette)
    {
        if (c != QStringLiteral("white"))
            return c;
    }
    return m_palette.isEmpty() ? QString() : m_palette.first();
}

QString TrackEngine::familyOf(const QString &name)
{
    // The eye does not read names, it reads FIGURES. Measured in the log of
    // 2026-09-17: the wash changed programme 110 times and 88 of those were
    // another "Row" figure - "Row Outer In", "Row Inner Out", "Row Trade",
    // "Row Thirds" - so 82 % of the changes looked like the same thing in a
    // different colour. Tobias: "Programmerne der kører er de samme med
    // forskellige farver, de ser alt for ens ud."
    //
    // It is arithmetic, not luck. A COLOURLESS programme sits in every
    // colour's pool; a coloured one sits only in its own. The colourless Row
    // family is 426 of the wash's 472 colourless programmes, against ~121
    // coloured ones per colour - so Row is about 72 % of the pool whatever
    // the room is wearing.
    //
    // The family is the first word that says what the SHAPE is: everything
    // before it is the group, the tier, the colour or a partner tag, and
    // everything after it is a rhythm.
    static const QStringList skip = {
        "auto", "wash", "bars", "mini", "strobes", "laser", "lasers", "eyes4",
        // "row" is what the whole shape family calls itself - "Row Walk",
        // "Row Ripple", "Row Outer In" - so it says nothing about the figure
        "row",
        // ... and "eyes" is the same kind of word: it says the figure lives
        // in the laser bars' per-eye channels rather than in whole bars, not
        // what the figure IS. "eyes4" was already here; plain "eyes" was
        // missed, and it cost more than the omission looks.
        //
        // Measured on the show file 2026-09-22 (runde 129): 1112 of the
        // bars' 1904 programmes - 58 % - came back as one family called
        // "eyes", while the 792 whole-bar ones spread over 34. So the rule
        // right below ("not the same FIGURE twice in a row") did the exact
        // opposite of its job for the bars: after any eye programme it had
        // 34 other families to prefer and would never pick a second one,
        // even though "Row Run" (one beam travelling the room) and "Mirror
        // Out" (every bar opening from its centre) are not remotely the same
        // picture. It pushed the bars back towards the whole-bar blinking
        // that runde 125 was written to get rid of.
        //
        // With "eyes" skipped the family becomes the shape word - run,
        // comet, diagonal, mirror, middle, edges, sparkle, snake - which is
        // what this function says it is looking for.
        "eyes",
        // ... and "bar" (runde 178): "Bar Outer In", "Bar Middle Out", "Bar
        // Run", "Bar Ping-Pong", "Bar Alternate", "Bar Random" - 216 bar
        // programmes, six different figures, one family "bar"; the scope
        // word again, as "row" and "eyes" were
        "bar",
        "break", "groove", "drop", "normal", "medium", "fan",
        "slow", "fast", "calm", "low", "high", "soft", "wide", "far", "cross",
        "red", "green", "blue", "cyan", "magenta", "orange", "white", "yellow",
        "fire", "ember", "lime", "ice", "deep", "rose", "frost",
        "all", "the", "a" };
    static const QRegularExpression nonWord(QStringLiteral("[^a-z0-9]+"));
    const QStringList parts = name.toLower().split(nonWord,
                                                   Qt::SkipEmptyParts);
    foreach (const QString &w, parts)
    {
        if (skip.contains(w))
            continue;
        return w;
    }
    return QString();
}

int TrackEngine::tierOf(const QString &text) const
{
    // "vifte": the animation lasers' flat fan ("ANIMATION Flad vifte",
    // "Fladviftebølge") is the one pattern of theirs a break may show
    static const QStringList breakWords  = { "break", "slow", "center", "centre", "calm", "low", "vifte" };
    static const QStringList grooveWords = { "fan", "groove", "medium", "normal" };
    static const QStringList dropWords   = { "drop", "cross", "high", "wide", "eight", "fast" };
    if (hasWord(text, dropWords))   return 2;
    if (hasWord(text, breakWords))  return 0;
    if (hasWord(text, grooveWords)) return 1;
    return -1;
}

quint32 TrackEngine::homePosition(const QString &group) const
{
    // Asked on every beat for the bars, again inside every laserAimSafe() and
    // laserSweepSafe() - once per laser candidate when positionFunction()
    // draws - and each answer is a candidates() call plus up to two walks of
    // the whole table. The answer only changes with the table (and with the
    // gate, which the first pass reads through candidates()).
    if (m_building)
        return findHomePosition(group);
    const QString key = group + QLatin1Char('|') + QString::number(candidateGate());
    QHash<QString, quint32>::const_iterator hc = m_homeCache.constFind(key);
    if (hc != m_homeCache.constEnd())
        return hc.value();
    const quint32 home = findHomePosition(group);
    m_homeCache.insert(key, home);
    return home;
}

quint32 TrackEngine::findHomePosition(const QString &group) const
{
    // The aim the operator calls "up" - the bars shooting straight out over
    // the room. It is their normal look and the only one they get below the
    // top of the fader, so it has to be found by name rather than by the
    // rotation the cursor happens to land on.
    auto isHome = [](const TrackFuncInfo &info) -> bool
    {
        if (info.type != int(Function::SceneType))
            return false;                      // a chase is a move, not an aim
        QString n = info.name.toLower();
        if (n.contains(QStringLiteral("down")) || n.contains(QStringLiteral("beat"))
            || n.contains(QStringLiteral("wiggle")) || n.contains(QStringLiteral("move")))
            return false;
        // "UP", "laser upp", "Bars Up Home": the word has to stand on its
        // own, or "setup" and "wake up" would match as well.
        // And "LaserUPP" - the name in this show - only reads as a word
        // because of the capitals, which toLower() has already thrown away.
        // So the camel-case boundary is matched against the ORIGINAL name.
        static const QRegularExpression up(QStringLiteral("(^|[^a-z])upp?([^a-z]|$)"),
                                           QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression upCamel(QStringLiteral("[a-z]UPP?([^A-Za-z]|$)"));
        return up.match(n).hasMatch() || upCamel.match(info.name).hasMatch();
    };

    const TrackFuncInfo *best = nullptr;
    foreach (TrackFuncInfo *info, candidates(ENGINE_ROLE_POSITION, group))
    {
        if (isHome(*info) && (best == nullptr || info->fixtureCount > best->fixtureCount))
            best = info;
    }
    if (best != nullptr)
        return best->id;

    // Nothing left, which for this one function means the ban has to give.
    //
    // Home is not a look competing for a turn in the rotation - it is where
    // the beams park. Everything above 60 % energy may roam; below it, in a
    // break, in a build, in CALM, the bars are driven back here. Take that
    // away and the rule quietly stops holding: the bars keep whatever drop
    // aim they were last given, through the whole quiet passage. Banning it
    // is banning the brakes, and it is the one place where "he said never"
    // loses to what the fixtures are pointed at.
    //
    // Only the ban is ignored. Role, group and the name test all still apply,
    // and they are the same test the loop above used - one copy, so the two
    // cannot drift apart.
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin();
         it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.banned == false || info.role != ENGINE_ROLE_POSITION)
            continue;
        if (info.groups.count() != 1 || info.groups.contains(group) == false)
            continue;
        if (isHome(info) && (best == nullptr || info.fixtureCount > best->fixtureCount))
            best = &info;
    }
    if (best != nullptr)
        return best->id;

    // Still nothing - and this is the case the rig has been in ALL ALONG.
    // "LaserUPP" is a STEP in two of the operator's chasers (UPDOWbeatlaser,
    // UPDOWbeatlaserDOWN). A scene that sits inside a chaser gets role -1
    // ("not used": chase steps must not be drawn as programmes), so it is
    // never a POSITION candidate, and the two loops above never saw it. The
    // bars had no home: below the roam line they fell through to the
    // generic pick (2026-09-17: LaserDOWN, 553 beats at the floor), and once
    // laserAimSafe() started measuring against the home (runde 96b/103)
    // every laser aim failed the measurement and the bars got NO aim at all
    // (2026-09-19: not one pos:LaserBars in 3088 beats). Being a step is
    // not a reason for a scene not to be where the beams park: a home is
    // found by its NAME and its values, and by nothing else. Role and ban
    // are ignored here too - the only thing that still has to hold is that
    // it is a scene of this one group, and that it says UP.
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin();
         it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.junk || info.generated)
            continue;
        if (info.groups.count() != 1 || info.groups.contains(group) == false)
            continue;
        if (m_doc == nullptr || m_doc->function(info.id) == nullptr)
            continue;
        if (isHome(info) && (best == nullptr || info.fixtureCount > best->fixtureCount))
            best = &info;
    }
    return best != nullptr ? best->id : Function::invalidId();
}

quint32 TrackEngine::positionFunction(const QString &group, int cursor, int tier, qreal fader) const
{
    QList<TrackFuncInfo *> all = candidates(ENGINE_ROLE_POSITION, group);
    bool lasers = m_groups.value(group).lasers;
    // The laser bars and the fader (Tobias, 2026-09-18): "de skal ikke pege
    // nedad foer energi-slideren er over minimum 60 %, og jo hoejere
    // derefter, jo mere nedad. Indtil 40 % energi skal de slet ikke
    // bevaege sig." The 40 % is the roam gate in tick() (mayRoam); this is
    // the downward allowance: nought below 60 %, then a straight line to
    // the full reach at 100 %. A figure that dips 14 units (the drop dives
    // from gen_programs) is therefore reachable from about 83 %.
    const int down = lasers ? laserDownAllowed(fader) : -1;

    QList<TrackFuncInfo *> safe;
    foreach (TrackFuncInfo *info, all)
    {
        // a laser sweep runs on its own only when its name says it stays low -
        // the rest are there for the operator to choose by hand
        // "low" as a WORD: "Slow" and "Yellow" are not a promise to stay low
        //
        // EVERY candidate, not just chasers and sweeps. A SCENE went straight
        // into this pool with no aim check at all, because it is neither -
        // and the operator's own "LaserDOWN" is a scene, 115 units below the
        // home aim, pointing at the floor. In the log of 2026-09-17 the
        // engine chose it for 26 % of the beats. Everything done the day
        // before to keep the generated tilt figures above the horizontal was
        // bypassed by one scene that was never measured. (2026-09-17.)
        // No word is a pass any more - "low" used to be, and the operator's
        // "LasermovingTest LOW" (an absolute EFX, tilt 130-150 = 4-24 units
        // under the aim) walked through it at any energy. Every laser
        // candidate is MEASURED against the fader's allowance: a scene or a
        // chaser by its steps (laserAimSafe), an EFX by its offset and
        // amplitude (laserSweepSafe).
        if (lasers && (info->sweep ? laserSweepSafe(info->id, group, down) == false
                                   : laserAimSafe(info->id, group, down) == false))
            continue;
        // ... and never one that switches the FIXTURE'S OWN movement macro on
        // (Tobias, 2026-09-15: "de bevaeger sig konstant?? ... du skal bygge
        // dine egne bevaegelser i full-auto"). A bar's "Movement Effect"
        // channel starts a pattern inside the fixture that runs at its own
        // speed for ever, ignores the beat, and is only stopped by writing a
        // zero back. LaserWiggle (ch 8 = 146) and LaserMovewithStops (96) are
        // aim scenes by name, so they were in the position pool - one pick and
        // the bars never stood still again. The engine's own movement is the
        // EFX sweep, which it can start, shape and stop.
        if (lasers && macroPosition(info->id))
            continue;
        // the heads' ballyhoo (runde 254) is the top of the fader only: every
        // head on its own path, up to 44 units off the floor - "Først fra 95%
        // skal de være helt frie" (Tobias, runde 212)
        if (lasers == false && fader < 0.95
            && info->name.contains(QStringLiteral("ballyhoo"), Qt::CaseInsensitive))
            continue;
        // ... and at ENERGY 0 the heads take plain aims, no moving figure
        // (runde 255): a chaser keeps its own pace where the sweep stands still
        if (lasers == false && fader < 0.03 && info->type != int(Function::SceneType))
            continue;
        safe.append(info);
    }
    if (safe.isEmpty())
        return Function::invalidId();
    // in a build, the build's own head figures first (runde 258), as
    // motionFor() does for programmes (runde 227)
    if (lasers == false && m_buildLen > 0)
    {
        QList<TrackFuncInfo *> climbs;
        foreach (TrackFuncInfo *info, safe)
        {
            if (info->name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
                climbs.append(info);
        }
        if (climbs.isEmpty() == false)
            safe = climbs;
    }

    // this tier's looks first, then the untagged ones, then anything
    QList<TrackFuncInfo *> tagged, plain;
    foreach (TrackFuncInfo *info, safe)
    {
        if (info->tier == tier) tagged.append(info);
        else if (info->tier < 0) plain.append(info);
    }
    // the tier's own looks count double, the untagged ones join the pool:
    // twenty positions are only a variety if they all get their turn
    QList<TrackFuncInfo *> pool = tagged + tagged + plain;
    if (pool.isEmpty())
        pool = safe;
    // R405_AIM_DRAW: the heads' aims are drawn by rest. A place on in this
    // list per walk took the heads round the same tour all night (the next
    // aim 60-70 % predictable from 33). The bars keep the cursor (runde 190).
    if (lasers == false)
        return drawAim(group, pool);
    return pickWeighted(pool, cursor);
}

quint32 TrackEngine::drawAim(const QString &group, const QList<TrackFuncInfo *> &pool) const
{
    // R405_AIM_DRAW (runde 405). Every entry of the pool is a ticket - the
    // tier's own aims are in it twice - times how long the aim has rested:
    // 1 for the one before, up to 8 for one not taken in the last eight (or
    // never). Never the aim the heads stand on: a walk is a move. With rating
    // on, the stars weigh in as pickWeighted() lets them.
    if (pool.isEmpty())
        return Function::invalidId();
    const quint32 now = m_position.value(group, Function::invalidId());
    const QHash<quint32, int> taken = m_aimTakenAt.value(group);
    const int seq = m_aimSeq.value(group, 0);
    QList<quint32> ids;
    QList<int> tickets;
    int total = 0;
    foreach (TrackFuncInfo *info, pool)
    {
        if (info->id == now)
            continue;
        const int rest = taken.contains(info->id) ? qBound(1, seq - taken.value(info->id), 8) : 8;
        const int t = rest * (m_ratingOn ? rateWeight(*info) : 1);
        ids.append(info->id);
        tickets.append(t);
        total += t;
    }
    if (ids.isEmpty())
        return now;                          // the pool is the aim they stand on
    int r = int(QRandomGenerator::global()->bounded(total));
    for (int i = 0; i < ids.count(); i++)
    {
        r -= tickets.at(i);
        if (r < 0)
            return ids.at(i);
    }
    return ids.last();
}

bool TrackEngine::laserSweepSafe(quint32 fid, const QString &group, int downAllowed) const
{
    if (m_doc == nullptr)
        return false;
    EFX *efx = qobject_cast<EFX *>(m_doc->function(fid));
    if (efx == nullptr)
        return false;
    Scene *home = qobject_cast<Scene *>(m_doc->function(homePosition(group)));
    if (home == nullptr)
        return false;
    // A relative EFX rides on whatever aim is under it; measured against the
    // home aim, which is where the bars stand when nothing else has them.
    // An absolute one names its own centre. Either way the tilt travels
    // centre +- height (rotation 0; a rotated laser figure is the standing-
    // still bug of round 96, and nothing here makes one).
    int amp = qMax(efx->height(), efx->width());
    // ... and a rotated one reaches w*|sin| + h*|cos| (fejljagt 09-27, from
    // EFX::rotateAndScale): a square or diamond corner turned 45 degrees goes
    // ~1.41 x further than max(w, h). None in the show today; an operator's
    // rotated EFX on the bars would have passed the check and dived.
    if (efx->rotation() % 360 != 0)
    {
        const double r = double(efx->rotation()) * 3.14159265358979323846 / 180.0;
        amp = qMax(amp, int(std::ceil(double(efx->width()) * std::fabs(std::sin(r))
                                      + double(efx->height()) * std::fabs(std::cos(r)))));
    }
    foreach (EFXFixture *ef, efx->fixtures())
    {
        Fixture *fxi = m_doc->fixture(ef->head().fxi);
        if (fxi == nullptr)
            continue;
        int tiltCh = -1, homeTilt = -1;
        for (quint32 i = 0; i < fxi->channels(); i++)
        {
            const QLCChannel *qch = fxi->channel(i);
            if (qch != nullptr && qch->group() == QLCChannel::Tilt && qch->controlByte() == QLCChannel::MSB)
            {
                tiltCh = int(i);
                break;
            }
        }
        if (tiltCh < 0)
            continue;
        foreach (const SceneValue &sv, home->values())
        {
            if (sv.fxi == fxi->id() && int(sv.channel) == tiltCh)
                homeTilt = int(sv.value);
        }
        if (homeTilt < 0)
            return false;                    // a bar the home aim says nothing about
        // a relative EFX adds (yOffset - 127) to whatever is under it -
        // Universe::writeRelative, RELATIVE_ZERO_8BIT - so its own offset
        // counts on top of the home aim; an absolute one names its centre
        int centre = efx->isRelative() ? homeTilt + (efx->yOffset() - 127) : efx->yOffset();
        int top = centre - amp - homeTilt;   // negative = above the aim
        int bottom = centre + amp - homeTilt;
        if (top < -ENGINE_AIM_REACH || bottom > qMax(0, downAllowed))
            return false;
    }
    return true;
}

int TrackEngine::laserDownAllowed(qreal energy)
{
    qreal e = qBound(0.0, energy, 1.0);
    if (e < 0.60)
        return 0;
    return int(qRound(ENGINE_AIM_REACH * (e - 0.60) / 0.40));
}

bool TrackEngine::laserAimSafe(quint32 fid, const QString &group, int downAllowed) const
{
    // The "low" rule below this is a promise made in a NAME: an aim the
    // operator called "low" is one they have checked. Our own generated tilt
    // figures cannot make that promise - "low" is also a break word, and a
    // figure called "Bars Tilt Sway Low" would file itself as a break
    // programme - so all nineteen of them were rejected and the bars have
    // never used one (found 2026-09-16). This is the same promise, measured
    // instead of spelled: every step stays within ENGINE_AIM_REACH of the
    // aim the operator set as home, and writes nothing but pan and tilt.
    if (m_doc == nullptr)
        return false;
    quint32 homeFid = homePosition(group);
    Scene *home = qobject_cast<Scene *>(m_doc->function(homeFid));
    if (home == nullptr)
        return false;                    // no home to measure against: no promise
    QHash<QPair<quint32, quint32>, int> aim;
    foreach (const SceneValue &sv, home->values())
        aim.insert(qMakePair(sv.fxi, sv.channel), int(sv.value));

    QList<quint32> steps;
    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return false;
    Chaser *chaser = qobject_cast<Chaser *>(func);
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            steps.append(step.fid);
        if (steps.isEmpty())
            return false;
    }
    else
        steps.append(fid);

    foreach (quint32 sid, steps)
    {
        if (macroPosition(sid))
            return false;                // writes the fixture's own effect engine
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            return false;
        foreach (const SceneValue &sv, scene->values())
        {
            Fixture *fxi = m_doc->fixture(sv.fxi);
            const QLCChannel *qch = fxi == nullptr ? nullptr : fxi->channel(sv.channel);
            if (qch == nullptr)
                continue;
            if (qch->group() != QLCChannel::Pan && qch->group() != QLCChannel::Tilt)
                continue;                // macroPosition() has already judged the rest
            QPair<quint32, quint32> k = qMakePair(sv.fxi, sv.channel);
            if (aim.contains(k) == false)
                return false;            // an axis the home aim says nothing about
            // UP is a smaller tilt value, DOWN a bigger one (LaserUPP 126,
            // LaserDOWN 242 - the operator's own scenes, and the log of
            // 2026-09-17 confirmed the direction on the rig). Upward the
            // reach is always the full one; downward it is what the caller
            // allows - the fader's decision, see laserDownAllowed().
            int dev = int(sv.value) - aim.value(k);
            bool tilt = qch->group() == QLCChannel::Tilt;
            int belowMax = (tilt && downAllowed >= 0) ? downAllowed : ENGINE_AIM_REACH;
            if (dev < -ENGINE_AIM_REACH || dev > belowMax)
                return false;
        }
    }
    return true;
}

bool TrackEngine::macroPosition(quint32 fid) const
{
    // An aim scene may write pan, tilt (and their fine channels) and the
    // pan/tilt speed. Anything else non-zero on a laser is the fixture's own
    // effect engine - a movement macro, a built-in pattern, a strobe - and
    // once it is on, nothing the engine does moves those beams.
    if (m_doc == nullptr)
        return false;
    Scene *scene = qobject_cast<Scene *>(m_doc->function(fid));
    if (scene == nullptr)
        return false;
    foreach (const SceneValue &sv, scene->values())
    {
        if (sv.value == 0)
            continue;                    // a zero is what turns a macro OFF
        Fixture *fxi = m_doc->fixture(sv.fxi);
        const QLCChannel *qch = fxi == nullptr ? nullptr : fxi->channel(sv.channel);
        if (qch == nullptr)
            continue;
        switch (qch->preset())
        {
            case QLCChannel::PositionPan:
            case QLCChannel::PositionPanFine:
            case QLCChannel::PositionTilt:
            case QLCChannel::PositionTiltFine:
            case QLCChannel::SpeedPanTiltFastSlow:
            case QLCChannel::SpeedPanTiltSlowFast:
                continue;
            default:
                break;
        }
        if (qch->group() == QLCChannel::Pan || qch->group() == QLCChannel::Tilt)
            continue;
        return true;
    }
    return false;
}

bool TrackEngine::ownEffectOf(quint32 fid, const QString &group) const
{
    // (review 305: only the group's own fixtures - a programme that also runs
    // the animation laser's or a strobe's macro is not the bars' own effect)
    const QList<quint32> mine = m_groups.value(group).fixtures;
    // Runde 303 (bane B's headless B23 D2): in plain AUTO the engine took the
    // operator's "Laser Chase 1 Preset Red" as the bars' MOTION - it writes
    // their Effect / Effect Speed channels (79 / 165), the bar's own show,
    // which REGLER keeps at nought ("aldrig barens egen Movement Effect").
    // macroPosition() only judged aims. A channel of the Effect group, or one
    // whose name says "Effect" (the Yuer's "Movement Effect" has no group).
    if (m_doc == nullptr)
        return false;
    QList<quint32> ids;
    Chaser *chaser = qobject_cast<Chaser *>(m_doc->function(fid));
    if (chaser != nullptr)
    {
        foreach (const ChaserStep &step, chaser->steps())
            ids.append(step.fid);
    }
    else
        ids.append(fid);
    foreach (quint32 sid, ids)
    {
        Scene *scene = qobject_cast<Scene *>(m_doc->function(sid));
        if (scene == nullptr)
            continue;
        foreach (const SceneValue &sv, scene->values())
        {
            if (sv.value == 0 || mine.contains(sv.fxi) == false)
                continue;
            Fixture *fxi = m_doc->fixture(sv.fxi);
            const QLCChannel *qch = fxi == nullptr ? nullptr : fxi->channel(sv.channel);
            if (qch == nullptr)
                continue;
            if (qch->group() == QLCChannel::Effect
                || qch->name().contains(QStringLiteral("effect"), Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

QString TrackEngine::drawColour(const QStringList &pool, int keyBias, QRandomGenerator *rng) const
{
    // one colour from the pool - weighted towards the key's side of the
    // wheel when the key is known: minor leans cold (blue, cyan, magenta,
    // purple, uv), major leans warm (red, amber, yellow, orange). Green sits
    // on neither side. Three to one, not all or nothing: a warm track in a
    // cold palette still gets a colour, and a cold night still sees red.
    if (pool.isEmpty())
        return QString();
    // Green: one ticket where any other colour has three (Tobias, 2026-09-26:
    // "en fed farve, men den skal ikke bruges så tit"), known key or not.
    static const QStringList cold = { "blue", "cyan", "magenta", "purple", "uv", "pink" };
    static const QStringList warm = { "red", "amber", "yellow", "orange" };
    QStringList weighted;
    foreach (const QString &c, pool)
    {
        int n = (c == QLatin1String("green")) ? 1 : 3;
        if (keyBias >= 0 && (keyBias == 0 ? cold : warm).contains(c))
            n *= 3;
        // runde 357 (R357_TASTE, Tobias: "lad motoren laere mine valg"): his
        // taste from the colour tiles, in tenths of a ticket
        n = qMax(1, int(qRound(qreal(n) * 10.0 * colourTaste(c))));
        for (int i = 0; i < n; i++)
            weighted << c;
    }
    return weighted.at(int(rng->bounded(weighted.count())));
}

QString TrackEngine::accentFor(const QString &colour, bool allowWhite) const
{
    // RUNDE 329: the accent's partners are gen_programs' HARMONY - the same
    // pairs the mix, the look's partner and the tiles draw from
    // (engineMixesWith) - and white only under blue, magenta and cyan (B22,
    // as partnerOk). The table this replaced was written for a rig with
    // amber: on this one (no amber) a RED drop's only accent was white -
    // red/white, not a pair - so three red drops in four had no accent at
    // all, and green's and amber's could be white too. The accent becomes
    // the look's partner (m_partnerPick) and the bars' echo colour, so it
    // has to be one of the pairs. Drawn like a room colour (drawColour):
    // green one ticket where the others have three (runde 233).
    // Colours HARMONY does not name keep a coloured partner of their own.
    static const QMap<QString, QStringList> offRig =
    {
        { "yellow",  { "amber" } },
        { "amber",   { "red", "orange" } },
        { "uv",      { "magenta", "blue" } },
    };
    QStringList partners = engineMixesWith().value(colour);
    if (partners.isEmpty())
        partners = offRig.value(colour);
    const bool whiteFits = colour == QLatin1String("blue") || colour == QLatin1String("magenta")
                        || colour == QLatin1String("cyan");
    if (allowWhite && whiteFits)
        partners << QStringLiteral("white");
    // of the partners the palette has, one at random - the same pair every
    // drop would be a habit, not a choice
    QStringList have;
    foreach (const QString &p, partners)
    {
        if (m_palette.contains(p) && engineBannedColour(p) == false && p != colour)
            have << p;
    }
    if (have.isEmpty())
        return QString();
    return drawColour(have, -1, QRandomGenerator::global());
}

qreal TrackEngine::tempoScore(const TrackFuncInfo &info, qreal bpm) const
{
    // 0 = a step is exactly a beat, a bar, a half beat ...; larger = worse
    if (info.durationMs == 0 || bpm <= 0.0)
        return 1.0;
    qreal beatMs = 60000.0 / bpm;
    qreal best = 99.0;
    const qreal mult[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
    for (int i = 0; i < 5; i++)
        best = qMin(best, qAbs(std::log2(qreal(info.durationMs) / (mult[i] * beatMs))));
    return best;
}

quint32 TrackEngine::flashFunction(const QSet<QString> &cast, const QString &colour) const
{
    QList<TrackFuncInfo *> list = candidates(ENGINE_ROLE_FLASH, QString());
    QList<TrackFuncInfo *> ok;
    foreach (TrackFuncInfo *info, list)
    {
        // ONE of its groups in the cast is enough. It used to need every
        // one of them, and a fixture belongs to as many groups as the
        // operator has put it in - "Flash Strobes WHITE" covers three
        // strobes that sit in five different groups, so the test could
        // essentially never pass and FLASH did nothing. (Tobias, 2026-09-15:
        // "Flash white knappen virker ikke mere.") A flash is a moment on
        // top of the look; it does not need the whole rig to be lit first.
        bool inside = false;
        foreach (const QString &g, info->groups)
        {
            if (cast.contains(g))
                inside = true;
        }
        if (inside)
            ok.append(info);
    }
    if (ok.isEmpty())
        return Function::invalidId();

    // Strobes in exactly this colour, then this colour ANYWHERE, then the
    // strobes in white, then white, then anything. The white-strobe rung used
    // to sit second, so a rig with a white strobe scene and no coloured one
    // flashed white every single time - which is what "too much white
    // blinking" was.
    auto pick = [&ok](std::function<bool(TrackFuncInfo *)> test) -> quint32 {
        foreach (TrackFuncInfo *info, ok)
        {
            if (test(info))
                return info->id;
        }
        return Function::invalidId();
    };
    auto onStrobes = [this](TrackFuncInfo *i) {
        foreach (const QString &g, i->groups) if (m_groups.value(g).strobes) return true;
        return false;
    };

    quint32 fid = pick([&](TrackFuncInfo *i) { return onStrobes(i) && i->colour == colour; });
    if (fid != Function::invalidId()) return fid;
    fid = pick([&](TrackFuncInfo *i) { return i->colour == colour; });
    if (fid != Function::invalidId()) return fid;
    fid = pick([&](TrackFuncInfo *i) { return onStrobes(i) && (i->colour == "white" || i->colour.isEmpty()); });
    if (fid != Function::invalidId()) return fid;
    fid = pick([&](TrackFuncInfo *i) { return i->colour == "white" || i->colour.isEmpty(); });
    if (fid != Function::invalidId()) return fid;
    return ok.first()->id;
}

/*********************************************************************
 * The engine
 *********************************************************************/

void TrackEngine::tick(const QString &state, int beat, int secStart, int secEnd,
                       qreal energy, qreal sectionEnergy, int division, bool sectionChanged,
                       const QString &nextState, int beatsToNext, qreal bpm, qreal levelScale,
                       qreal kick, qreal high, bool turn, qreal riser, qreal hats, qreal bass,
                       qreal kickAhead, int rawGap, int rawQuiet)
{
    if (m_doc == nullptr)
        return;
    // B23: a Doc edit (or a project load) has not settled yet - this
    // beat was already queued behind it, and slotDocSettled() runs on the next
    // turn of the event loop. Not on the old ids, as the pulse, fade and
    // closing timers already hold back (runde 202/220). Drawing it rebuilt the
    // table, and the settle then stopped the look, cleared the scene maps and
    // set m_dirty again: two rebuilds for one edit, and the next beat drawn a
    // second rebuild late. A landing on this beat is owed to the next (R233).
    if (m_docTimer.isActive())
    {
        m_sectionOwed = m_sectionOwed || sectionChanged;
        return;
    }
    if (m_testTimer.isActive())  // a track started under the self test: the test yields
        selfTest();
    labShutdown(false);          // R410_LAB: so does the lab (the table rebuilds below)
    if (m_startScene)            // the opening picture is up: nothing else runs
    {
        clearMusicDark();        // runde 356
        // ... but the clock still speaks: at 22:30 it is what takes a start
        // scene SHOW ON put up back down (TrackManager::noteShowRunning,
        // runde 184). Returning first held ENERGY at 0 all night. And this
        // beat ends here either way: its energy was read before the clock
        // moved, so the show starts clean on the next one (runde 185).
        announceRoom();
        return;
    }
    // R233_SAME_BEAT: a second call on a beat already drawn (a SECTION tap,
    // a flag edit or undo, a resend) is not a landing. On 25-26 Sep it
    // re-landed the section 334 times - a second drop look, lead and cast a
    // splitsecond after the first. What it asks for is owed to the next beat.
    // idle/release/trackLoaded clear m_lastState, so a resume on the same
    // beat still draws at once.
    if (beat == m_lastBeat && m_lastState.isEmpty() == false)
    {
        // ... unless this beat already landed one (runde 234): the look path
        // (updateState -> applyLook -> runEngine(true)) follows a landing on
        // the same beat, and owing it drew a second drop look on the next.
        m_sectionOwed = m_sectionOwed || (sectionChanged && m_landedBeat != beat);
        return;
    }
    m_hardStart = m_autoDark;    // runde 356: the light is dark as this beat begins
    m_curBeat = beat;            // runde 357
    if (m_miniLandUntil >= 0 && (beat > m_miniLandUntil || beat < m_miniLandLast))
        m_miniLandUntil = -1;    // R362_MINI_LAND: over (or a jump back - R363: to before it, a two-beat loop kept it on)
    // R363_LOOP_LAND: a jump back to before the landing ends it too - a short
    // loop over (last build beat, landing) kept landFree on the build beat,
    // the strobes over MASTER outside the landing
    if (m_landBurstUntil >= 0 && (beat > m_landBurstUntil || beat < m_landBurstUntil - 8 || beat < m_landBurstFrom))
    {
        m_landBurstUntil = -1;   // the landing is over: the strobes back under MASTER and the trim
        reapplyLevels();
    }
    ensureTable();
    tickFades();
    // idle/release clear the state. Resuming the same marker span must still
    // invalidate the old section's programmes and delayed-drop offset.
    sectionChanged = sectionChanged || m_lastState.isEmpty() || m_sectionOwed;
    m_sectionOwed = false;
    if (sectionChanged)
        m_landedBeat = beat;
    if (beat < m_lastBeat || beat - m_lastBeat > 8)
    {
        m_fillUntil = -1;
        // A jump FORWARD lands somewhere new: a fill may come at once. A jump
        // BACK moves the stamp back with it, like the other stamps below
        // (runde 272): `beat - 8` there made a fill possible on every pass of
        // a DJ loop, and a loop whose first beat carries the turn (a crash,
        // the kick coming back) ran the rhythm lead at double speed - with
        // dark beats in a gap - for as long as the DJ held it. The eight
        // beats between two fills are the promise ("never an endless
        // acceleration").
        if (beat < m_lastBeat)
            m_fillLast = qMin(m_fillLast - (m_lastBeat - beat), beat);
        else
            m_fillLast = beat - 8;
        m_sequenceGroups.clear();
        // The exposure rest counts beats PLAYED (runde 284), like the stamps
        // below: it was cleared, and its charge was already spent (90 s
        // before the next), so a DJ loop inside the four bars brought the
        // group back on the loop's first pass and the room got no rest at
        // all. A jump forward lands somewhere new and ends it, as before.
        // (+ 1: the beat after the jump is the next one PLAYED - a two-bar
        // loop would otherwise add a beat per pass and end it mid-bar)
        if (beat < m_lastBeat && m_restUntil > m_lastBeat)
            m_restUntil -= m_lastBeat + 1 - beat;
        else
            m_restUntil = -1;
        // a jump FORWARD: the build is measured from where we land (runde 237).
        // Not back: a DJ loop is a jump back on every pass, and clearing it
        // there brought the sawtooth back inside a looped build (review). A
        // jump back to before the build is caught by m_buildFrom > secStart.
        if (beat > m_lastBeat)
            m_buildFrom = -1;
        // A jump BACK (a hot cue, a scrub) leaves every "beats since" stamp
        // later than now, and `beat - stamp` negative: no colour change, no
        // aim on a section change, no cast step and no echo until the track
        // had played back past where it was. Treat the jump as the moment
        // they last moved - the floors hold for their bar or two, and then
        // the room is free (runde 168). The stamps that count beats PLAYED
        // move back with the jump instead (m_dropFrom, m_hitBeats, the
        // colour's age, m_darkUntil - runde 266/283/285).
        // CALM and the mix are counted in track beats too: a loop inside CALM
        // never reached its end, and a looped outgoing track (the common way
        // to stretch a mix) never turned the base - or turned it back and
        // forth on every pass. Both move back with the jump (runde 172).
        if (beat < m_lastBeat)
        {
            const int back = m_lastBeat - beat;
            if (m_calmUntil > m_lastBeat)
                m_calmUntil -= back;
            if (m_mixBeat >= 0)
                m_mixBeat = qMax(0, m_mixBeat - back);
            // ... and the drop's settle clock (runde 266): a DJ loop inside
            // the first sixteen bars of a drop set `beat - m_dropFrom` back
            // on every pass, and a loop under 64 beats never settled
            if (m_dropFrom >= 0)
                m_dropFrom = qMax(0, m_dropFrom - back);
            // R363_GROW_JUMP: and the growth's own (runde 360) - an 8-beat DJ
            // loop after the drop grew held it grown, and unsettled, for good
            if (m_dropGrowAt >= 0)
                m_dropGrowAt = qMax(0, m_dropGrowAt - back);
            // ... and the hits' own window (runde 283). The comment above
            // said m_hitBeats "already did this", but it was CLEARED on a
            // jump back (the ceiling line below): no stamp, so neither the
            // ceiling nor the gap held, and every pass of a DJ loop was a
            // fresh 32 beats - a two-bar loop at the top of a build (eNow
            // 0.5: five in 32 beats, five apart) flashed on beats 0 and 5 of
            // every pass, eight in 32 and three apart across the loop point.
            // Moved back with the jump, the window counts beats PLAYED.
            for (int &h : m_hitBeats)
                h -= back;
            // ... and the colour's age and the bars' planned dark (runde
            // 285), both in beats PLAYED. The colour was only clamped to the
            // loop's start (below), so its age never got past the loop's
            // length: a four-bar DJ loop in a drop at 90 % (the eight-bar
            // clock) never changed colour for as long as it ran. 0 (adopted)
            // and -1 (fresh track) are not beats and stay.
            // m_darkUntil did NOT do this (the comment above said it did):
            // four bars of dark planned on beat b end on b + 15, and a
            // two-bar loop from b never got there - the bars stood dark for
            // the whole loop, and one that had just ended came back dark.
            const int played = m_lastBeat + 1 - beat;
            if (m_colourSince > 0)
                m_colourSince = qMax(1, m_colourSince - played);
            foreach (const QString &key, m_darkUntil.keys())
            {
                if (m_darkUntil.value(key) > m_lastBeat)
                    m_darkUntil[key] -= played;
                else
                    m_darkUntil.remove(key);
            }
        }
        // ... and forward (a hot cue ahead, or the next track, whose first
        // beat is wherever the deck stands - trackLoaded() leaves CALM as a
        // count from 0): what was left of CALM is kept (runde 201)
        else if (m_calmUntil > m_lastBeat)
            m_calmUntil += beat - m_lastBeat;
        if (m_colourSince > beat)
            m_colourSince = beat;
        if (m_effectsBeat > beat)
            m_effectsBeat = beat;
        if (m_echoBeat > beat)
            m_echoBeat = beat;
        if (m_curveTurnBeat > beat)        // runde 193: the kick-turn spacing too
            m_curveTurnBeat = beat;
        // (a clamp, not a new aim - named apart from the two stamps
        // verify_flicker counts)
        foreach (const QString &group, m_aimSince.keys())
        {
            if (m_aimSince.value(group) > beat)
                m_aimSince[group] = beat;
        }
        foreach (const QString &group, m_turnBeat.keys())
        {
            if (m_turnBeat.value(group) > beat)
                m_turnBeat[group] = beat;
        }
    }
    // R358_LOOP (Tobias 10-05, "DJ'ens loops styrer lyset"): the loop as the
    // beat numbers show it. 10-02: 42 loops, 4 and 8 beats most, 13 of them
    // halved on their way (the roll by hand). A step back of up to 32 beats is
    // a jump; the same jump again (to the same beat from the same top) is a
    // loop, and its length is read off the time between two jumps - the beat
    // numbers say 7 for an eight-beat loop half the time and 1 for a two-beat
    // one. The first jump could still be a cue: nothing happens on it.
    if (m_lastBeat > 0 && beat < m_lastBeat && m_lastBeat - beat <= 32)
    {
        const qint64 nowMs = m_clock.elapsed();
        const bool same = m_loopPasses > 0 && qAbs(beat - m_loopTo) <= 1 && qAbs(m_lastBeat - m_loopFrom) <= 1;
        int len = 1;
        while (len < m_lastBeat - beat && len < 32)
            len *= 2;
        if (same && m_loopJumpMs >= 0 && m_beatMs > 0.0)
        {
            const qreal passBeats = qreal(nowMs - m_loopJumpMs) / m_beatMs;
            if (passBeats >= 0.75)
            {
                len = 1;
                while (len < 32 && std::abs(std::log2(passBeats / qreal(len))) > 0.5)
                    len *= 2;
            }
        }
        m_loopPasses = same ? m_loopPasses + 1 : 1;
        m_loopLen = len;
        m_loopFrom = m_lastBeat;
        m_loopTo = beat;
        m_loopJumpMs = nowMs;
    }
    else if (m_loopPasses > 0 && (beat > m_loopFrom + 1 || beat < m_loopTo - 1))
        clearDjLoop();                   // played out of it, or jumped away
    m_lastBeat = beat;
    // runde 302 (bane B's B19): the hit, the automatic white/colour flash on
    // the strobes and a hardware strobe burst each live for a beat and are
    // ended by the NEXT beat. With the deck stopped or the link quiet no next
    // beat comes, and they stood lit - strobing - for 1-4 s until the stop's
    // grace or the stale link took the room idle. A beat and a half with no
    // tick ends them (slotBeatWatch).
    m_beatWatch.start(qBound(400, int(m_beatMs * 1.5) + 150, 2500));   // + a lost beat packet's status (review 305)

    // the clock moves the ENERGY slider (through roomChanged), so the time
    // of night is already in the energy that arrives here
    announceRoom();

    // NEXT: treat this beat as a fresh section with a fresh colour
    bool forceNext = m_forceNext;
    m_forceNext = false;
    // ENERGY at zero is the restaurant: the base stands in its colour and
    // nothing moves or changes - no pulse, no patterns, no colour rotation,
    // no positions. A hold the slider imposes.
    // the house closing takes the energy down whatever the slider says
    qreal closing = closingCap();
    energy = qMin(energy, closing);
    // THE FADER, as the operator reads it. `energy` is the slider scaled by
    // the section's loudness (trackmanager: x 0.80..1.0), and that is right
    // for everything continuous - size, pace, level, depth. The hard lines
    // are another matter: "under 30 % vises et drop ikke", "under 40 % staar
    // barerne stille", "fra 60 % maa de pege nedad" are promises about the
    // NUMBER ON THE SCREEN. At slider 35 in a quiet section the scaled energy
    // is 0.29, and a drop would have hidden under a fader that read 35. So
    // the lines are drawn on the slider itself, recovered here; the closing
    // cap still applies (the house coming down is a real ceiling).
    qreal fader = energy;
    if (sectionEnergy >= 0.0)
        // + 1e-6: the division lands a hair under the slider's own value, and
        // a slider on exactly 30 must count as 30, not 29.999
        fader = qMin(closing, qMin(1.0, energy / (0.80 + 0.20 * qBound(0.0, sectionEnergy, 1.0)) + 1e-6));
    m_faderNow = fader;
    // How long the kick has been away, in beats - the vocal passage the
    // analysis did not flag as a break. Counted once per beat: a section
    // change runs tick() a second time on the same beat, and that would
    // have counted the same silent beat twice.
    if (beat != m_kickBeat)
    {
        m_kickBeat = beat;
        if (kick < 0.0 || kick >= 0.20)
            m_kickGone = 0;
        else
            m_kickGone++;
    }
    bool still = fader < 0.03;         // the slider's bottom, whatever the section says (round 112)
    QRandomGenerator *rng = QRandomGenerator::global();
    bool hold = (m_hold || still) && forceNext == false;      // NEXT breaks a hold for one beat
    if (forceNext)
    {
        sectionChanged = true;
        m_moves.clear();
        m_sweep.clear();
    }

    // a track is playing: the start scene steps aside - at once: fading out
    // over a second it held the heads' pan/tilt and they swung back off the
    // beat (runde 199)
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            stopSlot(slot, true);
    }

    // Rekordbox' own phrase analysis, when the track has one, hands us two
    // more section types than our own analysis produces. Both behave like a
    // break - quiet, one figure moving - but an intro is the room before the
    // track has started and an outro is it letting go, so both sit a little
    // lower and neither gets the hardware strobe.
    bool isIntro = (state == QStringLiteral("intro"));
    bool isOutro = (state == QStringLiteral("outro"));
    // A DRIVE is a groove high in the track's own range - BLT's analyser names
    // it (qlc-section-type, analysis version 7), after Time-line's taxonomy:
    // "a steady, high-energy groove that maintains momentum without actively
    // building or releasing tension". It stays TIER 1 - the programme pool,
    // the colour rules and the section machinery are a groove's - but the
    // curves that a groove and a drop disagree about are read half way
    // between the two. An older BLT never sends it and nothing changes.
    bool isDrive = (state == QStringLiteral("drive"));
    bool isBreak = (state == QStringLiteral("break")) || isIntro || isOutro;
    bool isBuild = (state == QStringLiteral("build"));
    bool isDrop  = (state == QStringLiteral("drop"));
    int tier = isBreak ? 0 : (isDrop ? 2 : 1);
    bool isCalm = beat < m_calmUntil;
    // MIX hands colour over; musical state and ENERGY still drive activity.
    // A drop under ENGINE_DROP_SHOW is a groove with a louder record on. The
    // state stays "drop" for the log and the verdicts; everything that
    // decides what the room DOES reads isDrop / tier, and those say groove.
    // (dropHidden is set below, once beatInBar is known: runde 279.)
    // a flag edited to sit ahead of us, or a jump back in the track, makes
    // beat - secStart negative: bar and beatInBar go with it, every downbeat
    // test stops firing and patternMask() picks a negative step, which leaves
    // a chase dark. Clamp once, here, and everything downstream is safe.
    secStart = qMin(secStart, beat);
    int len = qMax(1, secEnd - secStart);
    qreal prog = qBound(0.0, qreal(beat - secStart) / qreal(len), 1.0);
    int bar = (beat - secStart) / 4;
    int beatInBar = (beat - secStart) % 4;
    // ... and the line is read on the BAR LINE (runde 279), like the strobes'
    // pool and the composition's tier (runde 199): read on every beat, the
    // clock's 22:45 step, the closing lid or a hand crossing 30 % on beat 2
    // turned the drop on (or off) for beats 2-4 - the level jumped 0.70 ->
    // 1.0, the accent colour came in, held two-colour programmes failed the
    // partner rule and were re-picked, a drop style was drawn - while the
    // composition waited for the downbeat. A hand resting on 29/30 flickered
    // all of it beat by beat. m_dropShown holds what the last bar line (or a
    // new section) saw; the build's line and the floor round's read it too.
    if (beatInBar == 0 || sectionChanged)
        m_dropShown = fader >= ENGINE_DROP_SHOW;
    bool dropHidden = isDrop && m_dropShown == false;
    if (dropHidden)
    {
        isDrop = false;
        tier = 1;
        division = 0;          // not the drop's SETUP step either: the programmes keep their own
    }
    // prog a bar ago, where prog is NOT the section's own clock (the kick
    // wait, a riser-promoted build, a build of several flags); -1 = the
    // section's own (runde 172)
    qreal progBefore = -1.0;
    // prog on this bar's FIRST beat, and how far one bar moves it, in the
    // same frame as prog (runde 277). barProg below is progBar; rampProg is
    // barProg stretched so a build's LAST bar reads the top of its climb.
    // rampLo is where the frame starts (a later build flag starts at
    // `reached`), rampTop where it ends (1.0 = at the drop / the section's end).
    qreal progBar = qBound(0.0, qreal(beat - beatInBar - secStart) / qreal(len), 1.0);
    qreal barStep = 4.0 / qreal(len);
    qreal rampLo = 0.0;
    qreal rampTop = 1.0;
    // the build hits keep the SECTION's clock (runde 237): the last 18 % of
    // the last flag before the drop, as before - not the last 18 % of a 128-
    // beat build (23 beats of hits). -1 = use prog.
    qreal hitProg = -1.0;
    // A BUILD OF SEVERAL FLAGS climbs once (runde 237, Tobias: "ja, lav 1").
    // 19 of the 22 builds over 64 beats on 25-26 Sep were split by inner
    // build flags, and prog started again from nought at each: the level fell
    // (median 0.17), a group left, the chases stopped until the middle came
    // round again and the build hits fired before every phrase instead of
    // before the drop - a sawtooth, not a climb. Measured from the build's
    // FIRST flag instead: to the drop when the next flag is it; an inner
    // section also counts 32 beats more, so it stops short of the top; and
    // never below where the section before it ended.
    // ... and EVENLY inside each flag (runde 265): prog runs in a straight
    // line from where this flag starts (`reached`) to where it ends (`top`:
    // the next inner flag's `reached`, or 1.0 at the drop). It was
    // qMax(reached, (beat - first flag) / span), and span grew with every
    // flag, so the line for flag two started BELOW `reached`: 32 + 64 beats
    // sat flat on 0.50 for 16 beats before it climbed, 32 + 32 + 32 did the
    // same in the middle flag, and 64 + 16 jumped 0.66 -> 0.80 on the
    // second flag's first beat. A single flag and every first flag read
    // exactly as before.
    if (isBuild)
    {
        if (m_buildFrom < 0 || m_buildFrom > secStart || m_lastState != QStringLiteral("build"))
            m_buildFrom = secStart;
        const bool inner = nextState == QStringLiteral("build");
        const int sofar = secStart - m_buildFrom;
        const qreal reached = sofar > 0 ? qreal(sofar) / qreal(sofar + 32) : 0.0;
        const qreal top = inner ? qreal(sofar + len) / qreal(sofar + len + 32) : 1.0;
        prog = reached + (top - reached) * qBound(0.0, qreal(beat - secStart) / qreal(len), 1.0);
        progBefore = reached + (top - reached) * qBound(0.0, qreal(beat - 4 - secStart) / qreal(len), 1.0);
        hitProg = inner ? 0.0 : qBound(0.0, qreal(beat - secStart) / qreal(len), 1.0);
        progBar = reached + (top - reached) * qBound(0.0, qreal(beat - beatInBar - secStart) / qreal(len), 1.0);
        barStep = (top - reached) * 4.0 / qreal(len);
        rampLo = reached;
        rampTop = top;
    }
    else
        m_buildFrom = -1;

    // the drop is one bar away: pull the cast in now so the hit lands lit
    // A FAKE DROP: the analysis says the release is here, the kick says it is
    // not. Time-line calls it "a moment where the music intentionally delays
    // the expected release, maintaining tension instead of delivering the
    // Drop" - and it is the one thing that makes a light show look foolish,
    // because everything has already committed: the blackout beat, the white
    // hit, the strobe burst, two bars of impact.
    //
    // We cannot predict it - their model is trained for that and ours is not
    // - but we can refuse to fire until the kick is actually there. The
    // landing waits, a bar at a time, up to eight bars; the build's look
    // holds meanwhile, which is exactly what a delayed release wants. Without
    // curves nothing changes: `kick` is -1 when BLT sent none, so the test
    // never fires and the drop lands on bar 0 as before. (kick >= 0.0 spelled
    // out rather than haveCurves - that local is declared further down.)
    //
    // m_dropLand is the bar the drop is treated as landing on. Everything
    // that fires "on the landing" reads dropBar below, not bar.
    // Reset HERE, not in the section-change block forty lines down: that one
    // runs after this and would leave the first beat of a new drop reading a
    // previous drop's offset - dropBar = 0 - 3 = -3, and the landing lost.
    // NEXT is a new look, not a new drop: it forces sectionChanged, and a
    // wait that was on stopped waiting - the drop drawn with no kick and no
    // landing (runde 172).
    if (isDrop == false || (sectionChanged && forceNext == false))
        m_dropLand = 0;
    // Not `hold == false` here: HOLD freezes the LOOK, and the look is frozen
    // anyway. Under HOLD the wait simply stopped, and a fake drop got its
    // white hit and its strobe burst on bar 0 with no kick (runde 172).
    // ... and only when the drop ARRIVES (runde 262): a second drop flag inside
    // a running drop is a sectionChanged too, so a quiet downbeat there armed
    // the wait again - the room back to the build look for up to eight bars in
    // the middle of the drop, and the white hit, the strobe burst and the
    // impact fired a second time when the kick came back. m_lookState still
    // holds the previous beat's look here (it moves further down), and it
    // reads "build" all through a wait, so the wait itself goes on.
    if (isDrop && kick >= 0.0 && beatInBar == 0 && m_lookState != QStringLiteral("drop")
        && bar == m_dropLand && m_dropLand < 8 && kick < 0.20)
        m_dropLand++;
    const int dropBar = isDrop ? bar - m_dropLand : bar;
    // R360_LATE_LAND (Tobias 10-05, "mixet som musik"): a track that comes in
    // during a mix - the DJ handing MASTER over, or BLT handing the lights to
    // the deck whose drop just landed - often arrives a beat or three INTO its
    // drop (10-02: 18 of 95 handovers landed in the drop's first bars). Its
    // first beat is the landing then: the strobes, the hit and the flash, on
    // this beat rather than never.
    // R361_LATE_KICK: ... but not before the kick. A late drop (FAKE DROP)
    // arms its wait on the bar line only, and a track that comes in on beat
    // 2 of the flag never saw that line - it landed white on a beat with no
    // kick. With the curves, the kick has to be there.
    // The owed landing waits out the rest of that first bar for it.
    const bool lateKick = kick < 0.0 || kick >= 0.20;
    const bool lateLand = m_landOwed && m_mixing && isDrop && dropBar == 0 && beatInBar > 0 && hold == false
                       && lateKick;
    m_landOwed = m_landOwed && lateLand == false && lateKick == false && m_mixing && isDrop && dropBar == 0
              && hold == false;          // R361_LATE_KICK: still owed while the first bar waits for its kick
    m_lateLandNow = lateLand;
    const bool dropWaiting = isDrop && dropBar < 0 && m_lookState != QStringLiteral("drop");   // a loop back into a drop that has landed does not wait again
    if (dropWaiting)
    {
        isDrop = false;
        isBuild = true;
        tier = 1;
        division = 0; // do not run the waiting build at the drop's forced step speed
        // Tension, never the final build's automatic hits (0.82) - and the
        // build's look HOLDS. This read qMin(prog, 0.75), but `prog` here is
        // the DROP's own progress, nought to a tenth, so the wait fell back to
        // an early build: the level down to 0.65, no chases (runde 172).
        prog = 0.75;
        progBefore = 0.75;       // ... and the figure is not tightened on every bar of it
        progBar = 0.75;
        barStep = 0.0;
    }
    else if (isDrop && m_dropLand > 0 && dropBar == 0 && beatInBar == 0 && hold == false && m_lookState != QStringLiteral("drop"))
        sectionChanged = true;  // draw the drop look WHEN the kick lands

    bool preDrop = isDrop == false && dropWaiting == false && hold == false
                && nextState == QStringLiteral("drop")
                && beatsToNext > 0 && beatsToNext <= 4;

    // THE DROP SETTLES (analyse 10g). Tobias, 2026-09-27: "et drop starter jo
    // meget ekstremt, men saa laenge det ikke bliver ved med at vaere
    // ekstremt og 'normalisere' sig lidt, er det fint med lange drop
    // sektioner". Analysis 9 split a drop every 32 bars and every flag landed
    // it again; analysis 10 gives a 2-4 minute drop ONE landing, and nothing
    // in a drop had a time term - strobe lead, four effects, the hard/nervous
    // style and the strobe bursts ran to the section's end. Sixteen bars after
    // the kick landed (dropBar, so a kick wait moves it too), in a drop of 32
    // bars or more: the strobes give up the lead and strobe as in a groove,
    // one effect less at the top, and the style steps down once - on the bar
    // line, with a redraw. The landing itself, FLASH and HOLD are untouched.
    // (fejljagt 3, review of 10g) counted from the beat the drop's LOOK was
    // drawn - the kick landing, or the beat DROP was forced - not from the
    // section under it: a DROP pressed 20 bars into a section started out
    // settled. And latched to the next section: a DJ loop over bar 16 turned
    // the lead back on at every pass, and HOLD over bar 16 skipped the style
    // step for the rest of the drop (now it steps on the first bar line after
    // HOLD lets go).
    // ... and the clock starts when the drop ARRIVES (runde 266), not at
    // every section change inside it: a second drop flag in a running drop
    // (an older analysis split drops every 32 bars; a DJ's own flags) and
    // NEXT are sectionChanged too, and both reset m_dropCalm - a drop that
    // had settled got the strobe lead, the fourth effect, the hard style and
    // the landing burst back, and the sixteen bars began again. m_lookState
    // still holds the previous beat's look here and reads "drop" only while a
    // drop was on stage; a kick wait reads "build", a new track clears it.
    // The drop's length is the drop's, not the flag's: a drop flagged as two
    // sixteens (or 24 + 16) is a drop of 32 bars or more.
    if (isDrop == false)
    {
        m_dropFrom = -1;
        m_dropLine = -1;                 // runde 367
        m_dropCalm = false;
    }
    else if (m_dropFrom < 0 || (sectionChanged && m_lookState != QStringLiteral("drop")))
    {
        m_dropFrom = beat;
        // R368_GROW_BAR: the line counts from the drop's BAR line. A drop that
        // landed late (runde 360: a track handed over a beat or three into its
        // drop) or was forced mid-bar measured its 32s from that beat - the
        // line fell a bar late, its window took the grown bar into the "before"
        // half, and the growth was missed (harness lategrow.json: Bausa landed
        // on 58, the music grows at 89 - nothing; on 57: drop-grow at 89)
        m_dropLine = beat - beatInBar;   // runde 367/368
        m_dropCalm = false;
    }
    // R360_DROP_GROW (Tobias 10-05, "lyset vokser naar droppet vokser"): on
    // every 8-bar line of a drop, the next eight bars against the last eight -
    // the highs up 0.10 or the mids up 15 %, the waveform not falling and
    // still two bars of drop ahead. In the library 8 of 33 long drops step up
    // so after their first eight bars (Dennis Ferrer, Hugel x Guetta, Bieber
    // "Sorry"). Then the light steps up too, inside what ENERGY gives: one
    // group more, a step faster from 50 % on the slider, a hit on the line
    // from 60 % - and the drop does not settle while it grows.
    bool growNow = false;
    bool growOff = false;                // R361_GROW_BACK (the log)
    if (isDrop == false)
    {
        m_dropGrow = false;
        m_dropGrowLine = -1;             // runde 367
    }
    // R361_GROW_LINE: the first DOWNBEAT of each 32 - a drop that landed late
    // (runde 360) or was forced mid-bar has its m_dropFrom off the bar line,
    // and `% 32 == 0` never met beatInBar 0 there.
    // R361_GROW_WINDOW: the next eight bars stay inside the drop - a drop
    // ending in 8-31 beats compared its last bars plus the NEXT section.
    // R367_GROW_TRACK: the line is the MUSIC's - counted from the track beat
    // the drop arrived on. m_dropFrom is the settle clock and moves back on
    // every DJ jump (runde 266), so a 4-beat loop over the bar before the line
    // put the line inside the loop: the drop "grew" on the looped OLD bars,
    // a bar before the music did (the harness: drop-grow at 189 on the 2nd
    // pass, the music steps up at 193). And a loop ON the line measured it
    // again on every pass and pushed the growth's 64-beat clock forward each
    // time - measured once per track line now.
    // R368_GROW_BAR: eight bars of the TRACK behind the line is the test; the
    // played clock is never below it (a DJ loop only adds), and a late landing
    // has a beat or three fewer of it on the line.
    else if (m_dropFrom >= 0 && m_dropLine >= 0 && beatInBar == 0 && beat != m_dropGrowLine
             && beat - m_dropLine >= 32
             && ((beat - m_dropLine) % 32) < 4 && secEnd - beat >= 8
             && m_audHigh.isEmpty() == false && beat - 32 >= 1)
    {
        m_dropGrowLine = beat;           // runde 367
        const int ahead = qMin(32, nextState == QStringLiteral("drop") ? 32 : secEnd - beat);
        const int to = qMin(beat + ahead - 1, int(m_audHigh.count()));
        auto mean = [&](const QVector<quint8> &v, int a, int b) {
            qreal sum = 0.0;
            for (int i = a; i <= b; i++)
                sum += v.at(i - 1) / 255.0;
            return sum / qreal(b - a + 1);
        };
        const int n = to - beat + 1;
        const qreal h1 = mean(m_audHigh, beat - n, beat - 1), h2 = mean(m_audHigh, beat, to);
        const qreal l1 = mean(m_audLevel, beat - n, beat - 1), l2 = mean(m_audLevel, beat, to);
        bool up = h2 - h1 >= 0.10;
        if (m_audMid.count() == m_audHigh.count())
        {
            const qreal m1 = mean(m_audMid, beat - n, beat - 1), m2 = mean(m_audMid, beat, to);
            up = up || (m1 > 0.05 && m2 / m1 >= 1.15);
        }
        if (n < 8)
            up = false;
        if (up && l2 >= l1 - 0.05)
        {
            growNow = m_dropGrow == false || beat - m_dropGrowAt >= 32;
            m_dropGrow = true;
            m_dropGrowAt = beat;
            m_dropGrowHigh = h2;
            m_dropGrowLevel = l2;
            m_dropCalm = false;          // a drop that grows has not settled
        }
        // R361_GROW_BACK: it lets go again. Grown once, it held for the rest
        // of the drop, and a drop that grew never settled (runde 266/10g) -
        // a 3-minute drop at the top to its last bar. The music falling back
        // from where it grew lets go on the line.
        else if (m_dropGrow && (h2 < m_dropGrowHigh - 0.06 || l2 < m_dropGrowLevel - 0.06))
        {
            m_dropGrow = false;
            growOff = true;
        }
    }
    // ... and sixteen bars on the new level is the new normal: the drop may
    // settle from there as any drop does
    if (m_dropGrow && beat - m_dropGrowAt >= 64)
    {
        m_dropGrow = false;
        growOff = true;
    }
    const bool dropLong = len >= 128 || nextState == QStringLiteral("drop")
                       || (m_dropFrom >= 0 && secEnd - m_dropFrom >= 128);
    const bool settleNow = isDrop && m_dropCalm == false && m_dropFrom >= 0 && dropLong
                        && hold == false && beat - m_dropFrom >= 64 && beatInBar == 0
                        && m_dropGrow == false;   // runde 360: not while it grows
    if (settleNow)
        m_dropCalm = true;
    const bool dropSettled = isDrop && m_dropCalm;

    // The highs climbing for bars on end with a drop ahead IS the build,
    // whatever the flag on this stretch says - the riser is in the music,
    // not in the marker. Promoted, the climb is measured to the drop, so
    // everything shaped by prog (pulse depth, the bare blink, the sweep)
    // reaches its top as the drop lands. (Tobias, 2026-09-15: "musikken
    // foelger waveformen mere ift. hvordan det blinker, skifter".)
    // NOT latched (runde 280): runde 278 held the promotion to its drop
    // (m_riserDrop) so a riser hovering on 0.15 could not flick the room
    // between build and groove beat by beat. That is BACKLOG's "riser-build
    // uden hysterese", which Tobias said no to on 2026-09-23 (runde 182:
    // "ingen af dem" - they stay as they are; not proposed again without
    // something new from the rig). Taken back out; re-decided on every beat.
    if (isDrop == false && isBreak == false && isBuild == false
        && riser > 0.15 && nextState == QStringLiteral("drop")
        && beatsToNext > 0 && beatsToNext <= 32)
    {
        isBuild = true;
        prog = qBound(0.0, 1.0 - qreal(beatsToNext) / 32.0, 1.0);
        progBefore = qBound(0.0, 1.0 - qreal(beatsToNext + 4) / 32.0, 1.0);
        progBar = qBound(0.0, 1.0 - qreal(beatsToNext + beatInBar) / 32.0, 1.0);
        barStep = 4.0 / 32.0;
        rampLo = 0.0;
        rampTop = 1.0;
    }
    // THE CURVES OVER THE FLAGS (runde 192). Tobias, 2026-09-23: "analysen
    // saetter dem ofte forkert, men overordnet virker det. Det er bare ikke
    // altid en dj lige checker og faar rettet flagene." The flags carry what
    // only a look ahead can give - the countdown, the build, the dark beat,
    // the landing - so they stay the structure. But where the kick says
    // plainly that a flag is wrong, the room follows the kick:
    //   a GROOVE flag (normal, drive) with no kick in the next two bars plays
    //   as a BREAK - the breakdown the analysis missed or placed late;
    //   a BREAK flag with a solid kick through the next two bars plays as a
    //   GROOVE - the break that ended early, or never was.
    // kickAhead is the kick's mean over this beat and the seven after it,
    // read from the track's own curves (TrackManager), so the change lands
    // on the bar line where the music changes, not a bar late. Decided only
    // on a bar line, with room between the in and out thresholds so a fill
    // cannot flick it; not under HOLD; a build (flagged, or promoted by the
    // riser above - builds often have no kick) and a drop (the kick wait
    // above handles a drop without a kick) are left alone, and so are
    // rekordbox' intro/outro. Without curves kickAhead is -1: nothing changes.
    // A turn is a section change: a new look, and a colour change into the
    // break under the usual floors.
    {
        if (sectionChanged)
        {
            m_curveBreak = false;
            m_curveGroove = false;
        }
        const bool flagGroove = (state == QStringLiteral("normal") || state == QStringLiteral("drive"))
                             && isBuild == false;
        const bool flagBreak = (state == QStringLiteral("break"));
        bool curveTurn = false;
        // runde 193, from review: a flag cleared mid-section (a riser build
        // taking over) is a turn like any other, not a silent change of look;
        // with no curve data (its last beats) the correction simply stands;
        // it is re-armed on a section change too, not only on a bar line -
        // NEXT, a mix landing or a resume mid-bar drew a groove look and then
        // a break one to three beats later; no turn inside the last two bars
        // before a flag (it changes there anyway), none within four bars of
        // the last one (a kick in one bar of four flipped every other bar),
        // and no break armed with a drop ahead - a kickless climb there is a
        // build, not a breakdown.
        const bool flagSoon = beatsToNext > 0 && beatsToNext <= 8;
        const bool dropAhead = nextState == QStringLiteral("drop") && beatsToNext > 0 && beatsToNext <= 32;
        const bool mayTurn = hold == false && (beatInBar == 0 || sectionChanged) && flagSoon == false
                          && (sectionChanged || beat - m_curveTurnBeat >= 16);
        bool curveHeld = false;       // a correction waiting for the bar line (r199)
        if (flagGroove == false && flagBreak == false && (m_curveBreak || m_curveGroove)
            && sectionChanged == false && beatInBar != 0)
        {
            // a riser taking over mid-bar: the correction stands down for
            // these beats and turns on the bar line - not mid-bar (runde 199)
            curveHeld = true;
        }
        else if (flagGroove == false && flagBreak == false)
        {
            // a riser taking over mid-section: a turn only where turns may
            // happen, otherwise quietly (runde 194)
            // (no four-bar spacing here: a riser takes over once per section,
            // and holding it back cleared the correction with no turn - the
            // build played under the break look to the drop, runde 195)
            if ((m_curveBreak || m_curveGroove) && sectionChanged == false && hold == false
                && flagSoon == false)
                curveTurn = true;
            m_curveBreak = false;
            m_curveGroove = false;
        }
        else if (kickAhead < 0.0 && (m_curveBreak || m_curveGroove) && mayTurn)
        {
            // the flags turned hand-made (TrackManager stops the look-ahead,
            // R210) with a correction armed: it lets go on the bar line instead
            // of holding to the section's end - a break look over a running
            // kick for 32 bars (runde 213)
            m_curveBreak = false;
            m_curveGroove = false;
            curveTurn = true;
        }
        else if (kickAhead >= 0.0 && mayTurn)
        {
            // (48, not 32: a break armed just outside the drop's window was
            // taken over by the riser a bar later - two looks one bar apart,
            // runde 196)
            if (flagGroove && m_curveBreak == false && kickAhead < 0.15
                && (nextState == QStringLiteral("drop") && beatsToNext > 0 && beatsToNext <= 48) == false)
                m_curveBreak = curveTurn = true;
            // ... and one armed before the drop's 32-beat window lets go
            // when it opens - no breakdown look on a climb (runde 194)
            else if (flagGroove && m_curveBreak && (kickAhead >= 0.30 || dropAhead))
            {
                m_curveBreak = false;
                curveTurn = true;
            }
            else if (flagBreak && m_curveGroove == false && kickAhead >= 0.45)
                m_curveGroove = curveTurn = true;
            else if (flagBreak && m_curveGroove && kickAhead < 0.30)
            {
                m_curveGroove = false;
                curveTurn = true;
            }
        }
        if (curveHeld)
        {
            // the build look holds until the bar line; the flags stay
        }
        else if (m_curveBreak)
        {
            isBreak = true;
            isDrive = false;
            tier = 0;
        }
        else if (m_curveGroove)
        {
            isBreak = false;
            tier = 1;
        }
        if (curveTurn)
        {
            sectionChanged = true;
            m_curveTurnBeat = beat;
        }
    }

    // ... and nothing builds towards, or blinks before, a drop that will
    // not be shown (see dropHidden above)
    if (m_dropShown == false)
    {
        isBuild = false;
        preDrop = false;
    }

    // R358_REPEAT (Tobias 10-05, "omkvaedet faar sit eget look hver gang"):
    // the second drop of a track that SOUNDS like the first (the waveform,
    // kick, highs and mids over its first 16 beats, each within 0.12) lands
    // in the first one's colour, drop style and programmes - one group more.
    // Builds the same. The room learns the song: the hook comes back and so
    // does its picture. Not in a mix, under HOLD, CALM or NEXT, and only
    // where it ARRIVES (an inner flag of the same drop is not a repeat).
    {
        const bool arrives = sectionChanged && hold == false && m_mixing == false && forceNext == false
                          && isCalm == false && still == false
                          && ((isDrop && m_lastState != QStringLiteral("drop"))
                              || (isBuild && m_lastState != QStringLiteral("build")));
        if (arrives)
        {
            const QString key = isDrop ? QStringLiteral("drop") : QStringLiteral("build");
            const QVector<qreal> sig = soundSig(beat, 16);
            m_lookActive.clear();
            m_lookPending.clear();
            m_reuseColour.clear();
            m_reuseStyle = -1;
            m_reuseMotion.clear();
            // every distinct drop (build) of the track is remembered, four at
            // most: a track whose second drop is a new one and whose third
            // repeats the second (Fergie x Max Dean: 129, then 257 = 321)
            QVector<TrackLookMemory> &list = m_lookMemory[key];
            int match = -1;
            for (int m = 0; m < list.count() && match < 0; m++)
            {
                const TrackLookMemory &mem = list.at(m);
                bool alike = sig.count() == mem.sig.count() && sig.count() > 0 && sig.at(0) >= 0.0
                          && beat - mem.beat >= 32;
                for (int i = 0; alike && i < sig.count(); i++)
                {
                    if (sig.at(i) >= 0.0 && mem.sig.at(i) >= 0.0 && qAbs(sig.at(i) - mem.sig.at(i)) > 0.12)
                        alike = false;
                }
                if (alike)
                    match = m;
            }
            if (match >= 0)
            {
                const TrackLookMemory &mem = list.at(match);
                m_lookActive = key;
                m_lookRepeat++;
                m_reuseColour = mem.colour;
                m_reuseStyle = isDrop ? mem.dropStyle : -1;
                m_reuseCoin = mem.landCoin;
                m_reuseMotion = mem.motion;
            }
            else if (sig.count() > 0 && sig.at(0) >= 0.0 && list.count() < 4)
            {
                m_lookPending = key;
                m_lookPendingIdx = list.count();
                m_lookFrom = beat;
                TrackLookMemory mem;
                mem.sig = sig;
                mem.beat = beat;
                list.append(mem);
            }
        }
        else if (sectionChanged && (isDrop == false && isBuild == false))
        {
            m_lookActive.clear();
            m_lookPending.clear();
        }
    }

    /* ---- palette: one colour, changed rarely. A fresh track keeps the colour
     *      it arrived with until its first break or drop. ---- */
    // the hold is counted from the last change and varies around the SETUP
    // value (x0.5, x0.75, x1, x1.5), always ending on a bar line - so the
    // colour does not change on the same beat of every track
    // With the analysis curves in hand the timer becomes a window rather than
    // a clock: from half the hold onwards the colour changes on the next
    // musical TURN - the kick coming back after a fill, a crash, the bass
    // jumping - and only if no turn comes does it change on the timer, at one
    // and a half times the hold. Fewer changes, and each one lands on
    // something the ear heard too. Without curves it is the old timer.
    // How long a colour holds (runde 189, Tobias 2026-09-23: "lav energi er
    // faerre skift, og hoejere energi er mere skift"): the ENERGY FADER picks
    // it, 64 / 32 / 16 / 8 bars over its four quarters - read every beat, so
    // a hand on the fader shortens or lengthens the colour already up. A
    // SETUP tile fixes it instead. The colour's own stretch (x0.5 .. x1.5,
    // drawn at the change) keeps it off the same beat of every track.
    // Runde 291 (Tobias: "lad farverne glide jævnt, men med mulighed for at
    // override i advanced settings"): the fader's hold GLIDES - halving for
    // every quarter of the slider, and his four numbers are where it passes
    // through the middle of each quarter (64 at 12.5 %, 32 at 37.5, 16 at
    // 62.5, 8 at 87.5), 64 at most, 8 at least. It was four flat steps: a hand
    // moving inside a quarter changed nothing, and 75-100 % was one value.
    // The bars tiles in SETUP > ADVANCED ("Colour holds for") are the override.
    const int holdBase = m_holdAuto
        ? qBound(8, int(qRound(64.0 * std::pow(2.0, -4.0 * (qBound(0.0, m_faderNow, 1.0) - 0.125)))), 64)
        : m_holdBars;
    m_holdNow = qMax(4, int(qRound(holdBase * m_holdStretch)));
    int holdBeats = qMax(4, m_holdNow * 4);
    bool haveCurves = kick >= 0.0;
    // A drop or a break within two bars makes a colour change of its own -
    // and the two-bar floor below then held THAT one back: 16 turn changes
    // on 20 Sep came one to three beats before a drop, and the drop landed
    // in the colour it already had. And a turn waits for the bar line as the
    // timer does: 30 of 70 fell mid-bar (runde 188, from the tracklogs).
    // trackLoaded() stamps an adopted mix colour 0, "changed before the
    // first beat". The first beat after a mix is wherever the new deck has
    // got to - beat 63 to 463 in the logs - so 0 alone let the floor pass at
    // once; it becomes this beat here (beat 0 never reaches tick) (runde 190)
    if (m_colourSince == 0)
        m_colourSince = beat;
    const bool sectionSoon = beatsToNext > 0 && beatsToNext <= 8
        && (nextState == QStringLiteral("drop") || nextState == QStringLiteral("break"));
    // the colour clock turns on a PHRASE line - every eight beats from the
    // section's start - not on any bar line (runde 252, BACKLOG 94: 36 of
    // 107 timed changes on 25-26 Sep fell off the 8-beat grid). A musical
    // turn (turnUp) still takes its own bar.
    bool holdUp = m_colourSince >= 0 && beatInBar == 0 && ((isDrop ? dropBar : bar) % 2) == 0 && sectionSoon == false
               && beat - m_colourSince >= (haveCurves ? holdBeats * 3 / 2 : holdBeats);
    bool turnUp = haveCurves && turn && m_colourSince >= 0 && beatInBar == 0 && sectionSoon == false
               && beat - m_colourSince >= holdBeats / 2;
    // R358_LOOP_COLOUR: inside a DJ loop the bar and phrase lines are the
    // loop's - its first beat. A four-bar loop in a drop changes colour where
    // the loop starts over (beats PLAYED, as m_colourSince counts them), not
    // on a phrase line the loop may never reach.
    if (djLoopOn() && isDrop && m_mixing == false)
    {
        holdUp = m_colourSince >= 0 && beat == m_loopTo && sectionSoon == false
              && beat - m_colourSince >= (haveCurves ? holdBeats * 3 / 2 : holdBeats);
        turnUp = turnUp && beat == m_loopTo;
    }
    // A COLOUR LIVES TWO BARS, whatever asks for it (runde 157).
    //
    // holdUp and turnUp have floors of their own - holdBeats * 3/2 and
    // holdBeats / 2, and holdBeats is never under sixteen - but the section
    // branch had none, and it is the only one that offends. Measured on the
    // night of 2026-09-20: of 318 colour changes inside a track, SEVENTY-FIVE
    // came less than two bars after the one before, seven of them on the very
    // same beat, and every single one of the seventy-five was a section change
    // (48 in a drop, 27 in a break). A section that arrives two beats after
    // the last one is a pair of flags, not a new passage - and a colour that
    // lives one beat reads as a fault, not as a change. A colour change is
    // the one thing the whole room sees at once; it is worth a floor.
    //
    // m_colourSince is -1 on a fresh track (trackLoaded), so the first colour
    // of a track is never held back.
    // ... and not on a drop-out (analyse 10c): a break of two bars or less
    // with the drop right after it took the colour change, and the floor then
    // held it back at the drop - the room landed in a colour it had shown for
    // one dimmed bar. The drop takes it instead, like sectionSoon above.
    // (runde 291: the floor is a quarter of the hold, never under 8 beats - a
    // quiet room recoloured at every break and drop line as often as a full
    // one, whatever the 64-bar hold said; "lav energi er færre skift")
    bool sectionColour = sectionChanged && (isBreak || isDrop)
                      && (m_colourSince < 0 || beat - m_colourSince >= qMax(8, holdBeats / 4))
                      && (isBreak && nextState == QStringLiteral("drop")
                          && beatsToNext > 0 && beatsToNext <= 8) == false;
    bool changeColour;
    if (m_colour.isEmpty())
        changeColour = true;
    else if (m_colourBar < 0)
        changeColour = sectionColour;
    else
        changeColour = sectionColour || holdUp || turnUp;
    if (isCalm)
        changeColour = m_colour.isEmpty();
    if (forceNext)
        changeColour = true;
    // R358_REPEAT: the repeat lands in the first one's colour
    if (m_reuseColour.isEmpty() == false && m_reuseColour != m_colour && sectionChanged
        && m_override.isEmpty() && isCalm == false)
        changeColour = true;
    // NEXT is the operator asking for a new colour, and a mix does not
    // overrule him (runde 237): 10 of 10 NEXT presses during a mix on 25-26
    // Sep changed nothing - seven in nine seconds at 02:16, the room stayed
    // magenta. The next track's colour is drawn again against the new one
    // (the draw below clears m_nextColour). HOLD already lets NEXT through.
    if ((hold || (m_mixing && forceNext == false)) && m_colour.isEmpty() == false)
        changeColour = false;
    if (changeColour || m_colourBar >= 0)
        m_colourBar = 0;
    if (changeColour)
    {
        m_colourSince = beat;
        static const qreal stretch[4] = { 0.5, 0.75, 1.0, 1.5 };
        // the length itself is read every beat, above: the fader picks it
        // (it used to lean a fixed SETUP value by +-30 % here, once per
        // colour). Never under four bars.
        m_holdStretch = stretch[rng->bounded(4)];
    }

    // runde 304: two tiles or more - the lead turns to the next at every
    // colour change the room would have made anyway (hold clock, sections,
    // NEXT); HOLD and a mix hold it as they hold the room colour
    if (m_overrideSet.count() >= 2 && changeColour && m_layerStyle == 0)   // R370: the layer turns it
    {
        const int n = int(m_overrideSet.count());
        m_overrideIdx = (m_overrideIdx + 1) % n;
        // white is never the lead (review 307): the next tile leads instead
        if (m_overrideSet.at(m_overrideIdx) == QStringLiteral("white"))
            m_overrideIdx = (m_overrideIdx + 1) % n;
        m_override = m_overrideSet.at(m_overrideIdx);
    }
    if (engineBannedColour(m_override))
        m_override.clear();
    if (m_override.isEmpty() == false)
        m_colour = m_override;
    else if (changeColour && m_palette.isEmpty() == false)
    {
        // a next-track colour drawn for the OLD room colour (a mix that was
        // abandoned) must not turn the base in the next mix: redrawn against
        // this one when the next mix comes (runde 198). No mix is live here -
        // changeColour is false during one.
        m_nextColour.clear();
        // Drawn, not counted through. Round-robin means the same order every
        // night, and white sat in the rotation like a colour - it is not one,
        // it is a punctuation mark. It comes up about one change in six now,
        // and the flash still reaches for it whenever it likes.
        QStringList pool;
        // White is never the room's colour (Tobias, 2026-09-10: "hvid kun til
        // hits og accenter; en hvid base ser ud som arbejdslys"). It used to
        // come up one change in ten. The flash, the hits and the accent table
        // still reach for it - that is what it is for.
        foreach (const QString &c, m_palette)
        {
            if (c == m_colour || c == QStringLiteral("white"))
                continue;
            pool.append(c);
        }
        if (pool.isEmpty())
        {
            foreach (const QString &c, m_palette)
            {
                if (c != m_colour)
                    pool.append(c);
            }
        }
        if (pool.isEmpty())
            pool = m_palette;
        // Runde 236: nor the colour it came FROM. The draw had no memory: a
        // quarter of the changes on 25-26 Sep went straight back to the colour
        // before (141 inside 16 beats or 30 s) - a phrase flag, the hold timer
        // or a turn rolled the dice and the minor-key weighting gave the one
        // just left about one chance in three.
        if (pool.count() > 1)
            pool.removeAll(m_leftColour);
        m_leftColour = m_colour;
        m_colour = drawColour(pool, m_keyBias, rng);
        if (m_reuseColour.isEmpty() == false && m_palette.contains(m_reuseColour)
            && engineBannedColour(m_reuseColour) == false)
            m_colour = m_reuseColour;    // R358_REPEAT
        m_reuseColour.clear();
    }
    else if (m_colour.isEmpty() && m_palette.isEmpty() == false)
        m_colour = firstRoomColour();

    // A mix going out: the incoming track's colour is drawn the moment the
    // mix begins, and the BASE takes it over the mix's second half (below,
    // where the colour scenes run) while the effects keep the old one. So
    // the room turns towards the next record while it is being
    // mixed in, not a bar after it has landed. The draw leans on the next
    // track's key when BLT has sent it. (Tobias, 2026-09-15: "saa lyset
    // skifter MED musikken, ikke efter".)
    // HOLD is "no colour changes", and ENERGY 0 (STILL, which forces the
    // hold) is "nothing changes": neither may turn the base, nor hand the
    // whole room a new colour when the track lands (runde 171).
    // CALM too (runde 205: it only blocked changeColour, and the base turned
    // and the next track took the colour through a CALM mix), and on a bar
    // line, so a HOLD let go after the turn point does not turn the base mid-bar
    // gen_programs' HARMONY without white - the pairs the mix draws from, and
    // (runde 243) the look's partner colour below
    const QMap<QString, QStringList> &mixesWith = engineMixesWith();   // file scope since review 305
    if (m_mixing && m_mixBeat >= 0 && m_nextColour.isEmpty() && m_palette.isEmpty() == false
        && m_override.isEmpty() && hold == false && isCalm == false
        && (beatInBar == 0 || forceNext))
    {
        // ... or on NEXT's own beat (runde 285): NEXT clears the next colour
        // (above), and off the bar line the base, already turned, fell back
        // to the room's new colour for one to three beats and turned again
        // on the bar line - to a draw that could be the colour it had just
        // left (A-B-A on the base), and a handover in those beats found the
        // base unturned. NEXT is the operator's beat; the base turns on it.
        // Through the mix's second half the base wears the next colour while
        // every other group still wears this one - two colours on the rig for
        // four to eight bars, so they have to go together: "farverne ... skal
        // passe sammen, altid" (Tobias, 2026-09-22). A red room went green
        // under a red strobe. The pairs are gen_programs' HARMONY - the same
        // pairs the two-colour programmes are built from - without white,
        // which is punctuation, not a room. Only if none of them is in the
        // palette does the draw fall back to any colour (runde 171).
        QStringList pool, anyPool;
        foreach (const QString &c, m_palette)
        {
            if (c != m_colour && c != QStringLiteral("white") && engineBannedColour(c) == false)
            {
                anyPool.append(c);
                if (mixesWith.value(m_colour).contains(c))
                    pool.append(c);
            }
        }
        if (pool.isEmpty())
            pool = anyPool;
        if (pool.isEmpty())
            pool = m_palette;
        if (pool.count() > 1)
            pool.removeAll(m_leftColour);   // the mix does not go back either (runde 236)
        m_nextColour = drawColour(pool, m_nextKeyBias, rng);
    }
    // counted from the bar line the mix began in, so the base's turn to the
    // next colour lands on a bar line: 16 of 29 fell on a random beat on
    // 20 Sep - the mix is stamped on whatever beat BLT reported it (runde 188)
    const int mixFrom = m_mixBeat >= 0 ? m_mixBeat - ((m_mixBeat - secStart) % 4 + 4) % 4 : -1;
    int mixBarsOut = (m_mixing && m_mixBeat >= 0) ? qMax(0, beat - mixFrom) / 4 : -1;
    // Live on-air profile, never a guessed loaded deck. Older BLT versions
    // and ambiguous three-deck mixes retain the existing six-bar behaviour.
    const bool incomingFresh = m_fullAuto && m_mixing && m_incomingAt >= 0
        && m_clock.elapsed() - m_incomingAt < 8000 && !m_incomingTitle.isEmpty()
        && m_incomingTitle != m_trackTitle;
    int mixTurnBars = 6;
    if (incomingFresh)
        mixTurnBars = (m_incomingState == "break" || m_incomingState == "intro") ? 4
                    : (m_incomingState == "drop" ? 8 : 6);
    // ... but only BEFORE the base has turned: clearing it after snapped the
    // base back to the old colour the moment HOLD was pressed - HOLD making
    // the very colour change it is there to stop (runde 179). Once turned,
    // the base keeps it and the track takes it when it lands.
    // The turn is LATCHED (runde 270). mixTurnBars is read every beat from
    // the incoming deck's live section, and BLT's profile moves on with it:
    // a build coming in turned the base at bar 6, the incoming deck crossed
    // into its drop at bar 7, the threshold became 8 and the base snapped
    // back to the old colour for a bar - and a handover in that bar found it
    // unturned (trackLoaded does not adopt), so the drop drew a third colour.
    // HOLD in that bar cleared the next colour: runde 179 by another road.
    // Once turned, it stays turned until the mix ends (mixBarsOut -1) or the
    // next colour is gone.
    // R374_MIX_GLIDE (Tobias 10-06: "med mixerens fader ... et langsomt mix kan
    // ogsaa ske selvom faderen bliver hakket op hurtigt"): the mixer tells only
    // ON AIR, never where a fader stands. So the base glides from the fader
    // coming up (the mix) over 8, 12 or 16 bars - twice the old turn point, by
    // what is coming in - in a bar once the incoming track's drop is on, and it
    // is done whenever the old deck goes off air (the handover adopts it). The
    // turn the rest of the engine reads (the accent sitting out, the handover)
    // is half way. HOLD and CALM hold it where it is.
    const bool glideOn = m_mixing && mixBarsOut >= 0 && m_nextColour.isEmpty() == false
                      && m_override.isEmpty() && m_nextColour != m_colour && m_startScene == false;
    if (glideOn)
    {
        if (m_mixGlide == false || m_mixGlideFinish)
        {
            m_mixGlide = true;
            m_mixGlideFinish = false;
            m_mixGlideP = 0.0;
            m_mixGlideRate = 0.0;
            m_mixGlideBeat = beat;
            m_mixGlideFrom = m_colour;
        }
        if (beat != m_mixGlideBeat)
        {
            m_mixGlideP = qMin(1.0, m_mixGlideP + m_mixGlideRate);
            m_mixGlideBeat = beat;
        }
        m_mixGlideTo = m_nextColour;     // NEXT may draw it again
        m_mixGlideFrom = m_colour;       // R377_GLIDE_NEXT: ... and give the room a new colour
        m_mixGlideEnd = -1;
        m_mixGlideKey = baseGroup();     // R375_GLIDE_COLOURPROG: from its first beat
        m_mixGlideRate = (hold || isCalm) ? 0.0
                       : (incomingFresh && m_incomingState == QStringLiteral("drop")) ? 0.25
                       : 1.0 / (8.0 * mixTurnBars);
        if (m_layerTimer.isActive() == false)
            m_layerTimer.start();
    }
    else if (m_mixGlide && m_mixGlideFinish && m_override.isEmpty() && m_startScene == false
             && m_colour == m_mixGlideTo)   // R377_GLIDE_NEXT: NEXT on the new track ends it
    {
        // R376_GLIDE_FINISH: the handover took the colour part way through the
        // glide - a mix shorter than the glide's half, most of them. The rest
        // goes in at most a bar on the new track, not as a cut on its first beat
        if (beat != m_mixGlideBeat)
        {
            if (m_mixGlideBeat >= 0)
                m_mixGlideP = qMin(1.0, m_mixGlideP + 0.25);
            m_mixGlideBeat = beat;
        }
        m_mixGlideRate = 0.25;
        m_mixGlideKey = baseGroup();
        if (m_mixGlideP >= 1.0)
        {
            m_mixGlide = false;
            m_mixGlideFinish = false;
            m_mixGlideKey.clear();
        }
        else if (m_layerTimer.isActive() == false)
            m_layerTimer.start();
    }
    else if (m_mixGlide && (m_mixing == false || mixBarsOut < 0) && m_mixGlideP > 0.0
             && m_override.isEmpty() && m_startScene == false && m_nextColour == m_mixGlideTo)
    {
        // R375_GLIDE_END: the mix is over (mix false), the handover not yet
        // here - BLT sends the track ~100 ms after, and a beat falling between
        // the two snapped the base back and threw the glided colour away. The
        // glide stands 2 beats for the handover (trackLoaded adopts it), then
        // goes back over a bar: a mix pulled out again
        if (m_mixGlideEnd < 0)
            m_mixGlideEnd = beat;
        if (beat != m_mixGlideBeat)
        {
            m_mixGlideP = qBound(0.0, m_mixGlideP + m_mixGlideRate, 1.0);
            m_mixGlideBeat = beat;
        }
        m_mixGlideRate = (hold || isCalm || beat - m_mixGlideEnd < 2) ? 0.0 : -0.25;
        if (m_mixGlideP <= 0.0)
        {
            m_mixGlide = false;
            m_mixGlideKey.clear();
            m_mixGlideEnd = -1;
        }
        else if (m_layerTimer.isActive() == false)
            m_layerTimer.start();
    }
    else if (m_mixGlide)
    {
        m_mixGlide = false;
        m_mixGlideFinish = false;
        m_mixGlideKey.clear();
        m_mixGlideEnd = -1;
    }
    const bool mixTurnDue = mixBarsOut >= 0
        && (m_mixTurnLatched || (m_mixGlide ? m_mixGlideP >= 0.5 : mixBarsOut >= mixTurnBars));
    if ((hold || isCalm) && mixTurnDue == false
        && (m_mixGlide == false || m_mixGlideP <= 0.0))   // R375_GLIDE_HOLD: a glide under way freezes
        m_nextColour.clear();
    m_mixTurnLatched = mixTurnDue && m_nextColour.isEmpty() == false;
    // Once the base has turned to the next track's colour, the accent and
    // the bars' echo - both drawn to go with the OLD colour - could stand
    // next to it in a pair the rules leave out (a cyan room's magenta
    // accent by a green base). Drops ran as grooves in a mix until runde
    // 150-odd, so this never showed; now they sit out the mix's last bars.
    const bool mixTurned = mixTurnDue && m_nextColour.isEmpty() == false
                        && m_override.isEmpty();     // a tile holds the base: it never turned (runde 190)
    const qreal motionTarget = incomingFresh && m_incomingEnergy >= 0.0 && sectionEnergy >= 0.0
                               && mixBarsOut >= 4
        ? qBound(0.90, 1.0 + 0.20 * (m_incomingEnergy - sectionEnergy), 1.10) : 1.0;
    // A stale profile eases back too; no jump when data disappears.
    m_mixMotionScale += qBound(-0.02, motionTarget - m_mixMotionScale, 0.02);

    /* ---- eligible groups: enabled, and with a colour to take ---- */
    QStringList eligible;
    foreach (const QString &key, m_groupOrder)
    {
        if (m_groupOff.contains(key))
            continue;
        // Pattern devices carry colour in their motion scenes; requiring a
        // separate colour scene excludes an otherwise usable animation laser.
        if (candidates(ENGINE_ROLE_COLOR, key).isEmpty()
            && (m_groups.value(key).patternDevice == false
                || candidates(ENGINE_ROLE_MOTION, key).isEmpty()))
            continue;
        eligible.append(key);
    }

    /* ---- cast: the base group is always lit; effects are added on top as
     *      the evening's energy rises. Decided once per section, and never
     *      more than one step from the last section. ---- */
    // Runde 235: a "section" is mostly a phrase flag - five a minute on
    // 25-26 Sep, a median of 21 beats - and every one of them rolled the
    // laser bars' dice, the rotation and the rhythm lead again: 48 % of the
    // bars' stays ended at the first section change after they came in, and
    // Tobias called their chases random. Those three are drawn when the PART
    // changes (drop -> break, a new track), not at every phrase inside it.
    // m_lastState still holds the previous beat's state here.
    // ... and the LOOK's part as well (runde 236): a kick turn (m_curveBreak/
    // m_curveGroove) keeps the flag's string, and a track without flags has
    // no other part change - its rotation stood still for the whole track.
    // NEXT is a new part by definition. m_lookState only moves off HOLD, so a
    // change made under HOLD is still owed when HOLD lets go.
    const QString lookNow = dropWaiting ? QStringLiteral("build")
                          : (dropHidden ? QStringLiteral("normal")
                          // a look the kick chose is filed where it was chosen (runde 193)
                          : (m_curveBreak ? QStringLiteral("break")
                          : (m_curveGroove ? QStringLiteral("normal") : state)));
    const bool partChanged = m_lastState.isEmpty() || m_lastState != state
                          || m_lookState.isEmpty() || m_lookState != lookNow || forceNext;
    // R384_ALONE_LATCH: the bars' alone is decided where the section starts
    if (sectionChanged)
        m_aloneArmed = hold == false && fader >= 0.90;
    if (sectionChanged && hold == false)
    {
        // a random stride, so the rotation of groups, looks and positions
        // does not fall into the same order night after night
        if (m_lastState.isEmpty() == false && partChanged)
            m_castCursor += 1 + int(rng->bounded(2));
        m_motionCursor += 1 + int(rng->bounded(3));
        if (partChanged)
        {
            // R401_CAST_DRAW (Tobias 10-08: "det er bedre hvis det er random,
            // eller den traekker paa en bedre maade"): the effect groups'
            // order for this section, drawn by how long each has rested - the
            // ring the cursor turned only ever put neighbours on stage
            QList<QPair<QString, int>> bag;
            foreach (const QString &key, m_groupOrder)
            {
                const int rest = m_cast.contains(key) ? 0 : qMin(6, m_castRest.value(key, 3) + 1);
                m_castRest.insert(key, rest);
                bag.append(qMakePair(key, 1 + rest));
            }
            m_castOrder.clear();
            while (bag.isEmpty() == false)
            {
                int total = 0;
                for (int i = 0; i < bag.count(); i++)
                    total += bag.at(i).second;
                int roll = int(rng->bounded(qMax(1, total)));
                int pick = 0;
                while (pick < bag.count() - 1 && roll >= bag.at(pick).second)
                    roll -= bag.at(pick++).second;
                m_castOrder.append(bag.takeAt(pick).first);
            }
            // R401_SPECIAL_DRAW: each special a draw of its own, on its own
            // occasions, the odds growing with every one it missed
            m_drawAlone = isBreak && fader >= 0.90 && varietyDraw(m_aloneWait, rng);
            m_drawHome = fader >= 0.90 && varietyDraw(m_homeWait, rng);
            m_drawGrooveStrobes = isDrop == false && preDrop == false && isBuild == false
                               && isBreak == false && isIntro == false && isOutro == false
                               && fader >= ENGINE_STROBE_ON && varietyDraw(m_grooveWait, rng);
            // R401_DROP_LEAD: a drop with room for one group gave it to the
            // strobes every time - the night at 55 % had the wash and the
            // strobes alone in 28 of 39 drops. After such a drop the next
            // one hands the first place to the drawn order more often than not
            if (m_lastState == QStringLiteral("drop"))
                m_lastDropStrobesOnly = m_dropStrobesOnly;
            m_dropStrobesOnly = isDrop;
            m_dropLeadYields = isDrop && m_lastDropStrobesOnly && rng->bounded(100) < 60;
        }
        // the cooldown is judged from HERE for the whole section: judged from
        // "now" every beat, the programme that had just started counted as
        // recent on its second beat, fell out of the list, and the pick moved
        // on - one programme per beat through the whole pool
        m_cooldownMs = m_clock.elapsed();
        // What figure each group showed in the section that just ended. Read
        // HERE and nowhere else: the pick runs every beat, so if this were
        // kept up to date continuously it would hold the family of the
        // programme that is running right now - and motionFor() would walk
        // away from it on the very next beat, changing programme every beat
        // instead of every section.
        // AN INNER BUILD FLAG IS NOT A NEW SECTION FOR THE PROGRAMMES (runde
        // 276). A build of several flags climbs once (runde 237) and is drawn
        // once (runde 275), but this clear still re-picked every group's
        // programme at each inner flag, on a cursor stepped just above: a
        // 16 + 16 + 16 build changed chase twice on its way up, a held climb
        // or chase restarted from its first step, and the static look of the
        // first half was re-picked for no reason. A build that has drawn its
        // moves (m_buildDrawn, last beat's) holds what it picked - the build's
        // own releases still apply (the middle, a climb joining at 32 / 16
        // left, a colour change, the ceiling moving, a fader jump or a new
        // composition, which clear it below). The kick wait at the drop flag
        // holds too: "the build's look holds" there.
        const bool buildGoesOn = isBuild && m_buildDrawn;
        if (buildGoesOn == false)
        {
            m_sectionMotion.clear();
            m_lastFamily.clear();
            for (QMap<QString, quint32>::const_iterator it = m_active.constBegin();
                 it != m_active.constEnd(); ++it)
            {
                if (it.key().startsWith(QStringLiteral("mot:")) == false)
                    continue;
                const TrackFuncInfo &fi = m_funcs.value(it.value());
                if (fi.family.isEmpty() == false)
                    m_lastFamily.insert(it.key().mid(4), fi.family);
            }
        }
        // R358_REPEAT: the first one's programmes, where they still exist
        for (QHash<QString, quint32>::const_iterator it = m_reuseMotion.constBegin();
             it != m_reuseMotion.constEnd(); ++it)
        {
            if (m_funcs.contains(it.value()))
                m_sectionMotion.insert(it.key(), it.value());
        }
        m_reuseMotion.clear();
        // Do the laser bars lead this section? Drawn once, by the energy:
        // at a quarter of the fader roughly one drop in seven, at the top
        // nearly every drop (95 %) and six grooves in ten. Not always - "de
        // skal vaere der ofte i perioder, jo hoejere energi, jo mere" (Tobias,
        // 2026-09-16) - and a section they sit out is what makes the one they
        // come back in read as an arrival.
        qreal pLead = qBound(0.0, (energy - 0.25) / 0.70, 1.0);
        pLead = isDrop ? 0.15 + 0.80 * pLead : 0.10 + 0.50 * pLead;
        if (partChanged)
            m_barsLead = rng->bounded(1000) < int(pLead * 1000.0);
    }

    QString base = baseGroup();
    // "silent" is meant for the gap between tracks, not for a quiet passage.
    // At 0.12 it caught every break in the set and emptied the cast, which
    // is exactly the "in breaks the light just goes out" report. And the
    // base is now outside it altogether: the room is never black while a
    // track is playing.
    bool silent = sectionEnergy >= 0.0 && sectionEnergy < 0.04;

    // how many effect groups join the base: a ramp of the energy, with the
    // fraction decided by dice once per section - 55 % and 65 % differ
    // isDrive by value, energy by reference: the lambda has no capture-default
    auto effectsWant = [&energy, isDrive, fader](bool drop) -> qreal {
        qreal want = drop ? 3.0 * qBound(0.0, (energy - 0.05) / 0.80, 1.0)
                          : 2.0 * qBound(0.0, (energy - 0.10) / 0.75, 1.0);
        if (drop == false && isDrive)
            want = 2.5 * qBound(0.0, (energy - 0.08) / 0.78, 1.0);
        // R374_TOP_GROUP: ... or the SLIDER's last tenth - a quiet section at
        // 100 % kept the energy under 0.80 and the top added nothing there
        want += 1.0 * qMax(qBound(0.0, (energy - 0.80) / 0.20, 1.0), qBound(0.0, (fader - 0.90) / 0.10, 1.0));
        return want;
    };
    auto effectsFor = [&effectsWant, rng, fader](bool drop, bool brk) {
        // a break used to empty the room down to the base. Late in the night
        // it keeps one group as well - quieter than a groove, not dark.
        // A break is a quiet section, not an empty one. It always keeps one
        // group besides the base, and from half a fader upwards it keeps two -
        // so the ENERGY slider is felt in a break as well, which it was not.
        // A break is the base alone (Tobias, 2026-09-10: "tilbage til ikke
        // at have effekter"). Once in a while - one break in three - it may
        // add ONE thing, and the pool below makes sure that thing is either
        // the laser bars, at home, running a slow chase, or the animation
        // lasers on their flat fan ("flad vifte"). Nothing else, ever.
        // (runde 291: one break in five at the bottom of the slider, one in
        // two at the top - it was one in three at every fader)
        if (brk)
            return rng->bounded(1000) < int((0.20 + 0.30 * qBound(0.0, fader, 1.0)) * 1000.0) ? 1 : 0;
        // The top of the ENERGY fader has to mean something: at full it is
        // three groups on a drop and two in a groove, not two and one.
        // four groups on a drop at the stop, three in a groove: the fader's
        // last quarter has to add rig, not just brightness
        // The ramps start low and run the whole fader: a groove began adding
        // rig at 0.25, so the bottom third of the fader was the base alone
        // and only brightness told you it was moving (Tobias, 2026-09-18).
        qreal want = effectsWant(drop);
        int whole = int(want);
        qreal frac = want - whole;
        return whole + (rng->bounded(1000) < int(frac * 1000.0) ? 1 : 0);
    };
    // A NUDGE: a twentieth of the fader since the last decision, read on
    // the bar line. Smaller than a jump (0.20, which redraws the whole look)
    // and deterministic - no dice - so it moves the room IN THE DIRECTION OF
    // THE FADER by one step: a group joins or leaves, the star ceiling moves
    // a notch and a programme that is now too cold or too hot gives way.
    // Tobias, 2026-09-18: "hvis man f.eks. skifter den 2 % sker der jo
    // ikke rigtigt noget." Two per cent is inside the live curves (level,
    // figure, pace, pulse); five is where a discrete step is owed.
    bool faderNudge = hold == false && beatInBar == 0 && isBreak == false
                   && m_castEnergy >= 0.0 && qAbs(fader - m_castEnergy) >= 0.05;
    // A hand on the ENERGY fader: a fifth of it or more since the moves
    // were last drawn, read on the bar line. Used here for the cast and
    // further down for the moves, the figure, the zoom, the star ceiling and
    // the held programme - see the comment at `redraw`.
    // (runde 289: both on the SLIDER, as their comments say. On the section-
    // scaled energy a quiet section needed 7 % for a nudge and 25 % for a
    // jump, and a section energy measured late (a -1 flag, a BLT resend)
    // moved the scaled value up to 16 % with no hand on the fader at all.)
    bool faderJump = hold == false && beatInBar == 0 && m_movesFader >= 0.0
                  && qAbs(fader - m_movesFader) >= 0.20;
    bool nudgeOwed = false;
    if ((sectionChanged || m_lastState.isEmpty()) && hold == false)
    {
        // "the section before a build" is taken where the build STARTS
        // (runde 269). At an inner flag of the same build (m_lastState still
        // "build") it was overwritten with the build's own roll, which the
        // clamp below lets sit one under the groove's: groove 2, flag one
        // rolled 1 and showed 2 (3 past the middle), flag two then floored on
        // 1 and showed 2 - a group left the room half way up the climb, the
        // sawtooth runde 237 took out of the level. A fader jump or nudge
        // inside the build still moves the floor (below), and a kick wait
        // (isBuild, m_lastState "build" at the drop flag) keeps the build's.
        if (!(isBuild && m_lastState == QStringLiteral("build")))
            m_effectsBefore = m_effects;
        int want = effectsFor(isDrop, isBreak);
        // A break shows its OWN roll (runde 263). Through m_effects the roll
        // was clamped one step from the section before - after a groove or a
        // drop of two or more groups, qBound(m_effects - 1, 0, ...) lifted
        // the 0 to 1, and the final clamp to one group let it through: the
        // "one break in three" extra stood in nearly every break after a
        // drop (BACKLOG 86 asks whether breaks are too full). m_effects
        // still steps as before - it is the cast's memory for the section
        // after the break. Rolled when the PART changes, as the bars' lead
        // (runde 235): an inner phrase flag of the same break keeps it.
        if (isBreak && partChanged)
            m_breakExtra = want;
        // ... and the memory steps ONCE per break, where it starts (runde
        // 270). A break rolls 0 or 1, and every inner phrase flag stepped
        // m_effects one further down: a drop of four groups at 100 %, then a
        // 64-beat break flagged in four - 3, 2, 1, 0 - and the drop after it
        // landed with one or two groups where a one-flag break gave it four.
        // The more phrases the breakdown had, the smaller the drop it led to.
        // A groove's or a build's inner flag still steps (a walk around what
        // the energy asks for, not a drain).
        if (isBreak == false || partChanged)
            m_effects = qBound(m_effects - 1, want, m_effects + 1);
        m_effectsBeat = beat;
    }
    else if (faderJump)
    {
        // ... in a break as well (runde 263): the break shows m_breakExtra,
        // not m_effects, so the jump is stored as the memory the section
        // after the break steps from. It used to be skipped in a break and
        // spent anyway (m_castEnergy below) - 20 -> 100 % in a break landed
        // the drop one step from the break's budget, and no nudge followed.
        // (isDrop is false in a break: a groove's budget at the new energy.)
        // The one-step-per-section rule is for the music moving the room;
        // it is not for the operator. From 20 % to 100 % it took three or
        // four sections - up to two minutes - before the room was full, and
        // in that time the fader looked broken. A jump is a decision: the
        // cast goes straight to what the new energy asks for, on this bar.
        // ... and only the way the hand went (runde 269), as the nudge's
        // sameWay (runde 172): effectsFor dices, so a groove pushed from 0.55
        // to 0.76 rolled one group a time in four where it had two - the room
        // got SMALLER under a hand going up, and m_castEnergy below spent the
        // move so no nudge put it back. m_movesEnergy is the jump's own
        // reference and is only moved further down (redraw).
        const int jumpRoll = effectsFor(isDrop, false);
        m_effects = fader > m_movesFader ? qMax(m_effects, jumpRoll) : qMin(m_effects, jumpRoll);
        // AFTER the new value (runde 262): "a build never has fewer groups
        // than the section before it" is about the music, and the operator's
        // hand is the new "before". Set first, a build pulled from 90 to 40 %
        // kept the old three groups to the drop (qMax below).
        m_effectsBefore = m_effects;
        m_effectsBeat = beat;
    }
    else if (faderNudge)
    {
        // one step towards what the fader asks for - rounded, not diced
        //
        // ... but not oftener than every two bars (runde 159). `energy` here is
        // the slider TIMES the section's own loudness, so the analysis curve on
        // its own drifts past the 0.05 nudge threshold every bar or two with
        // nobody touching anything - and every step of this budget puts a group
        // on stage or takes one off. Measured on the night of 2026-09-20: NINETY-
        // NINE times a group left the cast and was back inside two bars, and 53
        // of those happened without any section change at all - the strobes ten
        // times inside a groove, the animation laser ten more. A group blinking
        // out for a bar and returning reads as a fault.
        //
        // A section change and a real hand on the fader (faderJump, a fifth of
        // the slider) both skip this: those are decisions, not drift.
        int want = int(qRound(effectsWant(isDrop)));
        // ... and only the way the fader moved. m_effects came from a dice
        // roll, so a fader pushed UP could round `want` below it and take a
        // group OFF (runde 172).
        const bool sameWay = (want > m_effects) == (fader > m_castEnergy);
        if (want != m_effects && sameWay && (m_effectsBeat < 0 || beat - m_effectsBeat >= 8))
        {
            m_effects = qBound(m_effects - 1, want, m_effects + 1);
            m_effectsBefore = m_effects;         // after, as the jump (runde 262)
            m_effectsBeat = beat;
        }
        else if (want != m_effects && sameWay)
            nudgeOwed = true;
    }
    // A nudge the dwell held back is OWED, not spent: the reference stays
    // where it was, so the step comes on the first bar the dwell allows. It
    // used to be moved to the new energy anyway - and from then on the fader
    // read as unmoved, so a hand pushing ENERGY 5-19 % within two bars of a
    // section change got no step until the next section (runde 168). Drift
    // that has gone back by then asks for nothing (want == m_effects) and is
    // spent as before.
    if (sectionChanged || faderJump || (faderNudge && nudgeOwed == false) || m_castEnergy < 0.0)
        m_castEnergy = fader;
    // Every re-pick below sits behind `hold == false`, so this is exactly the
    // moment the look on stage may change. A verdict belongs in the section
    // the look was CHOSEN for, not the one the track happens to have reached:
    // freeze a drop look, let it ride into the break, thumb it up, and without
    // this the drop look collects credit under "break" - and then gets
    // favoured in breaks, where nothing ever chose it. HOLD is the button you
    // press when you like what you see, so this is not a corner case.
    // A hidden drop (under ENGINE_DROP_SHOW) was CHOSEN with groove rules, so
    // its verdicts belong to the groove bucket, not the drop's
    // the drop ARRIVES (not a drop running on into its next section): while
    // one waits for its kick m_lookState reads "build" (runde 233)
    const bool dropArrives = isDrop && m_lookState != QStringLiteral("drop");
    // R369_FORCED_LAND: the landing is the drop's first downbeat ONCE per drop.
    // R365 tied it to the beat the drop look arrived - but a DROP the operator
    // presses a bar or three early (the build still running) arrives mid-bar
    // on the build's bars, and the music's own drop then came into a look that
    // was already "drop": no strobe landing, no white, no flash anywhere
    // (harness forceA.json: DROP at 46, the drop at 57 - only its impact).
    // A drop arriving any other way had its one chance on that beat (a track
    // loaded mid-drop, a second drop flag inside a running drop: no landing,
    // as in runde 365); a loop over the landing bar finds it landed.
    if (isDrop == false)
        m_dropLanded = false;
    else if (dropArrives && m_dropForced == false)
        m_dropLanded = true;
    m_dropArrivedNow = isDrop && dropBar == 0 && beatInBar == 0 && (dropArrives || m_dropLanded == false);   // R365_LAND_ONCE
    if (m_dropArrivedNow)
        m_dropLanded = true;
    // R370_COLOUR_LAYER: the tiles fading or chasing - the pace, the step and
    // the lead for this beat (the groups are painted in the cast loop below)
    updateColourLayer(beat, base, isBreak, isBuild, isDrop, prog, fader,
                      hold || still || isCalm, forceNext || m_dropArrivedNow || m_lateLandNow, kick);
    // R360_DROP_SIZE (Tobias 10-05: "landingens stoerrelse foelger droppets
    // stoerrelse - men energi-faderen skal stadig bestemme hvor vildt det er"):
    // measured where the drop arrives. ENERGY is the ceiling as before; the
    // drop's size only takes a small drop down from it - shorter strobes, one
    // impact bar, quarters, no white.
    if (dropArrives)
        // R361_DROP_FORCED: a DROP the operator pressed is a whole drop - the
        // music under it is often a groove with no jump at all (size 0)
        // R363_LATE_SIZE: a late landing is measured from its bar line - from
        // beat 2-4 the "before" window held the silence and the downbeat itself
        m_dropSize = m_dropForced ? 1.0 : dropSizeAt((dropBar == 0 && beatInBar > 0) ? beat - beatInBar : beat);
    // R362_BUILD_SIZE (Tobias 10-05, "builden efter det kommende drop"): the
    // drop ahead is measured as the landing will measure it (dropSizeAt), and
    // the build into a small one is smaller - shorter stab windows, no eighths
    // under 0.40, a lower top, a slower strobe riser and no black beat. A big
    // drop keeps everything ENERGY gives. Only ever down from the slider.
    // R363_NEXT_DROP: the next drop FLAG from the trackmanager, not the next
    // section - a build flagged 16 + 16 ran its first half at full size and
    // its second at the drop's (the top, the stabs and the strobe riser
    // jumped down mid-build); and a kick wait keeps what the build had (the
    // wait reads as a build with the section AFTER the drop next).
    if (dropWaiting)
        ;
    // R364_LONG_BUILD: a build (or the build part of one) reads its drop
    // however far it is - a 32-bar build switched from full to the drop's size
    // 64 beats out, a step in its top in the middle of the climb. Other
    // sections only the last 16 bars before a drop.
    else if (isDrop == false && m_nextDropBeat > beat && (m_nextDropBeat - beat <= 64 || (isBuild && m_nextDropBeat - beat <= 160)))
        m_nextDropSize = dropSizeAt(m_nextDropBeat);
    else if (isDrop == false)
        m_nextDropSize = 1.0;
    // R362_MINI_LAND (Tobias 10-05: "en kort pause inde i et drop faar en
    // mini-landing"): the kick gone for a bar or more inside a drop the
    // analysis did not split, and back. The nights of 10-02/03/04: 34 such
    // returns after 4-16 beats (Fergie 131 and 323, Hold On 353 and 385,
    // Keinemusik & Drake 257). On that beat the strobe lamps blink the
    // sixteenths as at the landing - under MASTER and the trim, the freedom
    // is the real landing's - one beat, two from 85 % on a big jump; a hit
    // with it from a jump of 0.40. Sized by how far the sound jumps out of the
    // pause. From 60 % on the slider; never in the landing bar, under CALM,
    // HOLD, FLASH or BLACKOUT, at most once in 16 beats.
    // R366_MINI_ONCE: THIS beat's mini landing. The hit and the log read
    // `m_miniLandLast == beat`, which is true again on every pass of a DJ loop
    // over the return beat (the harness: a hit and `mini-land` on all five
    // passes, though the 16-beat spacing held the blink back)
    bool miniNow = false;
    if (isDrop && dropBar > 0 && fader >= 0.66 && hold == false && isCalm == false && still == false   // R374_SPREAD
        && m_flash == false && m_blackout == false && (beat - m_miniLandLast >= 16 || beat < m_miniLandLast)
        && beat > 4 && beat <= m_audKick.count() && m_audKick.count() == m_audLevel.count()
        && m_audKick.at(beat - 1) >= 115)
    {
        int gap = 0;
        while (gap < 17 && beat - 2 - gap >= 0 && m_audKick.at(beat - 2 - gap) < 51)
            gap++;
        if (gap >= 4 && gap <= 16 && beat - gap - 3 >= 0
            && m_audKick.at(beat - gap - 2) >= 115 && m_audKick.at(beat - gap - 3) >= 115
            && beat + 3 <= m_audLevel.count())
        {
            qreal after = 0.0, during = 0.0;
            for (int i = beat; i <= beat + 3; i++)
                after += m_audLevel.at(i - 1) / 255.0;
            for (int i = beat - gap; i <= beat - 1; i++)
                during += m_audLevel.at(i - 1) / 255.0;
            const qreal jump = after / 4.0 - during / qreal(gap);
            m_miniSize = qBound(0.0, (jump - 0.10) / 0.40, 1.0);
            const int miniBeats = (fader >= 0.85 && m_miniSize >= 0.60) ? 2 : 1;
            m_miniLandUntil = beat + miniBeats - 1;
            m_miniLandLast = beat;
            miniNow = true;
        }
    }
    if (hold == false)
        m_lookState = lookNow;          // computed where partChanged is (runde 236)
    m_lastState = state;

    // r162: three minutes of sustained dense light earns four restrained bars
    // in a groove. Never flatten a drop/build or override HOLD/ENERGY.
    const qint64 stageNow = m_clock.elapsed();
    const qreal density = (m_fullAuto && !m_blackout && !m_flash)
        ? fader * qBound(0.0, qreal(m_cast.count()) / 4.0, 1.0) : 0.0;
    m_exposure.sample(stageNow, density);
    // Runde 284: the rest is four WHOLE bars, or it waits. It was taken on
    // the first bar line the charge was ready, whatever came next, and a
    // build/drop/break flag cleared it: after a drop, a 12-beat groove into
    // the next drop lost a group for three bars and got it back on the drop -
    // and the pre-drop bar, which pulls the cast IN so the drop lands lit
    // (runde 262), was the rest's last bar and pulled one OUT. A groove/drive
    // flag does not end it, so only the others must be 16 beats away (a
    // drop 20: its pre-drop bar ends a rest); the charge stays ready and the
    // rest comes a bar or a section later. The pre-drop bar ends one that is
    // running (a flag the operator moved closer).
    const bool restFits = beatsToNext <= 0
        || beatsToNext >= (nextState == QStringLiteral("drop") ? 20 : 16)
        || nextState == QStringLiteral("groove") || nextState == QStringLiteral("drive");
    if (!m_fullAuto || hold || isDrop || isBuild || isBreak || isCalm || m_mixing || preDrop)
        m_restUntil = -1;
    else if (beatInBar == 0 && m_restUntil < beat && restFits && m_exposure.ready(stageNow))
    {
        m_restUntil = beat + 16;
        m_exposure.rest(stageNow);
    }
    const bool exposureRest = m_restUntil > beat;
    int effects = isBreak ? m_breakExtra : m_effects;     // runde 263
    // a build is the room filling up: it never has fewer groups than the
    // section before it, and past the middle it reaches for one more
    // prog on the bar's FIRST beat (runde 242): a build's steps - the extra
    // group, the chases, the tighter figure, the faster roll - are decided on
    // this, so they land on a bar line and at the middle together. `prog >
    // 0.5` missed the middle itself (16/32 is exactly 0.5): on 25-26 Sep the
    // extra group joined on offset 17 and the figure changed on offset 20,
    // beat two of a bar and a bar later - nothing on the 16-beat phrase.
    // (runde 277: read where prog is set, progBar. It was interpolated back
    // from prog and progBefore, and progBefore is clamped to where a later
    // build flag starts - so on beats 2-4 of that flag's first bar barProg
    // crept above the bar's own value, and a step decided "on the bar line"
    // could flip mid-bar.)
    const qreal barProg = qBound(0.0, progBar, 1.0);
    // THE CLIMB REACHES ITS TOP (runde 277). Decided on the bar line, a
    // build's ramps (the bare blink's steps and subdivisions, the roll, the
    // zoom) read barProg, and the last bar of a build starts four beats
    // before the drop: barProg tops at 1 - 4/len. A 16-beat build never got
    // past 0.75 - at the top of the fader the bare blink stopped at eighths
    // ("sixteenths at the top", 2.93 subs of the 3.0 it needs), the zoom at
    // step 2 of 8, an 8-beat build's roll at one halving of two. Only builds
    // of 32 beats and more ever reached the end, and only the zoom's never.
    // rampProg stretches the frame so its last bar reads the top and its first
    // bar reads where it starts: 0 / .33 / .67 / 1 over a 16-beat build (was
    // 0 / .25 / .5 / .75). Only where the frame ends at the top (a single
    // build, the flag before the drop, a riser-promoted build); an inner flag
    // stops short of the top anyway (runde 237) and reads barProg. The kick
    // wait holds the build's top ("the build's look holds", runde 172) - it
    // read 0.75, a step back from the build's last bar. The build's MIDDLE
    // (the extra group, the chases, the climb joining, the figure's
    // tightening) stays on barProg / prog: that is the 16-beat phrase.
    qreal rampProg = barProg;
    if (dropWaiting)
        rampProg = 1.0;
    else if (isBuild && rampTop >= 1.0 && 1.0 - rampLo - barStep > 0.0)
        rampProg = qBound(0.0, rampLo + (barProg - rampLo) * (1.0 - rampLo) / (1.0 - rampLo - barStep), 1.0);
    // R359_RISER (Tobias 10-05, "builden foelger riseren i mere end
    // lysstyrken"): the build's ramps - the strobes' blink stepping up, the
    // chases' steps, the zoom closing - follow the measured riser where the
    // music climbs, read on the bar line as the ramps are (a quarter of the
    // count stays in). Flat builds keep the count.
    const int riserTo = (nextState == QStringLiteral("drop") && beatsToNext > 0) ? beat + beatsToNext : secEnd;
    const qreal riserBar = isBuild ? riserAt(beat - beatInBar, secStart, riserTo) : -1.0;
    if (riserBar >= 0.0 && dropWaiting == false)
        rampProg = qBound(0.0, 0.25 * rampProg + 0.75 * riserBar, 1.0);
    if (isBuild)
    {
        effects = qMax(effects, m_effectsBefore);
        if (barProg >= 0.5 && energy > 0.30)
            effects = qMax(effects, m_effectsBefore + 1);
    }
    if (preDrop)     // no dice here: four beats of joining and leaving would flicker
        effects = qMax(effects, int(qRound(3.0 * qBound(0.0, (energy - 0.05) / 0.80, 1.0))));
    if (exposureRest) effects = qMax(0, effects - 1);
    if (dropSettled && effects > 2) effects--;          // the drop settles (above)
    if (isCalm || still)
        effects = 0;
    // A long blend keeps its musical activity budget. There is no elapsed-
    // mix countdown to the base: the section and ENERGY decide when it rests.
    // One budget for the whole cast. Breaks never inherit a peak's groups;
    // the top of ENERGY can use four effects plus the base on a drop.
    // R361_ENERGY_TOP: the step up of a growing or a repeated drop goes to
    // the top of what ENERGY asks for (its fraction rounded UP), never past
    // it - the dice may have rounded down, the music may round up. It added
    // a group at any fader: at ENERGY 20 % a grown drop had two.
    const int energyTop = int(effectsWant(isDrop) + 0.999);
    if (m_dropGrow && isDrop && effects < energyTop)
        effects++;                       // R360_DROP_GROW: the music grew - one group more
    if (m_lookActive.isEmpty() == false && (isDrop || isBuild) && effects < energyTop)
        effects++;                       // R358_REPEAT: the repeat is one step up - one group more
    effects = qBound(0, effects, isBreak ? 1 : (isDrop || preDrop ? 4 : 3));
    if (base.isEmpty())
        effects = qMax(effects, 1);                 // no base: something must show

    // the strobes' place in the pool is decided on the bar line and held to
    // the next one (fejljagt 2, 09-27): read per beat, the pool was one
    // shorter on beats 2-4 whenever they had lost the budget on beat 1, and
    // two groups swapped every bar; kept in the pool and skipped instead, the
    // group after them in the rotation led twice as often under the line
    if (beatInBar == 0 || sectionChanged)
    {
        m_strobesPooled = fader >= ENGINE_STROBE_ON;
        m_oneLaser = fader < 0.85;
        m_aniPooled = fader >= ENGINE_ANI_ON;          // runde 344
    }
    QStringList pool;
    foreach (const QString &key, eligible)
    {
        if (key == base)
            continue;
        // the one thing a break may add is the laser bars - at home, with a
        // slow chase (see `moving` and the EFX gate below) - or the animation
        // lasers on their flat fan (tier 0 favours "vifte", see tierOf()).
        // Heads, strobes and the rest sit a break out.
        // Runde 235 (Tobias: "Breaks må gerne bruge andet end basen f.eks.
        // strobelamper"): the strobes may be that one thing too - a slow,
        // soft walk (drawMove, tier 0), never a burst (driveStrobe's kill).
        // The other wash groups are not in this pool: they join every break
        // outside the budget (below).
        if (isBreak && m_groups.value(key).lasers == false && m_groups.value(key).patternDevice == false
            && m_groups.value(key).strobes == false)
            continue;
        // the strobes are the club; under ENGINE_STROBE_ON this is a bar
        // ... read on the bar line (runde 242): the fader crossing the line
        // mid-bar put them in and out of a drop on beats 2-4 (24 times on
        // 25-26 Sep, the Minis standing in for 3-12 beats). Mid-bar they keep
        // what the last bar line decided (m_strobesPooled, above).
        // RUNDE 344 (Tobias, 10-03: "animationslaseren skal foerst komme i
        // spil ved 70%"). On 10-02 it stood on stage 511 beats under 70 % on
        // the slider - in breaks (its flat fan) and drops alike. Same bar-line
        // reading as the strobes below, so it never joins or leaves mid-bar.
        if (m_aniPooled == false && m_groups.value(key).patternDevice)
            continue;
        if (m_strobesPooled == false && m_groups.value(key).strobes)
            continue;
        pool.append(key);
    }

    // Hats availability is settled BEFORE allocating the budget, so another
    // eligible effect can take that place. Hysteresis is still on bar lines.
    if (hats >= 0.0 && beatInBar == 0 && hold == false)
    {
        if (m_hatsOut == false && hats < 0.20) m_hatsOut = true;
        else if (m_hatsOut && hats > 0.32) m_hatsOut = false;
    }
    if (hats < 0.0) m_hatsOut = false;
    QStringList priority;
    // R401_CAST_DRAW: the section's drawn order, then any group it did not know
    QStringList castOrder;
    foreach (const QString &key, m_castOrder)
        if (pool.contains(key))
            castOrder.append(key);
    foreach (const QString &key, pool)
        if (castOrder.contains(key) == false)
            castOrder.append(key);
    for (int i = 0; i < castOrder.count(); i++)
    {
        QString key = castOrder.at(i);
        if (m_hatsOut && m_groups.value(key).strobes)
            continue;
        priority.append(key);
    }
    // One rhythmic lead in a drop, INSIDE the same budget. The remainder
    // keeps the section's rotation; fixture names never decide who is cut.
    // Runde 233 (Tobias: "under grooves og i normal kan de godt køre en
    // langsom beat-chase en gang imellem"): one groove section in three,
    // above the dance-floor line, the strobes take the first effect place
    // too. m_castCursor only moves on a section change, so it holds for the
    // section.
    // ... and "above the line" is the bar line's reading, m_strobesPooled
    // (runde 279): the live fader here took the lead away on beat 2 of a bar
    // when a hand or the closing lid went under 55 %, while the strobes stayed
    // in the pool to the downbeat - the bars or the next group stepped in for
    // beats 2-4 and the downbeat swapped again (fejljagt 2 by a side door).
    const bool grooveStrobes = isDrop == false && preDrop == false && isBuild == false
                            && isBreak == false && isIntro == false && isOutro == false
                            && m_strobesPooled && m_drawGrooveStrobes;   // R401_SPECIAL_DRAW
    // (not the last bar of a BREAK before a drop, runde 236: the strobes are
    // in the break's pool now, and preDrop lifts the budget to one - they
    // took the slot before every drop and stood one lamp lit for a bar)
    const bool strobeLead = (isDrop && dropSettled == false && m_dropLeadYields == false)   // R401_DROP_LEAD
                         || (preDrop && isBreak == false) || grooveStrobes;
    if (strobeLead && hold == false && isCalm == false)
    {
        for (int i = 0; i < priority.count(); i++)
        {
            if (m_groups.value(priority.at(i)).strobes)
            {
                priority.prepend(priority.takeAt(i));
                break;
            }
        }
    }
    // The laser bars are the room's signature: when this section drew them
    // as lead (m_barsLead, by the energy - see the section-change block)
    // they take the first effect place, behind the drop's strobe lead,
    // inside the same budget. Left to the rotation alone they were in the
    // cast one section in three whatever the fader said.
    if (isBreak == false && m_barsLead && hold == false && isCalm == false)
    {
        int slot = 0;
        if (strobeLead && priority.isEmpty() == false && m_groups.value(priority.first()).strobes)
            slot = 1;
        for (int i = 0; i < priority.count(); i++)
        {
            const TrackGroup &pg = m_groups.value(priority.at(i));
            if (pg.lasers && pg.patternDevice == false)
            {
                if (i > slot)
                    priority.insert(slot, priority.takeAt(i));
                break;
            }
        }
    }
    // Runde 235: one laser type at a time below the top of the fader - the
    // bars or the animation laser, whichever the order put first; the next
    // group in line takes the other's place. On 25-26 Sep Tobias switched a
    // laser off twelve times for over a minute, five of them while both were
    // on stage. 0.85 is a guess from that - the rig decides.
    // ... read on the bar line with the strobes' pool (runde 279): the clock
    // reaches 85 at 02:14 on any beat, and the laser type that came back into
    // `priority` joined mid-bar and pushed the last group out mid-bar (laser
    // slots cut hard); the closing lid did the reverse at ~02:55.
    if (m_oneLaser)
    {
        bool haveBars = false, haveAni = false;
        for (int i = 0; i < priority.count(); )
        {
            const TrackGroup &pg = m_groups.value(priority.at(i));
            const bool isBars = pg.lasers && pg.patternDevice == false;
            if ((isBars && haveAni) || (pg.patternDevice && haveBars))
            {
                priority.removeAt(i);
                continue;
            }
            haveBars = haveBars || isBars;
            haveAni = haveAni || pg.patternDevice;
            i++;
        }
    }
    // R383_BARS_ALONE (Tobias 10-06: "eller taendt alene med 8-eye chase (uden
    // basen i breaks)"): from 90 % on the slider one break in three is the
    // laser bars alone - the base and the other washes sit it out
    QString aloneBars;
    // R385_BARS_FULL: under 85 % the alone lets go for the rest of the section
    if (m_aloneArmed && fader < 0.85)
        m_aloneArmed = false;
    if (isBreak && m_aloneArmed && fader >= 0.85 && m_drawAlone     // R401_SPECIAL_DRAW
        && (hold == false || m_barsAloneNow) && isCalm == false   // R384_ALONE_HOLD: HOLD keeps it
        && still == false && silent == false)
    {
        foreach (const QString &key, eligible)
        {
            const TrackGroup &ag = m_groups.value(key);
            if (ag.lasers && ag.patternDevice == false && ag.parts.count() >= 2)
            {
                aloneBars = key;
                break;
            }
        }
    }
    // R385_BARS_FULL: two kinds - the whole-bar chase, or full light with the
    // slow lift. R401_ALONE_KIND: drawn when the alone starts, the other kind
    // than last time more often than not (it was strictly in turn)
    if (aloneBars.isEmpty() == false && m_barsAloneNow == false)
    {
        m_aloneBreaks++;
        m_aloneFullLast = rng->bounded(100) < 65 ? m_aloneFullLast == false : m_aloneFullLast;
    }
    m_barsAloneNow = aloneBars.isEmpty() == false;
    m_aloneFull = m_barsAloneNow && m_aloneFullLast;
    QSet<QString> castSet;
    if (m_barsAloneNow)
        castSet.insert(aloneBars);
    else if (base.isEmpty() == false)
        castSet.insert(base);
    if (silent == false && m_barsAloneNow == false)
    {
        for (int i = 0; i < qMin(effects, int(priority.count())); i++)
            castSet.insert(priority.at(i));
        // Runde 235: the other WASH groups stand in every break beside the
        // base, outside the budget - the Minis with the heads, or the heads
        // with the Minis when those are the base ("jeg forventer at minis er
        // i breaks sammen med basen også, da de minder om hovederne
        // lysmæssigt" - Tobias, 2026-09-27). Not under CALM or a still room:
        // those are the base alone. HOLD rebuilds the cast from m_cast below.
        if (isBreak && isCalm == false && still == false)
        {
            foreach (const QString &key, eligible)
            {
                const TrackGroup &wg = m_groups.value(key);
                if (key != base && wg.strobes == false && wg.lasers == false && wg.patternDevice == false)
                    castSet.insert(key);
            }
        }
    }

    if (hold && still == false && isCalm == false && m_cast.isEmpty() == false)
    {
        castSet.clear();
        foreach (const QString &key, m_cast)
        {
            // HOLD freezes the look, not the strobe threshold: held at 70 %
            // and the fader (or the closing cap) brought under
            // ENGINE_STROBE_ON, the strobes kept walking (runde 171)
            if (eligible.contains(key)
                && (m_groups.value(key).patternDevice == false || fader >= ENGINE_ANI_ON)   // runde 344
                && (m_groups.value(key).strobes == false || fader >= ENGINE_STROBE_ON))
                castSet.insert(key);
        }
        if (base.isEmpty() == false && m_barsAloneNow == false)   // R384_ALONE_HOLD
            castSet.insert(base);
        // ... nor the one-laser line (runde 287): held with both laser types
        // on above 85 % and the fader brought under it, the first type in
        // the room's order stays and the other goes, as it does unheld. The
        // hand on the fader is read through HOLD, like the strobe line.
        if (m_oneLaser)
        {
            bool heldBars = false, heldAni = false;
            // the room's order as unheld: `priority` first (the bars' lead
            // puts them in front), then the rest (runde 288)
            QStringList order = priority;
            foreach (const QString &key, m_groupOrder)
                if (order.contains(key) == false) order << key;
            foreach (const QString &key, order)
            {
                if (castSet.contains(key) == false)
                    continue;
                const TrackGroup &hg = m_groups.value(key);
                const bool isBars = hg.lasers && hg.patternDevice == false;
                if ((isBars && heldAni) || (hg.patternDevice && heldBars))
                {
                    castSet.remove(key);
                    continue;
                }
                heldBars = heldBars || isBars;
                heldAni = heldAni || hg.patternDevice;
            }
        }
    }

    // R401_DROP_LEAD: does this drop show the strobes alone beside the base?
    if (isDrop)
    {
        QSet<QString> fx = castSet;
        fx.remove(base);
        const bool strobesOnly = fx.count() == 1 && m_groups.value(*fx.constBegin()).strobes;
        m_dropStrobesOnly = m_dropStrobesOnly && strobesOnly;
    }
    /* ---- accent: a partner colour on one effect group in drops ---- */
    QString accentColour;
    // runde 304: two tiles or more - the next one is the accent in every
    // section (not only drops, whatever the "Accent colour in drops" tile
    // says): the DJ asked for both colours in the room
    const QString setPartner = setPartnerOf(m_overrideIdx);
    if (setPartner.isEmpty() == false && setPartner != m_colour && isCalm == false && castSet.count() >= 2)
        accentColour = setPartner;
    else if (m_accent && isDrop && isCalm == false && castSet.count() >= 2 && m_override.isEmpty()
        && mixTurned == false)
    {
        // a section turn under HOLD redraws nothing else, so not this either -
        // HOLD is "no colour changes", and the accent is a colour
        // ... and when the ROOM colour changes inside the drop (the hold timer,
        // a musical turn): the accent was drawn to go with the old one, and a
        // cyan drop's magenta accent stood on in the green that followed -
        // green/magenta, a pair the rules leave out on purpose (runde 171)
        if ((sectionChanged && hold == false) || m_accentPick.isEmpty()
            || m_palette.contains(m_accentPick) == false
            || (changeColour && hold == false))
        {
            // White is punctuation, not a colour: it may be the accent about
            // one draw in four and never two sections running. It used to be
            // a partner of nearly every colour and so the drop's accent more
            // often than not - and on the strobes, every time (below), which
            // is the "white pulsing on the strobes almost constantly" of
            // 2026-09-15.
            bool allowWhite = m_accentWasWhite == false && rng->bounded(4) == 0;
            m_accentPick = accentFor(m_colour, allowWhite);
            m_accentWasWhite = (m_accentPick == QStringLiteral("white"));
        }
        accentColour = m_accentPick;
    }

    // The look's partner colour (runde 243): the accent where there is one,
    // the next track's colour through a mix, otherwise drawn from the room's
    // harmony pairs once per section (and when the room colour changes). HOLD
    // keeps it, as it keeps the accent.
    // white sits under blue, magenta and cyan in HARMONY (the "Frost"
    // programmes) but not in mixesWith - the mix never turns a room white
    // (the pairs come in as an argument: the static table is not a capture)
    // RUNDE 331 (headless audit, 20 Sep): ... and only in a DROP - Tobias,
    // 2026-09-22: "Hvid (RGB hvid) generelt skal kun bruges i drops". Runde
    // 246 let white be the partner in every section, so a cyan break ran the
    // Minis in "Break Blend Cyan Frost Slow" (cyan + white) for 120 beats.
    // A white partner drawn in a drop is let go at the next section (the
    // redraw below); HOLD keeps it, as it keeps the rest of the look.
    const bool whiteHere = isDrop || hold;
    const QStringList roomPairs = mixesWith.value(m_colour);
    auto partnerOk = [this, &roomPairs, whiteHere](const QString &c) {
        if (m_palette.contains(c) == false || engineBannedColour(c))
            return false;
        if (roomPairs.contains(c))
            return true;
        return c == QStringLiteral("white") && whiteHere
            && (m_colour == QStringLiteral("blue") || m_colour == QStringLiteral("magenta")
                || m_colour == QStringLiteral("cyan"));
    };
    if (setPartner.isEmpty() == false && setPartner != m_colour)
    {
        m_partnerPick = setPartner;      // runde 304: the tiles' pair is the look's pair
        m_partnerSolo = false;
        // (review 307: the spread wears it, not the accent block - so no accent
        // on the log either)
        accentColour.clear();
    }
    else if (m_override.isEmpty() == false)
    {
        m_partnerPick.clear();           // a colour tile is ONE colour (runde 205)
        m_partnerSolo = false;
    }
    else if (accentColour.isEmpty() == false)
    {
        m_partnerPick = accentColour;
        m_partnerSolo = false;
    }
    else if (m_mixing && m_nextColour.isEmpty() == false && m_nextColour != m_colour)
    {
        m_partnerPick = m_nextColour;
        m_partnerSolo = false;
    }
    else if (((sectionChanged || changeColour || faderJump) && hold == false)   // runde 292: a hand redraws it
             || (m_partnerPick.isEmpty() && m_partnerSolo == false)
             || (m_partnerPick.isEmpty() == false && partnerOk(m_partnerPick) == false))
    {
        QStringList pool;
        foreach (const QString &c, m_palette)
        {
            if (c != m_colour && partnerOk(c))
                pool << c;
        }
        m_partnerPick = pool.isEmpty() ? QString() : pool.at(int(rng->bounded(int(pool.count()))));
        // runde 292 (Tobias: "alt skal skalere efter energi-slideren"): a
        // quiet room is one colour more often - a partner in one look of ten
        // at 30 %, every look from 70 %. Drawn with the partner, held with it.
        m_partnerSolo = pool.isEmpty()           // nothing to pair with: one colour (review 294)
                     || rng->bounded(1000) >= int(1000.0 * (0.10 + 0.90 * qBound(0.0, (fader - 0.30) / 0.40, 1.0)));
        if (m_partnerSolo)
            m_partnerPick.clear();
    }

    bool hard = sectionChanged && isDrop;
    QStringList castSorted = castSet.values();
    castSorted.sort();
    // runde 306: the groups the tiles' colours spread over, in the rig's own
    // order - not the cast's, so a group joining or leaving mid-section does
    // not recolour the others (the base wears the lead)
    QStringList spreadKeys = m_groupOrder;
    spreadKeys.removeAll(base);
    // The accent's group is drawn per section from the effect groups in the
    // cast, and never the same group twice running. It used to be "the last
    // effect group" of an alphabetically sorted list - which was the strobes
    // in every drop with strobes in it.
    QString accentGroup;
    {
        QStringList cands;
        foreach (const QString &key, castSorted)
        {
            if (key != base)
                cands << key;
        }
        if (cands.isEmpty() == false)
        {
            bool redraw = cands.contains(m_accentGroup) == false
                       || (sectionChanged && hold == false && cands.count() > 1);
            if (redraw)
            {
                QStringList fresh = cands;
                fresh.removeAll(m_accentGroup);
                if (fresh.isEmpty())
                    fresh = cands;
                m_accentGroup = fresh.at(int(rng->bounded(fresh.count())));
            }
            accentGroup = m_accentGroup;
        }
    }

    // Runde 306 / review 307: the tiles' colours per group, worked out once a
    // beat. The order: the look's partner first, the other tiles after the
    // lead in turn, the lead last - so the colour next to the base is the one
    // the rules pair with it (setPartnerOf). Each group takes the first of them
    // from its own place (the rig's order) that it can show. With two tiles the
    // odd places wear the lead; when no group in the cast shows anything but the
    // lead, the accent group wears the partner (runde 304's promise).
    QHash<QString, QString> spreadColour;
    if (m_overrideSet.count() >= 2)
    {
        const QString partnerTile = setPartnerOf(m_overrideIdx);
        const int n = int(m_overrideSet.count());
        QStringList order;
        if (partnerTile.isEmpty() == false)
            order << partnerTile;
        for (int t = 1; t < n; t++)
        {
            const QString c = m_overrideSet.at((m_overrideIdx + t) % n);
            if (order.contains(c) == false)
                order << c;
        }
        if (order.contains(m_colour) == false)
            order << m_colour;
        bool otherShown = false;
        for (int j = 0; j < int(spreadKeys.count()); j++)
        {
            const QString &sk = spreadKeys.at(j);
            QString pick;
            for (int t = 0; t < int(order.count()); t++)
            {
                const QString c = order.at((j + t) % int(order.count()));
                if (colourForGroup(sk, c) == c)
                {
                    pick = c;
                    break;
                }
            }
            if (pick.isEmpty())
                pick = m_colour;
            spreadColour.insert(sk, pick);
            if (castSet.contains(sk) && pick != m_colour)
                otherShown = true;
        }
        if (otherShown == false && accentGroup.isEmpty() == false && partnerTile.isEmpty() == false
            && colourForGroup(accentGroup, partnerTile) == partnerTile)
            spreadColour.insert(accentGroup, partnerTile);
    }

    /* ---- the drop's character: one draw that leans every group's dice
     *      the same way, so a drop is one idea and the next drop another.
     *      hard = strobing, sparkling, fast; wide = full, slow trades, big
     *      figures; tight = chases and lines ---- */
    // Drawn ONCE per drop, where it arrives (runde 282), not at every
    // section change inside it. Two things were wrong with
    // `(sectionChanged || m_dropStyle == 0)`:
    // - 0 ("none", the plain drop) is one of the styles in the list below,
    //   and "== 0" also meant "not drawn yet": a drop that drew 0 on its
    //   first beat drew again on the second. Its moves and the heads'
    //   figure were already drawn as a plain drop, and the white hit, the
    //   strobe mode and the w3 lean then followed the second draw - one drop,
    //   two ideas, for eight bars - and the plain drop never lasted a beat.
    // - A second drop flag inside a running drop is a sectionChanged too: a
    //   drop that had settled (hard -> heavy) drew again and could come back
    //   hard or nervous, where runde 266 had already kept the settle clock
    //   (m_dropCalm) through that flag. The settle promise is "et drop starter
    //   ekstremt ... og normaliserer sig", not "until the next flag".
    // NEXT (a new look) still draws, and in a settled drop what it draws
    // steps down like the settle does, so NEXT never undoes the settle.
    if (isDrop && (m_dropStyleDrawn == false || forceNext) && hold == false)
    {
        // hard = strobing and fast; wide = full, slow trades; tight = chases
        // and lines; heavy = slow, broad, a deep pulse, few colours; nervous
        // = many small chases, quick colour trades, no pulse. Two more since
        // 2026-09-16, so two drops in a row read as two different ideas even
        // to someone who does not know what they are looking at.
        QList<int> styles = { 2, 3, 2, 3, 0 };
        if (energy > 0.30)
            styles << 4 << 4;
        if (energy > 0.45)
            styles << 1 << 1;
        if (energy > 0.55)
            styles << 5 << 5;
        if (energy > 0.7)
            styles << 1;
        if (fader > 0.85)                        // runde 290: the top keeps getting harder
            styles << 1 << 5;
        m_dropStyle = styles.at(int(rng->bounded(styles.count())));
        m_dropStyleDrawn = true;
        m_landCoin = rng->bounded(2) == 1;     // runde 287: see `landing`
    }
    else if (isDrop == false)
    {
        m_dropStyle = 0;
        m_dropStyleDrawn = false;
    }
    // R358_REPEAT: a repeat of the first drop takes its idea - on the beat
    // it arrives, where the style above was drawn
    if (m_reuseStyle >= 0 && isDrop && forceNext == false)
    {
        m_dropStyle = m_reuseStyle;
        m_landCoin = m_reuseCoin;
    }
    m_reuseStyle = -1;
    // ... and it steps down once when the drop settles: hard -> heavy,
    // nervous/tight -> wide (analyse 10g). The step is idempotent (4, 2 and
    // 0 map to themselves), so a NEXT in a drop that has settled is read
    // through it too - dropSettled is true from the settle beat on
    // (settleNow sets m_dropCalm above), and there it is the same one step.
    if (dropSettled)
        m_dropStyle = m_dropStyle == 1 ? 4 : ((m_dropStyle == 5 || m_dropStyle == 3) ? 2 : m_dropStyle);
    // R372_LAYER_DROP: AUTO's fade-or-chase for a drop reads the drop's style,
    // which is drawn just above - after the layer was updated for this beat.
    // On the landing it still read 0 (the drop landed as a chase and turned
    // into a fade a beat later). Made again now the style is known.
    if (m_layerStyle != 0 && m_colourMode == 0 && isDrop)
    {
        const int dropWay = (fader < 0.60 || m_dropStyle == 2 || m_dropStyle == 4) ? 1 : 2;
        if (dropWay != m_layerStyle)
        {
            m_layerStyle = dropWay;
            // ... and its pace with it: the chase's step rate left in made the
            // fade run half a colour in one beat
            if (m_layerRate > 0.0)
            {
                qreal sb = dropWay == 1
                    ? 4.0 * (fader < 0.30 ? 16 : fader < 0.60 ? 8 : fader < 0.85 ? 4 : 2)
                    : qreal(fader < 0.30 ? 16 : fader < 0.60 ? 4 : fader < 0.75 ? 2 : 1);
                if (m_speed < 0)             // R375_DROP_SPEED: as updateColourLayer
                    sb *= 2.0;
                else if (m_speed > 0)
                    sb = dropWay == 2 ? qMax(1.0, sb / 2.0) : sb / 2.0;
                m_layerRate = 1.0 / sb;
            }
            if (dropWay == 2)
                m_layerPattern = fader < 0.30 ? 0 : fader < 0.60 ? 1 : 2;
            if (dropWay == 1)
                m_layerTimer.start();
            else if (m_mixGlide == false)    // runde 374
                m_layerTimer.stop();
            emit liveChanged();
        }
    }

    // RUNDE 316 (Tobias, 2026-09-29): over 75 % the strobes' chase follows
    // the KICK - "et hårdt kick/bas er 1 trin pr slag, og et blødere
    // (hurtigere kick) er 1/8". Heard, not drawn: m_dropStyle above is a
    // look the engine picks at random, this is what the analysis measures -
    // the kick on each beat of the drop's first two bars (the impact runs
    // there anyway) and the lows over them. Then held for the drop: a chase
    // that changes pace from beat to beat reads as one that stutters.
    // Blended, not a switch at one line: from a kick+bass of 0.35 (eighths
    // every time) to 0.75 (on the kick every time), a draw in between.
    // Without the curves from BLT it stays on the kick, as before.
    if (isDrop == false)
    {
        m_dropKickSum = 0.0;
        m_dropKickN = 0;
        m_dropKickLast = -1;
        m_dropKickLocked = false;
        m_strobeOnKick = true;
    }
    else if (m_dropKickLocked == false)
    {
        if (dropBar >= 0 && dropBar < 2 && kick >= 0.0 && beat != m_dropKickLast)
        {
            m_dropKickSum += kick;
            m_dropKickN++;
            m_dropKickLast = beat;
        }
        if (dropBar >= 2)
        {
            // Runde 317 (review of 316): the curves are relative to the
            // track - kick divided by its 90th percentile, the lows by their
            // 75th - so a drop reads close to 1.0 in every track and 316
            // stepped on the kick every time. Back to absolute terms with the
            // numbers BLT divided by (setTrackPunch), then HOW HARD compared
            // with the drops heard before: harder than most, on the kick;
            // softer than most, eighths; blended between. Until six drops
            // are known (or without BLT's numbers) it stays on the kick.
            // Runde 322: ranked against earlier DROPS (rememberDropPunch),
            // not against the tracks' references - like against like.
            qreal hard = -1.0;
            if (m_kickRef > 0.0 && m_lowRef > 0.0 && m_dropKickN > 0 && bass >= 0.0)
            {
                const qreal k = m_dropKickSum / qreal(m_dropKickN) * m_kickRef;
                const qreal l = bass * m_lowRef;
                if (m_punchKick.count() >= 6)
                    hard = 0.5 * engineRankIn(m_punchKick, k) + 0.5 * engineRankIn(m_punchLow, l);
                rememberDropPunch(k, l);
            }
            const qreal onKick = hard < 0.0 ? 1.0 : qBound(0.0, (hard - 0.25) / 0.50, 1.0);
            m_strobeOnKick = rng->bounded(1000) < int(onKick * 1000.0);
            m_dropKickLocked = true;
            // ... and the strobes pick again now, at the pace just decided:
            // what they hold was picked in the impact bars, before the kick
            // had been heard (runde 317, review). Only where it changes
            // anything (runde 318, review): over 75 % and a soft kick - the
            // pick made before the lock was made on the kick already, and a
            // re-pick at bar 2 of EVERY drop swapped the strobes' chase for
            // nothing below 75 %.
            if (m_strobeOnKick == false && m_faderNow >= 0.75)
            {
                foreach (const QString &sk, m_groupOrder)
                {
                    if (m_groups.value(sk).strobes)
                        m_sectionMotion.remove(sk);
                }
            }
            logSignal(QStringLiteral("sig:strobe-pace:") + (m_strobeOnKick ? QStringLiteral("kick")
                                                                           : QStringLiteral("eighths"))
                      + QStringLiteral(":") + QString::number(hard, 'f', 2));
        }
    }

    // Stable roles for this room picture: base / rhythmic lead / support.
    // Keep the lead under HOLD and through a mix; replace it only when it
    // leaves the cast or the music starts a new section.
    // ... on a bar line: a fader wobbling round the 30 % drop line flipped
    // drop/groove on any beat, and each flip redrew figure, zoom and walk mid-
    // bar. The change waits for the bar line (or a new section) and is
    // remembered until then (runde 199)
    // Only the TIER waits: the base is the operator's pick (baseGroup()) and
    // does not wobble with a fader, and slotScale() reads m_compositionBase
    // while motionOwns reads `base` - a lagging base let the old base's chase
    // own the dimmers without MASTER or trim for up to three beats (runde 201).
    const bool baseMoved = m_compositionBase != base;
    const bool tierMoved = m_compositionTier != tier && (beatInBar == 0 || sectionChanged);
    const bool roleContextChanged = baseMoved || tierMoved;
    if (baseMoved || sectionChanged)
        m_compositionBase = base;
    if (tierMoved || sectionChanged)
        m_compositionTier = tier;
    bool compositionChanged = false;
    if (m_fullAuto && ((((sectionChanged && partChanged) || roleContextChanged) && hold == false)
        || (m_rhythmLead.isEmpty() == false && castSet.contains(m_rhythmLead) == false)
        || (m_rhythmLead.isEmpty() && castSet.count() > (castSet.contains(base) ? 1 : 0))))
    {
        QStringList leads;
        foreach (const QString &key, priority)
            if (castSet.contains(key) && m_groups.value(key).strobes == false) leads << key;
        if (leads.isEmpty())
        {
            foreach (const QString &key, castSorted)
            {
                // "A strobe group is never the rhythm lead" - this fallback
                // made it one whenever the cast was base + strobes (runde 172)
                if (key != base && m_groups.value(key).strobes == false) leads << key;
            }
        }
        // RUNDE 340 (Tobias, 10-03: "ensformigt"): the lead was the first in
        // the room's order every time - and with the bars' lead put first
        // (m_barsLead) the laser bars carried the rhythm in 56 % of the drop
        // beats of 10-02, everything else calm around them. The first in line
        // still leads more often than not; otherwise another group on stage
        // takes it for this part of the track. Drawn only here, when the
        // roles are re-composed - never mid-section.
        QString lead = leads.isEmpty() ? QString() : leads.first();
        if (leads.count() >= 2 && rng->bounded(100) >= 55)
            lead = leads.at(1 + int(rng->bounded(int(leads.count()) - 1)));
        compositionChanged = lead != m_rhythmLead || roleContextChanged;
        if (compositionChanged)
            m_sectionMotion.clear(); // programmes must obey the new roles too
        m_rhythmLead = lead;
    }

    /* ---- moves: every group in the cast draws how it moves this section.
     *      Long sections redraw every 16 bars, half the time. A pattern the
     *      group ran in its last two sections is not drawn again if the
     *      dice can help it. ---- */
    // A hand on the ENERGY fader is a decision, and it used to wait for the
    // next section line - up to 32 bars - before anything but the strobes
    // answered it (driveStrobe reads the fader every beat; the moves, the
    // figure, the zoom and the star ceiling were all drawn once per section).
    // A fifth of the fader or more since the last draw redraws all of them
    // on the next bar line, and lets go of the held programme too: at 100 %
    // the room should not be running the one-star walk it drew at 40 %.
    // The sweep's SIZE and PACE follow the fader every beat regardless
    // (applySweep); this is for the rest.
    // A BUILD IS DRAWN ONCE (runde 275). Everything a build shapes from
    // prog - the bare blink's steps, the roll, the pulse depth, the zoom, the
    // figure's tightening - is shaped live in the pass below, on every beat.
    // The 8-bar roll added nothing to that but a second throw of the dice:
    // drawMove re-rolled `bare` (0.35 + 0.55 prog), ownChaser and pulseOn,
    // and the history rule (no pattern from the last two draws) pushed the
    // re-roll AWAY from what the group ran - so a bare blink at one beat and
    // two subs became a FILL at two beats half way up a 32-bar build, four
    // times slower right where it should speed up, a climb chase stopped for
    // the engine's figure or the reverse, and the heads' figure restarted.
    // The dice is still thrown (the RNG's order is unchanged); a build that
    // has drawn its moves (m_buildDrawn) just does not act on it. A fader
    // jump, a new composition and a group joining still draw. A build
    // promoted by the riser mid-section has drawn nothing yet, so its first
    // roll still brings the build's moves in.
    bool redraw = hold == false
               && (sectionChanged || compositionChanged || m_moves.isEmpty() || faderJump || settleNow
                   || (bar > 0 && bar % 8 == 0 && beatInBar == 0 && rng->bounded(3) > 0
                       && (isBuild && m_buildDrawn) == false));
    // ... and an inner flag of the same build, or the kick wait at the drop
    // flag (isBuild, "the build's look holds"), keeps the moves of the groups
    // already on stage: the same second throw, at a section line. A group
    // that joins there (the extra group past the middle) draws its own. (In
    // FULL AUTO the drop flag is a part change and re-composes the roles, so
    // the wait draws there as before; an inner build flag does not.)
    const bool buildKeeps = isBuild && m_buildDrawn && faderJump == false && compositionChanged == false;
    // THE FLOOR ROUND (runde 214, Tobias: "det ser sejt ud, naar alle hoveder
    // peger lige ned i gulvet og skiftes til at blinke rundt i rummet zoomet
    // helt"). A build's look for the heads: every head straight down (the
    // "Center" aim, tilt 128), the sharpest beam, no figure, and ONE head lit
    // at a time, handed round faster and faster as the build climbs - the
    // bare blink's acceleration below. The drop lands wide on all of them.
    // Drawn once per build, more often the higher the room; never under
    // HOLD, CALM or a still room.
    // (runde 217: and it ends with FULL AUTO, below 30 % ENERGY - the rule it
    // was drawn by - and is not rolled again on a fake drop's waiting beat,
    // where the build's look holds)
    // RUNDE 339 (Tobias, 10-03: "jeg synes bare det er ensformigt"): every
    // build was the same build - the base filling up, the rest a bare blink
    // handed round faster, the heads straight down in a third to more than half
    // of them. Measured on 10-02: the base ran the engine's fill 84 % of the
    // build beats, the strobes 100 %. Each build now draws its SHAPE: the
    // fill, an odd/even roll that speeds up, a swell (the whole base breathing
    // deeper and faster), or the halves trading. Drawn on the build's first
    // beat, held to its end.
    if (isBuild && m_buildDrawn == false && sectionChanged)
        m_buildStyle = int(rng->bounded(4));
    if (isBuild == false || isCalm || still || m_fullAuto == false || (m_floorRound && m_dropShown == false))
        m_floorRound = false;
    // (not re-rolled at an inner flag of the same build - it was drawn at
    // the build's start; 13 of 16 early endings on 25-26 Sep, runde 233)
    // (and not in a build of under four bars, analyse 10c: the heads swung to
    // Center, had a bar of the hand-round and swung again at the landing)
    // (runde 264: the 30 % is the FADER's, like the line that ends it
    // above - on the scaled energy a slider on 33 in a quiet build read
    // 0.27 and never rolled, and the round could only start above ~37)
    else if (sectionChanged && hold == false && dropWaiting == false && m_floorRound == false
             && (beatsToNext <= 0 || beatsToNext >= 16))
        m_floorRound = m_fullAuto               // FULL AUTO only: it takes the heads' programmes and aims
                    // (runde 339: 25-50 %, was 30-60 % - the heads down in
                    // 44 % of the build beats of 10-02 made the builds alike)
                    && fader >= 0.30 && rng->bounded(100) < int(25.0 + 25.0 * qBound(0.0, energy, 1.0));
    if (redraw)
    {
        m_movesEnergy = energy;
        m_movesFader = fader;
    }
    if (faderJump)
        m_sectionMotion.clear();
    foreach (const QString &key, castSorted)
    {
        // ... only if it was on stage last beat too (m_cast is still last
        // beat's here). A group rejoining mid-section - a nudge, the end of a
        // rest, the pre-drop fill - ran whatever it drew the last time it was
        // on: the strobes back in a groove with a drop's move (runde 172).
        if ((redraw == false || buildKeeps) && m_moves.contains(key) && m_cast.contains(key))
            continue;
        // what the group ran on until now (runde 273) - read before the
        // samples below overwrite m_moves
        const bool wasOwn = m_moves.contains(key) && m_moves.value(key).ownChaser;
        QList<int> history = m_moveHistory.value(key);
        TrackMove fresh;
        int total = 0;
        int samples = m_fullAuto && m_ratingOn ? 6 : 1;
        for (int sample = 0; sample < samples; sample++)
        {
            TrackMove candidate = drawMove(key, tier, isBuild, energy, key == base, prog);
            for (int attempt = 0; attempt < 4 && candidate.pattern != ENGINE_PAT_STATIC && history.contains(candidate.pattern); attempt++)
                candidate = drawMove(key, tier, isBuild, energy, key == base, prog);
            if (m_fullAuto)
                candidate = composeMove(key, candidate, tier);
            m_moves.insert(key, candidate);
            int weight = samples == 1 ? 2 : autoLookWeight(autoLookKeys(castSet, energy), key);
            total += weight;
            if (sample == 0 || rng->bounded(total) < weight)
                fresh = candidate;
        }
        // what it was drawn at (runde 264): a group joining mid-section draws
        // at this beat's energy, not at m_movesEnergy's
        fresh.drawnE = energy;
        // THE 50/50 BOTH WAYS AT THE REDRAW INSIDE A SECTION (runde 273). The
        // bar-8 redraw (and the drop's settle) draws ownChaser again, but the
        // held programme (m_sectionMotion) only let go at a section line or a
        // fader jump. Generated -> programme took effect (nothing was held, so
        // the next pick found a chase); programme -> generated did not (the
        // held chase ran on and hid the new figure). A 32-bar drop ran the
        // show's chases ~80 % of the time on the effect groups and ~78 % on
        // the base, where the draw is 65 / 60 % (Tobias' 50/50, runde 156).
        // Not in a build (a climb is made for the whole build, runde 227) or a
        // break (moving does not follow ownChaser there), and not for the
        // animation lasers: a new pattern inside the section is what Tobias
        // said no to (BACKLOG, 2026-09-23, point 7).
        if (redraw && sectionChanged == false && wasOwn && fresh.ownChaser == false
            && isBuild == false && isBreak == false && m_groups.value(key).patternDevice == false)
            m_sectionMotion.remove(key);
        m_moves.insert(key, fresh);
        if (fresh.pattern != ENGINE_PAT_STATIC)
        {
            history.append(fresh.pattern);
            while (history.count() > 2)
                history.removeFirst();
            m_moveHistory.insert(key, history);
        }
    }
    // runde 275: the build's moves are drawn once they have been drawn IN it;
    // any beat that is not a build lets go (a hidden drop, a fader under 30 %)
    m_buildDrawn = isBuild && (m_buildDrawn || redraw);

    /* ---- texture: the lit fixtures of a group sit at slightly different
     *      levels and drift a little every bar - a flat group looks like a
     *      photo, this looks like light ---- */
    foreach (const QString &key, castSorted)
    {
        int n = m_groups.value(key).parts.count();
        if (n < 2)
            continue;
        QVector<qreal> tex = m_texture.value(key);
        if (tex.count() != n)
        {
            tex.resize(n);
            for (int i = 0; i < n; i++)
                tex[i] = rng->generateDouble();
        }
        else if (beatInBar == 0)
        {
            for (int i = 0; i < n; i++)
                tex[i] = qBound(0.0, tex.at(i) + (rng->generateDouble() - 0.5) * 0.25, 1.0);
        }
        m_texture.insert(key, tex);
    }

    /* ---- positions: sticky, tiered, only changed in the dark for lasers ---- */
    QSet<QString> darkGroups;
    // A planned dark stretch that is still running. The bars are blanked for
    // the beat they are told to move on, but the motor takes far longer than
    // a beat to walk a 20 m beam home - so the room saw them go out at the
    // top of a break and come back at some arbitrary point in the travel.
    // Four bars, decided here, so it is the same length every time (Tobias,
    // 2026-09-15: "hvis de altsaa slukker i det break, skiftes til 4 bars").
    // The upper guard bounds a backwards scrub: without it a jump to an
    // earlier beat would hold the group dark until the tape caught up.
    foreach (const QString &key, m_darkUntil.keys())
    {
        int until = m_darkUntil.value(key);
        if (beat <= until && until - beat < ENGINE_DARK_BARS * 4)
            darkGroups.insert(key);
        else if (beat > until || until - beat >= ENGINE_DARK_BARS * 4)   // runde 201: or a long jump back
            m_darkUntil.remove(key);
    }
    // the build's beats to go, BEFORE the aims are drawn (runde 258, review):
    // it was only set in the motion loop further down, so the position block
    // read last beat's value - 0 on a build's first beat, and beats-to-drop
    // plus one on the bar lines, never a whole 16 - and a head climb could
    // never pass candidates()'s climb rule. The motion loop sets it again,
    // to the same value.
    m_buildLen = (isBuild && dropWaiting == false && nextState == QStringLiteral("drop"))
               ? qMax(1, beatsToNext) : 0;
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (candidates(ENGINE_ROLE_POSITION, key).isEmpty())
            continue;
        if (m_groupOff.contains(key))
        {
            // switched off by hand: not even an aim (a position scene may
            // carry a shutter or a dimmer of its own)
            if (m_active.contains("pos:" + key))
                stopSlot("pos:" + key, true);
            m_position.remove(key);
            continue;
        }
        // R378_POSITIONS: a position chosen on the page holds the heads until AUTO
        if (positionHeld(key))
        {
            const quint32 holdAim = headHoldFunction(key);
            if (holdAim != Function::invalidId())
            {
                run("pos:" + key, holdAim, 1.0, 0, true);
                m_position.insert(key, holdAim);
                continue;
            }
        }

        bool inCast = castSet.contains(key);
        // lasers move only in a dark beat at a break; heads move at break or
        // drop starts, and half the time at other section changes - every
        // section would be restless
        bool mayMove = hold == false
                    && ((inCast == false)
                        || (sectionChanged && (g.lasers ? (isBreak && g.hasDimmer)
                                                        : (isBreak || isDrop || rng->bounded(2) == 0))));
        // The bars only leave the UP aim above three fifths of the fader, and
        // never in a break or a build. Everywhere else they sit exactly where
        // the operator put them, which is what "the primary one, and the one
        // we want in breaks and build-ups and at the start" means.
        // HOLD freezes the aim like everything else; without that guard the
        // fader dropping under 60 % would swing the bars home under a hold.
        // ... and at ENERGY 0 even under the HOLD it forces (runde 171): a
        // bar that was roaming stood running its figure at the bottom of the
        // fader - "ENERGY 0 = nothing moves". It goes home once and stands.
        // LASER SAFETY under HOLD (runde 190): HOLD skipped this whole block,
        // the every-beat check below included - an aim dipping under home,
        // taken at 80 %, stayed pointing down with the fader pulled to 45 %,
        // and a moving tilt chase kept moving under 40 %. HOLD freezes a SAFE
        // aim; one the fader no longer allows comes through here and goes home.
        bool heldUnsafe = false;
        if (g.lasers && hold && still == false)
        {
            const quint32 heldAim = m_position.value(key, Function::invalidId());
            if (heldAim != Function::invalidId() && heldAim != homePosition(key))
            {
                const TrackFuncInfo &hi = m_funcs.value(heldAim);
                const int downNow = (isBreak || isBuild) ? 0 : laserDownAllowed(fader);
                heldUnsafe = (fader < 0.40 && hi.type != int(Function::SceneType))
                          || (hi.sweep ? laserSweepSafe(heldAim, key, downNow) == false
                                       : laserAimSafe(heldAim, key, downNow) == false);
            }
        }
        // (runde 288: or out of the cast under HOLD - the one-laser line takes
        // a type out there now, and dark bars go home, below)
        if (g.lasers && (hold == false || still || heldUnsafe || inCast == false))
        {
            // ... and only while they are LIT. A bar outside the cast stood
            // at the top of the fader running its slow tilt chase in the dark
            // (Tobias, 2026-09-15: "laser bars staar stadig og bevaeger sig,
            // selvom de ikke lyser"): the pos: slot runs whether the group is
            // in the cast or not. Dark bars go home and stand still.
            // 40 %, not 60: from 40 % the bars may take a figure that lifts
            // them, from 60 % one that dips them (positionFunction / laserAimSafe)
            bool mayRoam = inCast && fader >= 0.40 && isBreak == false && isBuild == false
                        && isCalm == false && still == false;
            // LASER SAFETY (runde 171). The held aim was measured against the
            // fader only on the beat it was chosen: picked at 80 % - a figure
            // dipping 12 units under the home aim - it stayed there when the
            // fader came down to 45 %, where the bars may only point UP. Every
            // beat, a held aim that the fader no longer allows sends the bars
            // home, exactly as the fader dropping under 40 % does.
            if (mayRoam)
            {
                const quint32 heldAim = m_position.value(key, Function::invalidId());
                const int downNow = laserDownAllowed(fader);
                if (heldAim != Function::invalidId() && heldAim != homePosition(key)
                    && (m_funcs.value(heldAim).sweep ? laserSweepSafe(heldAim, key, downNow) == false
                                                     : laserAimSafe(heldAim, key, downNow) == false))
                    mayRoam = false;
            }
            quint32 home = mayRoam ? Function::invalidId() : homePosition(key);
            if (home != Function::invalidId())
            {
                if (m_position.value(key, Function::invalidId()) != home)
                {
                    // A beam that swings while it is lit is the one thing
                    // these fixtures must never do: the group goes dark for
                    // the beat it moves on, exactly as the generic path does.
                    // In a break it stays dark for four bars - long enough
                    // for the motor to finish, and the same length every
                    // time, so the room reads it as a deliberate pause and
                    // not as a fault.
                    // dark whether in the cast or not: the echo lights bars
                    // "in or out of the cast", and picked these mid-swing
                    // (runde 172). Out of the cast they are not lit anyway.
                    darkGroups.insert(key);
                    // R401_LASER_MOVE_DARK (harness, stress: a new track starting
                    // in a break - the bars sent home from a moving aim, and
                    // the programme in mot: still lit at its own level on the
                    // frame the mirrors moved, 11 units). darkGroups reaches
                    // the levels later in tick() and through an attribute; the
                    // light goes NOW, hard, as a lift and a cast exit do (runde
                    // 303/305). Not under the operator's FLASH.
                    if (m_flashHeld.contains(key) == false)
                    {
                        stopSlot("col:" + key, true);
                        stopSlot("mot:" + key, true);
                        for (int i = 0; i < g.parts.count(); i++)
                            stopSlot(partSlot(key, i), true);
                    }
                    {
                        // (fejljagt 3) the four bars from an UNKNOWN aim hold
                        // in the cast or not: out of the cast the bars got one
                        // dark beat, and the echo - which lights bars in or out
                        // of the cast - relit them while the motor still ran
                        // ... but never the base for four bars: that is the
                        // light the room stands on, and "the room never goes
                        // black" is the one promise the engine keeps
                        // everywhere else. The base blinks for its one beat
                        // and comes back.
                        // Four bars too when the bars come from an aim nobody
                        // knows (SHOW ON, a stop, the start scene): tilt is
                        // wherever it was last left - the floor, if DOWN was
                        // busked - and one beat relit the beams mid-swing
                        // (runde 190)
                        if (((inCast && isBreak) || m_position.contains(key) == false) && key != base)
                            m_darkUntil.insert(key, beat + ENGINE_DARK_BARS * 4 - 1);
                    }
                    m_position.insert(key, home);
                }
                run("pos:" + key, home, 1.0, 0, true);
                continue;
            }
        }

        quint32 want = m_position.value(key, Function::invalidId());
        // a laser group out of the cast never draws a fresh aim either: it
        // has been sent home above, and this is only reached with no home
        // AN AIM LIVES A BAR (runde 158). Measured on the night of
        // 2026-09-20: the wash took 963 fresh aims in 73 minutes - one every
        // 4.6 seconds - and NINETY-ONE of them came less than a bar after the
        // one before, seventy-four of those on a section change. Seven heads
        // swinging twice inside one bar is the busiest thing in the room, and
        // "the heads stay slow whatever the fader says" is written three
        // times in this file. The walk below already only fires on a bar
        // line; this branch did not, and it is the one that offends.
        //
        // The first aim of a group is never held back (m_aimSince is absent,
        // and `want` is invalid anyway).
        const bool aimSettled = m_aimSince.contains(key) == false
                             || beat - m_aimSince.value(key) >= 4;
        // the floor round aims every head straight down: the generated
        // "Center" position (runde 214). Without one (banned in SETUP) the
        // heads aim as they always do.
        quint32 centre = Function::invalidId();
        if (m_floorRound && g.heads && g.patternDevice == false && inCast)
        {
            foreach (TrackFuncInfo *pi, candidates(ENGINE_ROLE_POSITION, key))
            {
                if (pi->generated && pi->name.endsWith(QStringLiteral(" Center")))
                {
                    centre = pi->id;
                    break;
                }
            }
        }
        if (centre != Function::invalidId())
        {
            if (centre != want)
            {
                want = centre;
                m_position.insert(key, want);
                m_aimSince.insert(key, beat);
                m_headMoveBeats.insert(key, -8);
            }
        }
        else if (want == Function::invalidId()
            || (mayMove && sectionChanged && aimSettled && (g.lasers == false || inCast)))
        {
            // The laser block above sends the bars home everywhere they may
            // not roam - except under HOLD, which skips it. A bar with no aim
            // yet (a start, a stop, a FULL AUTO toggle) under HOLD drew one
            // here with nothing but the fader to go by - a tilt chase at the
            // bottom of the fader, say (runde 171). Home instead.
            quint32 np = (g.lasers && (hold || fader < 0.40 || inCast == false))
                       ? homePosition(key)
                       : positionFunction(key, m_motionCursor, tier, fader);
            // a laser group with nothing safe to roam to parks at home rather
            // than keeping whatever tilt the channel happens to hold
            if (np == Function::invalidId() && g.lasers)
                np = homePosition(key);
            if (np != want && (mayMove || want == Function::invalidId()))
            {
                if (g.lasers && (inCast || want == Function::invalidId()))
                {
                    darkGroups.insert(key);
                    // R401_LASER_MOVE_DARK: the light goes now, before the new aim
                    if (m_flashHeld.contains(key) == false)
                    {
                        stopSlot("col:" + key, true);
                        stopSlot("mot:" + key, true);
                        for (int i = 0; i < g.parts.count(); i++)
                            stopSlot(partSlot(key, i), true);
                    }
                    // four bars from an unknown aim as well (runde 190, above) -
                    // in the cast or not, as above (fejljagt 3)
                    if (((inCast && isBreak) || want == Function::invalidId()) && key != base)     // never the base, see above
                        m_darkUntil.insert(key, beat + ENGINE_DARK_BARS * 4 - 1);
                }
                want = np;
                m_position.insert(key, want);
                m_aimSince.insert(key, beat);
                m_headMoveBeats.insert(key, -8);     // our own move: grace before the Light Rider check
            }
        }
        // heads without a sweep of the user's (or in FULL AUTO): walk through
        // the positions every four bars in the groove, every two in a drop -
        // the pan/tilt speed channel turns each step into a slow sweep
        // How often the heads take a new aim: the fader decides. At the
        // bottom a groove keeps an aim for eight bars and a drop for four; at
        // the top every two bars and every bar. It was a flat 4 / 2 whatever
        // the fader said, so half of what "more energy" should look like on
        // the heads - they travel more - never happened. The SPEED tiles
        // still halve or double it.
        qreal eWalk = qBound(0.0, energy, 1.0);
        int walkBase = isDrop ? qMax(1, int(qRound(4.0 - 3.0 * eWalk)))
                     : isDrive ? qMax(1, int(qRound(6.0 - 4.5 * eWalk)))
                              : qMax(2, int(qRound(8.0 - 6.0 * eWalk)));
        int walkBars = qMax(1, walkBase * (m_speed < 0 ? 2 : 1) / (m_speed > 0 ? 2 : 1));
        // A position FIGURE (a chaser of aims, runde 254) finishes its loop
        // before the walk takes the next aim: at a high fader a drop walks
        // every bar or two, which cut an 8-16 beat figure off half way.
        // A head FIGURE does not run at ENERGY 0 or under CALM (runde 255,
        // review): the EFX sweep stops or slows there, a chaser keeps its own
        // pace. A plain aim instead (positionFunction at fader 0 hands out
        // scenes only).
        if (g.heads && (still || isCalm) && want != Function::invalidId()
            && m_funcs.value(want).type == int(Function::ChaserType))
        {
            quint32 np = positionFunction(key, m_motionCursor, 0, 0.0);
            if (np != Function::invalidId() && np != want)
            {
                want = np;
                m_position.insert(key, want);
                m_aimSince.insert(key, beat);
                m_headMoveBeats.insert(key, -8);
            }
        }
        // (a CHASER of aims: info.sweep is only ever an EFX - review, runde
        // 255 - and a PingPong loop is there and back, 2 * (n - 1) steps)
        bool figureRunning = false;
        if (want != Function::invalidId() && m_funcs.value(want).type == int(Function::ChaserType)
            && m_funcs.value(want).beats > 0.0)
        {
            Chaser *pc = qobject_cast<Chaser *>(m_doc->function(want));
            const int n = pc != nullptr ? int(pc->steps().count()) : 0;
            const int loopSteps = (pc != nullptr && pc->runOrder() == Function::PingPong) ? qMax(1, 2 * (n - 1)) : n;
            // (runde 293: at the step the slider gives it - figureBeats)
            const int fb = (g.heads && g.lasers == false) ? figureBeats(want) : 0;
            const qreal loopBeats = qreal(loopSteps) * (fb > 0 ? qreal(fb) : m_funcs.value(want).beats);
            figureRunning = m_aimSince.contains(key) && qreal(beat - m_aimSince.value(key)) < loopBeats;
        }
        if (g.heads && inCast && hold == false && isBreak == false && isCalm == false   // CALM: no walk (r199)
            && figureRunning == false
            && m_floorRound == false                                                    // the floor round holds Center (r214)
            && (m_fullAuto || (m_moves.value(key).ownChaser
                               && candidates(ENGINE_ROLE_MOTION, key).isEmpty()))
            && beatInBar == 0 && bar > 0 && (bar % walkBars) == 0
            // the aim floor (runde 158) here too: NEXT makes a section change
            // on any beat, and its aim was followed by this walk on the next
            // bar line - two swings one to three beats apart (runde 171)
            && (m_aimSince.contains(key) == false || beat - m_aimSince.value(key) >= 4))
        {
            // one step per walk, through the tier's own pool
            quint32 np = positionFunction(key, m_motionCursor + bar / walkBars, tier, fader);
            if (np != Function::invalidId() && np != want)
            {
                want = np;
                m_position.insert(key, want);
                m_aimSince.insert(key, beat);
                m_headMoveBeats.insert(key, -8);
            }
        }
        // Runde 293 (Tobias: "alt skal skalere efter energi-slideren ...
        // bevægelser"): a head FIGURE runs at the slider's pace - half as slow
        // again at 30 %, a quarter quicker at the top, never under two beats a
        // step, its fade the step. It ran at its own step at every fader (the
        // EFX sweep stops while a figure runs), so 35 % and 100 % moved alike.
        // Not the laser bars: their aims are the safety rules' (runde 190).
        if (want != Function::invalidId())
        {
            const int fb = (g.heads && g.lasers == false) ? figureBeats(want) : 0;
            run("pos:" + key, want, 1.0, fb > 0 ? fb * 1000 : 0, true);
            // R405_AIM_DRAW: the heads' aims counted, for the rest in drawAim()
            if (g.heads && g.lasers == false && m_aimLast.value(key, Function::invalidId()) != want)
            {
                const int seq = m_aimSeq.value(key, 0) + 1;
                m_aimSeq.insert(key, seq);
                m_aimTakenAt[key].insert(want, seq);
                m_aimLast.insert(key, want);
            }
        }
    }

    // the bar figure below must not stop for the blink: it is a dark beat,
    // not a move, and the figure restarted from nought at every drop (runde
    // 220 - it undid the "never redrawn while running" of runde 219)
    const QSet<QString> moveDark = darkGroups;
    /* ---- the blink: one dark beat right before a drop, then everything
     *      lands on the one. The base stays - the room never goes black. ---- */
    if (preDrop && beatsToNext == 1 && energy > 0.5 && isCalm == false && hold == false)
    {
        foreach (const QString &key, castSet)
        {
            if (key != base)
                darkGroups.insert(key);
        }
    }

    /* ---- runde 356: the music's own dark (R356_MUSIC_DARK) ----
     * Tobias, 2026-10-04: "i breaks eller lige foer drops, hvor der er ingen
     * eller naesten ingen lyd (ingen waveform), blackout lyset?" - and then
     * "Byg det hele". Measured on the nights of 10-02/03/04: 27 of 226 drops
     * had 1-8 silent beats right before them (Frank Ocean "Lost": seven of
     * pure silence) and the rig stood lit through every one; he blacked out
     * four. A silent beat - BLT's waveform under a fifth of the track's own
     * top, and no kick and no highs where BLT sends those - is black: the
     * whole rig, the base too. The rule "the room never goes dark" was about
     * a room with music in it. Two bars of silence and more: the base comes
     * back slowly to 30 % and waits there. Never at the ends of a track (the
     * mix), never while two decks play, never at ENERGY 0. And the last beat
     * before a drop from 60 % on the slider: the whole room black, so the
     * drop lands out of the dark (below 60 % the old blink, base lit). */
    const bool musicDarkOk = still == false && m_mixing == false && m_flash == false
                          && m_audLevel.isEmpty() == false
                          && beat > 16 && beat <= m_audLevel.count() - 16;
    const bool silenceNow = musicDarkOk && silentish(beat);
    const int intoSilence = silenceNow ? beat - silentRunStart(beat) : -1;
    const bool silenceLong = silenceNow && intoSilence >= 8;
    if (silenceLong)
    {
        foreach (const QString &key, castSet)
        {
            if (key != base)
                darkGroups.insert(key);
        }
    }
    const bool dropBlack = preDrop && beatsToNext == 1 && fader >= 0.62 && isCalm == false   // R374_SPREAD
                        && m_nextDropSize >= 0.40      // R362_BUILD_SIZE: a small drop lands from the blink
                        && hold == false && still == false && m_mixing == false && m_flash == false
                        && djLoopOn() == false;  // runde 358: a looped last bar stabs at its loop point instead
    // Runde 357 (Tobias 10-05: "paa frank ocean naar lyset gaar ud, er der
    // bitte smaa udsving lyde, at lyser reagere lidt paa dem, mens den er
    // black-outed"): inside the silence the base answers the small sounds -
    // its level follows the beat's waveform (0..the silence line), 4 % to
    // 24 %; a beat with nothing on it stays black. The rest of the rig stays
    // dark. The long silence breathes the same way under its 30 %.
    const qreal glowNorm = silenceNow ? qBound(0.0, audLevel(beat) / 0.20, 1.0) : 0.0;
    const bool silenceGlow = silenceNow && silenceLong == false && dropBlack == false && glowNorm >= 0.15;
    if (silenceGlow)
    {
        foreach (const QString &key, castSet)
        {
            if (key != base)
                darkGroups.insert(key);
        }
    }
    m_silenceDark = (silenceNow && silenceLong == false && silenceGlow == false) || dropBlack;
    m_musicDarkEvent = silenceLong ? QStringLiteral("silence-dim")
                     : silenceGlow ? QStringLiteral("silence-glow")
                     : silenceNow ? QStringLiteral("silence-dark")
                     : dropBlack ? QStringLiteral("drop-black") : QString();

    /* ---- runde 358: what the music does inside and around this beat ---- */
    // R358_HALFTIME (Tobias 10-05, "halvtakt-drops koerer i halv fart"): the
    // kick over the coming two bars, read off BLT's raw kick (the whole track
    // is known). Two to four kicks in eight beats, never two on neighbouring
    // beats, is half time - in 11 of 86 library tracks it is the
    // whole drop (Kelis, Smack That, Jamaican, La La La), in the rest it is
    // under four bars in ten. Read on the bar line; the chases walk at half
    // pace (divisionFor) and the pulse already waits for the kick.
    if (tier >= 1 && isBuild == false && m_audKick.isEmpty() == false && m_dropShown)
    {
        if (beatInBar == 0 || sectionChanged)
        {
            // (two kicks on neighbouring beats is not half time - and the
            // bar's own parity is not asked: a grid a beat off still halves)
            int kicks = 0, seen = 0;
            bool neighbours = false, last = false;
            for (int i = 0; i < 8; i++)
            {
                const int b = beat + i;
                if (b < 1 || b > m_audKick.count())
                    continue;
                seen++;
                const bool k = m_audKick.at(b - 1) >= 115;
                if (k)
                    kicks++;
                if (k && last)
                    neighbours = true;
                last = k;
            }
            m_halfTime = seen == 8 && kicks >= 2 && kicks <= 4 && neighbours == false;
        }
    }
    else
        m_halfTime = false;
    // R358_VOCAL (Tobias 10-05): the mid band carrying the music - mids from
    // 55 % of the track's own top, no kick, the bass under 35 % and the highs
    // under half. Measured in the library: 6 % of the kickless beats, a
    // vocal or a melody alone. Two of the last three beats (a voice sings
    // in phrases: in the library the beats come every other one as often as
    // in a row).
    if (beat != m_vocalBeat)
    {
        m_vocalBeat = beat;
        m_vocalRun = 0;
        for (int b = beat - 2; b <= beat; b++)
        {
            const qreal md = audMid(b);
            if (md >= 0.55 && b >= 1 && m_audKick.count() >= b
                && m_audKick.at(b - 1) < 51 && m_audHigh.at(b - 1) < 128
                && (bass < 0.0 || bass <= 0.35) && audLevel(b) >= 0.15)
                m_vocalRun++;
        }
    }
    m_vocalNow = m_vocalRun >= 2 && isDrop == false && still == false && m_mixing == false && isCalm == false;
    // R358_GROOVE (Tobias 10-05, "rytmen inde i slaget"): where the drums hit
    // BETWEEN the beats, over the last eight - BLT's highs per quarter beat.
    // A quarter that hits a quarter harder than the beat itself (an off-beat
    // hat in house: q2; the 3-3-2 of a dembow: q3/q2) is one the effects
    // pulse on too (slotPulseTimer). And claps/snare on 2 and 4 - the highs
    // on beats 2 and 4 at least 1.4 times beats 1 and 3, over 16 beats, in a
    // fifth of the library - is what the strobes pulse on instead of the kick.
    m_grooveSlots = 0;
    m_backbeat = false;
    m_grooveSeen.clear();
    if (m_onsetHigh.isEmpty() == false && isBuild == false && isBreak == false && fader >= 0.45
        && isCalm == false && still == false && m_mixing == false && preDrop == false)
    {
        qreal sq[4] = { 0.0, 0.0, 0.0, 0.0 };
        int n = 0;
        for (int b = beat - 7; b <= beat; b++)
        {
            if (onsetHigh(b, 0) < 0)
                continue;
            for (int q = 0; q < 4; q++)
                sq[q] += onsetHigh(b, q);
            n++;
        }
        if (n >= 6)
        {
            for (int q = 1; q < 4; q++)
            {
                if (sq[q] / n >= 4.0 && sq[q] >= 1.25 * sq[0])
                    m_grooveSlots |= 1 << q;
            }
        }
        qreal on24 = 0.0, on13 = 0.0;
        int n24 = 0, n13 = 0;
        for (int b = beat - 15; b <= beat; b++)
        {
            const int c = onsetHigh(b, 0);
            if (c < 0)
                continue;
            const int bib = ((b - secStart) % 4 + 4) % 4;
            if (bib == 1 || bib == 3) { on24 += c; n24++; }
            else { on13 += c; n13++; }
        }
        // (on whichever parity carries it: a grid a beat off has the clap on
        // its 1 and 3, and it is still the clap)
        if (n24 >= 6 && n13 >= 6)
        {
            on24 /= n24;
            on13 /= n13;
            if (on24 >= 5.0 && on24 >= 1.4 * on13 + 0.5)
            {
                m_backbeat = true;
                m_backbeatParity = 1;
            }
            else if (on13 >= 5.0 && on13 >= 1.4 * on24 + 0.5)
            {
                m_backbeat = true;
                m_backbeatParity = 0;
            }
        }
    }

    // R359_PUMP (Tobias 10-05, "pumpen foelger nummerets egen pumpen"): how
    // far the sound falls between two kicks, over the next eight kicked beats
    // (the music is known ahead: a drop is read on its own kicks from its
    // first beat, not on the groove before it).
    // In 86 library tracks it runs from 0.60 (Titanium, Set Fire To The Rain:
    // full the whole way) to 1.0 (Hotel Room Service: nothing between the
    // kicks); median 0.93. The pulse's depth follows it below.
    if (m_audPump.isEmpty() == false && m_audKick.count() == m_audPump.count())
    {
        qreal sum = 0.0;
        int n = 0;
        for (int b = qMax(1, beat); b <= m_audKick.count() && b < beat + 32 && n < 8; b++)
        {
            if (m_audKick.at(b - 1) >= 153)
            {
                sum += m_audPump.at(b - 1) / 255.0;
                n++;
            }
        }
        if (n >= 4)
            m_pumpNow = sum / n;
    }
    // R359_DENSITY (Tobias 10-05, "bevaegelsens fart foelger hvor travl
    // percussionen er"): the share of the quarters where the highs hit (an
    // onset of 6 or more, BLT's quarters) over this beat and the seven after
    // it - the two bars about to play, so a drop is not read on the silence
    // before it. Read on the bar
    // line in a groove or a drop: 0.70 and more is busy (a step faster), 0.35
    // and under sparse (a step slower), back at 0.62 / 0.42. In the drops of
    // 15 library tracks the middle half runs 0.44-0.59; Eminem's Monster sits
    // at 0.81, Sprinter at 0.28.
    if (m_onsetHigh.isEmpty() == false && tier >= 1 && isBuild == false && preDrop == false
        && isCalm == false && still == false && m_mixing == false)
    {
        if (beatInBar == 0 || sectionChanged)
        {
            int hits = 0, n = 0;
            for (int b = beat; b <= beat + 7; b++)
            {
                for (int q = 0; q < 4; q++)
                {
                    const int c = onsetHigh(b, q);
                    if (c < 0)
                        continue;
                    n++;
                    if (c >= 6)
                        hits++;
                }
            }
            if (n >= 24)
            {
                const qreal d = qreal(hits) / qreal(n);
                if (d >= 0.70)
                    m_density = 1;
                else if (d <= 0.35)
                    m_density = -1;
                else if ((m_density == 1 && d < 0.62) || (m_density == -1 && d > 0.42))
                    m_density = 0;
            }
        }
    }
    else
        m_density = 0;
    // R360_DROP_GROW: a grown drop walks a step faster, from 50 % on the slider
    if (m_dropGrow && isDrop && fader >= 0.50 && m_speed == 0 && m_halfTime == false)
        m_density = 1;

    /* ---- levels: the build climbs like a snare roll, not a straight line ---- */
    // The build used to start at 0.40 and only reach 0.55 at the halfway
    // mark - below the groove it came out of, so it read as the light going
    // DOWN. It starts at the groove's level now and climbs from there.
    // Every tier is also opened up by the energy: at a full fader a break is
    // as bright as a groove used to be, and a groove is nearly a drop.
    // ... and the build starts ON the groove's line and rolls from it to the
    // drop's 1.0 (runde 265). It read 0.65 + 0.35 prog^2 + 0.15 e - equal
    // to the groove only at e 0.25 and under it everywhere above, where
    // builds actually run (they are shown from a fader of 30 %): at e 0.80
    // the groove's 0.88 fell to 0.77 on the build's first beat, and it took
    // until half the build to get back - the light going DOWN into a build,
    // the very thing the note above says was fixed. It also sat on 1.0 for
    // the last quarter at the top of the fader. Same snare-roll shape.
    // R358_RISER (Tobias 10-05, "builden foelger den rigtige riser"): the
    // build's climb read off the music - the highs and the waveform over four
    // beats, from where the build starts to its top just before the drop (the
    // whole track is known). A riser that flattens holds the light where it
    // is - it never climbs on past the music. A quarter of the count stays
    // in, so the light still moves on a flat build. Only where the music
    // climbs at all (12 % or more); otherwise the count as before.
    // (runde 359: the measurement is riserAt(), shared with the ramps)
    qreal buildProg = prog;
    const qreal measured = isBuild ? riserAt(beat, secStart, riserTo) : -1.0;
    const bool riserFollow = measured >= 0.0;
    if (riserFollow)
        buildProg = qBound(0.0, 0.25 * prog + 0.75 * measured, 1.0);
    qreal eNow = qBound(0.0, energy, 1.0);
    const qreal grooveLevel = 0.60 + 0.35 * eNow;
    qreal tierLevel = isBreak ? (0.45 + 0.35 * eNow)
                    : isBuild ? (grooveLevel + (1.0 - grooveLevel) * buildProg * buildProg
                                 * (0.50 + 0.50 * m_nextDropSize))   // runde 358; R362_BUILD_SIZE: a small drop, a lower top
                    : isDrop  ? 1.0
                              : grooveLevel;
    if (preDrop)
        tierLevel = qMax(tierLevel, 0.60);
    if (isIntro || isOutro)
        tierLevel *= 0.80;               // the ends of a track are not the middle
    if (isCalm)
        tierLevel = qMin(tierLevel, 0.55);
    // ENERGY 0 is the still room: the same brightness whatever section the
    // track is in - it rose through a build and jumped on a drop (runde 198)
    if (still)
        tierLevel = 0.60;
    // The energy is already inside tierLevel above, per section type; here it
    // only keeps a very quiet room from running at full. The LEVEL slider is
    // a straight brightness trim.
    // 45 % at the bottom of the fader, 100 % at the top - a real spread, and
    // linear the whole way so every ten per cent is visible. The tier decides
    // the shape (a break dips, a drop is full); this decides how loud the
    // whole picture is.
    qreal level = qBound(0.0, tierLevel, 1.0) * (0.45 + 0.55 * eNow)
                * qBound(0.0, levelScale, 1.0);

    // Runde 356, feature 4: a break follows the music's own loudness. The
    // waveform of this beat and the one before against the break's
    // surroundings (16 beats either side, the median): a vocal that swells
    // lifts the light, a thinning dips it - 0.80 to 1.15 of the level, never
    // under 0.80, so the break is never darker than it was meant to be by
    // more than a fifth. The breath and the pump ride on top as before.
    if (isBreak && isCalm == false && still == false && silenceNow == false
        && m_audLevel.isEmpty() == false && beat > 1 && beat <= m_audLevel.count())
    {
        // runde 358 (R358_VOCAL): while the mids carry the break alone (a
        // voice, a melody), the light follows THEM rather than the whole mix
        auto loud = [&](int b) {
            const qreal m = m_vocalNow ? audMid(b) : -1.0;
            return m >= 0.0 ? m : audLevel(b);
        };
        QVector<qreal> around;
        for (int b = qMax(1, beat - 16); b <= qMin(int(m_audLevel.count()), beat + 16); b++)
            around.append(loud(b));
        std::sort(around.begin(), around.end());
        const qreal med = qMax(0.05, around.at(around.count() / 2));
        const qreal now = 0.5 * (loud(beat) + loud(beat - 1));
        level = qBound(0.0, level * qBound(0.80, 0.70 + 0.30 * now / med, 1.15), 1.0);
    }
    // two bars of silence and more: the base back, slowly, to 30 %
    if (silenceLong)
        level = qMin(level, 0.30 * qBound(0.25, qreal(intoSilence - 7) / 4.0, 1.0)
                            * (0.60 + 0.40 * glowNorm));      // runde 357: it breathes with the small sounds
    if (silenceGlow)
        level = qMin(level, 0.04 + 0.20 * glowNorm);         // runde 357
    // R358_LOOP_LIFT: a loop the DJ holds in a build, a break or a groove is
    // the build he is making by hand - four per cent more light on every
    // pass, a fifth at most. Not in a drop (it is full), a mix or CALM.
    // R360_MIX_LOOP: in a mix too, when the other deck is quiet (its section a
    // break, an intro, a build or an outro by BLT's mix profile) - the DJ
    // looping the track that still carries the room. 10-02: all 47 loops were
    // in a mix.
    const bool mixQuietIn = m_mixing && m_incomingAt >= 0 && m_clock.elapsed() - m_incomingAt < 8000
                         && (m_incomingState == QStringLiteral("break") || m_incomingState == QStringLiteral("intro")
                             || m_incomingState == QStringLiteral("build") || m_incomingState == QStringLiteral("outro"));
    const bool loopNow = djLoopOn() && (m_mixing == false || mixQuietIn) && hold == false && isCalm == false && still == false;
    if (loopNow && isDrop == false && silenceNow == false)
        level = qMin(1.0, level * (1.0 + qMin(0.20, 0.04 * qreal(m_loopPasses - 1))
                                        * qBound(0.0, fader, 1.0)));   // R363_LOOP_ENERGY: the slider sets how far a loop lifts

    // how hot a chase may be right now: the stars a motion needs. Drawn once
    // per section from ramps of the energy, not read off a step
    // ... and on a fader NUDGE (see faderNudge) the ceiling is re-read
    // deterministically - the ramp's midpoint decides, no dice - so a hand
    // pushing the fader up meets a hotter pool on the next bar and never a
    // colder one by luck. The dice stay for the section draw, where the
    // variety is wanted.
    bool ceilNudge = hold == false && beatInBar == 0 && isBreak == false
                  && m_ceilEnergy >= 0.0 && qAbs(energy - m_ceilEnergy) >= 0.05;
    int ceilBefore = m_starCeil;
    // runde 276: an inner flag of a build that has drawn (buildKeeps) keeps
    // its ceiling as it keeps its moves - the redraw there diced it again, so
    // a build drawn at three stars could fall to two half way up and the
    // climb joining at 32 / 16 left was picked from the colder pool. A new
    // section energy there still moves it, as a nudge: deterministic and
    // only the way the energy went.
    const bool ceilRedraw = redraw && buildKeeps == false;
    if (ceilRedraw || ceilNudge || m_starCeil <= 0)
    {
        // two stars from a fifth of the fader, three from the middle: the
        // hot programmes used to wait for 0.60 in a groove, so the pool the
        // room drew from did not change between 30 % and 60 % of the fader
        qreal p2 = isDrop ? qBound(0.0, (energy - 0.10) / 0.35, 1.0) : qBound(0.0, (energy - 0.20) / 0.35, 1.0);
        qreal p3 = isDrop ? qBound(0.0, (energy - 0.40) / 0.35, 1.0) : qBound(0.0, (energy - 0.50) / 0.40, 1.0);
        if (isDrive)
        {
            p2 = qBound(0.0, (energy - 0.15) / 0.35, 1.0);
            p3 = qBound(0.0, (energy - 0.45) / 0.38, 1.0);
        }
        bool diced = ceilRedraw || m_starCeil <= 0;
        const bool ceilUp = energy > m_ceilEnergy;      // read before it moves
        m_starCeil = 1;
        if (diced ? (rng->bounded(1000) < int(p2 * 1000.0)) : (p2 >= 0.5))
        {
            m_starCeil = 2;
            if (diced ? (rng->bounded(1000) < int(p3 * 1000.0)) : (p3 >= 0.5))
                m_starCeil = 3;
        }
        // ... and a nudge moves it only the way the fader went (runde 263),
        // as the cast's nudge does (sameWay, runde 172). The ceiling came
        // from dice, so the midpoint read could land BELOW it on a fader
        // pushed up: a groove diced to three stars at 0.60, nudged to 0.66,
        // reads p3 = 0.40 and fell to two - the hand going up met a colder
        // pool, and ceilMoved let the hot programme go on that bar.
        // (runde 269: and on a fader JUMP between section lines - a jump is
        // a redraw, so it diced, and a groove pushed from 0.62 to 0.85 fell
        // from three stars to two one time in eight. A section change keeps
        // its free roll - the section, not the hand, decides there - and so
        // does the eight-bar redraw with nobody on the fader.)
        if (diced == false || (faderJump && sectionChanged == false && ceilBefore > 0))
            m_starCeil = ceilUp ? qMax(m_starCeil, ceilBefore) : qMin(m_starCeil, ceilBefore);
        m_ceilEnergy = energy;
    }
    // the ceiling moved under a hand on the fader: a held programme that is
    // now above it, or two notches below it, gives way on this bar
    bool ceilMoved = ceilNudge && m_starCeil != ceilBefore;
    int maxStars = isBreak ? 1 : m_starCeil;

    /* ---- sweeps: the heads draw a figure around their aim - circle, eight,
     *      line, leaf, lissajous... - sized and paced by the energy, redrawn
     *      with the moves. A relative EFX: it needs a running aim under it,
     *      and it steps aside while a sweep of the user's own runs. ---- */
    foreach (const QString &key, m_sweepFunc.keys())
    {
        const TrackGroup &g = m_groups.value(key);
        QString slot = "efx:" + key;
        quint32 mf = m_active.value("mot:" + key, Function::invalidId());
        // Only a programme that steers pan or tilt itself sends the sweep
        // away (aimsOf). A dimmer walk on the heads is not a reason for the
        // heads to stand still - it was, for most of the night; see aimsOf().
        bool userMoves = mf != Function::invalidId() && m_funcs.value(mf).aims;
        quint32 aimFid = m_active.value("pos:" + key, Function::invalidId());
        bool aimed = aimFid != Function::invalidId();
        // An aim that MOVES - one of our own tilt figures, now that they are
        // reachable again (round 56) - is the group's movement. Running the
        // EFX on top of it would put two hands on the same tilt channel, and
        // the beams would judder between them. One or the other: the figure
        // this section drew, or the sweep.
        bool aimMoves = aimed && m_funcs.value(aimFid).type != int(Function::SceneType);
        // HOLD freezes the figure rather than stopping it; STILL, CALM and a
        // blackout do stop it
        // and no laser figure in a break: there the bars stay in the home
        // aim and the slow chase is the whole movement.
        // CALM keeps the heads drifting at break pace (Tobias, 2026-09-14) -
        // it stops the lasers, the pulse and the colour changes, not the one
        // slow figure that keeps the room from looking switched off.
        // The EFX stays on the bars. Tobias, 2026-09-15: "Det maa gerne vaere
        // en EFX, bare ikke laser-barens indbyggede kanal-bevaegelser." An EFX
        // writes pan and tilt - that is us steering the mirror, frame by
        // frame, and we can start it, shape it and stop it. What had to go was
        // the fixture's own Movement Effect channel, which runs a pattern
        // inside the bar at its own pace and ignores everything else; that is
        // held at zero everywhere now (round 41), and AUTO will not pick an
        // aim scene that switches it on (macroPosition(), round 38).
        //
        // It is also the only thing that moves them, deliberately: the pos:
        // slot underneath holds a STILL aim, so there is one hand on the
        // wheel. drawSweep() gives the bars 56-80 beats per figure (round 18),
        // which at 20 m is well under half a metre a second.
        //
        // No figure in a break - there the bars sit at the home aim - and CALM
        // stops the lasers while the heads keep drifting.
        // R385_BARS_FULL: the bars' slow lift in their full-light alone break
        const bool liftBreak = m_barsAloneNow && m_aloneFull && g.lasers && g.patternDevice == false
                            && (m_active.contains(slot) == false || m_sweepShown.value(key).upOnly);
        // R386_LIFT_ENDS: the lift is the break's - it does not run on into
        // the drop as the bars' figure (they are never redrawn while running)
        const bool liftLeft = g.lasers && m_active.contains(slot) && m_sweepShown.value(key).upOnly
                           && (m_barsAloneNow && m_aloneFull) == false;
        bool wanted = castSet.contains(key) && aimed && aimMoves == false
                   && userMoves == false && (g.lasers ? moveDark : darkGroups).contains(key) == false
                   && (isCalm == false || g.lasers == false) && still == false && m_blackout == false
                   && (g.lasers == false || (m_fullAuto && (isBreak == false || liftBreak) && isBuild == false && isCalm == false))
                   && liftLeft == false
                   // "Indtil 40 % energi skal de slet ikke bevaege sig"
                   // (Tobias, 2026-09-18). drawSweep() only asks on the beat
                   // a figure is DRAWN, so a figure drawn at 45 % kept running
                   // at 32 % until the next section (runde 171).
                   // A fader resting on 0.40 (or the closing slide passing it)
                   // stopped and restarted the EFX on alternate beats, from
                   // phase 0 each time - the bars jerking (runde 179). The
                   // margin is ABOVE the line, not under it: a running figure
                   // stops at 0.40, a new one starts from 0.43 - runde 179 let
                   // it run on down to 0.37, against the rule (runde 190)
                   && (g.lasers == false || fader >= (m_active.contains(slot) ? 0.40 : 0.43));
        if (wanted == false)
        {
            // R401_FIGURE_END_DARK (harness: stress, and the 98 % night - a new
            // track starting in a break, the bars in the cast). The figure was
            // stopped on the same beat the group went dark, and the MasterTimer
            // put the mirrors back on the home aim a frame before the light
            // went: 8-13 units with the programme in mot: still at its level.
            // Off its aim, a laser figure now ends in two beats: the light goes
            // on the first while the figure still runs, the figure stops on the
            // second, dark as well (below)
            EFX *endFx = qobject_cast<EFX *>(m_doc->function(m_sweepFunc.value(key)));
            const bool offAim = endFx != nullptr
                             && endFx->height() + endFx->width() + qAbs(int(endFx->yOffset()) - 127) >= 2;
            if (g.lasers && m_active.contains(slot) && offAim && m_flashHeld.contains(key) == false
                && m_figureEndDark.contains(key) == false)
            {
                stopSlot("col:" + key, true);
                stopSlot("mot:" + key, true);
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
                darkGroups.insert(key);
                m_figureEndDark.insert(key);
                continue;
            }
            m_figureEndDark.remove(key);
            // An EFX has no fade-out: a bar figure stopped puts the beams
            // straight back on the aim - up to 26 steps (20 degrees) lit.
            // Dark for that beat, like a position move (runde 220). The
            // levels below read darkGroups.
            if (g.lasers && m_active.contains(slot) && castSet.contains(key))
                darkGroups.insert(key);
            // leaving the cast: the light goes before the mirror does (runde
            // 303, bane B's headless B23 R1). The MasterTimer can tick between
            // this stop and the cast loop's cuts (col:/mot:/parts below); in
            // that order it showed the beam lit on the home aim for a frame
            // (headless A2 76.07 s: dimmer 45, 10 units)
            if (g.lasers && m_active.contains(slot) && castSet.contains(key) == false
                && m_flashHeld.contains(key) == false)
            {
                stopSlot("col:" + key, true);
                stopSlot("mot:" + key, true);
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
            }
            if (m_active.contains(slot))
                stopSlot(slot, true);
            m_sweep.remove(key);
            continue;
        }
        // the build tightens its figure ONCE, on the first downbeat past the
        // middle: "beats > 4" as the test redrew it every bar from there on
        // (a halved build figure is 5-14 beats), so the heads jumped to a new
        // figure every bar through the back half of every build
        // on the same clock as prog: a promoted build measures to the drop, and
        // the section's own clock made "past the middle" true on three bar
        // lines running in a four-bar groove - a new figure every bar (runde 172)
        qreal prevProg = progBefore >= 0.0 ? progBefore : qreal(beat - 4 - secStart) / qreal(len);
        // (runde 276: not at an inner flag of a build that has drawn - the
        // figure is the build's, like its moves; the redraw there restarted
        // the EFX with a figure the history rule pushed away from this one.
        // The middle below still tightens it, once.)
        bool fresh = (redraw && buildKeeps == false) || m_sweep.contains(key) == false
                  || (hold == false && isCalm == false        // HOLD/CALM freeze it too (r199)
                      && isBuild && prog >= 0.5 && prevProg < 0.5 && beatInBar == 0 && m_sweep.value(key).shape >= 0);
        // Runde 219: a bar figure is never redrawn while it runs. On 09-20
        // it lasted 10-14 s against a 30-60 s figure: every section line,
        // cast change, fader jump and two in three 8-bar lines drew a new
        // one, and a new one restarts the EFX - six bars jumping, and not
        // one cycle ever finished. The fader still moves it live (the dip,
        // applySweep) and every stop - a break, CALM, under 40 %, the bars
        // leaving the cast - draws afresh the next time.
        // Keyed on what SHOWS: NEXT, a new track and calm(0) empty m_sweep
        // while the EFX runs on, and a figure drawn then would be put on the
        // running EFX without a restart (review, runde 219).
        if (g.lasers && m_active.contains(slot) && m_sweepShown.contains(key))
        {
            fresh = false;
            m_sweep.insert(key, m_sweepShown.value(key));
        }
        if (fresh)
        {
            QList<int> history = m_sweepHistory.value(key);
            // how many heads the figure spans: the EFX holds one entry per
            // pan/tilt head, and a four-eye bar is one fixture with four of
            // them. Counting fixtures gave a bar no spread at all and gave
            // two four-eyes a fan of 180 degrees over eight heads.
            EFX *sweepEfx = qobject_cast<EFX *>(m_doc->function(m_sweepFunc.value(key)));
            int sweepHeads = sweepEfx != nullptr ? sweepEfx->fixtures().count() : g.fixtures.count();
            // calm draws at break pace whatever the section says
            int sweepTier = isCalm ? 0 : tier;
            TrackSweep sw;
            int total = 0;
            int samples = m_fullAuto && m_ratingOn && isCalm == false ? 6 : 1;
            for (int sample = 0; sample < samples; sample++)
            {
                TrackSweep candidate = drawSweep(sweepTier, isBuild && isCalm == false, prog, energy, sweepHeads, g.lasers, isDrive);
                for (int attempt = 0; attempt < 4 && candidate.shape >= 0 && history.contains(candidate.shape); attempt++)
                    candidate = drawSweep(sweepTier, isBuild && isCalm == false, prog, energy, sweepHeads, g.lasers, isDrive);
                if (m_fullAuto && g.lasers == false && sweepTier > 0 && key != m_rhythmLead)
                {
                    // the floor, not the curve: applySweep() scales `beats`
                    // with the fader and lays this under it (runde 271)
                    // ... not in a build (runde 287, Tobias: "de skal være fri
                    // for tempo-gulv på full-auto"): the build's halving is
                    // what makes the heads speed up to the drop
                    if (isBuild == false)
                        candidate.paceRole = key == base ? 1 : 2;     // the floor, live (runde 290)
                    // runde 254 (Tobias: "flere ... bevægelser (movingheads)
                    // på tværs af sektioner og energilevel"): from the middle of
                    // the fader the BASE keeps the relation it drew - wave,
                    // serial, a start offset, a fan - instead of every head
                    // mirroring its neighbour. On 25-26 Sep the mirror was 100 %
                    // of the heads' groove/build/drop beats: the 36 relations
                    // only ever played in breaks. The pace floor (16 beats a
                    // figure) stays - "passende fart" - and below 50 % and on
                    // the support groups the figure stays broad and mirrored.
                    // (runde 292: the base keeps it by chance along the slider,
                    // 0 at 40 % -> always from 70 % - it was a hard switch at 50;
                    // support heads keep a start offset (a trail) from 75 %,
                    // still mirrored and in one wave)
                    const bool keepRelation = key == base
                        && rng->bounded(1000) < int(1000.0 * qBound(0.0, (fader - 0.40) / 0.30, 1.0));
                    if (keepRelation == false)
                    {
                        const int drawnFan = candidate.fan;
                        candidate.spread = 0;
                        candidate.mirror = true;
                        candidate.fan = (key != base && fader >= 0.79) ? qMin(drawnFan, 45) : 0;   // R374_SPREAD   // a trail, not a criss-cross (review 294)
                    }
                }
                m_sweep.insert(key, candidate);
                int weight = samples == 1 ? 2 : autoLookWeight(autoLookKeys(castSet, energy), key);
                total += weight;
                if (sample == 0 || rng->bounded(total) < weight)
                    sw = candidate;
            }
            m_sweep.insert(key, sw);
            if (sw.shape >= 0)
            {
                history.append(sw.shape);
                while (history.count() > 2)
                    history.removeFirst();
                m_sweepHistory.insert(key, history);
            }
        }
        // the floor round: no figure on the heads - they stand straight down (r214)
        if (applySweep(key, ((m_floorRound || positionHeld(key)) && g.heads) ? TrackSweep() : m_sweep.value(key), bpm, energy)   // R378_POSITIONS: no figure on a held aim
            && g.lasers)
            darkGroups.insert(key);              // a lift is a reposition: dark (runde 303)
    }

    /* ---- zoom: a move of its own on the heads. Wide in a break, mid in
     *      the groove, by the drop's character in a drop, tightening through
     *      a build - and wide for the first bar when a drop lands ---- */
    foreach (const QString &key, m_zoomScenes.keys())
    {
        QString slot = "zoom:" + key;
        quint32 posId = m_active.value("pos:" + key, Function::invalidId());
        // only over our own positions: a position of the user's may set its own zoom
        bool ours = posId != Function::invalidId()
                    && (m_funcs.value(posId).generated
                        // runde 255: the AUTO head figures write pan/tilt/speed only
                        || m_funcs.value(posId).name.startsWith(QStringLiteral("AUTO ")));
        // ... and not under a programme that zooms by itself (runde 220): the
        // show has eight "AUTO Wash Zoom ..." / "... Zoom ..." chasers that
        // write only the zoom channel, and they pass every motion filter. Two
        // writers on one LTP channel - each chaser step took the zoom, each
        // zoom change of ours took it back. The programme wins; ours waits.
        const quint32 motId = m_active.value("mot:" + key, Function::invalidId());
        const bool progZoom = motId != Function::invalidId() && m_funcs.value(motId).generated == false
                           && m_funcs.value(motId).name.contains(QStringLiteral("zoom"), Qt::CaseInsensitive);
        if (castSet.contains(key) == false || ours == false || progZoom || m_blackout)
        {
            if (m_active.contains(slot))
                stopSlot(slot, true);
            m_zoom.remove(key);
            m_zoomMode.remove(key);
            continue;
        }
        // Runde 214 (Tobias: "Lav 1 (og saet ogsaa stram til 0)"): nine steps
        // (0 = the sharpest beam, 4 mid, 8 wide) and three ways to wear them -
        // held for the section, the drop's pulse (wide on the one, sharp on
        // the rest: the beam breathes with the kick) and alternating heads
        // (every other head sharp, trading every bar). A build tightens a step
        // a bar from wide to the sharpest beam at the drop.
        const QList<quint32> &zs = m_zoomScenes.value(key);
        int want = m_zoom.value(key, -1);
        int mode = m_zoomMode.value(key, 0);
        if (m_floorRound)
        {
            want = 0;                                        // the floor round: sharp spots
            mode = 0;
        }
        else if (m_vocalNow && isDrop == false && isBuild == false && hold == false)
        {
            want = 0;                                        // runde 358: the voice alone - sharp beams
            mode = 0;
        }
        // dropBar, not bar: a late drop (FAKE DROP) lands on bar m_dropLand,
        // and `bar == 0` lost it its wide landing (runde 168)
        else if (isDrop && dropBar == 0 && hold == false)
        {
            want = 8;                                        // the landing: everything wide
            mode = 0;
        }
        else if (isBuild && hold == false && dropWaiting == false)   // r217: the wait holds the build's last zoom
        {
            // rampProg (runde 277): prog on the bar line never passed 1 - 4/len,
            // so the build ended on step 2 of 8 (16 beats), 1 (32) - never tight
            if (want < 0 || beatInBar == 0)
                want = qBound(0, int(qRound(8.0 * (1.0 - rampProg))), 8);
            mode = 0;
        }
        else if (want < 0 || (hold == false && (redraw || (isDrop && dropBar == 1 && beatInBar == 0))))
        {
            int chosen = want, total = 0;
            int samples = m_fullAuto && m_ratingOn ? 6 : 1;
            for (int sample = 0; sample < samples; sample++)
            {
                int w3;                                      // 0 narrow, 1 mid, 2 wide
                if (isBreak)       w3 = rng->bounded(4) == 0 ? 1 : 2;
                else if (isDrop)   w3 = (m_dropStyle == 2 || m_dropStyle == 4) ? 2 : ((m_dropStyle == 3 || m_dropStyle == 5) ? 0 : (m_dropStyle == 1 ? int(rng->bounded(2)) : int(rng->bounded(3))));
                else
                {
                    // a groove: the fader picks the beam. Wide and soft at
                    // the bottom, mid through the middle, and from half a
                    // fader up more and more often TIGHT - a hard beam is
                    // what makes the big, quick figure (runde 100) read as a
                    // beam travelling and not as a wash breathing.
                    qreal ez = qBound(0.0, energy, 1.0);
                    int roll = int(rng->bounded(1000));
                    if (roll < int(600.0 * qBound(0.0, (ez - 0.50) / 0.50, 1.0)))
                        w3 = 0;
                    else if (roll < int(600.0 * qBound(0.0, (ez - 0.50) / 0.50, 1.0)) + int(500.0 * (1.0 - ez)))
                        w3 = 2;
                    else
                        w3 = 1;
                }
                want = w3 * 4;
                // runde 292: on the slider - a break's wide beam tightens a
                // little (8 -> 6), and a drop leans one half-step narrower
                // more and more often from 50 %
                if (isBreak && w3 == 2)
                    want = qBound(6, int(qRound(8.0 - 2.0 * qBound(0.0, fader, 1.0))), 8);
                else if (isDrop && want > 0
                         && rng->bounded(1000) < int(1000.0 * qBound(0.0, (fader - 0.50) / 0.50, 1.0)))
                    want -= 2;
                m_zoom.insert(key, want);
                int weight = samples == 1 ? 2 : autoLookWeight(autoLookKeys(castSet, energy), key);
                total += weight;
                if (sample == 0 || rng->bounded(total) < weight)
                    chosen = want;
            }
            want = chosen;
            // how the section wears it: the hard and the nervous drop pulse
            // with the kick from 55 %; a groove from 45 % trades sharp and
            // wide between the heads a third of the time
            mode = 0;
            if (isDrop && (m_dropStyle == 1 || m_dropStyle == 5) && energy >= 0.55)     // hard, nervous
                mode = 1;
            else if (isDrop == false && isBreak == false && energy >= 0.45
                     && rng->bounded(1000) < int(500.0 * qBound(0.0, (energy - 0.45) / 0.45, 1.0)))
                mode = 2;                            // runde 292: 0 -> 1 in 2 over 45-90 % (was 1 in 3)
        }
        m_zoom.insert(key, want);
        m_zoomMode.insert(key, mode);
        int idx = want;
        // neither under CALM nor in a still room ("nothing moves or changes",
        // runde 217): the held zoom stands
        if (mode == 1 && isCalm == false && still == false)
            idx = beatInBar == 0 ? 8 : 0;
        else if (mode == 2 && isCalm == false && still == false)
            idx = (bar % 2) == 0 ? 9 : 10;
        run(slot, zs.at(qBound(0, idx, int(zs.count()) - 1)), 1.0, 0, true);
    }

    /* ---- musical fills; the eight-bar clock is only a no-curves fallback ---- */
    int phraseBar = bar % 8;
    // not in the pre-drop bar: its dark beats piled onto the blink - two or
    // three dark beats in the last bar before a drop (runde 172)
    // ... and not in a fake drop's kick wait (runde 272): the wait is tier 1
    // with isDrop false, so a crash on the flagged downbeat fired a fill and
    // the silence after it blacked out every group but the base for three
    // beats - where the build's look is meant to hold, and the hits and the
    // strobes are already held back
    bool phraseAllowed = hold == false && isCalm == false && still == false
                      && preDrop == false && dropWaiting == false
                      && (isDrop || (tier == 1 && energy > 0.5));
    // Runde 324: with the RAW per-beat kick (BLT 323+) the fill is the fill
    // itself - no kick on this beat nor the one before. Measured in the drops
    // of 92 library tracks: of the smoothed rule's fires 68 % sat in a real
    // kick gap, and it caught about a third as many; the raw gap is in one
    // by definition. The kick COMING BACK (turn) is the end of the fill, and
    // accelerating the lead from there put the turnaround in the new phrase
    // rather than into it - so with the raw curves it is not a fill signal.
    bool fillSignal = haveCurves && (rawGap >= 0
                                     ? (rawGap == 1 || (high > 0.65 && kick < 0.35 && riser > 0.08))
                                     : (turn || (high > 0.65 && kick < 0.35 && riser > 0.08)));
    if (phraseAllowed && fillSignal && beat - m_fillLast >= 8)
    {
        m_fillLast = beat;
        m_fillUntil = beat + 3;   // one bar of response, never an endless acceleration
    }
    bool turnaround = phraseAllowed && (haveCurves ? beat <= m_fillUntil
                                      : (bar >= 6 && (phraseBar == 6 || phraseBar == 7)));
    bool landing = isDrop && dropBar == 0 && isCalm == false;
    // Runde 287 (Tobias: "du bestemmer, men måske det skal variere?"): the
    // landing bar is the fast IMPACT chase or the still, full-on picture by
    // the drop's character - hard and nervous land on the chase, wide and
    // heavy stand still, a plain or tight drop tosses a coin drawn with the
    // character. A settled drop has stepped down to heavy/wide, so a drop
    // flag inside it lands still and the impact comes in bar two (from 60 %).
    const bool landChase = isDrop && (m_dropStyle == 1 || m_dropStyle == 5
                                      || ((m_dropStyle == 0 || m_dropStyle == 3) && m_landCoin
                                          && dropSettled == false));
    // With curves a blackout accent needs a real gap. Never blank the kick
    // as it returns. The explicit pre-drop cue above remains unchanged.
    // (runde 324: on the raw beat when BLT sends it - the smoothed curves
    // average a silent beat with the four around it and rarely reach 0.20)
    bool phraseDark = haveCurves ? (rawQuiet >= 0 ? rawQuiet == 1
                                                  : (kick < 0.20 && high >= 0.0 && high < 0.20))
                                 : (phraseBar == 7 && beatInBar == 3 && ((bar / 8) % 2) == 0);
    if (turnaround && phraseDark && energy > 0.6)
    {
        foreach (const QString &key, castSet)
        {
            if (key != base)
                darkGroups.insert(key);
        }
    }

    // The breath's phase runs on across the section's inner flags (runde
    // 273). It was read off m_beatIndex, which restarts at every flag: in a
    // break of 6-bar breaths the base jumped from 0.74 to 0.85 of its level
    // in one beat at a 16-beat phrase line, and a redraw from 6 to 4 bars jumped it
    // too. Advanced here by the part of the beat that just ended, at the rate
    // it breathed at - so the sine is continuous whatever the flags, a DJ
    // loop or a new period do. A group that stops breathing drops its phase.
    {
        const qint64 breathNow = m_clock.elapsed();
        QMap<QString, qreal>::iterator it = m_breathPhase.begin();
        while (it != m_breathPhase.end())
        {
            const int bars = m_breathe.value(it.key(), 0);
            if (bars <= 0)
            {
                it = m_breathPhase.erase(it);
                continue;
            }
            if (m_beatMs > 0.0)
            {
                const qreal done = qBound(0.0, qreal(breathNow - m_beatStartMs) / m_beatMs, 1.0);
                it.value() = std::fmod(it.value() + done / (qreal(bars) * 4.0), 1.0);
            }
            ++it;
        }
    }
    // this beat's clock: the pulse timer measures its breath against it
    if (bpm > 0.0)
        m_beatMs = 60000.0 / bpm;
    m_beatStartMs = m_clock.elapsed();
    m_beatIndex = beat - secStart;
    // r162: an occasional three-group conversation within the chosen cast.
    // Only intensity is shaped: no extra fixtures, colour or laser aiming.
    // Runde 262: and not in the bar before a drop - that bar pulls the cast IN
    // so the drop lands lit; a voice held at 0.40 there, and the base the only
    // light on the blink beat, is the opposite.
    if (!m_fullAuto || hold || isCalm || isBreak || isBuild || m_blackout || m_mixing
        || sectionChanged || exposureRest || preDrop)
        m_sequenceGroups.clear();
    if (!m_sequenceGroups.isEmpty())
    {
        // (decided first, cleared after: clearing the list from inside a
        // range-for over that same list frees the storage the loop is
        // walking. The break made it survive; a later edit would not.)
        bool castLeft = false;
        for (const QString &key : std::as_const(m_sequenceGroups))
            if (!castSet.contains(key)) { castLeft = true; break; }
        if (castLeft)
            m_sequenceGroups.clear();
        if (stageNow - m_sequenceStart >= m_sequenceGroups.size() * 4 * m_sequenceBeatMs)
            m_sequenceGroups.clear();
    }
    // Twelve beats: a conversation begun with the next flag closer than that
    // was cut off mid-phrase by it (every flag is a sectionChanged) instead of
    // fading back into the show - on a 40-beat groove it began at offset 32
    // and the drop at 40 cut it (runde 262). beatsToNext <= 0: no flag known.
    if (m_fullAuto && m_sequenceGroups.isEmpty() && !hold && !isCalm && !isBreak
        && !isBuild && !m_blackout && !m_mixing && !sectionChanged && !exposureRest
        && !preDrop
        // (review 294: a voice is 1-2 beats since runde 292 - the whole
        // sequence 12-24 beats - and a flag inside it cut it mid-phrase)
        && (beatsToNext <= 0 || beatsToNext >= int(std::ceil(12.0 * (2.0 - qBound(0.0, (fader - 0.45) / 0.45, 1.0)))))
        && fader >= 0.45 && beatInBar == 0 && (beat - secStart) % 32 == 0
        && stageNow - m_sequenceLast >= qint64(60000.0 - 30000.0 * qBound(0.0, (fader - 0.45) / 0.45, 1.0)))
    {
        QStringList conversation;
        if (castSet.contains(base) && ambientBase(base)) conversation << base;
        // Eyes/heads answer first; laser bars finish. No switched pattern
        // devices or strobes: fractional levels must mean actual dimming.
        for (int laserPass = 0; laserPass < 2; ++laserPass)
            for (const QString &key : castSorted)
            {
                const TrackGroup &g = m_groups.value(key);
                if (key != base && g.hasDimmer && !g.parts.isEmpty() && !g.strobes
                    && !g.patternDevice && int(g.lasers) == laserPass)
                    conversation << key;
            }
        if (conversation.size() >= 3)
        {
            m_sequenceGroups = conversation.mid(0, 3);
            m_sequenceStart = m_sequenceLast = stageNow;
            // runde 292: each voice two beats at 45 %, one at 90 % (was one
            // beat, one every 45 s at every fader); 60 -> 30 s apart
            m_sequenceBeatMs = (bpm > 0 ? 60000.0 / bpm : 500.0)
                             * (2.0 - qBound(0.0, (fader - 0.45) / 0.45, 1.0));
        }
    }
    // Runde 260: who lights the room beside the base this beat - see
    // baseCovered(). Decided after every dark group of the beat is known.
    m_baseCover.clear();
    foreach (const QString &key, castSet)
    {
        const TrackGroup &cg = m_groups.value(key);
        if (key != base && key != m_compositionBase && darkGroups.contains(key) == false
            && m_groupOff.contains(key) == false && cg.lasers == false
            && cg.patternDevice == false && (cg.hasDimmer || cg.rgb))
            m_baseCover.insert(key);
    }
    bool anyPulse = !m_sequenceGroups.isEmpty();
    bool moveHit = false;
    // runde 153: did a strobe group draw one of the show's chases this beat?
    // On the log so the trial can be measured instead of remembered.
    bool strobeChase = false;

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        bool inCast = castSet.contains(key);

        if (inCast == false)
        {
            // a held FLASH keeps its parts, in the cast or not
            if (m_flashHeld.contains(key))
                continue;
            // lasers cut hard: their aim may change now, and a beam that is
            // still fading would swing while lit
            // ... and on a drop's landing everything that leaves cuts (runde
            // 242): the build's groups faded out over two beats across the
            // downbeat - a drop is a cut, not a crossfade
            stopSlot("col:" + key, g.lasers || hard);
            // B23: the bar programme owns the dimmer and the eyes - it faded
            // while the figure's EFX snapped the mirror home (headless run: up
            // to 24 tilt units at dimmer 255 for 180 ms)
            stopSlot("mot:" + key, g.lasers || hard);
            for (int i = 0; i < g.parts.count(); i++)
                stopSlot(partSlot(key, i), g.lasers || hard);    // effects fade out over a bar
            m_pulseDepth.remove(key);
            m_breathe.remove(key);
            continue;
        }

        TrackMove mv = m_moves.value(key);
        if (m_fullAuto)
            mv = composeMove(key, mv, tier);
        if (isCalm || still)
            mv = TrackMove();
        // The drop's first bar (two, from 60 % on the slider) is the
        // IMPACT: every effect group runs a fast chase through its lamps,
        // pulsing hard, whatever it drew for the section - the white hit lands
        // on the downbeat above it, and from bar two the section's own look
        // takes over. The strongest moment of the record used to arrive with
        // the same look it would then run for thirty-two bars. Pattern devices
        // (animation lasers) and the base sit it out; the bars step on the
        // beat, never between (they never do).
        int impactBars = (fader >= 0.56 && m_dropSize >= 0.40) ? 2 : 1;    // the slider, as the comment says (runde 288); a small drop one bar (runde 360)
        bool impactNow = false;                  // runde 287: the landing reads it
        if (isDrop && dropBar >= 0 && dropBar < impactBars && key != base
            && (m_fullAuto == false || key == m_rhythmLead || g.strobes)
            && hold == false && isCalm == false
            && still == false && g.patternDevice == false && darkGroups.contains(key) == false)
        {
            mv.pattern = ENGINE_PAT_CHASE;
            // runde 338: the bars' impact is a block of two, not one bar alone
            mv.width = (g.lasers && g.parts.count() >= 4 && fader >= 0.60) ? 2 : 1;
            // runde 313 (review): the strobes' impact keeps the slider's pace
            // too (strobePaceFloor) - two beats a lamp under 50 %, eighths
            // only from 75 %; they are on from 30 % now
            mv.stepBeats = (g.strobes && fader < 0.50) ? 2 : 1;
            // runde 290: on the slider - quarters until 70 %, eighths above;
            // the hit 0.55 deep at 30 % and 0.90 at the top (was 0.85 flat)
            mv.subSteps = (g.lasers || fader < (g.strobes ? 0.75 : 0.70) || m_dropSize < 0.40) ? 1 : 2;   // runde 360: a small drop's impact in quarters
            mv.pulse = qMax(mv.pulse, 0.55 + 0.35 * qBound(0.0, (fader - 0.30) / 0.70, 1.0));
            // runde 346: the bars' hit is a chase with a blink, 0.40 deep, and
            // the full 0.90 only in the amok (the last 15 % of the slider)
            if (g.lasers)
                mv.pulse = fader >= ENGINE_BARS_AMOK ? 0.90 : 0.40;   // runde 348: only at 100 %
            mv.pulseOn = 0;
            mv.ownChaser = false;
            impactNow = true;
        }
        // the floor round: the heads hand ONE lit head round (runde 214) - bare,
        // so the acceleration below takes it from every other beat to sixteenths
        if (m_floorRound && g.heads && g.patternDevice == false && g.parts.count() >= 2)
        {
            mv.pattern = ENGINE_PAT_CHASE;
            mv.bare = true;
            mv.ownChaser = false;
            mv.pulse = 1.0;
            mv.pulseOn = 0;
            mv.breatheBars = 0;
            mv.texture = 0.0;
            mv.stepBeats = 2;
            mv.subSteps = 1;
        }
        if (isBuild)
        {
            // drawMove() saw prog at the START of the build and is only asked
            // again every eight bars, so everything it shaped from prog was a
            // constant: the bare blink never accelerated, the strobes never
            // went over to the beat. The climb is shaped here, on every beat,
            // where prog is live - which is the whole point of a build.
            if (mv.bare)
            {
                // the blink handed round faster and faster: how far it gets by
                // the end is the fader's decision - every beat at the bottom,
                // eighths halfway up, sixteenths at the top. And it stays
                // BARE: nothing lit between the blinks
                qreal reach = 1.0 + 3.0 * qBound(0.0, energy, 1.0);
                // (the step decisions on the bar's first beat, runde 242;
                // on rampProg, so the last bar IS the top - runde 277)
                mv.stepBeats = rampProg < 0.30 ? 2 : 1;
                qreal sub = 1.0 + (reach - 1.0) * qBound(0.0, (rampProg - 0.30) / 0.70, 1.0);
                int subs = sub >= 3.0 ? 4 : (sub >= 1.6 ? 2 : 1);
                mv.subSteps = g.lasers ? 1 : qMin(g.strobes ? 2 : 4, subs);
                if (g.strobes == false)
                    mv.pulseOn = (rampProg < 0.20 && energy < 0.5) ? 1 : 0;
            }
            else
            {
                // the roll: steps halve as the build climbs, the pulse deepens
                mv.stepBeats = qMax(1, mv.stepBeats >> qBound(0, int(rampProg * 3.0), 2));   // on a bar line (r242, r277)
                mv.pulse *= 0.5 + 0.5 * prog;
            }
            // the strobes are handed over to the beat as the build runs out
            if (g.strobes)
                mv.pulseOn = barProg > 0.60 ? 0 : 1;
        }
        // the turnaround: bars 7-8 of an eight-bar phrase move twice as
        // fast, the way a drummer fills into the next phrase - down to
        // eighths and sixteenths when it is hot. The landing bar stands still
        // ... but not on the strobes (runde 154). ENGINE_PAT_STATIC lights
        // EVERY lamp in the group - patternMask returns an all-ones mask - so
        // on six powerful strobes hung across the ceiling a landing bar is a
        // full bank standing lit, which is the one picture Tobias ruled out.
        // They keep walking through it; the landing is still audible in the
        // rest of the room.
        else if (key != base && landing && g.strobes == false
                 && (landChase == false || impactNow == false))
            mv.pattern = ENGINE_PAT_STATIC;
        else if (key != base && (m_fullAuto == false || key == m_rhythmLead) && turnaround && mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL
                 && impactNow == false)     // the landing's impact is fast enough (runde 288)
        {
            if (mv.stepBeats > 1)
                mv.stepBeats = qMax(1, mv.stepBeats / 2);
            else if (mv.subSteps < 4 && energy > 0.55 && g.lasers == false)
                mv.subSteps *= 2;      // the bars never step between the beats
        }
        // a hats-only passage (highs up, no kick) sparkles rather than sits
        if (high > 0.65 && kick >= 0.0 && kick < 0.35 && mv.pattern == ENGINE_PAT_STATIC && g.lasers == false
            && key != base && (m_fullAuto == false || key == m_rhythmLead)
            && isCalm == false && still == false && isBreak == false && g.parts.count() >= 3)
        {
            mv.pattern = ENGINE_PAT_SPARKLE;
            mv.stepBeats = 2;
        }
        // the DJ's SPEED: half or double everything that steps
        if (m_speed < 0)
        {
            if (mv.subSteps > 1) mv.subSteps /= 2;
            else mv.stepBeats *= 2;
        }
        // R358_HALFTIME: a half-time groove halves what steps, as SPEED's half
        // does - the generated patterns as well as the programmes (divisionFor)
        else if (m_halfTime && m_speed == 0 && impactNow == false)
        {
            if (mv.subSteps > 1) mv.subSteps /= 2;
            else mv.stepBeats *= 2;
        }
        // R359_DENSITY: busy percussion a step faster, sparse a step slower
        // (not under half time - that already halves - nor SPEED)
        else if (m_density > 0 && m_speed == 0 && impactNow == false)
        {
            if (mv.stepBeats > 1) mv.stepBeats = qMax(1, mv.stepBeats / 2);
            else if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL
                     && g.lasers == false) mv.subSteps = qMin(4, mv.subSteps * 2);
        }
        else if (m_density < 0 && m_speed == 0 && impactNow == false)
        {
            if (mv.subSteps > 1) mv.subSteps /= 2;
            else mv.stepBeats *= 2;
        }
        else if (m_speed > 0)
        {
            if (mv.stepBeats > 1) mv.stepBeats = qMax(1, mv.stepBeats / 2);
            else if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL
                     && g.lasers == false) mv.subSteps = qMin(4, mv.subSteps * 2);
        }

        // The strobe lamps do not run between the beats outside a drop
        // (Tobias, 2026-09-15: "deres chases er alt for hurtige generelt paa
        // full auto"). Six lamps handing a pattern round on sixteenths reads
        // as a strobe whatever channel it is on, and the shutter is what a
        // strobe is for - driveStrobe() owns that, on a budget. Here they get
        // a whole beat in a groove and a break, eighths at most in a drop.
        //
        // Last, deliberately: the build path, the turnaround and the SPEED
        // slider all raise subSteps above, and every one of them has to land
        // under this ceiling. The generated chases were slowed the same way
        // (gen_programs.py: everything without a drop word in its name is a
        // whole beat or slower), so the two halves now agree.
        if (g.strobes)
            mv.subSteps = qMin(mv.subSteps, tier == 2 ? 2 : 1);
        // ... and for everyone, a ceiling in TIME (runde 244): sub-steps are
        // fractions of a beat, so at a fast tempo a quarter-beat walk on the
        // heads or the Minis is a full on/off flicker - 86 ms at 174 BPM, over
        // eleven a second. Nothing faster than 110 ms a step outside the
        // strobes' own shutter: it changes nothing below 136 BPM (the rig's
        // nights have been 102-130).
        while (mv.subSteps > 1 && m_beatMs > 0.0 && m_beatMs / mv.subSteps < 110.0)
            mv.subSteps /= 2;

        // ... and the third place (runde 153): when the strobes have drawn one
        // of the show's chases for this drop, pinning the generated picture to
        // STATIC and the pulse to the downbeat would run underneath it and put
        // back exactly the wobble the chase replaced. The chase IS the picture
        // in that case.
        if (m_fullAuto && tier > 0 && key != m_rhythmLead && g.strobes
            && mv.ownChaser == false
            && impactNow == false)   // runde 327: the landing's impact keeps its eighths (313)
        {
            // runde 154: this used to pin the picture to STATIC and the pulse
            // to the downbeat - a lit bank blinking once a bar, which is
            // exactly what "never standing still on a colour" rules out. The
            // walk and its beat stay; sub-beat flicker and the bar flash do
            // not, because those are the lead's job.
            // runde 338: a drop from 75 % on the slider keeps the eighths
            // drawMove drew - the fast walk Tobias missed
            if (tier != 2 || fader < 0.75)
                mv.subSteps = 1;
            mv.flashBar = false;
        }

        QString colour = m_colour;
        // the mix's second half: the base stands in the incoming track's colour
        if (key == base && mixTurnDue && m_nextColour.isEmpty() == false && m_override.isEmpty()
            && m_mixGlide == false)      // runde 374: the glide paints the base itself
            colour = m_nextColour;
        // Runde 306 (Tobias: "Når jeg har valgt fliser, må man vælge så mange
        // som man har lyst til"): EVERY tile on the rig at once. The base wears
        // the lead; the other groups in the cast take the tiles after it in
        // turn (the partner first), each the first of them it can actually
        // show (the bars have no orange: the next tile, never a substitute).
        // The lead turns at each colour change, and the whole spread with it.
        const bool spread = m_overrideSet.count() >= 2 && key != base;
        if (spread)
            colour = spreadColour.value(key, colour);    // worked out once, above (review 307)
        quint32 splitScene = Function::invalidId();
        QString tradeOther;   // the accent group's other half of the trade (runde 230)
        // (review 305: with the tiles' set, a group that cannot show the
        // partner - the bars have no orange - keeps the room colour, rather
        // than a substitute that makes a third colour)
        // (runde 306: with the tiles' set every group already has its own tile
        // colour - the spread above - so the accent sits out)
        if (accentColour.isEmpty() == false && key == accentGroup && spread == false
            && (m_overrideSet.count() < 2 || colourForGroup(key, accentColour) == accentColour))
        {
            // the accent either holds, or trades places with the palette
            // colour every few bars - never a third colour
            colour = accentColour;
            tradeOther = m_colour;
            if (mv.colourBars > 0 && ((bar / mv.colourBars) % 2) == 1)
            {
                colour = m_colour;
                tradeOther = accentColour;
            }
            // on a per-eye lamp the two colours share the bar, and swap eyes
            // every second bar
            if (g.perEye && m_fullAuto)
            {
                bool swap = ((bar / 2) % 2) == 1;
                // through colourForGroup(), as `colour` is below: the bars have
                // no orange, and an eye asked for one got nothing (runde 171)
                const QString roomHere = colourForGroup(key, m_colour);
                const QString accentHere = colourForGroup(key, accentColour);
                splitScene = splitColourFunction(key, swap ? accentHere : roomHere, swap ? roomHere : accentHere);
            }
        }

        // the base is the light the room stands on: brighter than the
        // effects in a break, where it is often the only thing lit
        // The kick has been away for a bar or more in a groove or a drop - a
        // vocal over held chords, a passage the analysis did not flag as a
        // break: the effects slide down to a little over half and snap back
        // the moment the kick returns, so the light is heard to listen.
        // Four beats before anything happens, and four more to the bottom:
        // two beats would have caught every half-time bar (kick on the one,
        // nothing on two, three, four) and the room would have pumped
        // between 55 % and 100 % every bar. The base is untouched; the room
        // never dims with it. (Tobias, 2026-09-16, forslag 3.)
        qreal duck = 1.0;
        // a groove or a drop, as the comment says - not a build, where the
        // kick routinely drops out for the last bars just as the room should
        // climb, nor the kick wait, which is kickless by definition (runde 172)
        if (m_kickGone >= 4 && isBreak == false && isCalm == false && key != base
            && isBuild == false && preDrop == false)
            duck = 1.0 - 0.45 * qBound(0.0, qreal(m_kickGone - 4) / 4.0, 1.0);
        // (runde 290: + 0.15 over 50-100 % on the slider - the support comes
        // up with the room instead of sitting at 70 % of it at every fader)
        qreal support = (m_fullAuto && tier > 0 && key != base && key != m_rhythmLead)
                      ? (g.strobes ? 0.55 : 0.70) + 0.15 * qBound(0.0, (fader - 0.50) / 0.50, 1.0) : 1.0;
        // RUNDE 352 (Tobias, 10-04: "synes ogsaa drops er lidt doede nu. De var
        // vildere foer i de hoeje energi niveauer"). The strobes are never the
        // rhythm lead, so as support they topped out at 0.70 of the level - a
        // lamp walking the row peaked at 160-180 even at 100 %. What punched
        // past that before was the bounce bug (a repeated step at FULL, fixed
        // in runde 351) and the automatic hits on the whole bank (gone in
        // runde 338, on his word). In a DROP the lit lamp now comes up to the
        // full level with the slider: 0.70 at 60 % -> 1.0 at 100 %. One lamp
        // or a few at a time - the walk - never the whole bank; MASTER and the
        // trim still apply.
        if (g.strobes && isDrop && support < 1.0)
            support = qMax(support, 0.70 + 0.30 * qBound(0.0, (fader - 0.60) / 0.40, 1.0));
        qreal groupLevel = qBound(0.0, level * ((isBreak && (key == base || m_barsAloneNow) && still == false) ? 1.4 : 1.0) * duck * support, 1.0);   // R383_BARS_ALONE: the bars alone have the base's lift
        // R385_BARS_FULL: full light - MASTER and the trim put on after
        if (m_barsAloneNow && m_aloneFull && g.lasers && g.patternDevice == false && still == false)
            groupLevel = 1.0;
        // runde 352: ... and from 75 % the walking strobe lamp punches on its
        // beat whatever the section's level is - 0.80 at 75 %, full at 100 %
        // (measured at 90 %: the lit lamp stood at 120-160 of 255 in drops,
        // Friday's punches had been the bounce bug). Still the walk, still
        // under MASTER, the trim and a missing kick.
        // Only a walk that BLINKS (pulse 0.5+, decays before the next beat):
        // a lamp held lit at that level stood as a glare half the drop.
        if (g.strobes && isDrop && fader >= 0.75 && mv.pulse >= 0.50 && darkGroups.contains(key) == false)
            groupLevel = qMax(groupLevel, (0.80 + 0.20 * qBound(0.0, (fader - 0.75) / 0.25, 1.0)) * duck);
        // run() puts MASTER and the trim on for us now, so the colour scene
        // gets the bare level - or the two would multiply. (The level WITH them,
        // `gl`, went with runde 174: its last reader was the chase, which gets
        // the bare level too now, and an unused local stops the -Werror build.)
        qreal glBase = darkGroups.contains(key) ? 0.0 : groupLevel;
        // the colour this group can actually show (a wheel has seven, the
        // palette has more) - and the motion pick below matches on it too,
        // so a bar programme in the substituted colour is found
        // RUNDE 330 (headless colour audit, 4 tracks of 20 Sep): a wheel with
        // no ROOM colour wears the look's partner when it has it, before a
        // neighbour. An orange room with a blue partner had the wash in
        // "Scissor Lift Orange Deep" (orange + blue) while the bars and the
        // animation laser, which have no orange, stood in red - three colours
        // in one look, against one partner colour per look (runde 243). With
        // a red partner (the common case) nothing changes: red is both. Not
        // with the tiles' set (it has its own spread, review 305/306). An
        // accent group on such a wheel keeps the partner through its trade
        // - the other half of the trade was the substitute, a third colour.
        // The bars' echo is then skipped (they already wear its hue, 286).
        if (colour == m_colour && m_partnerPick.isEmpty() == false && m_partnerPick != m_colour
            && m_overrideSet.count() < 2
            && colourForGroup(key, colour) != colour
            && colourForGroup(key, m_partnerPick) == m_partnerPick)
            colour = m_partnerPick;
        colour = colourForGroup(key, colour);

        // motion: real movement (chases, EFX) in drops, the climbing half of
        // a build, and on the base from the groove onward. Static pattern
        // scenes are looks and may show in any section. Never while calm.
        // the move drew whether this group runs one of the user's own chases
        // or EFX (never in a break, only the climbing half of a build); the
        // base may reach one star higher, it is what carries the room
        // ... except the laser bars in a break, which are only ever in a
        // break's cast to run a slow (tier 0: "break", "slow", "low") chase
        // the beats this build has still to go, for candidates() and
        // motionFor(): a build programme only in a build, and only with room
        // to reach its top. beatsToNext, not the section's length: a
        // riser-promoted build measures to the drop, and a pick half way in
        // must not start a 32-beat climb with eight beats left (review)
        // not while a fake drop waits for its kick: the build's top holds
        // there, and a fresh climb would start again from the bottom
        // ... and only when the next flag IS the drop: beatsToNext counts to
        // the next flag of any kind, and a climb that topped out on an inner
        // flag 40 beats before the drop was cut there (runde 233)
        m_buildLen = (isBuild && dropWaiting == false && nextState == QStringLiteral("drop"))
                   ? qMax(1, beatsToNext) : 0;
        bool breakLasers = isBreak && g.lasers;
        // The base may run a break programme of the show's own - but only one
        // that keeps the room lit (litOnly below, ENGINE_BREAK_LIT). Counted
        // 2026-09-16: of 220 break programmes for the heads, 104 have under a
        // third of them on at a time and 32 have over sixty per cent. The
        // first kind is what "in breaks the light just goes out" was; the
        // second is a swell or a halves trade, which is exactly the slow
        // movement a break is supposed to have. Not while CALM is held: that
        // button means "stop changing things".
        bool breakBase = isBreak && key == base && isCalm == false && still == false;
        // ... and a wash group beside the base (runde 235) runs its own calm
        // break programmes - the Minis have 48 (runde 234)
        bool breakWash = isBreak && key != base && isCalm == false && still == false
                      && g.strobes == false && g.lasers == false && g.patternDevice == false;
        bool moving = still == false
                   && (breakLasers || breakBase || breakWash
                       || (mv.ownChaser && isBreak == false
                           && (isBuild == false || barProg >= 0.5
                               // a group with build programmes starts one on
                               // the build's first beat: they are made to run
                               // the whole build (runde 227)
                               || m_climbGroups.contains(key))));
        int stars = qMin(3, maxStars + (key == base ? 1 : 0));
        // The animation lasers' cursor moves where the music turns - the same
        // turn the colours land on. At most once every two bars, and never
        // under HOLD; the offset is per group so the two lasers may differ.
        // (runde 273: it does NOT change the pattern on the turn itself - the
        // held pick below keeps the section's pattern, and Tobias said no to
        // changing that, BACKLOG 2026-09-23 point 7. The cursor is read by
        // the next re-pick: a colour change, a fader jump, the section line.)
        int cursor = m_motionCursor;
        if (g.patternDevice)
        {
            if (turn && hold == false && isCalm == false
                && beat - m_turnBeat.value(key, -100) >= 8)
            {
                m_turnCursor.insert(key, m_turnCursor.value(key, 0) + 1 + int(rng->bounded(3)));
                m_turnBeat.insert(key, beat);
            }
            cursor += m_turnCursor.value(key, 0);
        }
        // R384_BARS_WHOLE: the bars alone in a break take NO programme - the AUTO
        // bar programmes are mostly eye figures (two or three eyes of eight).
        // The colour scene lights the whole bar and the engine's move chases
        // whole bars (Tobias 10-07: "alle 8 oejne taendt samtidigt (hele baren)")
        const bool barsWhole = m_barsAloneNow && g.lasers && g.patternDevice == false && tier == 0;
        const int mTier = tier;
        quint32 mf = Function::invalidId();
        if (barsWhole)
            m_sectionMotion.remove(key);
        // no programme on the heads under the floor round: the round is the
        // engine's own blink on the dimmers (runde 214)
        if (isCalm == false && (m_floorRound && g.heads) == false && barsWhole == false)
        {
            // the base must leave the room lit: most in a break, least in a
            // drop, where a punch is the point
            qreal litFloor = (key == base) ? (isBreak ? ENGINE_BREAK_LIT
                                                      : (isDrop ? 0.25 : 0.35))
                                           : 0.0;
            // The quiet foundation (point 8, runde 162) wants the wash to keep
            // every head lit. Asked HERE, so the pick lands on a programme that
            // qualifies and is held for the section. Runde 162 only threw the
            // wrong one away after the pick - and m_sectionMotion went with it,
            // so the next beat picked the same programme at the same cursor and
            // threw it away again: a motionFor() sweep per beat, and the wash
            // with no AUTO programme at all for most of a groove. (Runde 164.)
            // Not in a drop - see patternMask().
            if (ambientBase(key) && m_compositionTier != 2)
                litFloor = qMax(litFloor, 0.99);
            // ... unless another lamp group keeps the room lit beside it
            // (runde 260, Tobias): then the base may go dark like the others
            if (key == base && baseCovered())
                litFloor = 0.0;
            // ONE figure for the section. The pick used to run on every
            // beat, and pickWeighted lands on `cursor % pool.count()` - so
            // when the star ceiling wobbled with the energy curve and a
            // single programme entered or left the pool, the modulo moved
            // and the room got a different figure. Measured in the log of
            // 2026-09-17: the wash held a programme for a MEDIAN OF FIVE
            // BEATS and the mini for two. Nothing lasts long enough to be
            // read as a figure, so a walk, a ripple and a fill all arrive as
            // the same flicker - "de ser alt for ens ud" (Tobias).
            //
            // Held per group and cleared at the section change, beside
            // m_lastFamily. The group leaving the cast clears it too: the
            // slot is stopped below, and the next section starts fresh.
            mf = m_sectionMotion.value(key, Function::invalidId());
            if (mf != Function::invalidId() && m_funcs.contains(mf) == false)
                mf = Function::invalidId();
            // ENERGY 0 is STILL: a chase or EFX held from before the fader came
            // down kept running - every place that clears the hold sits behind
            // `hold == false`, and STILL forces the hold (runde 171). A static
            // look may stay; the fresh pick below is static-only.
            if (still && mf != Function::invalidId()
                && m_funcs.value(mf).type != int(Function::SceneType))
                mf = Function::invalidId();
            // The colour changes INSIDE a section too - on the hold timer and
            // on a musical turn (above, holdUp / turnUp). A programme that
            // wears its own colour and was picked for the red room must not
            // be held through the change to blue: it keeps painting red, the
            // coversColour test below no longer matches, so the blue scene
            // runs under it, and HTP adds them up to magenta. A colourless
            // programme takes the new colour from the scene and may stay.
            if (mf != Function::invalidId())
            {
                const QString worn = m_funcs.value(mf).colour;
                // ... but NOT for the accent group's own trade. In a drop the
                // accent group flips between the accent colour and the room's
                // every colourBars bars (above), and the bars are the accent
                // group more often than not: every flip released the held
                // programme, and the bars changed programme every 2-4 bars
                // all night (2026-09-19: 21 of 33 bar changes were this,
                // median life 4 beats). The accent flip is a trade WITHIN one
                // look, so the programme stays through it; a change of the
                // ROOM colour (changeColour, the beat it happens) still lets
                // it go.
                // `colour` here is already the accent colour where there is
                // one, and already through colourForGroup() - so comparing
                // the held programme against it is comparing against what the
                // group is actually wearing. A second lookup was added on
                // 2026-09-20 and removed the same day: a no-op that cost a
                // candidates() sweep per group per beat.
                // Nor for a PATTERN DEVICE: motionFor() hands it a scene in a
                // partner colour when it has none in the room's (goesWith,
                // runde 170), and `worn` then never equals `colour` - the held
                // scene was thrown away and re-picked on every beat (runde 171).
                // It still lets go when the room colour changes.
                // RUNDE 333 (headless, 20 Sep, a mix out of a drop): the trade
                // exemption needs an ACCENT. accentGroup is drawn whether or
                // not there is one, so when the accent went (the mix's turn
                // cleared it, the cast fell under two, CALM) the bars kept
                // "Drop Eyes Row Snake Red" in a magenta room with the base on
                // the next track's blue - three colours, until the section ended.
                if (worn.isEmpty() == false && worn != colour
                    && (((key != accentGroup || accentColour.isEmpty() || spread) && g.patternDevice == false)
                        || changeColour))
                    mf = Function::invalidId();
                // ... and a two-colour programme whose partner is no longer the
                // look's (runde 243)
                if (mf != Function::invalidId()
                    && pairFits(m_funcs.value(mf), m_colour, m_partnerPick) == false)
                    mf = Function::invalidId();
            }
            // R400_HOLD_AIM: a position held on the page lets go of the
            // section's programme when it aims the heads (or zooms them under
            // STRAIGHT DOWN) - motionFor() would not draw it now
            // - and the one running goes hard: a soft stop fades its level
            // over up to two seconds and the chase goes on stepping pan and
            // tilt (LTP, not scaled) over the hold
            const quint32 motRunning = m_active.value(QStringLiteral("mot:") + key, Function::invalidId());
            if (motRunning != Function::invalidId() && heldAimBlocks(key, m_funcs.value(motRunning)))
                stopSlot(QStringLiteral("mot:") + key, true);
            if (mf != Function::invalidId() && heldAimBlocks(key, m_funcs.value(mf)))
                mf = Function::invalidId();
            // The fader moved the ceiling (ceilMoved): a programme hotter
            // than the new ceiling, or two notches colder, is not what the
            // hand asked for. One notch colder stays - the pool doubles the
            // top star, so the next pick will most likely be hotter anyway.
            // runde 313: a strobe chase held from higher up the slider lets
            // go when the hand comes down past its pace: held, the pace below
            // would stretch it - a slow version of a fast figure
            if (mf != Function::invalidId() && m_fullAuto && tier > 0 && g.strobes
                && m_funcs.value(mf).type != int(Function::SceneType)
                && stepBeats(m_funcs.value(mf), bpm) < strobePaceFloor(tier) * 0.95)
                mf = Function::invalidId();
            if (mf != Function::invalidId() && ceilMoved)
            {
                int have = qMax(1, m_funcs.value(mf).stars);
                if (have > stars || stars - have >= 2)
                    mf = Function::invalidId();
            }
            // ... and "DrypDryp" goes the beat the fader leaves 100 % (runde
            // 222) - candidates() will not hand it out again below that
            if (mf != Function::invalidId() && m_faderNow < 0.995
                && m_funcs.value(mf).name.contains(QStringLiteral("dryp"), Qt::CaseInsensitive))
                mf = Function::invalidId();
            // ... and a STATIC look held from the build's first half, once, on
            // the bar line where the build passes its middle (fejljagt 09-27):
            // `moving` turns true there, but the held scene was kept for the
            // rest of the build - "static until halfway, then the chase"
            // (runde 227, on barProg since runde 242) never came for it.
            // Same clock as the figure's tightening (prog/prevProg, beat 1).
            if (mf != Function::invalidId() && isBuild && moving && hold == false
                && beatInBar == 0 && barProg >= 0.5
                && (progBefore >= 0.0 ? progBefore : qreal(beat - 4 - secStart) / qreal(len)) < 0.5
                && m_funcs.value(mf).type == int(Function::SceneType))
                mf = Function::invalidId();
            // ... and a build programme outside a build (HOLD carried it
            // over the drop, runde 227)
            if (mf != Function::invalidId() && m_buildLen <= 0
                && m_funcs.value(mf).name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
                mf = Function::invalidId();
            // A long or odd build (136, 129, 93 beats): the climb joins on the
            // one beat where the beats left are a whole pass - 32 or 16 - so
            // its top still lands on the drop. Only when a climb is there to
            // take; the running programme is never dropped for nothing
            // (runde 233).
            if (mf != Function::invalidId() && moving && hold == false && m_climbGroups.contains(key)
                && (m_buildLen == 32 || m_buildLen == 16)
                && m_funcs.value(mf).name.contains(QStringLiteral("climb"), Qt::CaseInsensitive) == false)
            {
                const quint32 c = motionFor(key, colour, castSet, cursor, mTier, bpm, division, false, stars, litFloor);
                if (c != Function::invalidId()
                    && m_funcs.value(c).name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
                {
                    mf = c;
                    m_sectionMotion.insert(key, mf);
                }
            }
            if (mf == Function::invalidId())
            {
                mf = motionFor(key, colour, castSet, cursor, mTier, bpm, division,
                               moving == false, stars, litFloor);
                if (mf == Function::invalidId() && moving)
                    mf = motionFor(key, colour, castSet, cursor, mTier, bpm, division, true, stars, litFloor);
                // the first half of a build opens to a group with build
                // programmes FOR those programmes (runde 227). When none got
                // through (stars, a ban, the cast) it is the old rule: a
                // static look until half way, not an ordinary chase (review)
                // (barProg, as `moving`: prog <= 0.5 re-picked static on the
                // middle's own bar line and undid the release above - fejljagt 2)
                if (mf != Function::invalidId() && isBuild && barProg < 0.5 && m_climbGroups.contains(key)
                    && m_funcs.value(mf).type != int(Function::SceneType)
                    && m_funcs.value(mf).name.contains(QStringLiteral("climb"), Qt::CaseInsensitive) == false)
                    mf = motionFor(key, colour, castSet, cursor, mTier, bpm, division, true, stars, litFloor);
                if (mf != Function::invalidId())
                    m_sectionMotion.insert(key, mf);
            }
        }

        // The foundation uses generated intensity masks, whose minimum is
        // known. An opaque dimmer chase cannot promise a continuous floor.
        // Position-only programmes and all effect-group programmes stay intact.
        // (not in a drop: there the wash keeps its whole library - see
        // patternMask(), runde 164)
        if (ambientBase(key) && m_compositionTier != 2
            && mf != Function::invalidId() && m_funcs.value(mf).dimmer
            && m_funcs.value(mf).litShare < 0.99
            && m_funcs.value(mf).peakLit < ENGINE_OWN_FLOOR      // one lamp always on (runde 259)
            && baseCovered() == false)                           // ... or another group on (runde 260)
        {
            mf = Function::invalidId();
            m_sectionMotion.remove(key);
        }
        // R370_COLOUR_LAYER: the tiles' fade or chase owns this group's colour
        const bool layerOwns = layerGroup(key);
        if ((layerOwns || (m_mixGlide && key == base))   // R375_GLIDE_COLOURPROG
            && mf != Function::invalidId() && m_funcs.value(mf).setsColour)
        {
            mf = Function::invalidId();  // held from before the layer: drawn again without it
            m_sectionMotion.remove(key);
        }
        quint32 cf = splitScene != Function::invalidId() ? splitScene : colourFunction(key, colour);
        const quint32 cfWanted = cf;      // runde 285: the colour scene the group should wear
        // The programme paints a colour on every lamp in this group, so the
        // group's colour scene under it has nothing left to say - and it does
        // not stay silent: colour channels are QLCChannel::Intensity, which
        // QLC+ blends HTP, so magenta underneath cyan gives max(255,0),
        // max(0,255), max(255,255) = WHITE. That is what turned the two-
        // colour programmes into one colour and a white (2026-09-16: of the
        // twelve pair directions, four survived, four went white, two lost
        // the partner). The bars' split scene (the accent's two colours
        // shared on the eyes) goes too (runde 239): it was kept "because no
        // programme touches the per-eye channels" - but the eye programmes
        // do (Sweep, Braid, Pinch, Facing, and every bar programme writes
        // them to 0), and the split restarted at every swap and covered the
        // programme for up to two beats every two bars.
        // ... and only when the programme is actually wearing the colour the
        // room asked for. motionFor() falls back to another colour when this
        // one has nothing to offer, and a fallback must not be allowed to
        // repaint the room: there the scene underneath is the whole point.
        // ... and not while a strobe burst runs on the group: the colour scene
        // is what writes the shutter back to Open, and most programmes never
        // touch that channel - after a burst the washes strobed on at the
        // burst's rate until the next section (runde 210). driveStrobe() runs
        // after this, so the burst still wins while it lasts.
        if (mf != Function::invalidId()
            && m_funcs.value(mf).coversColour
            // ... or wearing the OTHER half of the accent trade: the held
            // programme is kept through the trade on purpose (above), and the
            // room's colour scene ran under it and HTP-added a third colour
            // for the other half of every trade (BACKLOG 65, runde 230)
            && (m_funcs.value(mf).colour == colour
                || (tradeOther.isEmpty() == false && changeColour == false
                    && m_funcs.value(mf).colour == colourForGroup(key, tradeOther)))
            && m_active.contains(QStringLiteral("str:") + key) == false)
            cf = Function::invalidId();
        // ... and not under a HELD FLASH: genFlash() stopped the colour scene
        // hard so the white is white (HTP), and this restarted it on the next
        // beat - the held white went back to white + the room's colour for
        // the rest of the hold (runde 223). The flash's own white scene is
        // running; the room colour comes back on the first beat after release.
        if (m_flash && m_flashHeld.contains(key))
            cf = Function::invalidId();
        // colour scenes swap hard: a soft fade left the old colour adding up
        // with the new one on RGB fixtures for a bar - a blend nobody asked for
        // ... and so does every other way a colour leaves the group (runde
        // 285). Only the col: swap was hard: a held programme let go for the
        // room's new colour, a programme giving way to the colour scene, or
        // the colour scene giving way to a programme in the new colour all
        // faded the old colour out over a second while the new one stood at
        // full - colour channels are HTP, so red -> green passed through
        // yellow ("aldrig gul"), red -> blue through a purple nobody drew.
        // A cut when the colour the group wore goes; a programme swap in the
        // same colour still crossfades.
        const quint32 motWas = m_active.value(QStringLiteral("mot:") + key, Function::invalidId());
        QString wearsNow = colour;
        if (mf != Function::invalidId() && m_funcs.value(mf).coversColour
            && m_funcs.value(mf).colour.isEmpty() == false)
            wearsNow = m_funcs.value(mf).colour;
        const bool motColourCut = motWas != Function::invalidId() && motWas != mf
            && m_funcs.value(motWas).setsColour
            && m_funcs.value(motWas).colour.isEmpty() == false
            && m_funcs.value(motWas).colour != wearsNow;
        if (layerOwns && cf != Function::invalidId())
        {
            m_layerOwned.insert(key);
            m_layerLevel.insert(key, glBase);
            applyColourLayer(key);
        }
        else if (m_mixGlide && key == base && cf != Function::invalidId())   // R374_MIX_GLIDE
        {
            m_mixGlideKey = key;
            m_layerLevel.insert(key, glBase);
            applyMixGlide(false);
        }
        else if (cf != Function::invalidId())
        {
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? glBase : 1.0, 0, true);
            stopSlot("colx:" + key, true);   // runde 370: the fade's second colour
        }
        else
            stopSlot("col:" + key,
                     m_active.value(QStringLiteral("col:") + key, Function::invalidId()) != cfWanted);

        if (mf != Function::invalidId())
        {
            const TrackFuncInfo &mi = m_funcs.value(mf);
            Function *mfunc = m_doc->function(mf);
            // a one-shot that has finished waits for its beat: every beat in
            // a drop, every other beat elsewhere
            bool wait = mi.oneShot && mfunc != nullptr && mfunc->isRunning() == false
                     && isDrop == false && (beatInBar % 2) != 0
                     && m_active.value("mot:" + key, Function::invalidId()) == mf;
            if (wait == false)
            {
                if (g.strobes)
                    strobeChase = true;
                int motionDivision = divisionFor(mi, bpm, division);
                // Enforce the support pace AFTER SETUP and SPEED overrides.
                // Scenes and EFX do not use the chaser's step-beat scale.
                // (runde 313: not the strobes - they step on the slider, next)
                if (m_fullAuto && tier > 0 && key != m_rhythmLead
                    && (mi.type == int(Function::ChaserType) || mi.type == int(Function::SequenceType))
                    && g.strobes == false)
                    motionDivision = qMax(2000, motionDivision);
                if (m_fullAuto && tier > 0 && g.strobes
                    && (mi.type == int(Function::ChaserType) || mi.type == int(Function::SequenceType)))
                    motionDivision = qMax(int(strobePaceFloor(tier) * 1000.0), motionDivision);
                // Runde 219: the strobes' ceiling above (a whole beat, eighths
                // at most in a drop) held for the engine's own picture only.
                // A show chase drawn for the strobes ran at its own tempo -
                // 23 "Fast" strobe chasers step on quarter beats. Same rule
                // for both now. Chasers and sequences only: 0 is "its own
                // time" for a one-shot, a scene or an EFX.
                if (g.strobes && motionDivision > 0
                    && (mi.type == int(Function::ChaserType) || mi.type == int(Function::SequenceType)))
                    motionDivision = qMax(tier == 2 ? 500 : 1000, motionDivision);
                // the bare level: run() adds MASTER and the trim (slotScale)
                const bool mayOwn = canOwnDimmers(mi, key == base);   // see motionOwns (runde 231)
                // RUNDE 341 (headless 10-03, the night's log): a laser group's
                // dark beat (a figure stopping, an aim moving) did not reach a
                // programme that cannot own the dimmers - it ran at 1.0, its
                // own dimmer values lit, and at a track change the bars swung
                // 24 units back to the aim with a Climb's first step on them
                // (tilt_jump_lit x5). Dark means dark: the programme runs at
                // nought for that beat, as the colour scene and the parts do.
                const qreal motLevel = (g.lasers && darkGroups.contains(key)) ? 0.0
                                     : (mayOwn ? glBase : 1.0);
                run("mot:" + key, mf, motLevel, motionDivision, hard || motColourCut);
                m_recentUse.insert(mf, m_clock.elapsed());     // the cooldown starts from its last beat
            }
        }
        else
        {
            stopSlot("mot:" + key, motColourCut);     // runde 285
            m_motionDim.remove(key);
            m_sectionMotion.remove(key);
        }

        if (g.hasDimmer)
        {
            // the pulse: on its beats the dimmers jump to the level and fall
            // back until the next one
            bool pulseBeat = mv.pulseOn == 0
                          || (mv.pulseOn == 1 && (beatInBar == 0 || beatInBar == 2))
                          || (mv.pulseOn == 2 && (beatInBar == 1 || beatInBar == 3))
                          || (mv.pulseOn == 3 && beatInBar == 0);
            qreal depth = darkGroups.contains(key) ? 0.0 : mv.pulse;
            // The fader, live, on the kick: the move drew its depth at
            // m_movesEnergy; between draws the fader scales it by the ratio
            // of a gentle curve (half depth at the bottom, full at the top),
            // so two per cent on the fader is two per cent on the pulse -
            // on this beat. Never past 0.95 (a switch-dimmer reads that as off).
            // (runde 264: the ratio is against what THIS move was drawn at -
            // a group joining mid-section drew at the energy of its own beat,
            // and against m_movesEnergy it was scaled for the fader's travel
            // since the last redraw a second time: up to a fifth too deep or
            // too shallow until the next redraw)
            const qreal drawnAt = mv.drawnE >= 0.0 ? mv.drawnE : m_movesEnergy;
            if (depth > 0.0 && drawnAt >= 0.0)
            {
                qreal eRef = qBound(0.0, drawnAt, 1.0);
                depth = qMin(0.95, depth * (0.5 + 0.5 * eNow) / (0.5 + 0.5 * eRef));
            }
            // The bass sets how far the light FALLS between two beats: a
            // heavy sub and the room pumps deep, a thin bass and it rides
            // light. The kick (below) is the hit, the bass is the weight
            // under it. Around two thirds up the curve nothing changes;
            // without curves nothing changes at all. Never past 0.95, or a
            // dimmer-as-switch fixture reads it as off.
            if (bass >= 0.0 && depth > 0.0)
                depth = qMin(0.95, depth * qBound(0.70, 0.70 + 0.60 * bass, 1.25));
            // R359_PUMP: and the track's own fall between the kicks, around
            // the library's median (0.93 = the depth as drawn): a quarter of
            // the tracks fall 0.85 or less and breathe at 0.80 of it, a full
            // one (0.75 and under) at 0.55; one that empties between its
            // kicks a little deeper, 1.15 at most (the strobes go all the way
            // down below, whatever this says)
            if (m_pumpNow >= 0.0 && depth > 0.0)
                depth = qMin(0.95, depth * qBound(0.55, 1.0 + 2.5 * (m_pumpNow - 0.93), 1.15));
            // ... but a strobe goes ALL the way down, whatever the bass or the
            // fader since the draw: "puls op og HELT ned" (Tobias, 2026-09-21).
            // The two scalers above only ever meant to cap the depth; they also
            // scaled it DOWN - thin bass left the walking lamp standing at a
            // third between its beats, lit on its colour with nothing
            // happening (runde 171).
            // 1.0, not 0.95: the "never past 0.95" is for a dimmer that is a
            // switch - the animation lasers, which have their own gate -
            // and the strobes are real dimmers; 0.95 left them at 5 %
            // between beats (runde 198)
            if (g.strobes && depth > 0.0)
                depth = 1.0;
            // A KICKLESS STRETCH ON THE BASE (runde 268). No kick is no pulse
            // beat, so the base sat on (1 - depth) for the whole passage: 8 %
            // of its level at the top of a drop, where that floor is meant for
            // the gap BETWEEN two hits ("reads as the light having failed",
            // drawMove) - and the duck above promises "the base is untouched;
            // the room never dims with it". The duck's own clock: four beats
            // of held breath (a half-time bar never gets there), then the
            // pump eases out over four more and the base stands on its level
            // until the kick is back - that beat resets m_kickGone, and the
            // full depth hits on it. The base only: the effects duck and may
            // go dark with the kick, a strobe is never lit with nothing
            // happening, and a build or the kick wait climbs on its own ramp.
            if (key == base && m_kickGone >= 4 && depth > 0.0 && isBreak == false && isCalm == false
                && isBuild == false && preDrop == false && g.strobes == false && g.patternDevice == false)
                depth *= 1.0 - qBound(0.0, qreal(m_kickGone - 4) / 4.0, 1.0);
            // the kick the analysis heard on this beat: no kick, no pulse;
            // a soft kick, a soft pulse. The kick scales the HIT, never the
            // depth: depth is how far the light falls between two beats, so
            // scaling it down for a soft kick RAISED the floor - a bare strobe
            // chase sat half-lit through every kickless passage
            // ... except in a BUILD (runde 274). "The kick routinely drops out
            // for the last bars just as the room should climb" (the duck
            // below) - and with no kick there was no hit at all: a bare
            // group (pulse 1.0) stood on (1 - depth), 5 % of its level, a
            // strobe on exactly nought, so the blink "handed round faster and
            // faster" was dark from the moment the snare roll took over, and
            // the base, whose depth deepens with prog, ended the build darker
            // than it started. A build keeps its grid: the pulse's own beats
            // hit, at the kick's strength (a deep pulse still at 0.90). The
            // kick wait is a build too (isBuild) - its look holds, blink and
            // all. A groove, a drop and a break still listen to the kick.
            qreal strength = 1.0;
            if (kick >= 0.0)
            {
                // (runde 342: not the base's pump in a break from 90 % - a
                // break has no kick by definition, and the pump is on the
                // beat the DJ's tempo keeps, as Tobias' own BLACKOUT taps were)
                const bool breakPump = isBreak && key == base && fader >= 0.75 && isCalm == false;   // runde 356: 0.90 -> 0.75
                if (kick < 0.20 && isBuild == false && breakPump == false)
                    pulseBeat = false;
                strength = 0.5 + 0.5 * qBound(0.0, kick, 1.0);
                // ... but a DEEP pulse reaches full every time. The peak is
                // (1 - depth) + depth * strength: with the base's floor down
                // at 7 % (round 54) a middling kick would have topped out at
                // half, and a room whose heads never reach full at the top of
                // the fader is a darker room than the one below it. The kick's
                // say is how far the light FALLS, and that is the floor's job
                // now; on a soft-pulsing group it still scales the hit as
                // before.
                if (depth > 0.60)
                    strength = qMax(strength, 0.90);
            }
            // A fixture whose dimmer is a switch is driven by a square gate,
            // and a square gate stays SHUT until something re-opens it. On a
            // beat that carries no pulse - an off-beat, or one the analysis
            // heard no kick on - it would sit dark until the next beat that
            // does, which on a quiet passage is the whole section. For those,
            // "no pulse this beat" has to mean "no gate this beat".
            // R358_BACKBEAT: claps or a snare on 2 and 4 - the strobes pulse
            // on THEM, not on the kick (a pulse never passes the level: MASTER
            // and the trim hold, runde 357)
            if (m_backbeat && g.strobes && depth > 0.0 && isBuild == false && preDrop == false
                && (isDrop || tier == 1))
            {
                pulseBeat = (beatInBar % 2) == m_backbeatParity && onsetHigh(beat, 0) >= 4;
                strength = qMax(strength, 0.90);
            }
            if (g.patternDevice && pulseBeat == false)
                depth = 0.0;
            if (depth > 0.0 && pulseBeat)
            {
                m_pulseStart.insert(key, m_clock.elapsed());
                m_pulseStrength.insert(key, strength);
                m_subStepSeen.insert(key, 0);
            }
            if (depth > 0.0 || mv.breatheBars > 0 || mv.subSteps > 1)
                anyPulse = true;
            m_pulseDepth.insert(key, depth);
            m_breathe.insert(key, darkGroups.contains(key) ? 0 : mv.breatheBars);
            // a breath that starts, starts at the top of the sine (runde 273):
            // from full, where the light was, instead of a step to 0.85
            if (m_breathe.value(key, 0) > 0 && m_breathPhase.contains(key) == false)
                m_breathPhase.insert(key, 0.25);

            // a real chase or EFX of theirs is the movement; the generated
            // pattern only runs when the look is static. A colour scene that
            // sets the dimmers itself hides the parts (HTP), so no pattern
            bool patterned = (mf == Function::invalidId() || m_funcs.value(mf).type == int(Function::SceneType))
                          && (cf == Function::invalidId() || m_funcs.value(cf).dimmer == false)
                          && (mf == Function::invalidId() || m_funcs.value(mf).dimmer == false);
            m_patterned.insert(key, patterned);

            // WHO OWNS THE DIMMERS. The engine's own dimmer parts are written
            // with Universe::ReplaceBlend - "own this channel while AUTO runs"
            // - and a chase's step scenes are ordinary HTP. An HTP write that
            // is LOWER than what is already there is thrown away
            // (universe.cpp: "if (value < currentValue) return false"), so a
            // chase could raise a dimmer but never lower one. Every step that
            // said "this lamp is dark" was ignored, and the whole of the AUTO
            // programme library was invisible on any group with a master
            // dimmer: the room only ever showed the engine's own figures.
            // Found 2026-09-16, two days before the rig had to work.
            //
            // So when the motion IS a chase that works the dimmers, the parts
            // hand the channel over: they write nought, and the chase's own
            // values are what the room sees. The group level, MASTER and the
            // trim went into the chase through run() above; the pulse follows
            // below, on the chase's own intensity.
            const TrackFuncInfo &mInfo = m_funcs.value(mf);
            // ... and never on the BASE (runde 178). Measured on the show: 120
            // of the wash's 1384 AUTO chases have steps with EVERY head dark -
            // "Wash Pulse Long", "Wash Blink", "Wash Pulse Backbeat" - and in a
            // drop the base may pick one (its lit floor there is 0.25). Owning
            // the dimmers, those put the room in the black for a beat at a
            // time, and "the base stays - the room never goes black" is the
            // one promise this engine keeps everywhere. The base keeps its own
            // figures, floors and pulse; the chase still brings its colour and
            // its heads. The effect groups show their programmes' patterns.
            // Runde 231: canOwnDimmers() - the base too, but only a chase
            // that keeps every head at ENGINE_OWN_FLOOR or more, so the
            // promise above holds.
            bool motionOwns = mf != Function::invalidId()
                           && canOwnDimmers(mInfo, key == base)
                           && darkGroups.contains(key) == false
                           // a colour scene that sets the dimmer itself sits
                           // under every lamp (HTP) and the chase's dark
                           // steps would never show (runde 174)
                           && (cf == Function::invalidId() || m_funcs.value(cf).dimmer == false);
            if (motionOwns)
                m_motionDim.insert(key);
            else
                m_motionDim.remove(key);

            m_liveMove.insert(key, mv);                  // the shaped move, for the sub-beat steps
            if ((m_flash && m_flashHeld.contains(key)) == false)
            {
                if (motionOwns)
                {
                    // STOP the parts, do not drive them to nought. A part
                    // scene writes with ReplaceBlend, and QLC+ applies faders
                    // in the order their functions started: a part that
                    // started AFTER the chase would replace the chase's values
                    // with its zero and the group would stand dark. That is
                    // exactly what happens when a group joins the cast on the
                    // same beat as its chase is drawn - col:, then mot:, then
                    // the parts. A stopped function has no fader at all, so
                    // the chase is the only writer whatever the order.
                    // setPart() starts them again by itself the moment the
                    // motion stops owning them.
                    for (int i = 0; i < g.parts.count(); i++)
                        stopSlot(partSlot(key, i), true);
                    m_moveLevel.insert(key, groupLevel);
                }
                else
                {
                    applyMove(key, darkGroups.contains(key) ? 0.0 : groupLevel,
                              beat, secStart, prog, mv, patterned);
                }
            }
            if (mv.flashBar && beatInBar == 0 && (bar % 2) == 1 && (haveCurves == false || turn))
                moveHit = true;
        }
    }

    if ((anyPulse || (m_landSixteenths && blink16Now())) && m_pulseTimer.isActive() == false)   // runde 359 (R362: or the mini landing)
    {
        m_pulseTimer.start();
    }
    else if (anyPulse == false && m_pulseTimer.isActive() && !(m_landSixteenths && blink16Now()))   // R363: not under the sixteenths
    {
        // the last factor the timer wrote may have been the shut half of a
        // gate: level everything again or that zero stays until the next beat
        m_pulseTimer.stop();
        reapplyLevels();
    }

    // the cast is settled here, before the hits: genFlash() reads m_cast to
    // find the groups it has to hand back, and a beat-old cast left a group
    // that has just dropped out sitting at full for a beat
    m_cast = castSet;

    applyGroupOff();
    // (driveStrobe moved below the hits, runde 220)

    /* ---- hits ---- */
    // the minimal guard: more than eight hits in 32 beats is a strobe show,
    // not an accent - then only the drop's own landing may flash
    if (m_hitBeats.isEmpty() == false && m_hitBeats.last() > beat)
        m_hitBeats.clear();
    while (m_hitBeats.isEmpty() == false && m_hitBeats.first() < beat - 32)
        m_hitBeats.removeFirst();
    // How often an accent may land is the energy's job: three in thirty-two
    // beats when the room is quiet, ten when it is not, and never two on top
    // of each other. Quiet nights stay calm; a full fader gets a real show.
    // Two ramps, no steps: from two accents in thirty-two beats at the
    // bottom to twelve at the top, and the shortest gap between two of them
    // slides from eight beats down to one. Both read eNow, the section-
    // scaled energy, not the slider: a quiet section at 100 % gets eight and
    // two (runde 283, comment only - the ramp is continuous, not a line on
    // the screen).
    int hitCeil = 2 + int(qRound(10.0 * eNow * eNow));
    int hitGap = qMax(1, int(qRound(8.0 - 7.0 * eNow)));
    bool crowded = m_hitBeats.count() >= hitCeil
                || (m_hitBeats.isEmpty() == false && beat - m_hitBeats.last() < hitGap);
    // ... and never on the pre-drop's one dark beat: a build hit there drove
    // the strobes to full and the blink became a flash (runde 172)
    // Not under BLACKOUT (runde 283): the hit ran its flash slots at nought
    // and spent the budget - and a white landing in the dark stamped
    // m_whiteLandMs, so the next big drop within three minutes landed in the
    // room's colour. driveStrobe is already held back there.
    // The landing passes the ceiling only where the drop ARRIVED (m_dropFrom
    // is its beat, moved back with a jump): a DJ loop over the drop's first
    // bar was "the drop's own landing" on every pass - two hits in four
    // beats for as long as the loop ran, what the ceiling calls a strobe
    // show (runde 283).
    // RUNDE 357 (R357_KICK_BACK, Tobias 10-05: "Ja, men ikke hver eneste gang,
    // kun store drops"): the kick coming back after a gap of two beats or
    // more inside a BIG drop is a hit of its own - the whole cast at full on
    // that beat. A big drop is the white landing's line: it arrived with the
    // slider at 85 % or more (75 % for a hard or heavy one). Fri+Sat: 243
    // kick returns inside drops. At most one in 16 beats, never in the
    // landing bar, and the hit budget below still decides.
    if (isDrop == false)
        m_bigDrop = false;
    else if (dropArrives)
        m_bigDrop = fader >= 0.85 || ((m_dropStyle == 1 || m_dropStyle == 4) && fader >= 0.78);   // R374_SPREAD
    // ... out of a four-on-the-floor: the two beats before the gap had their
    // kick, so a half-time or broken beat (a kick on one in four - Nelly
    // Furtado's drop fired every bar) is not a return. Fri+Sat: 39 such
    // returns in the drops of 28 tracks, gaps of 2-16 beats.
    int kickGap = 0;
    if (isDrop && m_bigDrop && dropBar > 0 && beat > 3 && beat <= m_audKick.count() && m_audKick.at(beat - 1) >= 115)
    {
        while (kickGap < 17 && beat - 1 - kickGap >= 1 && m_audKick.at(beat - 2 - kickGap) < 51)
            kickGap++;
    }
    const bool kickBack = kickGap >= 2 && kickGap <= 16 && beat - kickGap - 2 >= 1
                       && m_audKick.at(beat - kickGap - 2) >= 115 && m_audKick.at(beat - kickGap - 3) >= 115
                       && beat - m_kickBackLast >= 16;
    bool hit = isCalm == false && still == false && dropWaiting == false
            && m_blackout == false
            && (preDrop && beatsToNext == 1) == false
            && ((isBuild && (hitProg >= 0.0 ? hitProg : prog) > 0.82 && crowded == false
                 && (haveCurves == false || turn || (kick > 0.45 && riser > 0.08)))
                || (isDrop && dropBar == 0 && ((beatInBar < 2 && m_landOwed == false) || lateLand)   // runde 360: or late
                    // R363_LATE_HIT: a landing still owed (waiting for its kick,
                    // R361) has no hit on the beat without it - the hit comes
                    // with the late landing, and the hit budget does not take it
                    && (crowded == false || lateLand || (m_dropFrom >= 0 && beat - m_dropFrom < 2)))
                || (moveHit && crowded == false)
                || (kickBack && crowded == false)       // runde 357
                || (miniNow && m_miniSize >= 0.40 && crowded == false));   // R362_MINI_LAND (R366: this beat's)
    // R360_DROP_GROW: the line where the drop grows is a hit, from 60 %
    if (growNow && fader >= 0.58 && crowded == false && isCalm == false && still == false
        && m_blackout == false && hold == false)   // R361: HOLD holds
        hit = true;
    if (hit && kickBack)
        m_kickBackLast = beat;
    if (m_flash == false)
    {
        if (hit)
        {
            m_hitBeats.append(beat);
            // white on the downbeat of a drop - that is the one moment it
            // reads as a punch rather than as a lamp somebody forgot to
            // colour. Everywhere else the accent is in the room's colour.
            // White is for the BIG drops (Tobias, 2026-09-26: "fedt på store
            // drops. Men den kom ret ofte" - 240 white landings in 4.8 h):
            // an arriving drop, ENERGY 0.85 up (0.75 for a hard or heavy
            // one), at most one every three minutes. Every other drop still
            // lands with its hit, in the room's colour (runde 233).
            const bool whiteLand = isDrop && dropBar == 0 && (beatInBar == 0 || lateLand) && (m_dropArrivedNow || lateLand)   // R361_LATE_KICK (R369: the landing beat)
                // runde 360 (R360_DROP_SIZE): white is for a big drop - the
                // music's own jump, as well as the slider's line below
                && m_dropSize >= 0.50
                // the SLIDER, as every hard line in this engine: `energy` is
                // scaled by the section and could not reach 0.85 before the
                // clock's 02:00 (review)
                && (fader >= 0.85 || ((m_dropStyle == 1 || m_dropStyle == 4) && fader >= 0.78))   // R374_SPREAD
                && (m_whiteLandMs < 0 || m_clock.elapsed() - m_whiteLandMs >= 180000)
                // ... and not under a colour tile: a tile is ONE colour (runde
                // 205), as the echo, the accent and the partner already hold -
                // the BLUE tile at 90 % landed a drop on white strobes (runde 283)
                && m_override.isEmpty();
            if (whiteLand)
                m_whiteLandMs = m_clock.elapsed();
            QString hue = whiteLand ? QStringLiteral("white") : m_colour;
            // with the tiles' set the strobes flash in the tile they wear
            // (review 307), not the lead's
            if (whiteLand == false && spreadColour.isEmpty() == false)
            {
                foreach (const QString &sk, castSorted)
                {
                    if (m_groups.value(sk).strobes && spreadColour.contains(sk))
                    {
                        hue = spreadColour.value(sk);
                        break;
                    }
                }
            }
            quint32 ff = flashFunction(castSet, hue);
            // ... but only if his scene is actually in this colour. The
            // ranking in flashFunction() falls back to white and then to
            // "anything", which was harmless while it was the ONLY thing
            // that ran - it was the hit. Now that the generated flash runs
            // alongside it (below), a cyan hit would fire "Flash Strobes
            // RED" on top of the generated cyan and the three 8+8 strobes
            // would HTP-mix the two into a muddle. A colourless scene still
            // runs: it imposes no colour of its own.
            if (ff != Function::invalidId())
            {
                const QString fc = m_funcs.value(ff).colour;
                if (fc.isEmpty() == false && fc != hue)
                    ff = Function::invalidId();
            }
            // ... and never on strobes that are off stage (see genFlash) - nor,
            // on strobes, a scene that is not in the hit's colour. A colourless
            // one passed the colour test above, and "StrobStrobStrobe3lights"
            // (white 255 + the hardware strobe on the 8+8s) or "Flash
            // Everything" then fired white over a cyan hit, past the 70 %
            // ceiling; which one came first was down to the hash order of the
            // night. genFlash() below lights every strobe in the hit's own
            // colour, so nothing is lost (runde 207).
            if (ff != Function::invalidId())
            {
                foreach (const QString &fg, m_funcs.value(ff).groups)
                {
                    // runde 338: never on the strobes at all - see genFlash below
                    if (m_groups.value(fg).strobes)
                    {
                        ff = Function::invalidId();
                        break;
                    }
                }
            }
            // ... and never on a group that is dark for a move this beat or
            // in its hold - a flash scene on the laser bars would light the
            // beams mid-swing (fejljagt 3)
            if (ff != Function::invalidId())
            {
                foreach (const QString &fg, m_funcs.value(ff).groups)
                {
                    if (darkGroups.contains(fg))
                    {
                        ff = Function::invalidId();
                        break;
                    }
                }
            }
            // runde 292: the engine's own hit is 70 % bright at 55 % on the
            // slider and full at the top (was full at every fader); the held
            // FLASH button stays full strength (runde 199)
            if (ff != Function::invalidId())
                run("flash", ff, 0.70 + 0.30 * qBound(0.0, (fader - 0.55) / 0.45, 1.0), 0, true);
            // ... and the generated flash as well, not only as a fallback -
            // the same correction the manual button got in runde 131, for
            // the same reason. Measured on the show file (runde 134), every
            // flash scene in this show:
            //
            //   Flash Strobes WHITE     3 of 3 8+8 (white lamp), 0 of 3 80seg
            //   Strob 4 white 80 %      1 of 3 8+8 (white lamp), 0 of 3 80seg
            //   Flash Strobes RED       3 of 3 8+8 (RGB),        0 of 3 80seg
            //   Flash Strobes BLUE      3 of 3 8+8 (RGB),        0 of 3 80seg
            //
            // The three 80-segment strobes are in none of them, so HALF THE
            // STROBES HAVE NEVER FLASHED on a hit - and only red, blue and
            // white exist, so a hit in cyan, magenta, green or orange fell
            // through the ranking onto one of those and punched in the wrong
            // colour. genFlash() covers the whole group in the hit's own
            // colour; the operator's scene still runs on top of it.
            // RUNDE 338 - NOT ANY MORE. Tobias, 2026-10-03, after the night:
            // "du skal helt fjerne det der faar strobe-lamperne til at gaa paa
            // 100% dimmer (puler op et kort oejeblik)". This was it: every
            // automatic hit (a drop landing, a build's top, the bar flash)
            // put the WHOLE strobe bank on its dimmer at 70-100 % for a beat,
            // static, with the pulse switched off - 405 beats on the night of
            // 10-02. The strobes' own walk and the hardware shutter are their
            // accent now; the held FLASH button (setFlash) is the operator's
            // hand and still flashes them.
            // genFlash(true, hue);
            //
            // ... EXCEPT THE DROP ITSELF (runde 344, Tobias the next morning:
            // "strobe-lamperne maa stadig gerne gaa 100% paa droppet"). The
            // beat a drop ARRIVES - its first downbeat, not a loop over it,
            // not a build's top, not the bar flash - the whole bank lands at
            // full, in the hit's colour (white on a big one, whiteLand).
            if (isDrop && dropBar == 0 && (beatInBar == 0 || lateLand) && (m_dropArrivedNow || lateLand))   // runde 360: or late (R361: after its kick; R369: the landing beat)
                genFlash(true, hue);

            // The laser bars answer the hit: half a beat later, once, in the
            // colour opposite the room's, for a third of a beat - an echo.
            // In or out of the cast; at most one every four beats; not under
            // a break, calm or a still room. (Tobias, 2026-09-16, forslag 5.)
            if (energy >= 0.40 && isBreak == false && isCalm == false && still == false
                && mixTurned == false
                && (m_override.isEmpty() || m_overrideSet.count() >= 2)   // one tile: no echo in another (205); two: yes (304)
                && beat - m_echoBeat >= 4 && m_echoTimer.isActive() == false)
            {
                // One laser type at a time under the one-laser line (runde 235,
                // m_oneLaser from the bar line, runde 279): the bars answer in or
                // out of the cast, so with the animation laser on stage the echo
                // lit the second laser type for a third of a beat (runde 286).
                bool aniOnStage = false;
                if (m_oneLaser)
                {
                    foreach (const QString &key, castSet)
                    {
                        const TrackGroup &ag = m_groups.value(key);
                        if (ag.patternDevice && m_groupOff.contains(key) == false
                            && darkGroups.contains(key) == false)
                            aniOnStage = true;
                    }
                }
                QString echoKey;
                foreach (const QString &key, m_groupOrder)
                {
                    const TrackGroup &eg = m_groups.value(key);
                    if (aniOnStage == false
                        && eg.lasers && eg.patternDevice == false && m_groupOff.contains(key) == false
                        && darkGroups.contains(key) == false)
                    {
                        echoKey = key;
                        break;
                    }
                }
                if (echoKey.isEmpty() == false)
                {
                    // A CONTRAST THAT GOES WITH THE ROOM, not the opposite. The
                    // echo was the opposite colour (forslag 5, 2026-09-16):
                    // green -> magenta, red -> cyan, orange -> cyan. Tobias,
                    // 2026-09-22: "farverne ... skal passe sammen, altid" - and
                    // green/magenta is one of the pairs left out on purpose.
                    // These are the strongest contrasts in gen_programs'
                    // HARMONY (runde 171).
                    static const QMap<QString, QString> contrast = {
                        { "red", "blue" }, { "blue", "magenta" }, { "cyan", "magenta" }, { "magenta", "cyan" },
                        { "green", "cyan" }, { "orange", "blue" }, { "white", "blue" }, { "amber", "red" },
                        { "purple", "blue" }, { "pink", "magenta" }, { "uv", "magenta" } };
                    // ... and the LOOK'S partner where it has one (runde 286). One
                    // partner colour per look (runde 243): the table gave its own
                    // contrast whatever the look had drawn, so a red room with an
                    // orange partner (or accent, or the next track's colour in a
                    // mix) echoed blue - three colours. The table is only the
                    // fallback for a look with no partner.
                    // (runde 292: a one-colour look echoes in its own colour -
                    // which the bars already wear, so no echo)
                    QString echoHue = m_partnerSolo ? m_colour
                        : (m_partnerPick.isEmpty() == false && m_partnerPick != m_colour)
                        ? m_partnerPick
                        : contrast.value(m_colour, QStringLiteral("white"));
                    if (m_palette.contains(echoHue) == false || engineBannedColour(echoHue))
                        echoHue = QStringLiteral("white");
                    const bool echoShows = m_overrideSet.count() < 2
                                        || colourForGroup(echoKey, echoHue) == echoHue;   // review 305
                    echoHue = colourForGroup(echoKey, echoHue);
                    quint32 ef = echoShows ? colourFunction(echoKey, echoHue) : Function::invalidId();
                    // not the scene the group already wears (fejljagt 09-27):
                    // the contrast can fall back to the room's own colour, and
                    // two slots on one function share one intensity override -
                    // stopSlot() leaves a shared function alone, so the echo's
                    // level stayed on the bar after it ended, and a hard stop
                    // of col: (laserFaderCheck, selfTest) stopped nothing
                    if (ef == m_active.value("col:" + echoKey, Function::invalidId()))
                        ef = Function::invalidId();
                    if (ef != Function::invalidId())
                    {
                        m_echoKey = echoKey;
                        m_echoFid = ef;
                        m_echoBeat = beat;
                        m_echoTimer.start(int(qMax(120.0, m_beatMs * 0.5)));
                    }
                }
            }
        }
        else
        {
            stopSlot("flash", true);
            if (m_flashHeld.isEmpty() == false)
                genFlash(false);
        }
    }

    // AFTER the hits (runde 220): genFlash() restarts a colour scene, and
    // since runde 219 a colour scene writes the strobe channel to 0 - the
    // scene started last in a tick wins, so a hit cut the burst on its own
    // beat: the drop's landing burst lands on the hit beats and never showed
    // at 0.7. Nothing in the hits reads the strobe state.
    // dropBar, not bar: the landing burst waits for the kick (see m_dropLand).
    // In a drop the only thing driveStrobe reads `bar` for IS the landing.
    driveStrobe(castSet, beat, energy, isDrop && dropSettled == false, isBuild, riserFollow ? buildProg : prog, isDrop ? dropBar : bar, beatInBar,
                isCalm || still || dropWaiting || m_flash || m_blackout
                || isBreak || isIntro || isOutro || exposureRest); // nobody strobes a break/intro/outro/rest (runde 235)

    checkConflicts(castSet);

    m_autoStageKeys.clear();
    bool impactActive = isDrop && dropBar >= 0 && dropBar < ((fader >= 0.56 && m_dropSize >= 0.40) ? 2 : 1);   // R374_SPREAD   // as impactBars (runde 360)
    if (m_fullAuto && m_blackout == false && m_flash == false && m_mixing == false
        && isCalm == false && still == false && m_override.isEmpty()
        && darkGroups.isEmpty() && hit == false && impactActive == false && turnaround == false)
        m_autoStageKeys = autoLookKeys(castSet, energy);

    QStringList moveNames;
    foreach (const QString &key, castSorted)
    {
        // (runde 332, headless ENERGY sweep: under STILL the move is not run
        // - mv = TrackMove() above - but the log still wrote the drawn one,
        // "pulse 55" at slider 0 in a drop, and tracklog_report counted it)
        QString mn = (isCalm || still) ? QString() : moveName(m_moves.value(key));
        if (m_active.contains("efx:" + key))
            mn = (mn.isEmpty() ? QString() : mn + " ") + sweepName(m_sweep.value(key));
        moveNames << (mn.isEmpty() ? key : QString("%1 %2").arg(key).arg(mn));
    }
    m_lastMoves = moveNames.join(" + ");

    // Stage time, the denominator rateWeight() divides by. Counted here and
    // not in logBeat(), which the operator can switch off - the arithmetic
    // must not quietly change meaning because somebody turned the log off.
    // Same filter as rate(): once per program however many slots it holds,
    // and never the engine's own generated scenes, which are rebuilt with
    // the table and would be counting something that does not persist.
    {
        int b = rateBucket();
        QSet<quint32> onceEach;
        foreach (quint32 fid, m_active.values())
        {
            if (onceEach.contains(fid))
                continue;
            onceEach.insert(fid);
            QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.find(fid);
            if (it != m_funcs.end() && it.value().generated == false)
                it.value().seen[b] += 1;
        }
    }

    // for the night report: what carried the accent, and what moved this
    // beat - so "did the colour change on a turn or on the clock" and "how
    // much white on the strobes" can be read off a log instead of guessed
    m_logAccent = accentColour.isEmpty() ? QString() : accentGroup + "=" + accentColour;

    /* ---- runde 356: the stabs. On 10-03/04 Tobias pressed BLACKOUT 1,291
     * times, 70 % of them short stabs (0.13-0.25 s) in breaks and builds, a
     * beat apart or on the eighths - and in a build the eighths grow toward
     * the drop (30 % of the stabs 16+ beats out, half of them in the last
     * eight). The engine stabs for him now: dark for the last slice of the
     * beat, so the light HITS on the next one.
     *   build (and the bars into a drop): from 60 % on the slider. A stab
     *     every beat over the last 8 beats at 60 %, the last 32 at 100 %;
     *     eighths over the last 4 from 80 %, the last 8 from 95 %. The last
     *     beat itself is black (dropBlack above). A build without a drop
     *     ahead stabs its last quarter from 75 %.
     *   break: a stab every beat from 90 % (the pump does 75-90 %).
     * Never under CALM, HOLD, FLASH, at ENERGY 0, in a mix, or while the
     * music itself is dark. ---- */
    bool chopNow16Riser = false;         // R362_RISER_16 (the log)
    {
        m_chopPlan.clear();
        int chop = 0;                    // 0 none, 1 quarters, 2 eighths
        const bool chopOk = musicDarkOk && isCalm == false && hold == false && m_blackout == false
                         && m_silenceDark == false && silenceLong == false && dropWaiting == false
                         && m_beatMs > 0.0;
        const int toDrop = (nextState == QStringLiteral("drop") && beatsToNext > 0) ? beatsToNext : -1;
        // a build, or the last two bars into a drop from any other section
        // (a groove straight into the drop) - not a whole groove (the harness:
        // Clean Bandit's normal section stabbed for 26 beats)
        if (chopOk && (isBuild || (toDrop > 0 && toDrop <= 8)) && isDrop == false && fader >= 0.60)
        {
            const qreal w = qBound(0.0, (fader - 0.60) / 0.40, 1.0);
            if (toDrop > 1)
            {
                // runde 359 (R359_RISER): where the riser is measured the stabs
                // wait for it - a stab every beat from 45 % of the climb,
                // eighths from 80 % - inside the same windows as before
                // R362_BUILD_SIZE: a small drop ahead, a shorter window
                const qreal ns = m_nextDropSize;
                if (toDrop <= qRound((8.0 + 24.0 * w) * (0.50 + 0.50 * ns)) && (riserFollow == false || measured >= 0.45))
                    chop = 1;
                int eighths = fader >= 0.95 ? 8 : fader >= 0.80 ? 4 : 0;
                if (ns < 0.40)
                    eighths = 0;
                else if (ns < 0.70)
                    eighths = qMin(eighths, 4);
                if (toDrop <= eighths && (riserFollow == false || measured >= 0.80))
                    chop = 2;
                // R362_RISER_16 (Tobias 10-05, "huggene foelger trommehvirvlen"):
                // the roll itself cannot be heard in rekordbox's waveform - in
                // 174 builds of the library the hits per sixteenth did not rise
                // into the drop, and the flutter rose only with the riser (r 0.76,
                // on its own in 4 of 102). What climbs with a roll is the riser:
                // where it is measured and at its top (95 %), the last bar into a
                // big drop stabs the sixteenths, from 90 % on the slider.
                // rekordbox' own fill-ins still stab the sixteenths (runde 357).
                if (toDrop <= 4 && fader >= 0.90 && riserFollow && measured >= 0.95 && ns >= 0.60)
                {
                    chop = 3;
                    chopNow16Riser = true;
                }
            }
            else if (toDrop < 0 && isBuild && fader >= 0.72 && prog >= 0.75)   // R374_SPREAD
                chop = 1;
        }
        else if (chopOk && isBreak && fader >= 0.90)
            chop = 1;
        // Runde 357 (Tobias: "se om det er muligt og goer det hvis det er"):
        // the stabs follow the drum roll where rekordbox has marked one. The
        // waveform cannot hear a snare roll - its own onset count stood at
        // 6.3-6.5 a beat in every section and did not rise before 163 drops -
        // but rekordbox' phrase analysis marks the fill-in at the end of a
        // phrase (264 of 1,425 phrases in the library, 2-4 beats). Inside one,
        // in a build or the bars into a drop: sixteenths.
        if (chopOk && isDrop == false && fader >= 0.64 && inFill(beat) && toDrop != 1   // R374_SPREAD
            && (isBuild || (toDrop > 0 && toDrop <= 8)))
            chop = 3;
        // R358_LOOP_CHOP: the DJ's loop sets the stab. Eight beats and more:
        // one stab on the loop's last beat, where it jumps back. Four: every
        // beat. Two: eighths. One: sixteenths - so a loop he halves on its way
        // (8, 4, 2, 1) stabs faster and faster with it. In a drop only the
        // one- and two-beat loops stutter. From 60 % on the slider.
        bool loopChop = false;
        // (runde 360: the stabs' own line keeps a mix out - a loop's stab in a
        // quiet mix is let through here)
        const bool loopMixOk = mixQuietIn && isCalm == false && hold == false && m_blackout == false
                            && m_flash == false && m_beatMs > 0.0
                            && still == false && dropWaiting == false && m_silenceDark == false;   // R361
        if ((chopOk || loopMixOk) && loopNow && fader >= 0.68 && toDrop != 1)   // R374_SPREAD
        {
            const int c = m_loopLen <= 1 ? 3 : m_loopLen == 2 ? 2
                        : (isDrop ? 0 : (m_loopLen == 4 ? 1 : (beat == m_loopFrom ? 1 : 0)));
            if (c > chop)
            {
                chop = c;
                loopChop = true;
            }
        }
        if (chop > 0)
        {
            const qint64 t0 = m_beatStartMs;
            const qreal bm = m_beatMs;
            const qint64 stab = qint64(chop == 3 ? qBound(35.0, 0.10 * bm, 55.0)
                                     : chop == 2 ? qBound(50.0, 0.18 * bm, 90.0)
                                                 : qBound(70.0, 0.25 * bm, 130.0));
            const qint64 end = t0 + qint64(bm);
            if (chop == 3)
            {
                for (int q = 1; q <= 3; q++)
                    m_chopPlan.append(qMakePair(t0 + qint64(bm * q / 4.0) - stab, t0 + qint64(bm * q / 4.0)));
            }
            if (chop == 2)
                m_chopPlan.append(qMakePair(t0 + qint64(bm / 2.0) - stab, t0 + qint64(bm / 2.0)));
            m_chopPlan.append(qMakePair(end - stab, end));
            if (m_musicDarkEvent.isEmpty())
                m_musicDarkEvent = (loopChop ? QStringLiteral("loop%1-").arg(m_loopLen) : QString())
                                 + (chop == 3 ? QStringLiteral("chop/16")
                                 : chop == 2 ? QStringLiteral("chop/8") : QStringLiteral("chop/4"));
        }
        slotChopTimer();                 // and the dark for this beat, now
    }
    // R358_REPEAT: what the first drop / build looked like, over its first
    // two bars (a group joining on beat three is part of the picture)
    if (m_lookPending.isEmpty() == false && m_lookPendingIdx >= 0
        && m_lookPendingIdx < m_lookMemory.value(m_lookPending).count()
        && beat >= m_lookFrom && beat < m_lookFrom + 8
        && ((m_lookPending == QStringLiteral("drop") && isDrop) || (m_lookPending == QStringLiteral("build") && isBuild)))
    {
        TrackLookMemory &mem = m_lookMemory[m_lookPending][m_lookPendingIdx];
        mem.colour = m_colour;
        mem.dropStyle = isDrop ? m_dropStyle : -1;
        mem.landCoin = m_landCoin;
        mem.motion = m_sectionMotion;
    }
    else if (m_lookPending.isEmpty() == false && beat >= m_lookFrom + 8)
        m_lookPending.clear();
    int impactBarsLog = (fader >= 0.56 && m_dropSize >= 0.40) ? 2 : 1;   // R374_SPREAD
    const bool fakeDrop = isDrop && m_dropLand > 0;
    {
        QStringList ev;
        if (sectionChanged) ev << "section";
        if (turn) ev << "turn";
        if (turnaround) ev << (haveCurves ? "music-fill" : "clock-fill");
        if (changeColour) ev << (turnUp ? "colour-on-turn" : (holdUp ? "colour-on-clock" : "colour"));
        if (m_mixGlide) ev << QString("mix-glide%1").arg(int(qRound(100.0 * m_mixGlideP)));   // runde 374
        if (m_layerStyle == 1) ev << "colour-fade";          // runde 370
        else if (m_layerStyle == 2) ev << "colour-chase";
        if (isBuild && state != QStringLiteral("build")) ev << "riser-build";
        if (m_hatsOut) ev << "hats-out";
        if (closing < 1.0) ev << "closing";
        if (m_kickGone >= 4 && isBreak == false) ev << "kick-gone";
        if (m_curveBreak) ev << "curve-break";          // runde 192
        if (m_curveGroove) ev << "curve-groove";
        if (dropHidden) ev << "drop-hidden";
        if (dropWaiting) ev << "drop-wait";
        if (m_floorRound) ev << "floor";    // the moves column cannot show it (runde 233)
        // runde 358
        if (djLoopOn()) ev << QString("loop%1x%2").arg(m_loopLen).arg(m_loopPasses);
        if (m_lookActive.isEmpty() == false && sectionChanged) ev << ("repeat-" + m_lookActive);
        if (m_halfTime) ev << "half-time";
        if (m_vocalNow) ev << "vocal";
        if (m_grooveSlots != 0) ev << QString("groove%1").arg(m_grooveSlots);
        if (m_backbeat) ev << "backbeat";
        if (riserFollow) ev << QString("riser%1").arg(int(qRound(100.0 * buildProg)));
        if (m_deckSpeed < 0.85) ev << "brake";
        // runde 359
        if (m_pumpNow >= 0.0) ev << QString("pump%1").arg(int(qRound(100.0 * m_pumpNow)));
        if (m_density > 0) ev << "busy";
        if (m_density < 0) ev << "sparse";
        if (riserBar >= 0.0) ev << QString("riser-ramp%1").arg(int(qRound(100.0 * riserBar)));
        // runde 360
        if (lateLand) ev << "late-land";
        if (dropArrives) ev << QString("drop-size%1").arg(int(qRound(100.0 * m_dropSize)));
        if (growNow) ev << "drop-grow";
        // runde 362
        if (miniNow) ev << QString("mini-land%1").arg(int(qRound(100.0 * m_miniSize)));   // R366
        if (sectionChanged && isBuild && m_nextDropBeat > beat)   // R364: a build in parts logs its drop from the first part
            ev << QString("build-for%1").arg(int(qRound(100.0 * m_nextDropSize)));
        if (chopNow16Riser) ev << "riser-16";
        if (growOff) ev << "drop-grow-off";      // runde 361
        if (settleNow) ev << "drop-settle";
        if (mixQuietIn && djLoopOn()) ev << "mix-loop";
        if (fakeDrop) ev << QString("drop-late@%1").arg(m_dropLand);
        if (isDrop && m_dropStyle > 0) ev << ("drop-" + dropStyleName(m_dropStyle));
        if (isDrop && dropBar >= 0 && dropBar < impactBarsLog) ev << "impact";
        if (mixTurnDue && m_nextColour.isEmpty() == false) ev << "mix-turn";
        if (m_fullAuto && m_rhythmLead.isEmpty() == false)
            ev << "lead=" + QString::fromLatin1(m_rhythmLead.toUtf8().toHex());
        if (exposureRest) ev << "exposure-rest";
        if (!m_sequenceGroups.isEmpty()) ev << "room-sequence";
        if (incomingFresh) ev << "incoming-profile";
        if (strobeChase) ev << "strobe-chase";
        if (m_mixing) ev << "mix-energy";
        if (m_keyBias >= 0) ev << (m_keyBias == 0 ? "key-minor" : "key-major");
        if (m_musicDarkEvent.isEmpty() == false) ev << m_musicDarkEvent;   // runde 356
        if (m_hardStart && sectionChanged) ev << "land-hard";
        if (kickBack && m_kickBackLast == beat) ev << "kick-back-hit";   // runde 357
        if (landBurstNow()) ev << "strobe-land";
        m_logEvent = ev.join('+');
    }
    logBeat(state, beat, level, energy, sectionEnergy);
    m_hardStart = false;                 // runde 356: only this tick's own starts
    m_report = QString("%1  |  %2%3  |  %4%5%6%7")
        .arg(moveNames.isEmpty() ? (silent ? tr("(silence)") : tr("(no groups)"))
                                 : moveNames.join(" + "))
        .arg(m_colour.isEmpty() ? tr("(no colour)") : m_colour)
        .arg(accentColour.isEmpty() ? QString() : QString(" + %1").arg(accentColour))
        .arg(state + (isDrop && m_dropStyle > 0 ? QString(" %1 %2").arg(QChar(0xb7)).arg(dropStyleName(m_dropStyle)) : QString())
             + (landing ? tr(" landing") : (turnaround ? tr(" turn") : QString())))
        .arg(preDrop ? tr("  (drop in %1)").arg(beatsToNext) : QString())
        .arg(isCalm ? tr("  CALM") : QString())
        .arg(QString(m_hold ? tr("  HOLD") : QString()) + (still ? tr("  STILL") : QString())
             + (m_blackout ? tr("  BLACKOUT") : QString()) + (m_mixing ? tr("  MIX") : QString())
             + (m_fullAuto ? tr("  FULL AUTO") : QString()));
    emit liveChanged();
}

/*********************************************************************
 * Generated motion
 *********************************************************************/

TrackMove TrackEngine::composeMove(const QString &group, TrackMove move, int tier) const
{
    if (tier == 0)
        return move; // the existing break's base and occasional fan are deliberate
    // what drawMove() chose, kept before this function flattens it - a strobe
    // group gets its walk back at the end (runde 154)
    const int drawnPattern = move.pattern;
    const int drawnPulseOn = move.pulseOn;
    const int drawnStepBeats = move.stepBeats;
    const int drawnPhase = move.phase;            // runde 339: the section's dice for the support's beat
    const int drawnSubSteps = move.subSteps;      // runde 338: the strobes' eighths
    const qreal drawnPulse = move.pulse;
    move.phase = 0;  // one rhythmic origin across the room
    if (group == m_rhythmLead)
        return move;
    move.flashBar = false;
    move.subSteps = 1;
    move.stepBeats = qMax(4, move.stepBeats);
    move.colourBars = 0;
    // Runde 233 (rig 25-26 Sep): the calm is the PACE - four beats a step, no
    // sub-steps, no hits - not a blank mask. Flattening every group but the
    // lead to STATIC left the wash and the Minis on a unison pulse 75 % of
    // the night (09-20: odd/even 47 %, halves 41 %, fill 11 %). A wide shape
    // drawMove chose now survives on the base, and the walks too on support.
    // patternMask keeps the floors (heads 0.35, the ambient base 0.45 outside
    // a drop). Sparkle stays the lead's.
    const bool wide = drawnPattern == ENGINE_PAT_ODDEVEN || drawnPattern == ENGINE_PAT_HALVES
                   || drawnPattern == ENGINE_PAT_FILL;
    if (group == m_compositionBase)
    {
        // heads only: the 0.45 floor is the ambient base's (ambientBase needs
        // heads); a base of two Minis in odd/even would stand at 0.15 (review)
        move.pattern = (wide && m_groups.value(group).heads) ? drawnPattern : ENGINE_PAT_STATIC;
        // runde 353: a drop at the top keeps the walk drawMove gave the base
        // (a block of heads, never one) and its pace - not four beats a step
        if (tier == 2 && m_faderNow >= 0.80 && m_groups.value(group).heads)
        {
            if (drawnPattern == ENGINE_PAT_CHASE || drawnPattern == ENGINE_PAT_PINGPONG)
                move.pattern = move.width >= 2 ? drawnPattern : ENGINE_PAT_HALVES;
            move.stepBeats = drawnStepBeats;
        }
        move.bare = false;
        return move; // keep the base's requested deep kick pulse and floor
    }
    const TrackGroup &g = m_groups.value(group);
    move.pattern = (wide || drawnPattern == ENGINE_PAT_CHASE || drawnPattern == ENGINE_PAT_PINGPONG)
                 ? drawnPattern : ENGINE_PAT_STATIC;
    // Runde 290 (Tobias: "det er vigtigt at alt skalerer efter energi-
    // slideren"): the support is still the calm part of the room, but not
    // equally calm at 40 % and at 100 %. It was four beats a step, one pulse
    // a bar and at most 0.30 deep at every fader - two or three of the four
    // or five groups on stage changed only in brightness from 50 % up. Now,
    // on the slider: the pulse deepens 0.25 -> 0.50 over 40-100 %, lands on
    // beats one and three from 75 %, and a chase may step every two beats
    // from 80 % (REGLER: support >= 2 beats a step). The bars keep four
    // (runde 288). (The animation laser's gate is capped the same way - review
    // 294: uncapped it opened wider than before at every fader.)
    const qreal sf = qBound(0.0, m_faderNow, 1.0);
    // RUNDE 339 (Tobias, 10-03: "ensformigt"): the support's beat was the same
    // in every section of the night - beats one and three from 75 %, the
    // downbeat under it, on every support group at once. Now each section's
    // draw (drawMove's phase, held for the section) picks where it lands: the
    // downbeat, one and three, the backbeat (two and four) from 75 %, and
    // every beat - shallow, the depth below is capped - from 85 %. Still the
    // calm part of the room; the lead keeps its own.
    {
        static const int under[] = { 3, 3, 1, 3 };
        static const int over[]  = { 1, 2, 1, 3, 2, 1, 0, 0 };
        const int k = qAbs(drawnPhase);
        move.pulseOn = sf >= 0.75 ? over[k % (sf >= 0.85 ? 8 : 6)] : under[k % 4];
    }
    move.bare = g.strobes;
    move.pulse = g.strobes ? 1.0 : qMin(0.25 + 0.25 * qBound(0.0, (sf - 0.40) / 0.60, 1.0), move.pulse);
    if (sf >= 0.80 && g.lasers == false)
        move.stepBeats = qMax(2, drawnStepBeats);
    // ... except that a STROBE group must never stand still (runde 154). This
    // is the composition rule making every group but the lead calm, and for
    // the strobes "calm" was a lit bank pulsing once a bar. They keep their
    // walk and their own beat; what they give up is the pace - two beats a
    // lamp rather than one, which is the slow chase Tobias asked for.
    if (g.strobes && move.bare)
    {
        move.pattern = drawnPattern;
        // and the pace drawMove drew, not the flat four beats this function
        // gives everyone else: four beats a lamp on six lamps is six bars to
        // cross the room, which reads as one lamp standing lit rather than as
        // a walk. drawMove already slows it down for a quiet room (four beats
        // a lamp while its `wild` is under 0.30, two under 0.65, one above -
        // wild runs from ENGINE_STROBE_ON to the top of the fader) and the
        // lamp blinks on every one of its beats while it is the lit one.
        move.stepBeats = drawnStepBeats;
        move.pulseOn = drawnPulseOn;
        // ... and their eighths, crisp, when drawMove drew them (runde 338)
        if (drawnSubSteps > 1)
        {
            move.subSteps = drawnSubSteps;
            move.pulse = drawnPulse;
        }
    }
    // A strobe group is never the rhythm lead (chooseLead leaves it out), so
    // this line used to be the second of three places that shut the door on
    // the show's strobe chases. drawMove has already made that call - a drop,
    // above half the fader, one section in four - and it is narrow enough to
    // survive the composition rule. Everything else here still applies: the
    // generated picture underneath stays static and slow.
    return move;
}

TrackMove TrackEngine::drawMove(const QString &group, int tier, bool build, qreal energy, bool isBase, qreal prog) const
{
    // The menu grows with the energy. Low: a static look, nothing else.
    // Middle: colour trades and a soft pulse. High: everything, fast.
    // The base group (the heads) never sparkles or goes dark in halves
    // for long - it is the light the room stands on.
    TrackMove mv;
    QRandomGenerator *rng = QRandomGenerator::global();
    auto pick = [rng](const QList<int> &opts) { return opts.at(int(rng->bounded(opts.count()))); };
    auto chance = [rng](qreal p) { return rng->bounded(1000) < int(qBound(0.0, p, 1.0) * 1000.0); };
    const TrackGroup &g = m_groups.value(group);
    qreal e = qBound(0.0, energy, 1.0);
    mv.phase = int(rng->bounded(8));

    // a pattern device has no intensity to pulse or chase: its own pattern
    // scenes are its movement, and it runs them most of the time
    if (g.patternDevice)
    {
        mv.ownChaser = tier > 0 && rng->bounded(10) < 8;
        // An animation laser drawing the same pattern for a whole section is
        // wallpaper. Its dimmer is on/off only, so pulseFactor gives it a
        // square gate instead of a fade and the depth below decides how much
        // of the beat it is ON for: about half at the bottom of the fader,
        // a fifth at the top - short, hard bursts when the room is going.
        // Every beat, always: the gate is re-triggered on the beat and shuts
        // itself. How LONG it is open is the energy's job - about half a beat
        // at the bottom of the fader, a fifth at the top - and how often the
        // two of them swap is stepBeats below. Choosing beats to skip would
        // just leave it dark, because a square gate cannot decay back.
        // ... but not in a break: there the fan stands still and steady, the
        // way it does on his own button - a gate chopping it on the beat is
        // a drop's idea of it
        mv.pulse = tier == 0 ? 0.0 : 0.35 + 0.55 * e;
        mv.pulseOn = 0;
        // and with two of them, they take the beat in turns rather than
        // firing together - the mask is on/off too, which suits them
        if (g.parts.count() >= 2 && chance(0.55))
        {
            mv.bare = true;
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_ODDEVEN, ENGINE_PAT_PINGPONG });
            mv.stepBeats = tier == 0 ? pick({ 2, 4 }) : pick({ 1, 2 });
            mv.ownChaser = tier > 0 && rng->bounded(10) < 5;
        }
        mv.breatheBars = 0;
        mv.texture = 0.0;
        return mv;
    }

    // The laser bars. Their movement is handled in drawSweep - none at all
    // below 40 % of the fader - so what is left here is how hard they
    // work: at the bottom they simply stand lit, and from there the fader
    // adds a chase down the row, then a blink on the beat, then both at once.
    // Nothing about this changes their aim.
    if (g.lasers && g.parts.count() >= 2)
    {
        qreal busy = qBound(0.0, (e - 0.12) / 0.78, 1.0);
        mv.breatheBars = 0;
        mv.texture = 0.0;
        mv.ownChaser = tier > 0 && chance(0.25 + 0.45 * busy);
        // Runde 287 (Tobias: "En rolig laser chase må gerne bruges helt ned
        // til bevægelserne starter og skaleres op med energi-slideren så
        // chase farten følger"): from the line where they start to move (0.40
        // on the slider), outside a break/intro/outro, a calm chase is at
        // least as likely as the still picture, and its pace is the SLIDER's -
        // eight beats a step at 40 %, four from 61, two from 87, one from 96.
        // Read at the draw (runde 288): every section, the 8-bar redraw and a
        // fader jump draw again. Written into a running move on the bar line
        // it made the chase jump across the row (step = beat / stepBeats).
        // R383_BARS_ALONE: alone in a break the bars chase at the slider's
        // pace - the groove's menu, not the break's still picture. R384: whole
        // bars, all eight eyes lit (no AUTO programme runs on them then)
        const bool alone = m_barsAloneNow && tier == 0;
        // R385_BARS_FULL: the full kind - every bar lit at once, steady
        if (alone && m_aloneFull)
        {
            mv.ownChaser = false;
            mv.pattern = ENGINE_PAT_STATIC;
            mv.stepBeats = 8;
            mv.subSteps = 1;
            mv.bare = false;
            mv.width = 1;
            mv.pulse = 0.0;
            mv.pulseOn = 3;
            mv.colourBars = 0;
            mv.flashBar = false;
            return mv;
        }
        const bool slider = (tier > 0 || alone) && m_faderNow >= 0.40;
        if (alone || chance(slider ? qMax(busy, 0.50) : busy))
        {
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG,
                                ENGINE_PAT_ODDEVEN, ENGINE_PAT_CHASE });
            // eight beats a step at the bottom, one at the top - and the
            // steps in between are really in between
            // on the bar grid - 1, 2, 4 or 8 beats (runde 233: 3, 5, 6 and 7
            // against a 4/4 bar and a pulse read as random)
            const qreal paceBy = slider ? qBound(0.0, (m_faderNow - 0.40) / 0.60, 1.0) : busy;
            const int sb = qMax(1, int(qRound(8.0 - 7.0 * paceBy)));
            // B22: the rule's own lines - "4 fra 61, 2 fra 87, 1 fra 96" - on
            // the slider's percent; the rounded ramp switched at 62 and 88
            const int pct = qRound(m_faderNow * 100.0);
            mv.stepBeats = slider ? (pct >= 96 ? 1 : (pct >= 87 ? 2 : (pct >= 61 ? 4 : 8)))
                                  : (sb <= 2 ? sb : (sb <= 5 ? 4 : 8));
            mv.bare = chance(0.35 + 0.45 * busy);
            // RUNDE 338 (Tobias, after 10-02: "laser-bars i de hoeje energier
            // brugte for meget 'et-oeje' chases"). The menu above is one bar at
            // a time three draws in four - CHASE twice and PINGPONG - and from
            // 80 % on the slider the bars stood on a sixth of their beams 47 %
            // of the night. From 55 % the fader takes the menu over: odd/even,
            // halves and a fresh random half (sparkle) join, and a chase or a
            // ping-pong walks a BLOCK of two or three bars. At 90 % and up a
            // single bar walking alone is rare.
            const qreal hot = qBound(0.0, (m_faderNow - 0.55) / 0.35, 1.0);
            if ((tier > 0 || alone) && hot > 0.0 && chance(0.35 + 0.60 * hot))
                mv.pattern = pick({ ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES, ENGINE_PAT_SPARKLE,
                                    ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG });
            if ((mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG)
                && g.parts.count() >= 4 && (tier > 0 || alone) && hot > 0.0 && chance(0.30 + 0.70 * hot))
                mv.width = g.parts.count() >= 6 ? pick({ 2, 3, 3 }) : 2;
        }
        else
        {
            mv.pattern = ENGINE_PAT_STATIC;
            mv.stepBeats = 8;
        }
        mv.subSteps = 1;                       // never between the beats: too twitchy
        // RUNDE 346 (Tobias, after 10-02: "laser-bars 'strobede' lidt for
        // meget, specielt paa de enkeltoejede chases ... hold de vilde strobs
        // til naar Energien er 100%"). The blink on the beat was 0.15 + 0.75 x
        // busy - 0.90 deep from about 90 % on the slider, every beat three
        // draws in four: one eye walking the row and blinking hard on each
        // beat IS a strobe. Now the depth tops out at 0.60 under 85 % and
        // only the last 15 % (the amok) takes it to 0.90; the every-beat
        // blink is the amok's too; a single bare eye walking alone blinks at
        // most 0.25 until the amok; and the bar hit is the amok's.
        // runde 348 (Tobias: "Laserbarene skal kun gaa 'amok' paa 100% energi
        // ikke fra 15% fra 100"): the bars' amok is the stop itself, not the
        // last 15 % the strobes have
        const qreal amok = m_faderNow >= ENGINE_BARS_AMOK ? 1.0 : 0.0;
        mv.pulse = busy < 0.15 ? 0.0 : 0.15 + 0.45 * busy + 0.30 * amok;
        // runde 352: a drop from 75 % blinks harder again - up to 0.80 just
        // under the stop (0.90 at it) - "drops ... vildere foer i de hoeje
        // energi niveauer". The single bare eye keeps its cap below.
        if (tier == 2 && amok < 1.0 && busy >= 0.15)
            mv.pulse += 0.20 * qBound(0.0, (m_faderNow - 0.75) / 0.25, 1.0);
        if (mv.bare && mv.width <= 1
            && (mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG))
            mv.pulse = qMin(mv.pulse, 0.25 + 0.65 * amok);
        mv.pulseOn = chance(busy * busy * (0.20 + 0.80 * amok)) ? 0 : (chance(busy) ? pick({ 1, 2 }) : 3);
        mv.colourBars = chance(0.5 * busy) ? pick({ 2, 4, 4 }) : 0;
        mv.flashBar = tier == 2 && chance(0.5 * busy * amok);
        return mv;
    }

    // A strobe is not a light source, it is a rhythm instrument. It belongs
    // ON the beat and dark between the beats - standing lit with a wobble on
    // top is what made them read as ugly floodlights. So: a deep pulse (full
    // on the beat, gone well before the next one), a still picture underneath
    // so nothing fights the blink, and the energy decides how often it lands.
    if (g.strobes)
    {
        // ALWAYS A WALK (runde 154). Tobias, 2026-09-22: "stroberne er kun
        // rytmiske ja, men de maa gerne lave langsomme chases ogsaa, altsaa
        // skift paa beat pr. lampe henover rummet, frem og tilbage, og puls op
        // og HELT ned. Det er bare vigtigt de aldrig staar statisk taendt paa
        // en farve uden at der 'sker noget'." Six of them, hung from the
        // ceiling two to three metres apart, and powerful: a lamp walking the
        // row is a sweep across the whole room, and a lit bank is a flat glare.
        //
        // What was here drew a still picture SIX TIMES IN TEN - the 40 % dice
        // below, and under 0.60 on the fader it was static every time. That is
        // the thing he is describing.
        // HOW WILD. Nought at the moment they come on, one at the top of the
        // slider - so every dial below is a ramp across the half of the fader
        // the strobes actually live in, not across the whole of it. A ramp
        // measured from 0 would have them nearly at full the instant they
        // appeared, which is the opposite of following the slider up.
        // (runde 267: on the FADER - the number they come on at, m_strobesPooled
        // - times the section's loudness. It was the scaled energy, x 0.80 in a
        // quiet section: there the strobes came on at 55 % and sat at nought
        // until the slider read 69, and at the top they only got to 0.56 - a
        // groove never walked a lamp a beat, a break never got its "two near
        // the top". A loud section (a drop) reads exactly as before.)
        const qreal fNow = qBound(0.0, m_faderNow, 1.0);
        const qreal loud = fNow > 0.001 ? qBound(0.0, e / fNow, 1.0) : 1.0;
        const qreal wild = qBound(0.0, (fNow - ENGINE_STROBE_ON) / (1.0 - ENGINE_STROBE_ON), 1.0) * loud;
        mv.bare = g.parts.count() >= 2;      // one lamp at a time, nothing between
        if (mv.bare)
        {
            // across the room, and back again: PINGPONG turns at the end of
            // the row, CHASE wraps round to the start
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG });
            // SIX BARS to cross the room when they have just come on - four
            // beats a lamp on six lamps. Tobias, 2026-09-22: "det er den
            // langsomme chase, og en af dem vi har brugt rigtigt meget i
            // busking. Det ser stilet ud." Then two beats, then a lamp a beat
            // at the top. Never faster than the beat: above it the hardware
            // shutter is what takes over (driveStrobe).
            // a groove walks on the beat now - two beats a lamp, one at the
            // top - so the slow beat-chase can be seen (runde 233; four beats
            // a lamp read as one blink a bar)
            // ... and a break walks slowly: four beats a lamp, two near the
            // top (runde 235)
            // (runde 290: drawn by chance along the slider, so the pace a room
            // gets on average climbs with every per cent - it was 2 beats flat
            // from 55 to 84 % in a groove. Whole beats still.)
            mv.stepBeats = (tier == 1 && build == false) ? (chance(qBound(0.0, (wild - 0.20) / 0.70, 1.0)) ? 1 : 2)
                         : tier == 0 ? (chance(qBound(0.0, (wild - 0.30) / 0.60, 1.0)) ? 2 : 4)
                         : (chance(qBound(0.0, wild / 0.50, 1.0))
                            ? (chance(qBound(0.0, (wild - 0.40) / 0.50, 1.0)) ? 1 : 2) : 4);
        }
        else
        {
            mv.pattern = ENGINE_PAT_STATIC;  // one lamp has no row to walk
            mv.stepBeats = 4;
        }
        // runde 313: the slider's pace holds the engine's own walk too - two
        // beats a lamp under 50 % (strobePaceFloor)
        if (fNow < 0.50)
            mv.stepBeats = qMax(2, mv.stepBeats);
        mv.subSteps = 1;
        mv.breatheBars = 0;
        mv.texture = 0.0;
        mv.colourBars = 0;
        // THE DOOR, ON THE LATCH (runde 153, Tobias' decision 2026-09-22).
        //
        // This was an unconditional false, and it is why the strobes have
        // never once run one of the show's programmes: measured on the night
        // of 2026-09-20 they were on stage for 5516 beats and got a `mot:`
        // exactly nought times, while 940 AUTO chases - a fifth of the whole
        // file - sat unreachable. The reason for the false is good and it
        // stands: a strobe is a rhythm instrument, and a chase running
        // underneath is what made them read as ugly floodlights.
        //
        // So this is deliberately the narrowest opening that is still worth
        // seeing: A DROP ONLY, one section in four, and only from wild 0.15 -
        // an energy of about 0.62, not half the fader as this said until
        // runde 239 (wild runs from ENGINE_STROBE_ON 0.55 to the top). The
        // ENERGY slider is its off switch - under that this never fires - and
        // a drop is where a coloured row across the strobes is a look rather
        // than a wobble. Everything else about them is unchanged.
        // Runde 257 (Tobias: "flere looks og chases på strobe-lysene"): the
        // latch opens a little wider now that there are looks built for it -
        // two drop sections in five, and one groove section in five for the
        // slow Groove Glide (four beats a step). Same fader line, and every
        // one of them is a support look: two beats a step or more, half the
        // lamps lit at least, the hardware strobe shut.
        // RUNDE 313 (Tobias: "det er de fleste af vores egne chases
        // allerede"): from the moment they are on, half the drops and a
        // third of the grooves; the pace is the slider's (strobePaceFloor).
        mv.ownChaser = fNow >= ENGINE_STROBE_ON && ((tier == 2 && chance(0.50))
                                                  || (tier == 1 && build == false && chance(0.35)));
        // ... and ALL the way down between the hits, at every energy. It was
        // 0.85 + 0.15 * e, so at the bottom of the fader the room sat at
        // fifteen per cent of a very bright lamp between the blinks: lit, on a
        // colour, with nothing happening. (The floor never quite reaches zero
        // - the fader and the bass scale the depth and both cap at 0.95, which
        // is there because a dimmer-as-switch fixture reads 1.0 as off.)
        mv.pulse = 1.0;
        // the downbeat while the room is quiet, the backbeat in between,
        // every beat once it is going. A build hands them over to the beat as
        // it runs out; a break gets the downbeat and nothing else.
        // ... and in a break (runde 235) no blink at all: the lit lamp stays
        // lit for its beats and hands on - a walk, not a hit
        if (tier == 0)
        {
            mv.pulseOn = 3;
            mv.pulse = 0.0;
        }
        else if (build)
            mv.pulseOn = prog > 0.60 ? 0 : 1;
        else
        {
            // A ramp, drawn: the downbeat nearly every time just after they
            // come on, every beat nearly every time at the top, and the middle
            // really is the middle - which a pair of thresholds could never
            // be. Measured from ENGINE_STROBE_ON, not from nought: the old
            // span started at 0.20, so by the time they were allowed on stage
            // they were already most of the way to every beat.
            mv.pulseOn = (tier == 1 || chance(wild * wild)) ? 0
                       : (chance(wild) ? pick({ 1, 2 }) : 3);
        }
        // RUNDE 338 (Tobias, after 10-02: "Strobe-lamperne var gode, men jeg
        // synes slet ikke der var nogle hurtige strob (igen ingen variation)").
        // Measured that night: the walk was one lamp, chase or ping-pong, a
        // beat a lamp, 90 % of the strobes' time on stage. Now, once the room
        // is going (wild from 0.25 in a drop, 0.45 in a groove), the walk can
        // be odd/even - every other lamp, the halves trading - or a fresh
        // random half each step (sparkle, never all six), and a chase or a
        // ping-pong can walk a pair. Never the whole bank: that is the hit
        // that went (genFlash, above).
        if (mv.bare && tier > 0 && build == false && g.parts.count() >= 4)
        {
            const qreal open = tier == 2 ? qBound(0.0, (wild - 0.25) / 0.55, 1.0)
                                         : qBound(0.0, (wild - 0.45) / 0.55, 1.0);
            if (open > 0.0 && chance(0.25 + 0.45 * open))
                mv.pattern = pick({ ENGINE_PAT_ODDEVEN, ENGINE_PAT_SPARKLE, ENGINE_PAT_CHASE,
                                    ENGINE_PAT_PINGPONG, ENGINE_PAT_ODDEVEN });
            if ((mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG)
                && open > 0.0 && chance(0.35 * open))
                mv.width = 2;
            // FAST: in a drop from 75 % on the slider the walk can run on the
            // eighths - a lamp every half beat across the room. Crisp: the
            // mask is the blink (no pulse - the pulse only re-triggers on the
            // beat, so its second half would be dark).
            if (tier == 2 && fNow >= 0.75
                && (mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG
                    || mv.pattern == ENGINE_PAT_ODDEVEN)
                && chance(0.20 + 0.40 * qBound(0.0, (fNow - 0.75) / 0.25, 1.0)))
            {
                mv.stepBeats = 1;
                mv.subSteps = 2;
                mv.pulse = 0.0;
                mv.pulseOn = 0;
            }
        }
        // the bar flash was a hit - and a hit was the whole bank at full
        // (runde 338): gone from the strobes
        mv.flashBar = false;
        // (the dimmer pulse cannot go faster than the beat - the sub-beat
        // timer only re-masks a pattern, it never re-triggers the pulse. Above
        // the beat it is driveStrobe's hardware strobe that takes over.)
        return mv;
    }

    if (tier == 0)
    {
        // A break is quiet, not frozen. The base breathes over two to six
        // bars, and roughly half the time something also moves - slowly:
        // halves or odd/even trading every second or fourth bar, never a
        // chase and never a step shorter than four beats.
        // at a low fader a break barely moves; at a high one it keeps trading
        // A break is slow at every energy - that never changes - but how much
        // of the rig takes part does: almost nothing at the bottom, most of
        // it near the top.
        bool stir = chance(0.10 + 0.80 * e);
        if (isBase && e > 0.45 && rng->bounded(3) == 0)
        {
            mv.pattern = ENGINE_PAT_HALVES;
            mv.stepBeats = 8;
        }
        else if (stir && g.parts.count() >= 2)
        {
            mv.pattern = pick({ ENGINE_PAT_HALVES, ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES });
            mv.stepBeats = pick({ 8, 16, 16 });
            mv.breatheBars = pick({ 4, 6 });
        }
        else if (e > 0.2)
            mv.breatheBars = pick({ 2, 4, 4 });
        // a heartbeat on the beat - shallow at the bottom of the fader, a
        // real pulse near the top. A break is quiet, not dead.
        if (chance(0.45 + 0.50 * e))     // a break has a heartbeat far more often now
            mv.pulse = 0.15 + 0.60 * e;      // a heartbeat, from a hint to a real one
        mv.texture = 0.15;
        mv.ownChaser = false;            // a break moves on the engine's figure

        // Nearly half the time a break is ONE lamp at a time on the beat and
        // nothing else at all - no picture behind it, no floor to fall back
        // to. The quietest thing the rig can do that is still on the music,
        // and the heads slowly hand the beat to each other down the row.
        // NEVER the base. "bare" means nothing is lit between the blinks, so
        // on the group the room stands on it is a blackout with a lamp
        // walking through it - and since a break is the base alone (round
        // 30), that is the room going out. It happened on 15-75 % of breaks
        // depending on the fader, which is exactly the "lyset slukker helt i
        // breaks" report (Tobias, 2026-09-15: it is the moving heads = the
        // base). The drop path twenty lines down has carried this same guard
        // all along; the break path never got it.
        // Runde 235: a wash group beside the base (the Minis with the heads)
        // is in EVERY break now, so it gets the break's calm and nothing
        // busier: no one-lamp blink on the beat and no heartbeat - it trades
        // and breathes with the base. The blink above was for a break that
        // was the base alone plus, rarely, one extra - the one-lamp blink
        // that stood here, and the heartbeat just above, are gone for it.
        if (isBase == false)
        {
            mv.pulse = 0.0;
            if (mv.breatheBars == 0 && e > 0.2)
                mv.breatheBars = 4;
        }
        // RUNDE 342 - THE PUMP AT THE TOP. On 10-02 Tobias pressed BLACKOUT
        // 970 times, short (under half a second) and a beat apart, most of
        // them just BEFORE the beat - 452 of them in breaks with the slider at
        // 100 %. He was making the room hit on the beat where this file keeps
        // a break calm. From 90 % on the slider the base's heartbeat in a
        // break is a real pump instead: every beat, 0.45 deep at 90 % and 0.70
        // at the top - the floor stays at 30 % or more, so the room never goes
        // out (the rule for breaks), it only breathes with the kick.
        // Runde 356 (Tobias: "Byg det hele"): from 75 %, not 90 - 10-03/04 he
        // still tapped BLACKOUT through the breaks, most of it at 70-90 %.
        // 0.30 deep at 75 %, 0.54 at 90 %, 0.70 at the top: the floor is
        // still 30 % or more. From 90 % the stabs (planMusicDark) cut on top.
        if (isBase && m_faderNow >= 0.75)
        {
            mv.pulse = qMax(mv.pulse, 0.30 + 0.40 * qBound(0.0, (m_faderNow - 0.75) / 0.25, 1.0));
            mv.pulseOn = 0;
        }
        if (g.parts.count() < 2)
            mv.pattern = ENGINE_PAT_STATIC;
        return mv;
    }

    if (build)
    {
        // the fill grows with the build; the pulse comes in on the offbeats
        mv.pattern = ENGINE_PAT_FILL;
        // the pulse arrives with the build rather than waiting for the energy
        mv.pulse = (0.20 + 0.35 * prog) * (0.4 + 0.6 * e);
        mv.pulseOn = pick({ 0, 0, 2 });
        // A chase of the operator's switches the generated figure OFF - and a
        // build was drawing one half the time, so half of all builds stood
        // completely still. The engine's own figure is what a build needs.
        // ... except a group that has build programmes of its own ("...
        // Climb", runde 227): those ARE a build figure, so it takes one half
        // the time, as the 50/50 rule asks (runde 230). Not the base.
        mv.ownChaser = (isBase == false && m_climbGroups.contains(group))
                     ? rng->bounded(2) == 0 : rng->bounded(5) == 0;
        if (isBase)
        {
            // the base carries the build: a fill that grows across the heads,
            // stepping faster as the section runs out. It used to be pinned
            // to STATIC, which left the build as a slow brightness ramp on a
            // still picture - and the base is usually the only group lit.
            mv.pattern = ENGINE_PAT_FILL;
            mv.stepBeats = prog > 0.6 ? 1 : 2;
            mv.subSteps = 1;
            mv.pulse = qMin(mv.pulse, 0.45);
            // runde 339: the build's drawn shape (m_buildStyle). All of them
            // wide - the base never goes bare, and never below its floor.
            if (m_buildStyle == 1)
                mv.pattern = ENGINE_PAT_ODDEVEN;          // the roll: halves of the row trade
            else if (m_buildStyle == 2)
            {
                mv.pattern = ENGINE_PAT_STATIC;           // the swell: the whole base breathes
                mv.pulse = qMin(0.60, 0.20 + 0.45 * prog);
                mv.pulseOn = 0;
            }
            else if (m_buildStyle == 3)
                mv.pattern = ENGINE_PAT_HALVES;           // left against right
        }

        // The build's own shape: the same bare blink as the break, handed
        // round faster and faster the closer the drop gets. Nothing lit in
        // between, so the acceleration is the only thing in the room and you
        // cannot miss where it is going.
        // gated, or every build in the set is the same one: past the middle
        // it is nearly always this, early on it is often the fill above
        if (isBase == false && g.parts.count() >= 2 && chance(0.35 + 0.55 * prog))
        {
            mv.bare = true;              // not the base: see the break branch
            mv.ownChaser = false;        // a chase of theirs would swallow the pattern
            // runde 339: the blink's figure follows the build's shape - one
            // lamp handed round (fill, swell), the halves of the row (roll),
            // or a pair walking (halves); the acceleration is the same
            mv.pattern = m_buildStyle == 1 && g.parts.count() >= 4 ? ENGINE_PAT_ODDEVEN : ENGINE_PAT_CHASE;
            if (m_buildStyle == 3 && g.parts.count() >= 4)
                mv.width = 2;
            mv.pulse = 1.0;
            mv.breatheBars = 0;
            mv.texture = 0.0;
            // How far the acceleration gets by the end of the build is the
            // fader's decision: at the bottom it reaches every beat, halfway
            // up it reaches eighths, at the top it reaches sixteenths. The
            // SHAPE is always the same - it climbs - so a build is a build
            // at any energy, it is just a bigger one when the room is up.
            qreal reach = 1.0 + 3.0 * e;             // 1 .. 4 sub-steps at the end
            qreal into = qBound(0.0, prog, 1.0);
            mv.stepBeats = into < 0.30 ? 2 : 1;
            qreal sub = 1.0 + (reach - 1.0) * qBound(0.0, (into - 0.30) / 0.70, 1.0);
            mv.subSteps = sub >= 3.0 ? 4 : (sub >= 1.6 ? 2 : 1);
            mv.pulseOn = into < 0.20 && e < 0.5 ? 1 : 0;
        }
        return mv;
    }

    // the user's own chases and EFX: the base (heads) sweeps most of the
    // time, effects trade between their chases and the generated patterns
    // THIS is the door to every chase in the show. If it is shut the group
    // runs the engine's own generated dimmer pattern instead - a handful of
    // shapes - and none of the 1700 programmes in the file are reachable.
    //
    // It was shut most of the time: an effect group in a groove opened it
    // 300 * ramp(e, 0.25, 0.75) / 1000 of the time - 15 % at half a fader,
    // 30 % at the top, and NOTHING below a quarter. So the night ran on the
    // generated pattern and the programmes were never seen. Tobias, 2026-09-15:
    // "det er som om den aldrig bruger alle de forskellige programmer paa de
    // forskellige grupper ... Kan slet ikke forstaa hvis der skulle vaere saa
    // mange forskellige som du siger der er." He was right, and the count was
    // not the problem - this line was.
    // HALF THE NIGHT EACH (runde 156, Tobias' decision 2026-09-22). Two
    // libraries were built for full auto: the 4867 AUTO chases in the show
    // file, and the handful of shapes this file computes live. Measured on
    // the night of 2026-09-20, the live shapes won NINE SECTIONS IN TEN
    // wherever the fader was up - the wash had one of the AUTO chases in 11 %
    // of its drops and 15 % of its grooves, the Mini in 6 % and 18 %, while a
    // BREAK was 100 % because a break never reaches the branches below. That
    // is not a split, it is one library with the other kept as a spare.
    //
    // "hvis det er nogle programmer du har bygget til motoren, skal de jo
    // bruges 50/50? ogsaa hele aftenen vel?" - so: about half, at every
    // energy. The floor under the groove ramp is what makes it hold at the
    // quiet end too; the two kills further down are what used to eat it.
    if (isBase)
        mv.ownChaser = rng->bounded(100) < 60;
    else if (tier == 2)
        mv.ownChaser = rng->bounded(100) < 65;
    else
        mv.ownChaser = rng->bounded(1000) < int(650.0 * qBound(0.70, (e - 0.05) / 0.5, 1.0));

    // A linear slider deserves a linear engine: nothing below switches at a
    // threshold. Every chance and depth is a ramp of the energy, so 55 % and
    // 65 % look different, and 100 % is everything at once.
    auto ramp = [](qreal x, qreal from, qreal to) { return qBound(0.0, (x - from) / (to - from), 1.0); };

    if (tier == 1)
    {
        // groove: a static look at the bottom, patterns and a pulse growing in
        qreal live = ramp(e, 0.12, 0.70);            // 0 = still, 1 = full groove
        if (chance(live))
        {
            qreal quick = ramp(e, 0.55, 0.95);       // chases and short steps
            QList<int> menu = { ENGINE_PAT_STATIC, ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES };
            if (chance(quick))
                menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG;
            mv.pattern = pick(menu);
            mv.stepBeats = chance(quick) ? pick({ 2, 4 }) : pick({ 4, 8 });
            if (mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG)
                mv.stepBeats = qMin(mv.stepBeats, 4);   // these go to hard black
        }
        else if (isBase)
            mv.breatheBars = 4;                      // still, but alive
        // 0..45 % -> 15..70 %: the kick reads from a quarter of the fader
        // instead of only near the top. The SHAPE is unchanged (full on the
        // beat, down to 1 - depth a quarter beat later); this is the depth.
        mv.pulse = 0.15 + 0.55 * ramp(e, 0.08, 1.00) * (0.6 + 0.4 * rng->bounded(1000) / 1000.0);
        if (mv.pulse < 0.06)
            mv.pulse = 0.0;
        mv.pulseOn = chance(ramp(e, 0.30, 1.00)) ? pick({ 0, 1 }) : pick({ 1, 3 });
        if (chance(0.5 * ramp(e, 0.25, 1.00)))
            mv.colourBars = pick({ 2, 4, 4, 8 });
    }
    else
    {
        // drop: always moving; how fast, how deep, how wild follows the
        // energy - and the drop's character leans the menu
        qreal wild = ramp(e, ENGINE_DROP_SHOW, 1.00);   // a drop exists from 30 % (dropHidden); wild grows from there
        QList<int> menu = { ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES, ENGINE_PAT_STATIC };
        if (chance(ramp(e, ENGINE_DROP_SHOW, 0.70)))
            menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG;
        if (chance(ramp(e, 0.35, 1.00)))
            menu << ENGINE_PAT_SPARKLE << ENGINE_PAT_CHASE;
        if (m_dropStyle == 1)       menu << ENGINE_PAT_SPARKLE << ENGINE_PAT_SPARKLE << ENGINE_PAT_ODDEVEN;
        else if (m_dropStyle == 2)  menu << ENGINE_PAT_STATIC << ENGINE_PAT_HALVES << ENGINE_PAT_STATIC;
        else if (m_dropStyle == 3)  menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG << ENGINE_PAT_CHASE;
        else if (m_dropStyle == 4)  menu << ENGINE_PAT_STATIC << ENGINE_PAT_HALVES << ENGINE_PAT_STATIC << ENGINE_PAT_STATIC;
        else if (m_dropStyle == 5)  menu << ENGINE_PAT_CHASE << ENGINE_PAT_SPARKLE << ENGINE_PAT_PINGPONG << ENGINE_PAT_ODDEVEN;
        mv.pattern = pick(menu);
        mv.stepBeats = chance(wild) ? pick({ 1, 1, 2 }) : pick({ 2, 4 });
        if (m_dropStyle == 2)
            mv.stepBeats = qMax(mv.stepBeats, 2);
        if (m_dropStyle == 4)
            mv.stepBeats = qMax(mv.stepBeats, 2);
        if (m_dropStyle == 3 || m_dropStyle == 5)
            mv.stepBeats = 1;
        // eighths and sixteenths: the fast patterns run between the beats
        // when it is hot - what makes a drop roll instead of tick
        bool fast = mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG
                 || mv.pattern == ENGINE_PAT_ODDEVEN || mv.pattern == ENGINE_PAT_SPARKLE;
        if (fast && mv.stepBeats == 1 && m_dropStyle != 4
            && chance((m_dropStyle == 2 ? 0.2 : (m_dropStyle == 5 ? 0.9 : 0.6)) * ramp(e, 0.45, 0.95)))
            mv.subSteps = m_dropStyle == 1 ? pick({ 2, 4, 4 }) : pick({ 2, 2, 4 });
        mv.pulse = 0.35 + 0.45 * wild * (0.7 + 0.3 * rng->bounded(1000) / 1000.0);
        if (m_dropStyle == 2)
            mv.pulse *= 0.6;
        if (m_dropStyle == 4)
            mv.pulse = qMin(0.95, mv.pulse * 1.35);       // heavy: the room pumps deep
        // nervous: no pulse, all chase - but NOT on the base. The moving
        // heads pulse with the kick like the 4-eyes do (Tobias, 2026-09-15),
        // and a nervous drop would otherwise leave the one group the room
        // stands on sitting flat and bright for thirty-two bars.
        if (m_dropStyle == 5 && isBase == false)
            mv.pulse = 0.0;
        mv.pulseOn = chance(0.7) ? 0 : pick({ 1, 2 });
        if (m_dropStyle == 4)
            mv.colourBars = 8;                             // heavy: the colour stays
        else if (m_dropStyle == 5)
            mv.colourBars = pick({ 1, 2 });                // nervous: quick trades
        else if (chance((m_dropStyle == 2 ? 0.8 : 0.5) * ramp(e, 0.30, 0.90)))
            mv.colourBars = pick({ 1, 2, 4 });
        mv.flashBar = chance((m_dropStyle == 1 ? 0.7 : (m_dropStyle == 5 ? 0.6 : (m_dropStyle == 4 ? 0.15 : 0.35))) * ramp(e, ENGINE_DROP_SHOW, 1.00));
    }

    // texture: the groove and the break spread the lit fixtures a little
    mv.texture = tier == 1 ? 0.25 : (tier == 0 ? 0.15 : 0.0);

    if (isBase)
    {
        // the heads: slow trades only, the palette colour - and, since
        // 2026-09-15, the same pulse the mini 4-eyes get (see below)
        if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_ODDEVEN && mv.pattern != ENGINE_PAT_HALVES)
            mv.pattern = ENGINE_PAT_HALVES;
        mv.stepBeats = qMax(mv.stepBeats, tier == 2 ? 2 : 4);
        mv.subSteps = 1;
        // RUNDE 353 (the night of 10-03, read from the log): NEXT was pressed
        // 129 times, 62 of them in drops and 42 at 100 % - and the look most
        // often skipped was the wash standing STILL with a pulse (59), then
        // its halves and odd/even at four beats a step (43). The base in a
        // drop at the top of the slider moves now: never still from 80 %,
        // a block of three heads walking or bouncing the row from 85 %, two
        // beats a step from 80 % and one from 95 %. It is still never bare
        // (the heads keep their floor between steps, patternMask) and still
        // pulses with the kick.
        if (tier == 2 && m_faderNow >= 0.80)
        {
            if (mv.pattern == ENGINE_PAT_STATIC)
                mv.pattern = pick({ ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES });
            if (m_faderNow >= 0.85 && g.parts.count() >= 4 && chance(0.25 + 0.50 * qBound(0.0, (m_faderNow - 0.85) / 0.15, 1.0)))
            {
                mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG });
                mv.width = g.parts.count() >= 6 ? 3 : 2;
            }
            mv.stepBeats = m_faderNow >= 0.95 ? 1 : 2;
        }
        // NO ceiling on the pulse depth (Tobias, 2026-09-15: "den samme type
        // pulse FULL AUTO laver paa 4eyes lamperne er den samme type du skal
        // lave paa movingheads"). That cap - 0.30, then 0.45 - was the ONLY
        // thing that made the heads breathe where the mini 4-eyes punch:
        // every other part of the pulse is already shared. Same depth per
        // tier now (groove 15-70 %, drop 35-80 %, break 15-75 %), the same
        // fall-off (pulseFactor: full on the beat, down to 1 - depth a
        // quarter beat later), and the same choice of which beats it lands on
        // (pulseOn). The heads hit with the kick like the 4-eyes do.
        //
        // What the base still does NOT do is go BARE - nothing lit between
        // the blinks. That is a different thing from a deep pulse: a pulse
        // leaves a floor of (1 - depth), bare leaves nothing at all, and bare
        // on the base is what made breaks go dark (round 38).
        mv.colourBars = 0;
        mv.flashBar = false;

        // THE BACKGROUND BETWEEN THE HITS. A pulse only falls to (1 - depth),
        // and on the base that floor is exactly the "baggrundslys" Tobias saw
        // (2026-09-16): the heads pump, but from a lit room rather than from
        // the dark the 4-eyes punch out of. The 4-eyes get there by going
        // BARE - nothing lit between the blinks - and the base may never do
        // that (round 38: bare on the base is what made breaks go dark). The
        // honest version for the base is to take the floor down instead, and
        // to let the energy decide how far: at a quarter of the fader the
        // heads keep 60 % between hits - a room still being eaten in, where
        // a blinking wash would be wrong - and at the top they keep 8 %, as
        // close to the 4-eyes' punch as the base is allowed to get.
        //
        // Never in a break: there the base is often the only thing lit, and
        // the room does not blink. The build has its own ramp and returned
        // long before this line.
        if (tier != 0)
        {
            qreal floorWanted = 0.60 - 0.52 * qBound(0.0, (e - 0.25) / 0.70, 1.0);
            if (m_dropStyle == 2)
                floorWanted = qMax(floorWanted, 0.45);   // a wide drop keeps its wash
            if (m_dropStyle == 4)
                floorWanted = qMin(floorWanted, 0.25);   // a heavy one pumps deeper
            mv.pulse = qMax(mv.pulse, 1.0 - floorWanted);
            // A deep pulse that lands on every OTHER beat leaves the room at
            // the floor for a beat and a half at a time, which at 8 % reads as
            // the light having failed rather than as a groove. Deep means
            // every beat.
            if (mv.pulse > 0.60)
                mv.pulseOn = 0;
        }
    }
    // A drop may drop the backdrop too: a random handful of lamps hits each
    // beat and there is nothing lit in between. On the heads that reads as
    // the room being punched rather than washed, and it is the one place
    // where SPARKLE - a fresh random set every step - belongs.
    // never on the base: that group is the light the room stands on, and the
    // block just above spends fifteen lines saying so
    // (runde 156: this used to fire 0.10 + 0.55 * e - six drops in ten at the
    // top - and every one of them took an AUTO chase off the group. Sparkle is
    // a punch, not the drop's normal state.)
    if (tier == 2 && isBase == false && g.parts.count() >= 2 && chance(0.08 + 0.15 * e))
    {
        mv.bare = true;
        mv.ownChaser = false;            // or the pattern never reaches the rig
        mv.pattern = ENGINE_PAT_SPARKLE;
        mv.stepBeats = 1;
        mv.subSteps = e > 0.80 ? 2 : 1;
        mv.pulse = 1.0;
        mv.pulseOn = 0;
        mv.breatheBars = 0;
        mv.texture = 0.0;
    }

    // ---------------------------------------------------------------- the top
    // From three-quarters of the fader upwards the engine stops holding back:
    // faster patterns, colour trading every bar, a deeper pulse and more
    // accents, climbing all the way to the stop. Movement is deliberately
    // left out of this - the heads and the bars stay slow whatever the fader
    // says, because that is the one thing this room does not want.
    // Starts at 45 % and climbs all the way to the stop, so the whole top
    // half of the fader is a slide and not a switch.
    qreal fest = qBound(0.0, (e - 0.45) / 0.55, 1.0);
    if (fest > 0.0)
    {
        // runde 156: this was 0.55 + 0.40 * fest - up to 95 % of sections at
        // the top - and it is the single line that emptied the AUTO library
        // above 45 % on the fader. It still has to exist (see below), but a
        // quarter to two fifths is enough to make the top of the fader visible
        // without taking the other library away.
        if (chance(0.10 + 0.15 * fest))
        {
            // a generated pattern only reaches the rig when the group is not
            // already running one of the AUTO chases - and the base asks for
            // one most of the time, so without this the top of the fader
            // changed nothing at all on the heads
            mv.ownChaser = false;
            mv.pattern = isBase
                       ? pick({ ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES, ENGINE_PAT_ODDEVEN })
                       : pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG,
                                ENGINE_PAT_SPARKLE, ENGINE_PAT_ODDEVEN });
            mv.stepBeats = isBase ? 2 : 1;
            mv.subSteps = isBase ? 1 : (chance(fest) ? pick({ 2, 4, 4 }) : 2);
        }
        // the base is still the light the room stands on: it joins in, but it
        // does not sparkle, it does not trade colour every bar and it never
        // runs in sixteenths
        if (isBase == false && chance(fest))
            mv.colourBars = pick({ 1, 1, 2 });
        // qMax, so a bare look keeps its floor at nothing: the top of the
        // fader may deepen a pulse, never fill the dark back in
        mv.pulse = qMax(mv.pulse, isBase ? 0.30 + 0.15 * fest : 0.35 + 0.40 * fest);
        if (isBase == false)
            mv.flashBar = mv.flashBar || chance(0.50 * fest);
        mv.breatheBars = 0;                              // nobody breathes at the top
        if (isBase == false && chance(0.5 * fest))
            mv.texture = 0.0;                            // they all hit together
    }

    if (g.parts.count() < 2)
        mv.pattern = ENGINE_PAT_STATIC;                  // nothing to run across
    return mv;
}

void TrackEngine::applyMove(const QString &group, qreal level, int beat, int secStart, qreal prog,
                            const TrackMove &move, bool patterned)
{
    const TrackGroup &g = m_groups.value(group);
    int n = g.parts.count();
    if (n == 0)
        return;

    m_moveLevel.insert(group, level);
    // the step on the beat; the sub-beat steps come from the pulse timer
    int step = move.subSteps > 1 ? (beat - secStart) * move.subSteps + move.phase
                                 : (beat - secStart) / qMax(1, move.stepBeats) + move.phase;
    TrackMove eff = move;
    if (patterned == false)
        eff.pattern = ENGINE_PAT_STATIC;
    QVector<qreal> mask = patternMask(group, eff, step, prog);
    for (int i = 0; i < n; i++)
        setPart(group, i, level * mask.at(i));
}

QVector<qreal> TrackEngine::patternMask(const QString &group, const TrackMove &move, int step, qreal prog) const
{
    const TrackGroup &g = m_groups.value(group);
    int n = g.parts.count();
    int pattern = move.pattern;
    // the unlit fixtures of a pattern: dark on effects, dim on the base,
    // which must never look switched off
    // A bare look means BARE: nothing at all behind the lamp that has the
    // beat. Everything else keeps its floor - the heads must never read as
    // switched off in an ordinary section.
    qreal dim = move.bare ? 0.0
              : (g.heads ? 0.35 : (pattern == ENGINE_PAT_CHASE || pattern == ENGINE_PAT_PINGPONG ? 0.0 : 0.15));

    QVector<qreal> mask(n, 1.0);
    if (n == 0)
        return mask;
    switch (pattern)
    {
        case ENGINE_PAT_CHASE:
        {
            // runde 338: a block of `width` neighbours walks the row (wrapping),
            // so at the top of the fader the bars are a wide comet, not one bar
            const int w = qBound(1, move.width, qMax(1, n - 1));
            const int lit = ((step % n) + n) % n;           // the head of the block (a scrub back can make step negative)
            for (int i = 0; i < n; i++)
            {
                const int behind = (lit - i + n) % n;       // 0 = the head, 1 = just behind it
                mask[i] = behind < w ? 1.0 : (behind == w && move.bare == false ? 0.3 : dim);
            }
        }
        break;
        case ENGINE_PAT_PINGPONG:
        {
            // the block bounces between the two ends (runde 338: `width` wide)
            const int w = qBound(1, move.width, qMax(1, n - 1));
            const int span = n - w + 1;                     // positions the block can stand on
            int period = qMax(1, 2 * span - 2);
            int idx = span > 1 ? ((step % period) + period) % period : 0;
            if (idx >= span)
                idx = period - idx;
            for (int i = 0; i < n; i++)
                mask[i] = (i >= idx && i < idx + w) ? 1.0 : dim;
        }
        break;
        case ENGINE_PAT_ODDEVEN:
            for (int i = 0; i < n; i++)
                mask[i] = (i % 2) == (step % 2) ? 1.0 : dim;
        break;
        case ENGINE_PAT_HALVES:
            for (int i = 0; i < n; i++)
                mask[i] = ((i < n / 2) == ((step % 2) == 0)) ? 1.0 : dim;
        break;
        case ENGINE_PAT_SPARKLE:
        {
            // a fresh random half of the group each step, the same for the
            // whole step, never all dark
            QRandomGenerator local(quint32(step * 2654435761u) ^ quint32(qHash(group)));
            bool any = false;
            for (int i = 0; i < n; i++)
            {
                bool on = local.bounded(2) == 0;
                mask[i] = on ? 1.0 : dim;
                any = any || on;
            }
            if (any == false)
                mask[int(local.bounded(n))] = 1.0;
            // ... and a BARE one (the strobes, the bars blinking) never more
            // than half: five of six strobes at once is the bank flash that
            // went in runde 338
            if (move.bare)
            {
                int on = 0;
                for (int i = 0; i < n; i++)
                {
                    if (mask[i] >= 1.0 && ++on > qMax(1, n / 2))
                        mask[i] = dim;
                }
            }
        }
        break;
        case ENGINE_PAT_FILL:
        {
            // the build fills the group from one end; the drop gets it whole
            int lit = 1 + int(qRound(prog * (n - 1)));
            bool fromLeft = (move.phase % 2) == 0;
            for (int i = 0; i < n; i++)
            {
                int pos = fromLeft ? i : n - 1 - i;
                mask[i] = pos < lit ? 1.0 : dim;
            }
        }
        break;
        default:
        break;
    }

    // texture: the lit ones sit at slightly different levels
    if (move.texture > 0.0)
    {
        QVector<qreal> tex = m_texture.value(group);
        if (tex.count() == n)
            for (int i = 0; i < n; i++)
                if (mask.at(i) >= 1.0)
                    mask[i] = 1.0 - move.texture * (1.0 - tex.at(i));
    }
    // A generated wash keeps a quiet foundation even under a bare pattern.
    // MASTER/trim/blackout and dark re-aim are applied later, so zero stays zero.
    // NOT IN A DROP (runde 164). Point 8 as Tobias put it: "naar musikken gaar
    // i breakdown, eller effekterne holder pause, skal rummet stadig have en
    // bevidst belysning fra wash/basegruppen". Runde 162 applied the floors in
    // every section, and a drop is where they do harm: the pulse floor capped
    // the heads' drop pump at a 60 % dip where the drop is built to fall to
    // 8-12 %, and the chase filter took 976 of the wash's 1384 AUTO chases
    // away - every walk, comet, fill and ripple - the round after the AUTO
    // library was given half the night. m_compositionTier is this beat's tier,
    // set every tick before the group loop; a drop hidden under
    // ENGINE_DROP_SHOW is tier 1 and keeps the floor, which is right.
    // (not under the floor round either: its whole point is one sharp spot
    // walking through a dark wash, runde 214)
    // (runde 291: the floor slides 0.45 -> 0.35 over 50-100 % on the slider,
    // so the foundation's pump keeps deepening with the room - it stopped at
    // ~52 %. With pulseFactor's floor (0.40 -> 0.30) multiplied on, the base's
    // darkest moment is ~0.18 at 50 % and ~0.10 at 100 % - never out.)
    if (ambientBase(group) && m_compositionTier != 2 && (m_floorRound && move.bare) == false)
    {
        const qreal maskFloor = 0.45 - 0.10 * qBound(0.0, (m_faderNow - 0.50) / 0.50, 1.0);
        for (qreal &value : mask) value = qMax(maskFloor, value);
    }
    return mask;
}

bool TrackEngine::ambientBase(const QString &group) const
{
    // Is this the wash-base the rest of the show stands on? Used by the three
    // floors below (never in a drop - see there) and by the room sequences,
    // which ask whether the base takes part in the conversation.
    const TrackGroup &g = m_groups.value(group);
    return m_fullAuto && group == m_compositionBase && g.heads && g.hasDimmer
        && !g.lasers && !g.strobes && !g.patternDevice && !m_groupOff.contains(group);
}

QString TrackEngine::partSlot(const QString &group, int index) const
{
    return QString("dim:%1#%2").arg(group).arg(index);
}

QString TrackEngine::slotGroup(const QString &slot) const
{
    int colon = slot.indexOf(':');
    if (colon < 0)
        return QString();
    QString rest = slot.mid(colon + 1);
    if (slot.startsWith(QStringLiteral("dim:")))
    {
        int hash = rest.lastIndexOf('#');
        if (hash > 0)
            rest = rest.left(hash);
    }
    return m_groups.contains(rest) ? rest : QString();
}

qreal TrackEngine::slotScale(const QString &slot, quint32 fid) const
{
    // MASTER and the group trim belong on whatever actually carries the light.
    // The dimmer scenes get them in setPart(); a colour scene gets them here,
    // but only when it is the thing holding the intensity - otherwise the two
    // would multiply and MASTER would square itself.
    //
    // A chase that OWNS the dimmers (motionOwns, runde 173) is the thing
    // holding the intensity too. It used to be handed the level with MASTER
    // and the trim already multiplied in, so a hand on MASTER or a group fader
    // reached it only on the next beat - reapplyLevels() had nothing to scale.
    // It gets the bare level now, and MASTER and the trim here, live (runde 174).
    if (slot.startsWith(QStringLiteral("mot:")))
    {
        // ... a chase that can OWN them - motionOwns' own test. One that lights
        // every lamp in every step (litShare >= 0.99) never owns - unless it
        // keeps them all at ENGINE_OWN_FLOOR (runde 231, canOwnDimmers): the parts
        // keep the dimmer, and scaling the chase as well put the room at
        // level squared (runde 174).
        const TrackFuncInfo &mfi = m_funcs.value(fid);
        if (canOwnDimmers(mfi, slotGroup(slot) == m_compositionBase) == false)   // see motionOwns (runde 231)
            return 1.0;
        const QString group = slotGroup(slot);
        return masterOut() * (group.isEmpty() ? 1.0 : m_groupTrim.value(group, 1.0));
    }
    // the flashes. The HELD FLASH button is always full strength - no MASTER,
    // no group trim - under its own ceiling (white at 70 % on a strobe, in
    // the scene itself) (Tobias, 2026-09-24: "FLASH skal ikke foelge master,
    // den skal altid vaere fuld styrke (med dens loft)"). The engine's OWN hits
    // on a drop follow MASTER and the group's trim (runde 199).
    if (slot == QStringLiteral("flash"))
    {
        if (m_flash)
            return 1.0;
        // the operator's own flash scene on a hit spans several groups: it
        // gets the highest of their trims, so a strobe bank turned down for
        // the night stays down through the drop (runde 201)
        qreal ftrim = 0.0;
        foreach (const QString &fg, m_funcs.value(fid).groups)
            ftrim = qMax(ftrim, m_groupTrim.value(fg, 1.0));
        return masterOut() * (m_funcs.value(fid).groups.isEmpty() ? 1.0 : ftrim);
    }
    if (slot.startsWith(QStringLiteral("flash:")))
    {
        const QString fg = slotGroup(slot);
        if (m_flash && m_flashHeld.contains(fg))
            return 1.0;
        return masterOut() * m_groupTrim.value(fg, 1.0);
    }
    if (slot.startsWith(QStringLiteral("col:")) == false
        && slot.startsWith(QStringLiteral("colx:")) == false   // runde 370
        && slot.startsWith(QStringLiteral("idle:")) == false
        && slot.startsWith(QStringLiteral("echo:")) == false)
        return 1.0;
    QString group = slotGroup(slot);
    const TrackGroup &g = m_groups.value(group);
    bool carries = m_funcs.value(fid).dimmer || g.hasDimmer == false || g.parts.isEmpty();
    if (carries == false)
        return 1.0;
    return masterOut() * (group.isEmpty() ? 1.0 : m_groupTrim.value(group, 1.0));
}

void TrackEngine::reapplyLevels()
{
    // a fader moved: straight onto everything that is lit, without waiting
    // for the next beat to come round
    if (m_doc == nullptr)
        return;
    foreach (const QString &slot, m_active.keys())
    {
        quint32 fid = m_active.value(slot);
        Function *func = m_doc->function(fid);
        if (func == nullptr)
            continue;
        QString group = slotGroup(slot);
        qreal out = m_activeLevel.value(slot, 1.0);
        if (slot.startsWith(QStringLiteral("dim:")))
        {
            // A held flash is full, as in setPart() and slotPulseTimer(): no
            // pulse under it, and no group trim while the operator holds the
            // button - a trim or MASTER touched mid-flash pulled the strobes
            // down to trim x pulse x master until the next beat (runde 168).
            // Nor MASTER: the held button is always full (Tobias, 2026-09-24).
            const bool held = m_flashHeld.contains(group);
            const bool landFree = m_groups.value(group).strobes && landBurstNow();   // runde 357
            const qreal trim = ((m_flash && held) || landFree) ? 1.0 : m_groupTrim.value(group, 1.0);
            out *= (held ? 1.0 : pulseFactor(group)) * trim * (((m_flash && held) || landFree) ? 1.0 : masterOut());
            // and the same on/off squaring setPart() does: an animation
            // laser's dimmer is a switch, and a fraction written to it is
            // rounded by the fixture in a way nobody can predict
            if (m_groups.value(group).patternDevice)
                out = out > 0.10 ? 1.0 : 0.0;
        }
        else
        {
            out *= slotScale(slot, fid);
            // an owning chase: its pulse and, painting the colour, the root -
            // see run() (runde 174)
            if (slot.startsWith(QStringLiteral("mot:")))
            {
                if (m_motionDim.contains(group) && m_flashHeld.contains(group) == false)
                    out *= pulseFactor(group);
                if (canOwnDimmers(m_funcs.value(fid), group == m_compositionBase)   // runde 231
                    && m_funcs.value(fid).setsColour && m_funcs.value(fid).coversColour
                    && m_groups.value(group).rgb)
                    out = std::sqrt(qBound(0.0, out, 1.0));
            }
        }
        out = lightsOut() ? 0.0 : qBound(0.0, out, 1.0);
        m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
        m_activeOut.insert(slot, out);
    }
}

qreal TrackEngine::pulseFactor(const QString &group) const
{
    qreal factor = 1.0;
    qint64 now = m_clock.elapsed();

    // R359_LAND_16 (Tobias 10-05, "stroberne blinker i takt paa landingen"):
    // the strobe lamps' landing blinks on the sixteenths of THIS beat - lit
    // for the first 40 % of each, dark for the rest - instead of the
    // shutter's free-running rate. 8.5 blinks a second at 128 BPM, on the
    // beat the deck plays. MASTER and the trim are off for the landing (R357).
    if (m_landSixteenths && m_beatMs > 0.0 && blink16Now() && m_groups.value(group).strobes)   // R362: or the mini landing (under MASTER - landFree is the landing's alone)
    {
        const qreal q = std::fmod(qMax(0.0, qreal(now - m_beatStartMs)) / m_beatMs * 4.0, 1.0);
        return q < 0.40 ? 1.0 : 0.0;    // full on every sixteenth: no kick's decay under it
    }

    // the pulse: full on the beat, down to (1 - depth) a quarter beat later
    // and flat from there - a kick, not a sine
    qreal depth = m_pulseDepth.value(group, 0.0);
    if (depth > 0.0)
    {
        const TrackGroup &pg = m_groups.value(group);
        // no hit yet (the section opened on an off-beat, or on a beat the
        // analysis heard no kick on): the light sits on its floor, it does
        // not stand at full waiting for one
        qreal t = m_pulseStart.contains(group) ? qreal(now - m_pulseStart.value(group)) : 1.0e9;
        qreal strength = m_pulseStrength.value(group, 1.0);
        if (pg.patternDevice)
        {
            // An animation laser's master dimmer is a SWITCH, not a dimmer:
            // it is on or it is off, and nothing in between exists. An
            // exponential fall would spend the beat drifting through values
            // the fixture rounds one way or the other, so the light would go
            // out at a moment nobody can predict or hear. A square gate is
            // the honest version - full for a slice of the beat, nothing for
            // the rest - and a deeper "pulse" simply means a shorter slice.
            // qMax first: qBound asserts under Qt 6 when the low bound is
            // above the high one, and 60 ms is above beatMs * 0.9 for
            // anything over 900 bpm. Nothing plays at 900 bpm, but a garbled
            // tempo packet should not abort the application.
            const qreal bm = qMax(120.0, m_beatMs);
            qreal onMs = qBound(60.0, bm * (0.55 - 0.35 * depth), bm * 0.9);
            factor *= t < onMs ? 1.0 : 0.0;
        }
        else
        {
            // A quarter of a beat is a kick - right for a wash, far too soft
            // for a strobe, which has to be lit and dark again inside a tenth.
            qreal tau = pg.strobes ? qMax(20.0, m_beatMs * 0.09)
                                   : qMax(40.0, m_beatMs * 0.25);
            factor *= (1.0 - depth) + depth * strength * std::exp(-t / tau);
        }
    }

    // the breath: a slow sine over a few bars, 70..100 %, for the lit base
    // in a break - alive, not static, and never pumping
    int bars = m_breathe.value(group, 0);
    if (bars > 0 && m_beatMs > 0.0)
    {
        qreal within = qBound(0.0, qreal(now - m_beatStartMs) / m_beatMs, 1.0);
        // runde 273: on the group's own phase, not m_beatIndex (see tick())
        qreal pos = m_breathPhase.value(group, 0.25) + within / (qreal(bars) * 4.0);
        factor *= 0.70 + 0.30 * (0.5 + 0.5 * std::sin(pos * 6.283185307179586));
    }
    // The room conversation (r162) - BEFORE the floors below (runde 262): it
    // multiplied onto them, so the base out of its focus sat at 0.40 x 0.75
    // and its brightest owned lamp at 0.35 x 0.75 = 0.26 - under the promise
    // of runde 259, and a conversation's other voices may be a laser bar and
    // a group whose steps all go dark, which do not cover the base.
    const int sequenceIndex = m_sequenceGroups.indexOf(group);
    if (sequenceIndex >= 0)
        factor *= TrackStage::sequenceGain(qreal(now - m_sequenceStart) / m_sequenceBeatMs,
            sequenceIndex, m_sequenceGroups.size(), group == m_compositionBase);
    // the foundation's floor - not in a drop, see patternMask()
    // (runde 291: 0.40 -> 0.30 over 50-100 % on the slider, as patternMask)
    if (ambientBase(group) && m_compositionTier != 2 && m_floorRound == false)
        factor = qMax(0.40 - 0.10 * qBound(0.0, (m_faderNow - 0.50) / 0.50, 1.0), factor);
    // A chase that OWNS the base's dimmers (runde 231, canOwnDimmers) keeps
    // every head at minLit or more - that is the promise it owns them on. The
    // pulse and the breath multiply onto it, so without this the base's
    // darkest head went to minLit x pulse (0.35 x 0.08 in a drop, review):
    // the pulse may take it down to ENGINE_OWN_FLOOR of the level, no further.
    if (group == m_compositionBase && m_motionDim.contains(group))
    {
        // (runde 259: the promise is now the BRIGHTEST lamp - peakLit - so the
        // pulse keeps that one at ENGINE_OWN_FLOOR; the dim ones may go lower)
        const TrackFuncInfo &own = m_funcs.value(m_active.value(QStringLiteral("mot:") + group,
                                                                 Function::invalidId()));
        const qreal ml = qMax(own.minLit, own.peakLit);
        if (ml > 0.0)
            factor = qMax(factor, qMin(1.0, ENGINE_OWN_FLOOR / ml));
    }
    return factor;
}

QString TrackEngine::moveName(const TrackMove &move) const
{
    static const char *names[ENGINE_PAT_COUNT] = { "", "chase", "pingpong", "odd/even", "halves", "sparkle", "fill" };
    QString s;
    if (move.pattern > 0 && move.pattern < ENGINE_PAT_COUNT)
        s = move.subSteps > 1 ? QString("%1/1:%2").arg(names[move.pattern]).arg(move.subSteps)
                              : QString("%1/%2").arg(names[move.pattern]).arg(move.stepBeats);
    if (move.pulse > 0.0)
        s += QString(s.isEmpty() ? "pulse %1" : " pulse %1").arg(int(move.pulse * 100));
    if (move.breatheBars > 0)
        s += QString(s.isEmpty() ? "breathe/%1" : " breathe/%1").arg(move.breatheBars);
    if (move.colourBars > 0)
        s += QString(" swap/%1").arg(move.colourBars);
    if (move.width > 1 && (move.pattern == ENGINE_PAT_CHASE || move.pattern == ENGINE_PAT_PINGPONG))
        s += QString(" x%1").arg(move.width);
    if (move.bare)
        s += " bare";
    if (move.flashBar)
        s += " hits";
    return s.isEmpty() ? QString() : QString("(%1)").arg(s);
}

/*********************************************************************
 * Warnings, calm, log
 *********************************************************************/


TrackSweep TrackEngine::drawSweep(int tier, bool build, qreal prog, qreal energy, int heads, bool laser, bool drive) const
{
    // The heads' figure for a section. The energy decides whether they move
    // at all, how big and how fast; the dice pick the shape and how the
    // heads relate to each other, so no two sections look alike.
    TrackSweep sw;
    QRandomGenerator *rng = QRandomGenerator::global();
    auto pick = [rng](const QList<int> &opts) { return opts.at(int(rng->bounded(opts.count()))); };
    auto chance = [rng](qreal p) { return rng->bounded(1000) < int(qBound(0.0, p, 1.0) * 1000.0); };
    qreal e = qBound(0.0, energy, 1.0);

    // aim jitter: every section aims a little off the position, so two
    // sections on the same position do not look the same. Lasers sideways only
    sw.dx = int(rng->bounded(laser ? 13 : 25)) - (laser ? 6 : 12);
    sw.dy = laser ? 0 : int(rng->bounded(17)) - 8;

    // move at all? a break mostly rests, a drop nearly always moves
    // A break and a build were the two places a figure was least likely to be
    // drawn - and they are exactly where a still rig is noticed. Both are now
    // near-certain; a break just gets a very slow one.
    // A break ALWAYS moves (Tobias, 2026-09-14: "intro/break SKAL altid have
    // LANGSOM bevaegelse paa basen") - one in five used to stand still at
    // the bottom of the fader, and a still base in a quiet room is a photo.
    qreal moveP = tier == 0 ? 1.0 : (tier == 2 ? 0.90 + 0.10 * e : 0.85 + 0.15 * e);
    if (build)
        moveP = 0.90 + 0.10 * prog;
    if (chance(moveP) == false)
    {
        // "this group does not move" has to mean it: dx was already drawn
        // above, and applySweep only leaves the EFX alone when the shape and
        // BOTH offsets are zero - so a stray dx nudged the bars off their aim
        sw.dx = 0;
        sw.dy = 0;
        return sw;
    }

    // the shape: a gentle few at rest, the whole menu when it is hot.
    // SquareTrue and SquareChoppy came in 2026-09-16 with the rest of the
    // figure work - a choppy square is a corner-to-corner snap, which reads
    // as a figure rather than as a drift, so it stays in the drops.
    QList<int> shapes;
    shapes << int(EFX::Circle) << int(EFX::Line) << int(EFX::Eight);
    if (tier > 0 && (e > 0.30 || tier == 2))
        shapes << int(EFX::Leaf) << int(EFX::Diamond) << int(EFX::Line2) << int(EFX::Circle)
               << int(EFX::SquareTrue);
    if (tier > 0 && (e > 0.55 || tier == 2))
        shapes << int(EFX::Lissajous) << int(EFX::Square) << int(EFX::Lissajous) << int(EFX::Eight);
    if (tier == 2 && e > 0.45)
        shapes << int(EFX::SquareChoppy) << int(EFX::Diamond);
    sw.shape = pick(shapes);
    if (sw.shape == int(EFX::Lissajous))
    {
        // the ratio IS the figure: 1:2 is a bow, 2:3 a pretzel, 3:4 a weave,
        // 3:2 and 5:4 lean the other way. Eleven pairs where there were six.
        static const int ratios[][2] = { { 1, 2 }, { 1, 3 }, { 2, 3 }, { 3, 2 }, { 2, 1 },
                                         { 3, 4 }, { 4, 3 }, { 3, 5 }, { 5, 4 }, { 2, 5 }, { 5, 3 } };
        int k = int(rng->bounded(11));
        sw.fx = ratios[k][0];
        sw.fy = ratios[k][1];
    }

    // size: the energy sets the ceiling, the dice the figure. Tilt has less
    // room than pan, and a break barely stirs
    // a break's figure is wide enough to see but takes half a minute to walk
    // it; that is the whole point of a break
    // A break's figure is BIG - the room is quiet, so the one thing moving
    // has all the attention and it may as well travel. It is the pace that
    // makes a break a break, not the size.
    // The fader's half of the size lives in sweepReach() (trackengine.h), so
    // applySweep() can follow the fader live with the same curve. The dice
    // are 0.75..1.0 now, not 0.5..1.0: at 0.5 a groove figure at the top of
    // the fader could come out SMALLER than one at the bottom, and the fader
    // was not readable through it.
    sw.tier = build ? 1 : tier;
    sw.drawnE = e;
    sw.drive = drive && sw.tier == 1;
    qreal reach = sweepReach(sw.tier, e, sw.drive);
    // a build's figure grows with the climb AND with the slider (runde 291):
    // it was 26 + 30 x prog whatever the fader said - a build at 35 % and at
    // 100 % drew the same size. The groove's reach x 0.9 -> 1.4 over the climb
    // keeps today's middle and lets applySweep's live ratio follow the fader.
    if (build)
        reach = sweepReach(1, e, false) * (0.9 + 0.5 * qBound(0.0, prog, 1.0));
    qreal size = reach * (0.75 + 0.25 * rng->generateDouble());
    // pan has the whole room, tilt has the floor: the heads hang from the
    // ceiling and a figure must not climb the walls
    sw.width = qBound(6, int(size), 127);
    // 4 units of tilt is eight degrees - a circle that reads as a flat line
    sw.height = qBound(10, int(size * (0.45 + 0.55 * rng->generateDouble())), 28);
    if (sw.shape == int(EFX::Line) || sw.shape == int(EFX::Line2))
        sw.rotation = pick(QList<int>() << 0 << 0 << 90 << 30 << 150 << 60 << 120 << 15 << 165);
    else if (sw.shape == int(EFX::Eight) || sw.shape == int(EFX::Leaf)
             || sw.shape == int(EFX::Diamond) || sw.shape == int(EFX::Lissajous))
        // a closed figure on its side or on the diagonal is a different
        // figure, and a deliberate angle reads better than a random one
        sw.rotation = pick(QList<int>() << 0 << 0 << 45 << 90 << 90 << 135);
    else
        sw.rotation = chance(0.4) ? int(rng->bounded(360)) : 0;

    // Tempo: beats per figure. The ENERGY fader is the pace as well as the
    // size now - a straight slide between the slowest this room allows and
    // the quickest, in every section type, so 100 % feels different from
    // 50 % everywhere and not only in a drop. Nothing here is fast: even the
    // top of a drop is six beats for a whole circle.
    // The pace curves live in sweepPace() (trackengine.h) for the same
    // reason as the size: a break is a minute per figure at the bottom of
    // the fader and a quarter of that at the top; a groove 40 -> 8 beats; a
    // drop 24 -> 4. The dice are +-10 %, so the fader is what the eye reads.
    auto beatsFor = [rng](qreal f) {
        f *= 0.90 + 0.20 * rng->generateDouble();
        return qMax(3, int(qRound(f)));
    };
    sw.beats = beatsFor(sweepPace(sw.tier, e, sw.drive));
    if (build)
        sw.beats = beatsFor(28.0 - 20.0 * e) / (prog >= 0.5 ? 2 : 1);   // >= with the redraw's test (runde 246)

    // how the heads relate: in unison, as a wave, one after another,
    // mirrored, or fanned out around the figure
    if (heads >= 2)
    {
        // HOW THE HEADS RELATE. This used to be one of five, exclusive: all
        // together, a wave, one after another, mirrored, or fanned evenly
        // round the figure. Tobias, 2026-09-16: "langt flere stilfulde
        // figurer til movingheads, baade hvor de foelger hinanden, men ogsaa
        // hvor de koerer offset ift. hinanden". So the two axes are separate
        // now and combine freely:
        //
        //   propagation  together / a wave through them / one after another
        //   offset       how far apart they start on the figure:
        //                  even      360/heads - a ring, every head somewhere
        //                            else on the shape
        //                  half      180/heads - they cover half the figure
        //                            and the row reads as a chevron
        //                  pairs     180       - every second head opposite
        //                  trail     45 or 30  - a tight cascade, the row
        //                            following itself a beat behind
        //   mirror       every second head runs it backwards (with an offset
        //                that is a criss-cross; on its own, a breathing in-out)
        //
        // Together that is 3 x 6 x 2 = 36 ways for the heads to relate, and
        // with the shapes and ratios above the figure is rarely the same twice
        // in a night.
        sw.spread = tier == 2 ? pick(QList<int>() << 0 << 0 << 1 << 1 << 2)
                              : pick(QList<int>() << 0 << 0 << 0 << 1 << 2);
        QList<int> offsets;
        offsets << 0 << 0 << (360 / heads);
        if (heads >= 3)
            offsets << (180 / heads) << 180;
        if (tier > 0)
            offsets << 45 << 30;
        sw.fan = pick(offsets);
        // Mirroring reads as the row breathing in and out, which is a quiet
        // figure - so it is common at rest and rarer when it is hot, where the
        // offsets do the talking instead.
        sw.mirror = chance(tier == 0 ? 0.35 : (tier == 2 ? 0.20 : 0.30));
        // ... but not all three at once on a small row: two heads mirrored AND
        // offset AND serial is not a figure, it is a scribble
        if (heads <= 3 && sw.mirror && sw.fan > 0 && sw.spread == 2)
            sw.mirror = false;
    }

    // the drop's character: hard = big and fast, wide = big and slow,
    // tight = small quick lines and eights
    if (tier == 2 && m_dropStyle == 1)
    {
        sw.width = qBound(6, int(sw.width * 1.3), 127);
        sw.minBeats = qMax(sw.minBeats, 8);            // a floor under the live pace (runde 271)
    }
    else if (tier == 2 && m_dropStyle == 2)
    {
        sw.width = qBound(6, int(sw.width * 1.3), 127);
        sw.height = qBound(4, int(sw.height * 1.3), 28);
        sw.minBeats = qMax(sw.minBeats, 8);
    }
    else if (tier == 2 && m_dropStyle == 3)
    {
        if (sw.shape != int(EFX::Line) && sw.shape != int(EFX::Eight) && sw.shape != int(EFX::Line2))
            sw.shape = chance(0.5) ? int(EFX::Line) : int(EFX::Eight);
        sw.width = qBound(6, int(sw.width * 0.6), 60);
        sw.height = qBound(10, int(sw.height * 0.6), 28);
        sw.minBeats = qMax(sw.minBeats, 8);
    }
    else if (tier == 2 && m_dropStyle == 4)
    {
        // heavy: broad and slow - twice the time for a figure a little bigger
        sw.width = qBound(6, int(sw.width * 1.2), 127);
        sw.height = qBound(4, int(sw.height * 1.2), 28);
        sw.beats *= 2;                                 // the character: twice the time
        sw.minBeats = qMax(sw.minBeats, 16);
    }
    else if (tier == 2 && m_dropStyle == 5)
    {
        // nervous: small figures, still never under eight beats - the
        // nerves are in the chases, not in the mirrors
        sw.width = qBound(6, int(sw.width * 0.5), 50);
        sw.height = qBound(10, int(sw.height * 0.5), 28);
        sw.minBeats = qMax(sw.minBeats, 8);
    }

    // lasers: sideways only, never lifted off the aim the user gave them,
    // small, and never faster than a bar
    if (laser)
    {
        // A laser bar has ONE axis - tilt - and no pan at all. The figure
        // therefore has to live on the Y amplitude; width was driving an axis
        // these fixtures do not have, so every laser figure was a no-op.
        // Line gives x == y, and rotateAndScale sends the tilt output to
        // YOffset + y * height when the rotation is zero. So: Line, rotation
        // 0, and the amplitude on HEIGHT. (Rotation 90 cancels the height
        // term entirely - that is the same standing-still bug, reversed.)
        sw.shape = int(EFX::Line);
        sw.rotation = 0;
        sw.width = 0;
        // Below three fifths of the fader the bars do not move AT ALL. They
        // sit in the aim the operator set - straight out over the room - and
        // that is what a break, a build and a quiet night look like. Above
        // it they open up, and the only thing that grows is how far apart
        // they get: the pace is fixed, because the mirrors are the most
        // delicate thing in the rig and nothing here is ever allowed to hurry.
        // the 40 % line is the slider's, not the section-scaled energy's (see tick)
        qreal lw = qBound(0.0, (m_faderNow - 0.40) / 0.60, 1.0);
        // R383_LASER_HOME (Tobias 10-06: "Laserne maa ogsaa gerne ved de helt
        // hoeje energier staa paa deres hjem (stille) med chases"): from 90 % on
        // the slider one section in three the bars hold their home aim
        const qreal top = qBound(0.0, (m_faderNow - 0.90) / 0.10, 1.0);
        if (m_faderNow >= 0.90 && m_drawHome && m_barsAloneNow == false)   // R401_SPECIAL_DRAW
            lw = 0.0;
        // R385_BARS_FULL (Tobias 10-07: "fuldt lys med langsom opad"): the
        // whole row lifts together off the home aim, at the bars' slowest pace
        if (m_barsAloneNow && m_aloneFull && tier == 0 && build == false)
        {
            const qreal lwNow = qBound(0.0, (m_faderNow - 0.40) / 0.60, 1.0);
            // R387_LIFT_FULL (Tobias 10-07: "fint med 26 enheder max i breaket
            // ogsaa"): the bars' ordinary figure height - at most 13, so the
            // lift tops out 26 units over the aim, like every bar figure
            sw.height = int(4 + 6 * lwNow) + int(rng->bounded(4));
            sw.drawnF = m_faderNow;
            sw.beats = qMax(48, int(qRound(96.0 + 32.0 * rng->generateDouble())));
            sw.dx = 0;
            sw.dy = -sw.height;              // the lowest point on the aim
            sw.spread = 0;
            sw.mirror = false;
            sw.fan = 0;
            sw.laserForm = 1;                // the row in unison
            sw.upOnly = true;
            return sw;
        }
        if (lw <= 0.0 || tier == 0 || build)
        {
            sw.shape = -1;
            sw.dx = 0;
            sw.dy = 0;
            return sw;
        }
        // SMALL and VERY slow, whatever the energy says. The figure is 8-26
        // units (3-11 degrees) - the wide 34-85 figure is gone - and one
        // figure takes 96-128 beats (45-60 s at 128 bpm), so the tip of a 20 m
        // beam moves at well under half a metre a second. The fader only
        // decides whether they move at all (above three fifths) and how far
        // apart the bars run. Tobias, 2026-09-16: "selvom energien er hoej,
        // skal de fortsat vaere MEGET langsomme og smaa bevaegelser, da
        // laserne er saa lange. De helt langsomme bevaegelser ser ogsaa
        // mest stilet ud."
        // Half the old amplitude: the figure used to straddle the aim (+-h,
        // 8-26 units of travel); it now lies on one side of it, so h is
        // halved to keep the travel - and the promise of SMALL - the same.
        sw.height = int(4 + 6 * lw) + int(rng->bounded(4));
        sw.drawnF = m_faderNow;                  // runde 293: the height follows the slider live
        // The PERIOD is what made them read as still - not the size. The
        // travel is 2 x height = 20-26 units at the top of the fader, and the
        // operator's own scenes put a unit at about 0.78 degrees (LaserUPP
        // 126 -> LaserDOWN 242 is 116 units from horizontal to the floor), so
        // the beams already swing 16-20 degrees. Over 96-128 beats that is
        // 0.7 degrees a second: below what an eye reads as movement, and in a
        // drop, with the strobes and the heads going, invisible. Tobias,
        // 2026-09-19: "synes heller aldrig at barene bevaegede sig paa drops
        // i de hoeje energi vaerdier."
        //
        // So a DROP - and only a drop - shortens it with the fader: 96 beats
        // at two fifths, 64 at the top. 64 beats is 30 s a cycle at 128 bpm,
        // 15 s to cross the 20 degrees, which puts the tip of a 20 m beam at
        // 0.5 m/s - the limit the rest of this block is built around, and
        // still a drift rather than a sweep. A groove and a break keep the
        // 96-128 they had: "MEGET langsomme og smaa bevaegelser" (Tobias,
        // 2026-09-16) is the rule, and the drop is the exception to it.
        qreal period = 96.0 + 32.0 * rng->generateDouble();
        if (tier == 2)
            period = (96.0 - 32.0 * lw) * (0.90 + 0.20 * rng->generateDouble());
        // R383_LASER_FAST (Tobias 10-06: "op til dobbelt saa hurtige som de
        // nuvaerende (kun ved de rigtigt hoeje energier)"): from 90 % on the
        // slider the cycle shortens, to half at 100 % - a groove 48-64 beats, a
        // drop 32. The size and every safety line stay as they are
        period /= 1.0 + top;
        sw.beats = qMax(top > 0.0 ? 24 : 48, int(qRound(period)));
        sw.dx = 0;
        // The figure is a line of +-height around aim + dy. It is drawn
        // UP-ONLY: dy = -height puts its lowest point exactly on the home
        // aim, so between 40 and 60 % the beams lift off the ceiling and
        // never dip under it. applySweep() slides dy down again, live, by
        // laserDownAllowed(energy) - nought below 60 %, the whole figure
        // below the aim near 100 %. (Tobias, 2026-09-18.)
        sw.dy = -sw.height;
        // One after another along the wall, always - and the higher the fader
        // the further apart they run. At the top neighbouring bars are in
        // opposite directions, one up while the next goes down, which is the
        // only version of this you can see from the floor.
        if (heads >= 2)
        {
            // Parallel, with the start offset alone. Asymmetric adds its own
            // 360/(n+1) degrees per bar ON TOP of the fan, and the two stacked
            // put two bars opposite at the bottom of the fader and 60 degrees
            // apart at the top - the fader ran the wrong way
            sw.spread = 0;
            // ... and never mirrored: the head branch above dices sw.mirror,
            // and applySweep() runs every second fixture Backward - every
            // second bar in antiphase, the scatter this block exists to stop
            // (runde 168)
            sw.mirror = false;
            // A WAVE DOWN THE ROW, not a scatter. 60-180 degrees per bar put
            // every second bar in antiphase at the top, which from the floor
            // reads as six bars doing unrelated things. Tobias, 2026-09-20:
            // "offset er fint, men det skal vaere et offset ift. den laser
            // der er ved siden af, ikke bare random offset, saa de 'foelger'
            // hinanden."
            //
            // One figure spread evenly over the row - 360/heads, so the last
            // bar is one step behind the first - is a wave travelling along
            // the ceiling. The fader opens it: near the bottom the row moves
            // almost as one, at the top the wave is fully spread.
            // qBound(10, x, even) would INVERT if a row ever had more than
            // 36 bars - Qt's qBound is qMax(min, qMin(max, v)) with no assert,
            // so min > max silently returns min, the opposite of a clamp.
            int even = qMax(1, 360 / qMax(1, heads));
            sw.fan = qMax(1, qMin(even, int(qRound(even * (0.35 + 0.65 * lw)))));
            // R383_LASER_FAST: at the top of the slider three more figures -
            // the row in unison, from the middle out, the wave back - beside
            // the wave down the row; all of them a figure the row makes
            // together ("offset ift. naboen", 2026-09-20)
            if (top > 0.0)
                sw.laserForm = int(rng->bounded(4));
        }
    }
    return sw;
}

int TrackEngine::sweepFloor(const TrackSweep &sw) const
{
    if (sw.paceRole <= 0)
        return sw.minBeats;
    const qreal pf = qBound(0.0, m_faderNow, 1.0);
    return qMax(sw.minBeats, sw.paceRole == 1
                ? int(qRound(16.0 - 8.0 * qBound(0.0, (pf - 0.50) / 0.50, 1.0)))
                : int(qRound(24.0 - 12.0 * qBound(0.0, (pf - 0.40) / 0.60, 1.0))));
}

bool TrackEngine::applySweep(const QString &group, const TrackSweep &sw, qreal bpm, qreal energy)
{
    quint32 fid = m_sweepFunc.value(group, Function::invalidId());
    EFX *efx = m_doc ? qobject_cast<EFX *>(m_doc->function(fid)) : nullptr;
    if (efx == nullptr)
        return false;
    QString slot = "efx:" + group;
    if (sw.shape < 0 && sw.dx == 0 && sw.dy == 0)
    {
        if (m_active.contains(slot))
            stopSlot(slot, true);
        return false;
    }

    // the EFX counts milliseconds, the music beats: one figure = beats x the
    // DJ's beat, halved or doubled by the SPEED tiles
    qreal beatMs = bpm > 0.0 ? 60000.0 / bpm : 468.75;
    // The fader, LIVE. The figure was drawn at sw.drawnE; the room is at
    // `energy` now. Size and pace follow the tier's own curves (sweepReach /
    // sweepPace) by ratio, so a figure drawn at 40 % and pushed to 100 %
    // grows and quickens exactly as one drawn at 100 % would have been - on
    // this very beat, not at the next section line. Width, height and
    // duration are EFX attributes read on every frame (rotateAndScale,
    // durationChanged), so the running figure stretches without a restart.
    // Not the lasers: their pace is fixed on purpose (below, and drawSweep),
    // and their amplitude is the small, slow one Tobias asked for.
    bool laser = m_groups.value(group).lasers;
    int width = sw.width;
    int height = sw.height;
    // the draw's floor under it (runde 271), and FULL AUTO's role floor off the
    // slider, live (runde 290: see TrackSweep::paceRole)
    const int floorBeats = sweepFloor(sw);
    int beats = qMax(floorBeats, sw.beats);
    // the lasers' one live control: how far under the home aim the figure
    // may reach. Drawn up-only (dy = -height); the fader slides it down.
    int dy = sw.dy;
    int allowed = 0;
    if (laser && sw.shape >= 0)
    {
        // The figure rides on the aim under it (a relative EFX). From the
        // home aim it may use the fader's whole allowance; from any other aim
        // - which may already be dipping by that allowance - none, or the two
        // add up to twice what the fader allows (runde 171).
        allowed = (m_position.value(group, Function::invalidId()) == homePosition(group)
                   && sw.upOnly == false)     // R385_BARS_FULL: the lift never dips
                ? laserDownAllowed(m_faderNow) : 0;
        dy = -sw.height + qMin(2 * sw.height, allowed);
    }
    if (laser == false && sw.shape >= 0)
    {
        qreal e = qBound(0.0, energy, 1.0);
        qreal grow = sweepReach(sw.tier, e, sw.drive) / qMax(1.0, sweepReach(sw.tier, sw.drawnE, sw.drive));
        qreal pace = sweepPace(sw.tier, e, sw.drive) / qMax(1.0, sweepPace(sw.tier, sw.drawnE, sw.drive));
        grow *= m_mixMotionScale;
        pace /= m_mixMotionScale;
        width = qBound(6, int(qRound(sw.width * grow)), 127);
        height = qBound(4, int(qRound(sw.height * grow)), 28);
        // Over the horizon by ENERGY (runde 211, Tobias: "Heads maa gerne gaa
        // over vandret, jo mere energi, jo oftere maa de gaa over vandret").
        // The aim stays in the floor cone; the FIGURE on it reaches in tilt by
        // |width sin r| + |height cos r| + |dy| (EFX rotateAndScale). Up to
        // 30 % ENERGY that reach is held to 24 steps - inside the cone, never
        // across. From there it opens on a cube, so the heads stay pointed at
        // the floor through most of the fader and only the top lets them go:
        // free at 95 % (Tobias, runde 212: "Foerst fra 95% skal de vaere helt
        // frie. Og saa langsomt nedad derfra ... primaert vaere rettet ned mod
        // gulvet"). 60 %: 37 steps, 70 %: 55, 80 %: 84, 90 %: 127.
        const qreal rad = qreal(sw.rotation) * 3.14159265358979 / 180.0;
        const qreal reach = qAbs(width * std::sin(rad)) + qAbs(height * std::cos(rad)) + qAbs(sw.dy);
        // On the SLIDER (runde 271): "30 %" and "free at 95 %" are the
        // number on the screen, like the other hard lines (tick, m_faderNow).
        // `e` is the section-scaled energy: slider 95 in a quiet groove read
        // 0.76 and the heads were never free; slider 60 read 0.48 - 27 steps
        // where the table above promises 37.
        const qreal open = qBound(0.0, (m_faderNow - 0.30) / 0.65, 1.0);
        const qreal allow = 24.0 + 131.0 * open * open * open;
        if (reach > allow && reach > 0.0)
        {
            // only the tilt comes down: height all the way, width by its
            // share of the tilt (|sin r|) - a figure lying flat keeps its
            // sideways sweep (review, runde 211)
            const qreal k = allow / reach;
            width = qBound(6, int(qRound(width * (1.0 - (1.0 - k) * qAbs(std::sin(rad))))), 127);
            height = qBound(4, int(qRound(height * k)), 28);
        }
        // the live rescale may quicken a figure by a quarter at most: it
        // scaled the draw's floor down with it, and a drop figure pushed from
        // 80 to 99 % halved its beats - 2x on top made it frantic (runde 199)
        // ... and the draw's floor goes under the scaled CURVE, never into
        // it (runde 271: see TrackSweep::minBeats)
        beats = qMax(floorBeats, qMax(3, qMax(int(qRound(sw.beats * pace)), sw.beats * 3 / 4)));
    }
    // Not for the lasers. drawSweep() fixes their pace - "nothing here is ever
    // allowed to hurry" - and then this halved it whenever the DJ hit 2x. The
    // beams are 8-20 m long, so a 42-unit figure at half period puts the tip
    // past 6 m/s across the ceiling: a whip, not a sweep. The SPEED tile may
    // make them SLOWER (1/2x still applies); faster is not on offer.
    if (m_speed < 0)
        beats *= 2;
    else if (m_speed > 0 && laser == false)
        beats = qMin(beats, qMax(4, beats / 2));       // 2x: never under a bar, never slower (runde 199, 201)
    uint ms = uint(qMax(250.0, beats * beatMs));

    // THE BARS GROW OUT OF THE AIM (runde 219, the 09-20 log: "sigtet var
    // maerkeligt og bevaegelserne ikke smooth"). A figure used to start at
    // full size, its centre `height` steps above the aim and every bar at its
    // own point of the wave - six bars thrown 0-26 steps at once on every
    // start. Now a bar figure starts at size nought ON its aim and grows one
    // step of height every second beat, by the EFX's own clock read here on
    // the beat. The centre follows from that size by the rule it always had
    // (dy = -h + min(2h, allowed): the lowest point is on the aim, or as far
    // under it as the fader allows) and is set together with it.
    //
    // Not the EFX's fade-in (tried first in this round, review): that grows
    // on the timer thread whether beats come or not, and with the link lost
    // there are none for up to 30 s - the figure would outgrow a centre that
    // only moves on the beat. Grown here, nothing grows between beats, and
    // the safety line holds whatever the timing.
    // Runde 293 (Tobias: "alt skal skalere efter energi-slideren"): the size
    // follows the slider while the figure runs - the draw's 4 + 6 x lw, moved
    // by the slider since, 4..13 as the draw allows. A bar figure is never
    // redrawn while it runs (runde 219) and the bars change only per part, so
    // one started at 45 % stayed that small at 100 % for minutes. Growing is
    // still a step every second beat (barGrow), shrinking at once; the centre
    // rule below keeps the lowest point on the aim or within the allowance.
    int barHeight = sw.height;
    if (laser && sw.drawnF >= 0.0)
    {
        const qreal lwNow = qBound(0.0, (m_faderNow - 0.40) / 0.60, 1.0);
        const qreal lwThen = qBound(0.0, (sw.drawnF - 0.40) / 0.60, 1.0);
        barHeight = qBound(4, sw.height + int(qRound(6.0 * (lwNow - lwThen))), 13);   // R387_LIFT_FULL: the lift too
    }
    auto barGrow = [&](qreal elapsedMs, int &h, int &d) {
        h = qMin(barHeight, int(elapsedMs / beatMs) / 2);
        d = -h + qMin(2 * h, allowed);
    };

    bool running = m_active.contains(slot) && m_active.value(slot) == fid
                && efx->isRunning() && efx->stopped() == false;
    if (running && m_sweepShown.value(group) == sw)
    {
        // the pitch fader drifts the clock: keep the figure on the beat -
        // and the ENERGY fader resizes it (the live ratio above)
        if (qAbs(int(efx->duration()) - int(ms)) > int(ms / 50))
            efx->setDuration(ms);
        if (laser && sw.shape >= 0)
        {
            int h = 0, d = 0;
            barGrow(qreal(efx->elapsed()), h, d);
            // (review 294: the EFX clock never resets, so after 26 beats barGrow
            // handed the slider's new size over at once - a unit a beat instead,
            // either way; the centre follows from the same h, so it stays safe)
            h = qBound(int(efx->height()) - 1, h, int(efx->height()) + 1);
            d = -h + qMin(2 * h, allowed);
            int y = qBound(0, 127 + d, 255);
            // down one step a beat (a fader pushed up, a figure growing past
            // the fader's allowance); up - the safe way - at once
            const int cur = int(efx->yOffset());
            if (y > cur)
                y = qMin(y, cur + 1);
            // R401_GROW_ONE (harness top98 / the 98 % night: the centre a unit
            // down and the size a unit up on the same beat put the lowest point
            // two units down at once, lit - 2.01 with the fine channel). One or
            // the other a beat: the size first, the centre on the next
            if (y > cur && h > int(efx->height()))
                y = cur;
            // ... and a lift of two units or more is a reposition: dark for
            // the beat (runde 303, bane B's headless B23 R2 - the fader from
            // 100 to 50 % lifted the bars 8 units lit; REGLER: mørke ved
            // genplacering). The caller puts the group in darkGroups.
            const bool lifted = cur - y >= 2;
            // ... and the light goes BEFORE the mirror moves (review 305, as R1):
            // setYOffset() is read by the MasterTimer thread at once, and the
            // darkGroups the caller sets only reach the levels later in tick()
            if (lifted && m_flashHeld.contains(group) == false)
            {
                const TrackGroup &lg = m_groups.value(group);
                stopSlot("col:" + group, true);
                stopSlot("mot:" + group, true);
                for (int i = 0; i < lg.parts.count(); i++)
                    stopSlot(partSlot(group, i), true);
            }
            // R401_LIFT_TWO_BEATS (harness: wobble, latch_up - the ENERGY
            // fader moved and the bars' figure jumped 3-6 units with the light
            // on). The stops above take a MasterTimer tick; setYOffset() is read
            // at once - the mirror won. So a jump (a lift of two, or centre and
            // size together three or more) waits: this beat the light goes and
            // the figure stands, the next beat it moves in the dark, the beat
            // after it is lit again
            const int jump = qAbs(cur - y) + qAbs(int(efx->height()) - h);
            const bool moveDark = (lifted || jump >= 3) && m_flashHeld.contains(group) == false;
            if (moveDark && m_laserMoveDark.contains(group) == false)
            {
                const TrackGroup &lg = m_groups.value(group);
                stopSlot("col:" + group, true);
                stopSlot("mot:" + group, true);
                for (int i = 0; i < lg.parts.count(); i++)
                    stopSlot(partSlot(group, i), true);
                m_laserMoveDark.insert(group);
                return true;
            }
            m_laserMoveDark.remove(group);
            // the centre first: lifted before the figure grows under it
            if (efx->yOffset() != y)
                efx->setYOffset(y);
            if (efx->height() != h)
                efx->setHeight(h);
            return lifted || moveDark;
        }
        if (sw.shape >= 0 && efx->width() != width)
            efx->setWidth(width);
        if (sw.shape >= 0 && efx->height() != height)
            efx->setHeight(height);
        return false;
    }
    // (Point 4 - a new figure taking over from where the heads actually are -
    // was begun in runde 162 as a call to EFXFixture::requestPointTransition()
    // here. The method was never written, so the tree did not build. Taken
    // out in runde 164: it needs its own round, on the EFX engine, with the
    // laser limits checked, and it cannot be judged without the rig. The hook
    // point is right - non-laser relative figures only, a blend of about four
    // beats clamped to 0.6-3 s - so this is where it goes back in.)
    // reconfigured live: a stop and a start in the same tick would leave the
    // EFX stopped (stop() only asks; the timer thread does it later)

    // no figure but a jitter: a figure of size zero is a still point off the aim
    // a bar starts at size nought on its aim (barGrow) - unless the EFX is
    // still running (a soft stop taken over, a reconfigure): then its clock
    // is live and the size and centre are read from it, together
    if (laser && sw.shape >= 0)
    {
        const bool live = efx->isRunning() && efx->stopped() == false;
        barGrow(live ? qreal(efx->elapsed()) : 0.0, height, dy);
    }
    // R401_LIFT_TWO_BEATS: ... and a new figure on a bar EFX that is running
    // (a fader jump redraws it) takes its new centre and size the same way -
    // the light first, the mirror a beat later
    bool reconfDark = false;
    if (laser && sw.shape >= 0 && m_flashHeld.contains(group) == false
        && efx->isRunning() && efx->stopped() == false && m_active.contains(slot))
    {
        const int jump = qAbs(int(efx->yOffset()) - qBound(0, 127 + dy, 255))
                       + qAbs(int(efx->height()) - height) + qAbs(int(efx->width()) - width);
        if (jump >= 3)
        {
            if (m_laserMoveDark.contains(group) == false)
            {
                const TrackGroup &lg = m_groups.value(group);
                stopSlot("col:" + group, true);
                stopSlot("mot:" + group, true);
                for (int i = 0; i < lg.parts.count(); i++)
                    stopSlot(partSlot(group, i), true);
                m_laserMoveDark.insert(group);
                return true;
            }
            reconfDark = true;
        }
    }
    m_laserMoveDark.remove(group);
    efx->setAlgorithm(sw.shape < 0 ? EFX::Circle : EFX::Algorithm(sw.shape));
    efx->setYOffset(qBound(0, 127 + dy, 255));     // the centre before the size
    efx->setWidth(sw.shape < 0 ? 0 : width);
    efx->setHeight(sw.shape < 0 ? 0 : height);
    efx->setRotation(sw.rotation);
    efx->setXOffset(qBound(0, 127 + sw.dx, 255));
    efx->setIsRelative(true);
    efx->setXFrequency(sw.fx);
    efx->setYFrequency(sw.fy);
    efx->setPropagationMode(sw.spread == 1 ? EFX::Asymmetric : (sw.spread == 2 ? EFX::Serial : EFX::Parallel));
    efx->setRunOrder(Function::Loop);
    efx->setDirection(Function::Forward);
    efx->setDuration(ms);
    int i = 0;
    foreach (EFXFixture *ef, efx->fixtures())
    {
        ef->setDirection((sw.mirror && (i % 2) == 1) ? Function::Backward : Function::Forward);
        // R383_LASER_FAST: the bars' forms - unison, middle out, the wave back
        const int nFx = int(efx->fixtures().count());
        const int step = sw.laserForm == 1 ? 0
                       : sw.laserForm == 2 ? qAbs(2 * i - (nFx - 1)) / 2
                       : sw.laserForm == 3 ? (nFx - 1 - i) : i;
        ef->setStartOffset((sw.fan * step) % 360);
        i++;
    }
    m_sweepShown.insert(group, sw);
    run(slot, fid, 1.0, 0, true);
    return reconfDark;               // R401_LIFT_TWO_BEATS: dark on the beat it moved
}

QString TrackEngine::sweepName(const TrackSweep &sw) const
{
    if (sw.shape < 0)
        return QString();
    QString shape = EFX::algorithmToString(EFX::Algorithm(sw.shape)).toLower();
    QString rel = sw.spread == 1 ? "~" : (sw.spread == 2 ? ">" : (sw.mirror ? "><" : (sw.fan ? "*" : "")));
    if (sw.laserForm > 0)                // R383: = unison, <> middle out, << the wave back
        rel = sw.laserForm == 1 ? QStringLiteral("=") : (sw.laserForm == 2 ? QStringLiteral("<>") : QStringLiteral("<<"));
    return QString("(%1%2 %3 %4b)").arg(shape).arg(rel).arg(sw.width).arg(qMax(sweepFloor(sw), sw.beats));
}

void TrackEngine::stopSweeps()
{
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("efx:"))
            stopSlot(slot, true);
    }
    m_sweep.clear();
    m_sweepShown.clear();
}

void TrackEngine::checkConflicts(const QSet<QString> &castSet)
{
    // A group the engine holds at zero that still shows light on its master
    // dimmer is being driven by something else - usually a Group Dimmer
    // slider on the Virtual Console. HTP hides that; say it out loud.
    QStringList found;
    if (m_palette.isEmpty())
        found << tr("no colours found - the engine needs colour scenes, or RGB fixtures to make them from");
    if (m_blendSkipped.isEmpty() == false)
        found << tr("not used, built on an override/filter scene: %1")
                 .arg(QStringList(m_blendSkipped.values()).join(", "));
    // roles and groups only change with the table: counted once per table
    if (m_sharedLooks < 0)
    {
        m_sharedLooks = 0;
        for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
            if (it.value().role >= 0 && it.value().role != ENGINE_ROLE_IDLE && it.value().groups.count() > 1)
                m_sharedLooks++;
    }
    const int sharedLooks = m_sharedLooks;
    if (sharedLooks)
        found << tr("%1 whole-room looks reserved for VC/START; AUTO uses one group per look").arg(sharedLooks);
    QList<Universe *> universes = m_doc->inputOutputMap()->universes();

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        bool fading = false;
        foreach (quint32 part, g.parts)
        {
                if (m_fadeAttr.contains(part))
                    fading = true;
        }
        if (g.hasDimmer == false || castSet.contains(key) || m_groupOff.contains(key)
            || fading || m_flash)
        {
            m_conflictBeats.remove(key);
            continue;
        }

        bool lit = false;
        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            quint32 ch = dimmerChannel(fxi);
            int uni = int(fxi->universe());
            if (ch == QLCChannel::invalid() || uni < 0 || uni >= universes.count()
                || universes.at(uni) == nullptr)
                continue;
            const QByteArray *values = universes.at(uni)->postGMValues();
            int addr = int(fxi->address() + ch);
            if (values != nullptr && addr < int(values->size()) && uchar(values->at(addr)) > 10)
            {
                lit = true;
                break;
            }
        }

        // a fading dimmer or a flash hit reads as lit for a moment; only a
        // level that stays for two bars is somebody else's hand
        int beats = lit ? m_conflictBeats.value(key, 0) + 1 : 0;
        m_conflictBeats.insert(key, beats);
        if (beats >= 8)
            found << tr("%1 is lit from elsewhere (a slider?)").arg(key);
    }

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.hasDimmer == false && m_groupOff.contains(key) == false
            && castSet.contains(key))
            found << tr("%1 has no master dimmer - on/off only").arg(key);

        // The heads have had this warning all along, but it is guarded by
        // g.heads - and a laser bar is never a head: heads needs pan AND tilt
        // AND not lasers, and the bars have no pan at all. So the group that
        // most needs telling was the one group that never got told. The aims
        // loop in tick() skips a group with no position candidate entirely,
        // which means the bars keep pointing wherever they last were.
        if (g.lasers && m_groupOff.contains(key) == false
            && candidates(ENGINE_ROLE_POSITION, key).isEmpty())
            found << tr("%1 has no aim left - the beams stay where they are").arg(key);

        // homePosition() ignores the ban on purpose (see there), so a banned
        // home keeps running. That is the right call and a confusing sight:
        // the row says BANNED and the bars keep going there. Say it out loud
        // rather than leave him hunting for it.
        quint32 home = g.lasers ? homePosition(key) : Function::invalidId();
        if (g.lasers && m_groupOff.contains(key) == false && home == Function::invalidId())
            found << tr("%1 has no HOME aim - no scene of the group says UP/UPP; "
                        "the beams cannot park and no aim can be measured").arg(key);
        if (home != Function::invalidId() && m_funcs.value(home).banned)
            found << tr("%1: the home aim is banned, but it is still used - "
                        "the beams have to park somewhere").arg(key);
    }

    // a group the engine cannot make colours for (an animation laser) and
    // that lacks a palette colour will sit dark whenever that colour runs
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.generatable() || m_groupOff.contains(key))
            continue;
        QStringList missing;
        foreach (const QString &colour, m_palette)
        {
                if (colourFunction(key, colour) == Function::invalidId())
                    missing << colour;
        }
        if (missing.isEmpty() == false)
        {
            // Which of the two it is matters. Missing SOME colours means the
            // group sits dark whenever that colour comes round. Missing them
            // ALL means candidates() is empty, and a group with no colour
            // candidate never enters `eligible` in tick() - it is out of the
            // cast entirely and dark for the rest of the night.
            //
            // A ban can cause either. That is allowed: he said never, and
            // never is never - the arithmetic bends, the ban does not. But he
            // is told it was the ban, because "I banned one thing and a whole
            // group went away" is not something to work out during a gig.
            bool byBan = false;
            for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin();
                 it != m_funcs.constEnd(); ++it)
            {
                if (it.value().banned && it.value().role == ENGINE_ROLE_COLOR
                    && it.value().groups.contains(key))
                {
                    byBan = true;
                    break;
                }
            }
            QString why = byBan ? tr(" (banned)") : QString();
            if (candidates(ENGINE_ROLE_COLOR, key).isEmpty())
                found << tr("%1 has no colour at all%2 - it drops out of the cast and stays dark")
                         .arg(key).arg(why);
            else
                found << tr("%1 has no scene for %2%3").arg(key).arg(missing.join(", ")).arg(why);
        }
    }

    // Moving heads whose pan changes while none of our sweeps runs are being
    // steered from elsewhere - the Light Rider app on the iPad, usually.
    // Two programs on one head is a fight nobody wins.
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.heads == false || m_groupOff.contains(key))
            continue;

        quint32 mf = m_active.value("mot:" + key, Function::invalidId());
        bool ourSweep = (mf != Function::invalidId() && m_funcs.value(mf).type != int(Function::SceneType))
                     || m_active.contains("efx:" + key)
                     // ... or one of our AUTO head figures in the pos: slot (runde 255)
                     || m_funcs.value(m_active.value("pos:" + key, Function::invalidId())).type == int(Function::ChaserType);
        bool moved = false;
        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            quint32 pan = QLCChannel::invalid();
            for (quint32 i = 0; i < fxi->channels() && pan == QLCChannel::invalid(); i++)
                if (fxi->channel(i) != nullptr && fxi->channel(i)->group() == QLCChannel::Pan
                    && fxi->channel(i)->controlByte() == QLCChannel::MSB)
                    pan = i;              // coarse only: fine swings on every degree
            int uni = int(fxi->universe());
            if (pan == QLCChannel::invalid() || uni < 0 || uni >= universes.count() || universes.at(uni) == nullptr)
                continue;
            const QByteArray *values = universes.at(uni)->postGMValues();
            int addr = int(fxi->address() + pan);
            if (values == nullptr || addr >= int(values->size()))
                continue;
            int now = int(uchar(values->at(addr)));
            if (m_lastPan.contains(fid) && qAbs(now - m_lastPan.value(fid)) > 2)
                moved = true;
            m_lastPan.insert(fid, now);
        }
        // a position change of ours moves them once; a sweep of ours moves
        // them all the time - neither counts
        int beats = (moved && ourSweep == false && m_position.contains(key) && castSet.contains(key))
                    ? m_headMoveBeats.value(key, 0) + 1 : 0;
        m_headMoveBeats.insert(key, beats);
        if (beats >= 6)
            found << tr("%1 is moved from elsewhere (Light Rider?)").arg(key);
        if (g.heads && candidates(ENGINE_ROLE_POSITION, key).isEmpty())
            found << tr("%1 has no position scene - the heads cannot move").arg(key);
    }

    m_warnings = found;
}

QStringList TrackEngine::warnings() const { return m_warnings; }

void TrackEngine::calm(int bars)
{
    // bars <= 0 ends it early
    logSignal(bars <= 0 ? QStringLiteral("sig:calm-off") : QStringLiteral("sig:calm"));
    m_calmUntil = bars <= 0 ? 0 : m_lastBeat + bars * 4;
    if (bars > 0)
        stopEcho(); // a pending half-beat accent must not fire after CALM
    // the heads' figure is redrawn at the new pace on the next beat: calm
    // slows it to a break's drift, and calm-off lets the section draw again.
    // (A calm that simply runs out keeps the slow figure until the next
    // section or 16-bar redraw - a gentle way back, not a jump.)
    m_sweep.clear();
    emit liveChanged();
}

void TrackEngine::next()
{
    // The strongest free signal there is: whatever was on stage, he did not
    // want it. How long it had been up is in the log already - the beat lines
    // before this one say when funcs last changed.
    logSignal(QStringLiteral("sig:next"));
    // between tracks (or under the start scene) no beat takes it: it fired on
    // the next track's first beat and threw away the mix's colour (r199)
    if (m_lastState.isEmpty())
        return;
    m_forceNext = true;
    emit liveChanged();
}

void TrackEngine::logSignal(const QString &tag)
{
    // The section kind rides along: the state column is where it normally
    // lives, and a marker takes that column over. Without it a verdict cannot
    // be put in the right bucket by anything reading the log afterwards -
    // the engine knows from m_lastState, but the file would not, and the file
    // is what the rebuild tool has. Same principle as the track title: every
    // line self-contained, nothing that has to be recovered by scanning back.
    //
    // Commas out: the log is read with a plain split(','), and a group called
    // "Strobes, All" would shift every column after this one.
    // The SAME string rateBucket() files by - m_lookState with m_lastState as
    // the fallback - so the log and the engine cannot disagree about which
    // section a verdict belongs to. They differ only while HOLD is on, and
    // that is exactly when getting it wrong would matter. Every intervention
    // is about the look that is on stage, not about the bar the track has
    // reached, so this holds for the sig: lines too.
    QString sect = m_lookState.isEmpty() ? m_lastState : m_lookState;
    QString mark = QString(tag).replace(',', ' ');

    // A verdict is filed against the snapshot markVerdictPoint() took when
    // the finger landed - rate() and rateGroup() both read verdictStage()
    // and verdictBucket(). A long press, a look at the list and a tap can
    // take seven seconds, and in that time the section can turn or a thumb
    // down can already have swapped the look. The line has to describe the
    // same stage the engine credited, or the rebuild tool blames the look
    // that came AFTER. So for rate lines the funcs column and the @section
    // come from the snapshot too: m_active is swapped for the duration of
    // the one write (a QMap copy is a pointer), and the section is spelled
    // from the bucket - intro and outro both read "break", which is the
    // bucket the tool puts them in anyway.
    bool verdict = tag.startsWith(QStringLiteral("rate"));
    bool frozen = verdict && &verdictStage() != &m_active;
    QMap<QString, quint32> now;
    if (frozen)
    {
        now = m_active;
        m_active = m_verdictActive;
        static const char *const names[ENGINE_RATE_BUCKETS] = { "break", "build", "drop", "normal" };
        // ... but never invent one: with no section there is no track, and
        // rate() skips the verdict - the line must stay bare so the tool does
        int b = verdictBucket();
        if (sect.isEmpty() == false && b >= 0 && b < ENGINE_RATE_BUCKETS)
            sect = QLatin1String(names[b]);
    }
    if (sect.isEmpty() == false)
        mark += QLatin1Char('@') + QString(sect).replace(',', ' ');
    logBeat(mark, m_logBeatNo, m_logLevel, m_logEnergy, m_logSection);
    if (frozen)
        m_active = now;
}

int TrackEngine::rateBucket() const
{
    // The same four the engine already picks by. intro and outro count as
    // break: they are the same job for the lights, and splitting them would
    // spread already thin evidence over six buckets instead of four.
    // m_lookState, not m_lastState: see tick(). They are the same until HOLD
    // freezes the look, and then only the first one is still true about what
    // is actually on stage. Falls back for the case where a hold was already
    // on before the first tick of a track.
    const QString &st = m_lookState.isEmpty() ? m_lastState : m_lookState;
    if (st == QStringLiteral("build"))
        return ENGINE_RATE_BUILD;
    if (st == QStringLiteral("drop"))
        return ENGINE_RATE_DROP;
    if (st == QStringLiteral("break") || st == QStringLiteral("intro")
        || st == QStringLiteral("outro"))
        return ENGINE_RATE_BREAK;
    return ENGINE_RATE_NORMAL;
}

int TrackEngine::rateWeight(const TrackFuncInfo &info) const
{
    // How many times this one stands in the rotation. Not a probability: the
    // cursor still walks the list in order, so nothing becomes a habit and
    // "why did it pick that" always has an answer.
    //
    // The floor is 1, never 0. An earlier version returned 0 for a badly rated
    // program and leaned on the callers to "keep a floor" - but the caller's
    // floor only fires when EVERY candidate scored 0, so as soon as one decent
    // program was in the list the bad ones became unreachable. That is
    // "never", and never is the ban flag's job: a thing the operator decides
    // and can see, not something three votes on one night arrive at quietly.
    //
    // So the scale is 1..4 around a neutral 2. Disliked is half as likely as
    // unrated, the best is four times as likely as the worst, and nothing is
    // impossible.
    //
    // The score is net votes PER SHOWING, not net votes. A thumb lands on
    // every program on stage - four to six of them - and only one of them was
    // usually the thing that was wrong. Raw counts therefore punish the
    // ordinary: the base group's look and the haze are up for most of the
    // night, so they collect every verdict there is and drift to whatever the
    // room felt like on average, while the distinctive program that actually
    // caused it got one vote out of its three appearances. Dividing by stage
    // time turns that around - the same three thumbs mean nothing on a
    // program that ran all night and everything on one that showed up twice.
    // It costs the operator no extra taps, which is the point.
    if (m_ratingOn == false)
        return 1;
    int b = rateBucket();
    qreal d = info.up[b] * ENGINE_RATE_UPVOTE - info.down[b];
    if (qFuzzyIsNull(d))
        return 2;
    // Less than one showing still counts as one: a brand new program with a
    // single thumb should move, not be divided into silence.
    qreal shows = qMax(1.0, info.seen[b] / ENGINE_RATE_EXPOSURE);
    qreal s = d / shows;
    // Two gates, and both have to open. The rate says how strongly the
    // verdicts lean; qAbs(d) says whether there is enough of them to mean
    // anything.
    //
    // With UPVOTE at 3 the two directions land differently on purpose, and
    // it is the right way round. One thumb up gives d = 3 and clears the
    // gate on its own - he went out of his way to say it, and he only does
    // that for something worth keeping. One thumb down gives d = -1 and
    // does not: a down is the reflex when something is in his face, it lands
    // on every program on stage, and only one of them was the problem. Say
    // it twice, or aim it with a long press, and it moves.
    if (d >= 2 && s >= 0.50)
        return 4;
    if (s >= 0.15)
        return 3;
    if (d <= -2 && s <= -0.50)
        return 1;
    return 2;
}

quint32 TrackEngine::pickWeighted(const QList<TrackFuncInfo *> &okAll, int cursor) const
{
    if (okAll.isEmpty())
        return Function::invalidId();
    // The cooldown: a programme that ran in the last twelve minutes steps
    // back and lets the rested ones take the draw - so a night works its
    // way through the pool instead of circling its first few. Soft, not a
    // ban: when fewer than two rested ones are left the whole list is back
    // in, and a small pool simply repeats (Tobias, 2026-09-15: "der maa
    // gerne vaere gentagelser, bare ikke saa ofte ... vi kan loebe toer").
    // Judged against the clock reading of the last section change, not
    // against now, so the list is the same on every beat of a section. A
    // programme whose last use is AFTER that reading is the one running in
    // this very section - it stays in, or it would knock itself out.
    QList<TrackFuncInfo *> ok;
    qint64 since = m_cooldownMs < 0 ? m_clock.elapsed() : m_cooldownMs;
    foreach (TrackFuncInfo *info, okAll)
    {
        qint64 last = m_recentUse.value(info->id, -1);
        if (last < 0 || last > since || since - last >= ENGINE_COOLDOWN_MS)
            ok.append(info);
    }
    if (ok.count() < 2)
        ok = okAll;
    // Switch off: not "the same thing by another route" but the identical
    // line this used to be. Nothing to reason about when a night goes wrong.
    if (m_ratingOn == false)
        return ok.at(qAbs(cursor) % ok.count())->id;

    // Repeat the LIST, not the entry. positionFunction's own idiom is
    // `tagged + tagged + plain`, and the distinction turns out to be the
    // whole thing: appending a favoured program three times in a row gave
    // AAABCDD, so three consecutive sections ran the SAME look. More often
    // is what was wanted; back to back is a stuck record. Pass k holds
    // everything with weight >= k, which gives ABCDADA - the favoured come
    // round more often and still take their turn.
    int top = 1;
    foreach (TrackFuncInfo *info, ok)
        top = qMax(top, rateWeight(*info));

    QList<TrackFuncInfo *> pool;
    for (int k = 1; k <= top; k++)
    {
        foreach (TrackFuncInfo *info, ok)
        {
            if (rateWeight(*info) >= k)
                pool.append(info);
        }
    }
    // rateWeight() never returns less than 1, so this cannot fire today. It
    // stays because the alternative to a wrong weight scale is a section with
    // no light in it, and that is not a thing to discover on a Saturday.
    if (pool.isEmpty())
        return ok.at(qAbs(cursor) % ok.count())->id;
    return pool.at(qAbs(cursor) % pool.count())->id;
}

bool TrackEngine::banned(quint32 fid) const
{
    return m_funcs.contains(fid) && m_funcs.value(fid).banned;
}

void TrackEngine::setBanned(quint32 fid, bool on)
{
    if (m_funcs.contains(fid) == false || m_funcs[fid].banned == on)
        return;
    // Not on our own. A generated scene exists because the group had no
    // palette colour of its own, so banning one leaves that group with
    // nothing to be - and the flag could not be kept anyway: generated
    // scenes are matched by NAME when they already exist, but a fresh one
    // takes whatever id addFunction() hands out. Saved against that id, the
    // ban would after a reload sit on a completely different program, and
    // silently. Refuse at the source rather than filter it at save time.
    if (m_funcs[fid].generated)
        return;
    m_funcs[fid].banned = on;
    invalidateCandidates();          // no rebuild here (see below), so say it
    saveRoles();
    logSignal((on ? QStringLiteral("sig:ban:") : QStringLiteral("sig:unban:"))
              + QString::number(fid));
    // NOT m_dirty. The flag is already set in m_funcs, and that is where both
    // candidates() and table() read it - a rebuild would restore the same
    // value from QSettings and change nothing. It would however walk all 2892
    // functions again while the show is running, and autoAssign() carries the
    // scar from exactly that: "rebuilding the whole table tore the live page's
    // cast and colour tiles down under the operator's finger, twice."
    // Tapping BAN in SETUP is no different.
    emit tableChanged();
}

bool TrackEngine::ratingEnabled() const { return m_ratingOn; }

void TrackEngine::setRatingEnabled(bool on)
{
    if (on == m_ratingOn)
        return;
    m_ratingOn = on;
    QSettings().setValue(SETTINGS_ENGINE_RATINGON, on);
    // in the log too, so a night that felt wrong can be read back: was this
    // the engine's own rotation, or was it steering by the counts?
    logSignal(on ? QStringLiteral("sig:rating-on") : QStringLiteral("sig:rating-off"));
    emit tableChanged();
}


QMap<QString, QString> TrackEngine::autoLookKeys(const QSet<QString> &cast, qreal energy) const
{
    QMap<QString, QString> keys;
    QStringList groups = cast.values();
    groups.sort();
    QStringList rig;
    foreach (const QString &group, groups)
        rig << QString::fromLatin1(group.toUtf8().toHex()) + ':' + QString::number(m_groups.value(group).fixtures.count());
    QString context = QString("v1|%1|e%2|%3|%4|%5|d%6|lead=%7")
        .arg(rateBucket()).arg(qMin(3, int(qBound(0.0, energy, 1.0) * 4)))
        .arg(m_colour).arg(m_accent && m_dropStyle > 0 ? m_accentPick : QString())
        .arg(rig.join(';')).arg(m_dropStyle).arg(QString::fromLatin1(m_rhythmLead.toUtf8().toHex()));
    QStringList room;
    foreach (const QString &group, groups)
    {
        TrackMove mv = m_moves.value(group);
        TrackSweep sw = m_sweep.value(group);
        // Coarse bands let a 17-beat circle learn from a 16-beat circle.
        // Pulse shape, own-chase choice, sweep, zoom and accent belong
        // together; a transient generated function ID says nothing about them.
        QStringList f;
        f << QString::number(mv.pattern) << QString::number(mv.stepBeats)
          << QString::number(mv.subSteps) << QString::number(mv.pulseOn)
          << QString::number(qBound(0, int(mv.pulse * 3), 2))
          << QString::number(mv.bare) << QString::number(mv.ownChaser)
          << QString::number(mv.breatheBars > 0) << QString::number(mv.colourBars)
          << QString::number(sw.shape) << QString::number(sw.width / 16)
          << QString::number(sw.height / 16) << QString::number(qMax(sweepFloor(sw), sw.beats) / 8)
          << QString::number(sw.spread) << QString::number(sw.mirror)
          // the zoom in its three old bands (narrow / mid / wide), so the saved
          // ratings still match and a build's step a bar does not split them (r214)
          << QString::number(sw.fan > 0) << QString::number(m_zoom.value(group, -1) < 0 ? -1 : (m_zoom.value(group, -1) + 2) / 4)
          << QString::number(m_accent && m_dropStyle > 0 && group == m_accentGroup);
        QString recipe = QString::fromLatin1(group.toUtf8().toHex()) + ':' + f.join(',');
        keys.insert(group, context + '|' + recipe);
        room << recipe;
    }
    if (room.isEmpty() == false)
        keys.insert(QString(), context + "|room|" + room.join(';'));
    return keys;
}

int TrackEngine::autoLookWeight(const QMap<QString, QString> &keys, const QString &group) const
{
    if (m_ratingOn == false || m_fullAuto == false)
        return 2;
    auto weight = [this](const QString &key) {
        QPair<int, int> votes = m_autoRatings.value(key);
        qreal up = votes.first, down = votes.second, similarity = 1.0;
        if (up + down == 0 && key.isEmpty() == false)
        {
            // Exact random recipes rarely repeat. Learn from the closest
            // combinations in the SAME section/energy/palette/cast, allowing
            // at most one fifth of the coarse features to differ. Different
            // groups and room-vs-group judgements never share credit.
            int cut = key.lastIndexOf('|');
            QString prefix = key.left(cut + 1);
            QString tail = key.mid(cut + 1);
            QString identity = tail.left(tail.indexOf(':'));
            QStringList features = tail.split(',');
            int closest = int(features.count()) / 5 + 1;
            for (auto it = m_autoRatings.constBegin(); it != m_autoRatings.constEnd(); ++it)
            {
                if (it.key().startsWith(prefix) == false)
                    continue;
                QString other = it.key().mid(cut + 1);
                if (other.left(other.indexOf(':')) != identity)
                    continue;
                QStringList values = other.split(',');
                if (values.count() != features.count())
                    continue;
                int distance = 0;
                for (int i = 0; i < features.count(); i++)
                    if (features.at(i) != values.at(i)) distance++;
                if (distance >= int(features.count()) / 5 + 1 || distance > closest)
                    continue;
                if (distance < closest) { up = 0; down = 0; closest = distance; }
                up += it.value().first;
                down += it.value().second;
            }
            similarity = 1.0 - qreal(closest) / qMax(1, int(features.count()));
        }
        // Prior evidence keeps a single dislike from banishing a recipe.
        qreal score = similarity * (3.0 * up - down) / (up + down + 4.0);
        return qBound(1, 2 + int(qRound(score)), 4);
    };
    // Room and group evidence both count. Neutral (2) contributes zero;
    // a pointed dislike must still matter when the complete room is new.
    return qBound(1, weight(keys.value(QString())) + weight(keys.value(group)) - 2, 4);
}

const QMap<QString, QString> &TrackEngine::verdictAutoKeys() const
{
    if (m_verdictMs >= 0 && m_clock.elapsed() - m_verdictMs <= 20000)
        return m_verdictAutoKeys;
    return m_autoStageKeys;
}

bool TrackEngine::rateAutoLook(int verdict, const QString &group)
{
    const QMap<QString, QString> &keys = verdictAutoKeys();
    QSet<QString> once;
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it)
    {
        // A pointed verdict is evidence about ONE group, not the room.
        if (group.isEmpty() == false && it.key() != group)
            continue;
        if (once.contains(it.value()))
            continue;
        once.insert(it.value());
        QPair<int, int> &votes = m_autoRatings[it.value()];
        int amount = group.isEmpty() ? 1 : ENGINE_RATE_AIMED;
        if (verdict >= 0) votes.first = qMin(1000000, votes.first + amount);
        else votes.second = qMin(1000000, votes.second + amount);
    }
    return once.isEmpty() == false;
}

void TrackEngine::markVerdictPoint()
{
    m_verdictActive = m_active;
    m_verdictAutoKeys = (m_fullAuto && m_blackout == false && m_flash == false
                         && m_mixing == false && m_override.isEmpty())
                       ? m_autoStageKeys : QMap<QString, QString>();
    m_verdictBucket = rateBucket();
    m_verdictMs = m_clock.elapsed();
}

const QMap<QString, quint32> &TrackEngine::verdictStage() const
{
    // Twenty seconds is long enough for a long press, a look at the list and
    // a considered tap; past that he has wandered off and come back, and the
    // honest answer is what is on stage now. m_clock never resets, so there
    // is no wrap-around to reason about.
    if (m_verdictMs >= 0 && m_clock.elapsed() - m_verdictMs <= 20000)
        return m_verdictActive;
    return m_active;
}

int TrackEngine::verdictBucket() const
{
    if (m_verdictMs >= 0 && m_clock.elapsed() - m_verdictMs <= 20000 && m_verdictBucket >= 0)
        return m_verdictBucket;
    return rateBucket();
}

QVariantList TrackEngine::onStage() const
{
    // One row per group, named by the program that carries its look. The
    // colour slot is the look; a motion or a sweep is what it does. Groups
    // with neither are left out - there is nothing there to blame.
    QVariantList out;
    const QMap<QString, quint32> &stage = verdictStage();
    foreach (const QString &key, m_groupOrder)
    {
        // The first slot that holds a program HE can be asked about. Not the
        // first slot that holds anything: in FULL AUTO the colour slot is a
        // generated TRACK Colour scene on nearly every group, and stopping
        // there dropped the group from the list although his own chase was
        // running on mot: - and rateGroup() would have credited it.
        quint32 fid = Function::invalidId();
        // not "slots": under Qt that word is a macro and the line does not parse
        static const char *const slotPrefixes[] = { "col:", "mot:", "efx:" };
        for (const char *prefix : slotPrefixes)
        {
            quint32 cand = stage.value(QLatin1String(prefix) + key, Function::invalidId());
            if (cand == Function::invalidId() || m_funcs.contains(cand) == false
                || m_funcs.value(cand).generated)
                continue;
            fid = cand;
            break;
        }
        if (fid == Function::invalidId() && verdictAutoKeys().contains(key) == false)
            continue;
        QVariantMap row;
        row.insert("group", key);
        row.insert("name", fid == Function::invalidId() ? tr("FULL AUTO") : m_funcs.value(fid).name);
        out.append(row);
    }
    return out;
}

void TrackEngine::rateGroup(int verdict, const QString &group)
{
    logSignal((verdict >= 0 ? QStringLiteral("rate+1:") : QStringLiteral("rate-1:")) + group);
    if (m_lastState.isEmpty())
        return;

    // Exactly the programs sitting on this one group. No spreading, no
    // exposure arithmetic to undo it: he pointed, so the guess is not needed.
    int b = verdictBucket();
    const QMap<QString, quint32> &stage = verdictStage();
    QSet<quint32> counted;
    bool touched = rateAutoLook(verdict, group);
    for (QMap<QString, quint32>::const_iterator it = stage.constBegin(); it != stage.constEnd(); ++it)
    {
        if (slotGroup(it.key()) != group || counted.contains(it.value()))
            continue;
        counted.insert(it.value());
        QHash<quint32, TrackFuncInfo>::iterator fi = m_funcs.find(it.value());
        if (fi == m_funcs.end() || fi.value().generated)
            continue;
        if (verdict >= 0)
            fi.value().up[b] += ENGINE_RATE_AIMED;
        else
            fi.value().down[b] += ENGINE_RATE_AIMED;
        touched = true;
    }
    if (touched)
    {
        saveRoles();
        emit tableChanged();
    }
    // Told to its face that this is wrong, the light should not keep doing it
    // for another thirty seconds. The counting above already happened, and it
    // used the snapshot, so changing the look now cannot corrupt the verdict.
    if (verdict < 0)
        next();
    // The snapshot has been used. Left valid, a second verdict inside the
    // twenty seconds that did NOT come through onPressed - a script, a
    // future shortcut - would be filed against a stage that is gone.
    m_verdictMs = -1;
}

void TrackEngine::rate(int verdict)
{
    // A thumb goes in the tracklog as an ordinary line, so one parser reads
    // beats and verdicts alike: same columns, and the funcs column already
    // says what was on stage when the thumb was pressed. The numbers come
    // from the last beat logged, so the rating carries the energy and the
    // section it was given in.
    //
    // Deliberately no scoring, no average, no effect on the engine. Two or
    // three nights of this first, then we look at whether the verdicts are
    // even consistent before anything starts choosing by them.
    logSignal(verdict >= 0 ? QStringLiteral("rate+1") : QStringLiteral("rate-1"));

    // ... and count it. Only against the operator's OWN programs: the engine's
    // generated scenes are rebuilt from the fixtures every time the table is,
    // so a verdict on one is a verdict on something that will not exist in the
    // same shape tomorrow. The masks and the zoom scenes are plumbing, not a
    // look, and they are not in m_funcs as looks either.
    //
    // Every program on stage gets the same credit. That is the crude part, and
    // it is crude on purpose: which of the four was the one that made it is
    // exactly what we do not know yet, and guessing here would bake the guess
    // into the numbers. The log keeps the raw record, so a better rule can be
    // applied to the same nights later without losing anything.
    // Nothing is playing: release(), trackLoaded() and stopAll() all clear
    // m_lastState, so an empty one means there is no section to judge. What
    // is on stage between two tracks is the start scene, and EVERY start
    // scene runs - idle picks nothing, so a verdict on one cannot change
    // anything anyway. Counting it would only file noise under "normal" and
    // put a number in the SETUP row that means nothing.
    //
    // The line is still written to the log. It carries no @section, and the
    // rebuild tool already drops those, so the engine and the tool agree -
    // which they have to, or the two would drift apart over a season.
    if (m_lastState.isEmpty())
        return;

    int b = verdictBucket();
    const QMap<QString, quint32> &stage = verdictStage();
    bool touched = rateAutoLook(verdict);
    // Once per program, not once per slot: m_active is keyed by SLOT, and one
    // function can in principle hold two of them. candidates() makes that
    // unlikely today by demanding groups.count() == 1, but leaning on that
    // from over here means a change to the picker could quietly start
    // double-counting. One thumb, one verdict.
    QSet<quint32> counted;
    foreach (quint32 fid, stage.values())
    {
        if (counted.contains(fid))
            continue;
        counted.insert(fid);
        if (m_funcs.contains(fid) == false)
            continue;
        TrackFuncInfo &info = m_funcs[fid];
        if (info.generated)
            continue;
        if (verdict >= 0)
            info.up[b] += 1;
        else
            info.down[b] += 1;
        touched = true;
    }
    if (touched)
    {
        saveRoles();
        emit tableChanged();
    }
    if (verdict < 0)
        next();                    // same reasoning as rateGroup()
    m_verdictMs = -1;              // used, see rateGroup()
}

int TrackEngine::room() const { return m_room; }

void TrackEngine::setRoom(int room)
{
    // a hand on the dial ends the automatic evening
    m_roomAuto = false;
    saveNight();
    room = qBound(0, room, 3);
    if (room == m_room)
    {
        emit liveChanged();
        return;
    }
    m_room = room;
    m_moves.clear();             // the new energy draws new moves
    static const int percent[4] = { 35, 55, 75, 90 };
    m_roomSent = percent[m_room];
    emit roomChanged(m_roomSent);
    emit liveChanged();
}

QString TrackEngine::nightKey()
{
    return QDateTime::currentDateTime().addSecs(-12 * 3600).date().toString(Qt::ISODate);
}

void TrackEngine::saveNight() const
{
    QStringList trims;
    for (auto it = m_groupTrim.constBegin(); it != m_groupTrim.constEnd(); ++it)
        trims << it.key() + QLatin1Char('=') + QString::number(it.value(), 'f', 3);
    // all three together, always: a stamp written with only one of them
    // would carry last night's other two into tonight
    QSettings settings;
    settings.setValue(SETTINGS_ENGINE_NIGHT, nightKey());
    settings.setValue(SETTINGS_ENGINE_ROOMAUTO, m_roomAuto);
    settings.setValue(SETTINGS_ENGINE_GROUPTRIM, trims.join(';'));
}

bool TrackEngine::roomAuto() const { return m_roomAuto; }

void TrackEngine::setRoomAuto(bool on)
{
    if (on == m_roomAuto)
        return;
    m_roomAuto = on;
    saveNight();
    if (on)
    {
        m_roomSent = -1;             // hand the clock's value over right away
        announceRoom();
    }
    emit liveChanged();
}

int TrackEngine::roomPercent() const { return m_roomSent; }

void TrackEngine::loadClockCurve(const QSettings &settings)
{
    // Runde 211 (Tobias, 2026-09-24): "energi by time skal vaere pr. 15 minut
    // i perioden 20-03, baade koersel og knapperne". Twenty-eight points,
    // 20:00, 20:15 ... 02:45; the house closes at 03:00 (05:00 New Year).
    //
    // The six-point curve of runde 48 (21, 22, 23, 00, 01, 02 h, still until
    // 22:30) is read as it was and laid onto the quarters, so a curve set by
    // hand survives the change; it is also the default.
    m_clockCurve.clear();
    // Runde 233 (Tobias, 2026-09-26: "Du må gerne ændre energy-kurven til at
    // passe bedre til hvordan jeg satte den i løbet af aftenen"): the curve
    // read off his hand on 25-26 Sep, per quarter. It replaces the curve the
    // rig had stored then, and the old default - nothing he sets later.
    static const int fresh[ENGINE_CLOCK_POINTS] = { 0, 0, 0, 0,  0, 0, 0, 0,  0, 5, 15, 30,
                                                    40, 45, 55, 60,  65, 65, 65, 65,  65, 65, 65, 70,
                                                    80, 85, 90, 90 };
    static const char *const retired[] = {
        "0,0,0,0,0,0,0,0,0,0,10,10,20,25,30,35,50,60,70,80,90,83,90,88,90,100,100,90",
        "0,0,20,45,70,85" };
    {
        const QString raw = settings.value(SETTINGS_ENGINE_CLOCKCURVE, QString()).toString().remove(' ');
        bool renew = raw.isEmpty();
        for (uint i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
            renew = renew || raw == QLatin1String(retired[i]);
        if (renew)
        {
            for (int i = 0; i < ENGINE_CLOCK_POINTS; i++)
                m_clockCurve.append(fresh[i]);
            return;
        }
    }
    QStringList parts = settings.value(SETTINGS_ENGINE_CLOCKCURVE, QString()).toString().split(',', Qt::SkipEmptyParts);
    if (parts.count() == ENGINE_CLOCK_POINTS)
    {
        for (int i = 0; i < ENGINE_CLOCK_POINTS; i++)
        {
            bool ok = false;
            int v = parts.at(i).trimmed().toInt(&ok);
            m_clockCurve.append(ok ? qBound(0, v, 100) : 0);
        }
        return;
    }
    static const int dflt[6] = { 0, 0, 20, 45, 70, 85 };
    int six[6];
    for (int i = 0; i < 6; i++)
    {
        bool ok = false;
        int v = (parts.count() == 6) ? parts.at(i).trimmed().toInt(&ok) : 0;
        six[i] = ok ? qBound(0, v, 100) : dflt[i];
    }
    // the old anchors, minutes past 21:00: 21 h, 22 h held to 22:30, 23 h ... 02 h
    const int at[7] = { 0, 60, 90, 120, 180, 240, 300 };
    const int val[7] = { six[0], six[1], six[1], six[2], six[3], six[4], six[5] };
    for (int q = 0; q < ENGINE_CLOCK_POINTS; q++)
    {
        int m = q * 15 - 60;                 // minutes past 21:00
        int v = 0;
        if (m >= at[6])
            v = val[6];
        else if (m >= 0)
        {
            for (int k = 1; k < 7; k++)
            {
                if (m <= at[k])
                {
                    qreal f = qreal(m - at[k - 1]) / qreal(at[k] - at[k - 1]);
                    v = int(qRound(val[k - 1] + f * (val[k] - val[k - 1])));
                    break;
                }
            }
        }
        m_clockCurve.append(v);
    }
}

QVariantList TrackEngine::clockCurve() const
{
    QVariantList out;
    foreach (int v, m_clockCurve)
        out << v;
    return out;
}

void TrackEngine::cycleClockPoint(int index)
{
    if (index < 0 || index >= m_clockCurve.count())
        return;
    // a tap steps up to the next ten percent; past a hundred it comes round
    // to nought. (A converted 85 went 95 -> 0 and never showed 90.)
    int v = (m_clockCurve.at(index) / 10 + 1) * 10;
    m_clockCurve[index] = v > 100 ? 0 : v;
    QStringList parts;
    foreach (int p, m_clockCurve)
        parts << QString::number(p);
    QSettings().setValue(SETTINGS_ENGINE_CLOCKCURVE, parts.join(','));
    m_roomSent = -1;                       // announceRoom() re-sends on the next beat
    announceRoom();                        // ... or now: the deck may be stopped (r199)
    emit tableChanged();
}

int TrackEngine::clockPercent() const
{
    // The ENERGY the clock asks for now: the quarter-hour points from 20:00,
    // with a straight line between two points, so the slider reaches each
    // tile's value on its quarter and moves at most a percent or so a minute
    // on the way. Before 20:00 and from closing time: nought. Past the last
    // point (02:45) the value holds - on New Year's night until closing.
    // A hand on the slider still wins (Tobias, 2026-09-15: "det er stadig
    // energi-slideren der skal bestemme").
    // The house closing is a lid on top: the last five minutes to closing
    // time come down to nought in REAL seconds (runde 209: the two nights the
    // clocks change), from whatever the curve says five minutes before.
    const int close = closingMinutes();              // minutes past 20:00
    QTime now = QTime::currentTime();
    int minutes = now.hour() * 60 + now.minute() - 20 * 60;
    if (minutes < 0)
        minutes += 24 * 60;          // past midnight
    // with the closing sequence off the curve's last point holds past closing
    // time, until 05:00 - the house decides when it is over
    if (minutes >= (m_closingOn ? close : 540) || m_clockCurve.isEmpty())
        return 0;                    // closed, until 20:00
    const int last = int(m_clockCurve.count()) - 1;
    auto curveAt = [&](int m) {
        const int q = m / 15;
        if (q >= last)
            return m_clockCurve.at(last);
        const qreal f = qreal(m % 15) / 15.0;
        return int(qRound(m_clockCurve.at(q) + f * (m_clockCurve.at(q + 1) - m_clockCurve.at(q))));
    };
    int v = curveAt(minutes);
    QDate night = QDate::currentDate();
    if (now.hour() < 20)
        night = night.addDays(-1);
    const qint64 left = QDateTime::currentDateTime()
                            .secsTo(QDateTime(night.addDays(1), QTime(close / 60 - 4, 0)));
    if (m_closingOn == false)
        return v;
    if (left <= 0)
        return 0;
    // the closing sequence (runde 211, Tobias: "skal foerst starte 5 min foer
    // luk, og langsomt saette farten ned helt og paa lukke minuttet"): the
    // slider comes down from what the curve says five minutes before closing
    // to nought on the closing minute
    if (left < ENGINE_CLOSING_SECS)
        v = qMin(v, int(qRound(curveAt(qMax(0, close - 5)) * qreal(left) / qreal(ENGINE_CLOSING_SECS))));
    return v;
}

void TrackEngine::laserFaderCheck(qreal slider)
{
    // Tobias, 2026-09-23: "barerne hjem med det samme". Every laser rule is
    // kept by tick(), and tick() runs on a beat - with the link lost there is
    // none for up to 30 s, and a bar aimed under home at 80 % stayed pointing
    // down while the fader came down past 60 and 40. The same measure as
    // tick()'s laser block, on the slider (with the closing lid, as there).
    if (m_doc == nullptr || m_startScene)
        return;
    const qreal fader = qMin(closingCap(), qBound(0.0, slider, 1.0));
    const int downNow = laserDownAllowed(fader);
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.lasers == false || g.patternDevice)
            continue;
        // a figure: nothing moves under 40 %
        if (fader < 0.40 && m_active.contains("efx:" + key))
        {
            // RUNDE 336: dark first, as tick() does (runde 220/303): an EFX
            // has no fade-out, and the beams went straight back on the aim
            // lit - up to 26 steps. The next beat lights them again where
            // tick() says they may be. Not under a held FLASH (as tick()).
            if (m_flashHeld.contains(key) == false)
            {
                stopSlot("col:" + key, true);
                stopSlot("mot:" + key, true);
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
                m_cast.remove(key);
            }
            stopSlot("efx:" + key, true);
            m_sweep.remove(key);
        }
        // 40 % and up: a running figure dips no further than the fader allows
        // NOW. applySweep() re-derives it on the beat; with the link lost there
        // is none, and a figure drawn at 90 % kept dipping at 50 % (runde 213)
        else if (m_active.contains("efx:" + key) && m_sweepShown.contains(key))
        {
            const TrackSweep sw = m_sweepShown.value(key);
            EFX *efx = qobject_cast<EFX *>(m_doc->function(m_sweepFunc.value(key, Function::invalidId())));
            if (sw.shape >= 0 && efx != nullptr)
            {
                const int allowed = m_position.value(key, Function::invalidId()) == homePosition(key) ? downNow : 0;
                // the size that SHOWS: a bar figure grows from nought (runde
                // 219), and lifting to the full size's centre jumped it
                const int h = int(efx->height());
                const int y = qBound(0, 127 - h + qMin(2 * h, allowed), 255);
                // R401_FADER_LIFT_DARK (harness: wobble, latch_up - the ENERGY
                // fader brought down by 5 points or more lifted the running bar
                // figure 3-6 units at once, lit). A lift of two units or more
                // goes as a reposition does: the light now, the mirror 60 ms
                // later (three MasterTimer frames - the stops take a tick,
                // setYOffset() is read at once). The next beat lights the bars
                // again where tick() says they may be. A drag gives one-unit
                // steps and moves as before.
                const int lift = int(efx->yOffset()) - y;
                if (lift >= 2 && m_flashHeld.contains(key) == false)
                {
                    stopSlot("col:" + key, true);
                    stopSlot("mot:" + key, true);
                    for (int i = 0; i < g.parts.count(); i++)
                        stopSlot(partSlot(key, i), true);
                    m_cast.remove(key);
                    const quint32 efxId = m_sweepFunc.value(key, Function::invalidId());
                    QTimer::singleShot(60, this, [this, efxId, y]() {
                        EFX *later = m_doc ? qobject_cast<EFX *>(m_doc->function(efxId)) : nullptr;
                        if (later != nullptr && int(later->yOffset()) > y)
                            later->setYOffset(y);
                    });
                }
                else if (lift > 0)
                    efx->setYOffset(y);
            }
        }
        // an aim idle() already stopped is no aim: forget it, and the first
        // aim when the music comes back goes through the unknown-aim dark hold
        // - starting home here left m_position saying home while the tilt
        // stood elsewhere, and the bars relit mid-swing on resume (runde 193)
        if (m_active.contains("pos:" + key) == false)
        {
            m_position.remove(key);
            // ... but if the bars are still LIT (pinned as the base, held at
            // 35 % between tracks) their tilt is unknown - wherever the
            // stopped aim left it. Under 60 % that may not point down: dark
            // (runde 194)
            // (only for bars the engine aims at all: a laser group with no
            // position scenes never has a pos: slot, and was darkened on
            // every fader move, runde 195)
            bool lit = m_active.contains("col:" + key);
            for (int i = 0; i < g.parts.count() && lit == false; i++)
                lit = m_active.contains(partSlot(key, i));
            if (lit && fader < 0.60 && candidates(ENGINE_ROLE_POSITION, key).isEmpty() == false)
            {
                stopSlot("col:" + key, true);
                for (int i = 0; i < g.parts.count(); i++)
                    stopSlot(partSlot(key, i), true);
                m_cast.remove(key);
            }
            continue;
        }
        const quint32 held = m_position.value(key, Function::invalidId());
        const quint32 home = homePosition(key);
        if (held == Function::invalidId() || held == home)
            continue;
        const TrackFuncInfo &hi = m_funcs.value(held);
        // as tick() under HOLD: a still, safe aim stands under 40 % while HOLD
        // is on; one that moves or dips does not (runde 193)
        const bool unsafe = (fader < 0.40 && (m_hold == false || fader < 0.03 || hi.type != int(Function::SceneType)))
                         || (hi.sweep ? laserSweepSafe(held, key, downNow) == false
                                      : laserAimSafe(held, key, downNow) == false);
        if (unsafe == false)
            continue;
        // dark first, then home: a beam never swings lit. The next beat
        // lights it again where tick() says it may be.
        stopSlot("col:" + key, true);
        stopSlot("mot:" + key, true);
        for (int i = 0; i < g.parts.count(); i++)
            stopSlot(partSlot(key, i), true);
        m_cast.remove(key);
        if (home != Function::invalidId())
        {
            m_position.insert(key, home);
            run("pos:" + key, home, 1.0, 0, true);
        }
        else
        {
            stopSlot("pos:" + key, true);
            m_position.remove(key);
        }
    }
    emit liveChanged();
}

void TrackEngine::announceRoom()
{
    if (m_roomAuto == false)
        return;
    int p = clockPercent();
    if (p == m_roomSent)
        return;
    m_roomSent = p;
    // the same scale setRoom() sends: 35 / 55 / 75 / 90. The old
    // thresholds were 70/90/115 against a percentage that stops at 85, so
    // the room never got past "warming" all night.
    m_room = p < 45 ? 0 : (p < 65 ? 1 : (p < 82 ? 2 : 3));
    emit roomChanged(p);
    emit liveChanged();      // 'room' notifies on this one, and it moves now
}

int TrackEngine::closingMinutes()
{
    // 03:00 every night; 05:00 on New Year's night (the evening of the 31st
    // and the small hours of the 1st are the same night here)
    // The night is named by its evening: before 20:00 it is still the night
    // that began yesterday. Testing today's date made the small hours of the
    // 31st - the night of the 30th - a New Year's night too, and the house
    // closed at 05 with no slide down at 03 (runde 168).
    // Minutes past 20:00 since runde 211 (the curve starts at 20:00).
    QDate d = QDate::currentDate();
    if (QTime::currentTime().hour() < 20)
        d = d.addDays(-1);
    bool newYear = d.month() == 12 && d.day() == 31;
    return ((newYear ? 5 : 3) + 24 - 20) * 60;
}

qreal TrackEngine::closingCap() const
{
    // A lid on the ENERGY that comes down by itself over the last forty
    // minutes - but ONLY while the clock is still driving the slider. A
    // hand on ENERGY turns "ENERGY by clock" off, and from that moment the
    // lid is gone: the rig can be tested at four in the morning, and a
    // night that runs late is the operator's call, not the clock's.
    // (Tobias, 2026-09-16: "saa laenge vi overrider det ved at traekke i
    // energi-slideren er det fint ... hvad nu hvis vi vil teste lyset
    // efter luk?")
    if (m_roomAuto == false || m_closingOn == false)
        return 1.0;
    int close = closingMinutes();                    // minutes past 20:00 (runde 211)
    QTime now = QTime::currentTime();
    int minutes = now.hour() * 60 + now.minute() - 20 * 60;
    if (minutes < 0)
        minutes += 24 * 60;
    // day from 06:00 (runde 211: the room is DARK after closing now, and on
    // New Year's night closing is 05:00 - a day starting at 05:00 lit it again
    // on the closing minute)
    if (minutes >= 600)
        return 1.0;
    if (minutes >= close)
        return 0.0;
    // The last minutes count REAL seconds to closing time, which is a
    // local wall time that exists exactly once on both change-over nights.
    // On the wall clock the October night ran the slide twice (02:20-02:59
    // CEST, then the clock fell back to 02:00 and the lid sprang open) and the
    // March night never ran it (01:59 CET, then 03:00 CEST = closed) (runde 209).
    QDate night = QDate::currentDate();
    if (now.hour() < 20)
        night = night.addDays(-1);
    const qint64 left = QDateTime::currentDateTime()
                            .secsTo(QDateTime(night.addDays(1), QTime(close / 60 - 4, 0)));
    if (left <= 0)
        return 0.0;
    // five minutes, not forty (runde 211): the lid starts at 02:55 and is shut
    // on the closing minute - energy, pace AND light (closingTick() dims the
    // output with it, so the room is dark at closing)
    if (left < ENGINE_CLOSING_SECS)
        return qreal(left) / qreal(ENGINE_CLOSING_SECS);
    return 1.0;
}

bool TrackEngine::closingSequence() const { return m_closingOn; }

void TrackEngine::setClosingSequence(bool on)
{
    if (on == m_closingOn)
        return;
    m_closingOn = on;
    QSettings().setValue(SETTINGS_ENGINE_CLOSING, on);
    m_roomSent = -1;
    announceRoom();
    // (the light follows on the next 200 ms closingTick() from TrackManager:
    // a bare closingTick() here did not compile, and closingTick(true) would
    // have darkened a room with the show OFF - runde 215)
    emit liveChanged();
}

void TrackEngine::closingTick(bool showOn)
{
    // The closing sequence runs on the clock, not on beats: the DJ may stop
    // before closing, and the room must still be dark on the minute.
    // - dark: MASTER x the lid on every output, and the BLACKOUT masks at nought
    // - the light: every output is scaled by the lid (masterOut())
    // - the hazer: off as the sequence starts, so the room is clear at closing
    // - the ENERGY slider follows the clock (announceRoom, as a beat would)
    // A hand on ENERGY (ENERGY by clock off) or the toggle lifts the lid, and
    // the light comes back: the rig can be tested after closing.
    // Not while a project load settles (runde 220): the ids below may still
    // be the old show's, as in the pulse and fade timers.
    if (m_docTimer.isActive())
        return;
    const qreal dim = showOn ? closingCap() : 1.0;
    const bool closingStarts = dim < 1.0 && m_closingDim >= 1.0;
    if (qAbs(dim - m_closingDim) > 0.004 || (dim == 0.0) != (m_closingDim == 0.0))
    {
        const bool darkChanged = (dim <= 0.0) != (m_closingDim <= 0.0);
        m_closingDim = dim;
        reapplyLevels();
        // at nought the masks go on, as BLACKOUT's do: the intensity scaling
        // does not reach a laser bar's beam (a colour channel) or an animation
        // laser's (effect channels) - they stayed lit at closing (runde 213)
        // ... only with the show running (runde 215): with SHOW OFF release()
        // has already dropped every mask, and applyGroupOff() here put the
        // OFF groups' masks back - over the Virtual Console until SHOW ON
        if (darkChanged && showOn)
            applyGroupOff();
        emit liveChanged();
    }
    if (dim < 1.0 && m_haze > 0.0)
        setHaze(0.0);
    // Runde 221: haze set from a Virtual Console or MIDI fader never passed
    // through m_haze, so the closing left the hazer running. Once, as the
    // closing starts: the haze scene at nought over whatever set it.
    else if (closingStarts)
    {
        ensureTable();
        applyAtmos(m_hazeScene, m_hazeChannels, 0.0);
    }
    announceRoom();
}

bool TrackEngine::startScene() const { return m_startScene; }

bool TrackEngine::positionHeld(const QString &key) const
{
    // R378_POSITIONS: the moving heads only (Tobias 10-06) - the laser bars and
    // the animation lasers keep the engine's aims and their safety lines
    if (m_positionMode.isEmpty())
        return false;
    const TrackGroup &g = m_groups.value(key);
    return g.heads && g.patternDevice == false && g.lasers == false;
}

bool TrackEngine::varietyDraw(int &wait, QRandomGenerator *rng) const
{
    // R401_SPECIAL_DRAW: about one occasion in three, never by a pattern and
    // never far apart - the odds grow with every occasion it missed, so the
    // gap is one to five occasions, each about as likely as the others
    // (steeper odds bunched it on two and three - a rhythm again)
    static const int odds[] = { 20, 25, 35, 50, 100 };
    const bool hit = int(rng->bounded(100)) < odds[qBound(0, wait, 4)];
    wait = hit ? 0 : wait + 1;
    return hit;
}

bool TrackEngine::heldAimBlocks(const QString &key, const TrackFuncInfo &info) const
{
    // R400_HOLD_AIM: what a position held on the page keeps off the heads -
    // a programme that aims them (R378), and under STRAIGHT DOWN one that
    // zooms them by itself (R398)
    if (positionHeld(key) == false)
        return false;
    if (info.aims)
        return true;
    return m_positionMode == QStringLiteral("down")
        && info.name.contains(QStringLiteral("zoom"), Qt::CaseInsensitive);
}

quint32 TrackEngine::headHoldFunction(const QString &key)
{
    // R378_POSITIONS: LIGE NED is the generated Center (tilt 128, straight
    // down, on the learned pan); START POSITION and SOEJLE MIDT are the
    // START scene's and the rider's "Soejle stop" pan, tilt, speed and zoom
    // for these heads, in a hidden scene of their own - written again on
    // first use (QLC+ saves a hidden scene's values as nought)
    if (positionHeld(key) == false || m_doc == nullptr)
        return Function::invalidId();
    const QString ck = key + QLatin1Char('|') + m_positionMode;
    if (m_holdScenes.contains(ck) && m_doc->function(m_holdScenes.value(ck)) != nullptr)
        return m_holdScenes.value(ck);
    if (m_positionMode == QStringLiteral("down"))
    {
        // R398_DOWN_SHARP (Tobias 10-07: "straight down skal zoome hoveder til
        // at vaere HELT skarpe (som ogsaa skal resette naar man de-selecter
        // den)"): the Center's pan and tilt and the sharpest beam, in a hold
        // scene of its own. The engine's zoom runs only over its own positions
        // (the zoom: slot below), so it steps aside while this holds and takes
        // the beam again on the beat the hold lets go - an LTP value would
        // otherwise stay where the hold left it
        const QString want = ENGINE_POS_PREFIX + key + QStringLiteral(" Center");
        Scene *center = nullptr;
        foreach (Function *func, m_doc->functions())
        {
            if (func != nullptr && func->name() == want)
                center = qobject_cast<Scene *>(func);
        }
        if (center == nullptr)
            return Function::invalidId();
        QList<SceneValue> downValues = center->values();
        const TrackGroup &dg = m_groups.value(key);
        foreach (quint32 fxid, dg.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fxid);
            if (fxi == nullptr)
                continue;
            for (quint32 ch = 0; ch < fxi->channels(); ch++)
            {
                const QLCChannel *qch = fxi->channel(ch);
                if (qch == nullptr)
                    continue;
                if (qch->preset() == QLCChannel::BeamZoomSmallBig)
                    downValues.append(SceneValue(fxid, ch, 0));      // the Shark: 0 is the sharp beam
                else if (qch->preset() == QLCChannel::BeamZoomBigSmall)
                    downValues.append(SceneValue(fxid, ch, 255));
            }
        }
        const QString downName = ENGINE_HOLD_PREFIX + key + QLatin1Char(' ') + m_positionMode;
        Scene *downScene = nullptr;
        foreach (Function *func, m_doc->functions())
        {
            if (func != nullptr && func->name() == downName)
                downScene = qobject_cast<Scene *>(func);
        }
        if (downScene != nullptr)
        {
            foreach (SceneValue old, downScene->values())
                downScene->unsetValue(old.fxi, old.channel);
        }
        else
        {
            downScene = new Scene(m_doc);
            downScene->setName(downName);
            downScene->setVisible(false);
            if (m_doc->addFunction(downScene) == false)
            {
                delete downScene;
                return Function::invalidId();
            }
        }
        foreach (SceneValue sv, downValues)
            downScene->setValue(sv);
        m_holdScenes.insert(ck, downScene->id());
        return downScene->id();
    }
    QList<Scene *> sources;
    if (m_positionMode == QStringLiteral("start"))
    {
        foreach (TrackFuncInfo *info, candidates(ENGINE_ROLE_IDLE, QString()))
        {
            Scene *sc = qobject_cast<Scene *>(m_doc->function(info->id));
            if (sc != nullptr)
                sources << sc;
        }
    }
    else if (m_positionMode == QStringLiteral("column"))
    {
        const QString column = QString::fromUtf8("S\xc3\xb8jle stop");
        foreach (Function *func, m_doc->functions())
        {
            Scene *sc = qobject_cast<Scene *>(func);
            if (sc != nullptr && sc->name().startsWith(column, Qt::CaseInsensitive))
            {
                sources << sc;
                break;
            }
        }
    }
    const TrackGroup &g = m_groups.value(key);
    QList<SceneValue> values;
    foreach (quint32 fxid, g.fixtures)
    {
        Fixture *fxi = m_doc->fixture(fxid);
        if (fxi == nullptr)
            continue;
        QSet<quint32> aimChannels;
        for (quint32 ch = 0; ch < fxi->channels(); ch++)
        {
            const QLCChannel *qch = fxi->channel(ch);
            if (qch == nullptr)
                continue;
            switch (qch->preset())
            {
                case QLCChannel::PositionPan:
                case QLCChannel::PositionPanFine:
                case QLCChannel::PositionTilt:
                case QLCChannel::PositionTiltFine:
                case QLCChannel::SpeedPanTiltFastSlow:
                case QLCChannel::SpeedPanTiltSlowFast:
                case QLCChannel::BeamZoomSmallBig:
                case QLCChannel::BeamZoomBigSmall:
                    aimChannels.insert(ch);
                    break;
                default:
                    break;
            }
            if (qch->group() == QLCChannel::Pan || qch->group() == QLCChannel::Tilt)
                aimChannels.insert(ch);
        }
        foreach (Scene *src, sources)
        {
            bool named = false;
            foreach (const SceneValue &sv, src->values())
            {
                if (sv.fxi == fxid && aimChannels.contains(sv.channel))
                {
                    values.append(sv);
                    named = true;
                }
            }
            if (named)
                break;                   // the first scene that aims this head
        }
    }
    if (values.isEmpty())
        return Function::invalidId();
    const QString name = ENGINE_HOLD_PREFIX + key + QLatin1Char(' ') + m_positionMode;
    Scene *scene = nullptr;
    foreach (Function *func, m_doc->functions())
    {
        if (func != nullptr && func->name() == name)
            scene = qobject_cast<Scene *>(func);
    }
    if (scene != nullptr)
    {
        foreach (SceneValue old, scene->values())
            scene->unsetValue(old.fxi, old.channel);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
    }
    else
    {
        scene = new Scene(m_doc);
        scene->setName(name);
        scene->setVisible(false);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
        if (m_doc->addFunction(scene) == false)
        {
            delete scene;
            return Function::invalidId();
        }
    }
    m_holdScenes.insert(ck, scene->id());
    return scene->id();
}

void TrackEngine::applyHeadHold(bool restart)
{
    // R378_POSITIONS: restart puts the hold after whatever else aims the heads
    // (the START scene in the opening picture and the pauses): pan and tilt
    // are LTP, and the function started last wins them
    foreach (const QString &key, m_groupOrder)
    {
        if (positionHeld(key) == false || m_groupOff.contains(key))
            continue;
        const quint32 fid = headHoldFunction(key);
        if (fid == Function::invalidId())
            continue;
        if (restart && m_active.value(QStringLiteral("pos:") + key, Function::invalidId()) == fid)
            stopSlot(QStringLiteral("pos:") + key, true);
        stopSlot(QStringLiteral("efx:") + key, true);
        // R400_HOLD_AIM: a programme that aims them (or, straight down, zooms
        // them) lets go now - its next step's fader came after the hold's and
        // took pan and tilt back for a beat, lit (harness: CENTER COLUMN from
        // AUTO, the heads snapped back and glided to the column again)
        const quint32 motNow = m_active.value(QStringLiteral("mot:") + key, Function::invalidId());
        if (motNow != Function::invalidId() && heldAimBlocks(key, m_funcs.value(motNow)))
        {
            stopSlot(QStringLiteral("mot:") + key, true);    // hard: a soft stop fades
            m_motionDim.remove(key);                          // its level and goes on stepping
            m_sectionMotion.remove(key);
        }
        // ... and the position it takes over from goes hard too: the AUTO head
        // figures run in pos: (runde 255) - the snap in the harness was one of
        // them, "AUTO Wash Drop Ripple Tilt Offset", soft-stopped by run() and
        // stepping on for its fade. Pan and tilt are LTP: a soft stop fades
        // nothing on them (tick() hands the hold over hard as well)
        run(QStringLiteral("pos:") + key, fid, 1.0, 0, true);
        m_position.insert(key, fid);
    }
}

void TrackEngine::setPositionMode(const QString &mode)
{
    // R378_POSITIONS (Tobias 10-06: "Positions med en AUTO knap ... START
    // POSITION, Soejle midt, lige ned"): held until AUTO
    if (mode.isEmpty() == false && mode != QStringLiteral("start")
        && mode != QStringLiteral("column") && mode != QStringLiteral("down"))
        return;
    if (mode == m_positionMode)
        return;
    ensureTable();
    // the hold that goes lets go of the heads: AUTO draws their next aim on the
    // next beat, the opening picture gives them back to the START scene
    foreach (const QString &key, m_groupOrder)
    {
        if (positionHeld(key) == false)
            continue;
        const quint32 was = headHoldFunction(key);
        if (was != Function::invalidId() && m_active.value(QStringLiteral("pos:") + key, Function::invalidId()) == was)
            stopSlot(QStringLiteral("pos:") + key, true);
        m_position.remove(key);
        m_sectionMotion.remove(key);     // a programme that aims them is drawn again
    }
    m_positionMode = mode;
    logSignal(QStringLiteral("sig:position:") + (mode.isEmpty() ? QStringLiteral("auto") : mode));
    if (m_startScene)
        startLook();
    else if (mode.isEmpty() == false)
        applyHeadHold(true);
    emit liveChanged();
}

void TrackEngine::setStartScene(bool on)
{
    setStartScene(on, false);
}

void TrackEngine::setStartScene(bool on, bool keepTiles)
{
    if (on)
        labShutdown(false);              // R410_LAB: SHOW ON puts the start picture up
    if (on == m_startScene)
        return;
    m_startScene = on;
    if (on)
    {
        m_startWatchCount = 0;               // runde 345: the watchdog is armed again
        // the palette is the table's: SHOW ON before the Track page had built
        // it found no red and opened on the first colour, blue (runde 337)
        ensureTable();
        // it always opens on red (Tobias, 2026-09-23: "den skal altid starte
        // paa roed") - also over a tile the DJ left picked, which it replaces
        // - and the tile lights up, so it is clear which it is. A tile tapped
        // while it is up is the DJ's and outlives it (m_startColour).
        // R379_ZERO_START: ENERGY pulled to 0 in the show keeps the DJ's own
        // tiles - red is the start scene's colour only when there are none
        if (keepTiles == false || m_overrideSet.isEmpty() || m_startColour)
        {
            QString want = m_palette.contains(QStringLiteral("red"))
                           ? QStringLiteral("red")
                           : (m_palette.isEmpty() ? QString() : m_palette.first());
            if (want.isEmpty() == false)
            {
                m_override = want;
                m_overrideSet = QStringList(want);   // runde 304
                m_overrideIdx = 0;
                m_colour = want;
                m_startColour = true;
            }
        }
        stopAll();
        startLook();
    }
    else
    {
        // The start picture's colour never outlives the start picture - but a
        // colour the DJ chose while it was up is theirs and stays. m_startColour
        // is exactly that distinction, and it was being written in three places
        // and read in none, so the DJ's tile was thrown away every time.
        if (m_startColour)
        {
            m_override.clear();
            m_overrideSet.clear();               // runde 304
            m_overrideIdx = 0;
        }
        m_startColour = false;
        foreach (const QString &slot, m_active.keys())
        {
            // not the OFF and BLACKOUT masks: with SHOW OFF nothing puts them
            // back, and the BLACKOUT tile stood lit over a lit room (r199)
            if (slot.startsWith("off:") || slot.startsWith("black:"))
                continue;
            // R381_HOLD_STAYS: nor a held position - stopped here, the START
            // scene's aim (still fading out) had the heads for half a second
            if (slot.startsWith(QStringLiteral("pos:")) && positionHeld(slotGroup(slot)))
                continue;
            stopSlot(slot, false);
        }
        m_cast.clear();
        m_baseCover.clear();                 // runde 260: only tick() says who covers the base
        if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
            m_fadeTimer.start();
        m_report = tr("(stopped)");
        emit liveChanged();
    }
}

qreal TrackEngine::startLevel() const { return m_startLevel; }

void TrackEngine::setStartLevel(qreal level)
{
    level = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(level + 1.0, m_startLevel + 1.0))
        return;
    m_startLevel = level;
    if (m_startScene)
        startLook();
    else
        emit liveChanged();
}

void TrackEngine::startLook()
{
    // The evening's opening picture: the IDLE functions hold the aim, and
    // the engine lights every group that is on, in one colour, standing
    // still. The colour tiles, MASTER and the cast faders all work on it.
    // (The rider's "START scene" is not pan/tilt/zoom only, as this said: it
    // also holds the wash heads' and the Minis' dimmer and red at full.)
    if (m_doc == nullptr)
        return;
    ensureTable();
    // an OFF group stays off here as in idle(): setStartScene() comes through
    // stopAll(), which stops the masks, and the rider's scene lights every
    // head it names - a group switched off last night stood in full red
    // (runde 185)
    applyGroupOff();
    tickFades();
    stopSweeps();

    QString colour = m_override.isEmpty() ? m_colour : m_override;
    if (engineBannedColour(colour))
        colour.clear();
    if (colour.isEmpty() || m_palette.contains(colour) == false)
        colour = m_palette.isEmpty() ? QString() : m_palette.first();
    m_colour = colour;

    QList<TrackFuncInfo *> idles = candidates(ENGINE_ROLE_IDLE, QString());
    QSet<QString> lit;

    // the aim, from the start scene(s) - and ONLY the aim. The rider's START
    // scene also holds the wash heads' and the Minis' dimmer and red at full,
    // and HTP added that red to every colour tile: blue came out magenta,
    // green yellow. At intensity 0 its Intensity channels (dimmer, R, G, B,
    // W) are nought while pan, tilt and zoom still land (GenericFader scales
    // only FadeChannel::Intensity); the engine's colour scene and dimmer
    // parts below make the light (Tobias, 2026-09-23, runde 186). idle()
    // between tracks is not this and keeps the scene's own levels.
    foreach (TrackFuncInfo *info, idles)
        run("idle:" + QString::number(info->id), info->id, 0.0, 0, false);
    // R378_POSITIONS: a position chosen on the page holds the heads here too,
    // started after the START scene so it wins their pan and tilt
    applyHeadHold(true);

    // R378_START_LAYER (Tobias 10-06: "det skal ogsaa vaere muligt at saette
    // fades/chases paa start-scenen"): two tiles or more take turns on the
    // opening picture as they do in the show - AUTO and FADE glide (the room
    // eats: the calm way), CHASE steps the groups through them. No beats
    // here: slotLayerTimer() moves it on a clock of its own
    m_layerOwned.clear();
    const int startStyle = m_overrideSet.count() >= 2 ? (m_colourMode == 2 ? 2 : 1) : 0;
    if (startStyle != m_layerStyle)
    {
        if (m_layerStyle == 0)
            m_layerPos = qreal(m_overrideIdx);
        m_layerStyle = startStyle;
    }
    m_layerPattern = 0;                  // a chase: the whole group, a colour at a time
    m_layerRate = 0.0;                   // the clock moves it, not the beats
    m_layerBaseKey = baseGroup();
    m_startLayerMs = m_clock.elapsed();
    if (startStyle != 0 && m_layerTimer.isActive() == false)
        m_layerTimer.start();

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        // the strobes and the lasers stay out of the opening picture: it is
        // the light people eat under, not a show
        if (m_groupOff.contains(key) || g.strobes || g.lasers)
        {
            stopSlot("col:" + key, false);
            for (int i = 0; i < g.parts.count(); i++)
                stopSlot(partSlot(key, i), false);
            continue;
        }
        quint32 cf = colourFunction(key, colour);
        if (cf != Function::invalidId() && startStyle != 0 && layerGroup(key))   // R378_START_LAYER
        {
            m_layerOwned.insert(key);
            m_layerLevel.insert(key, m_startLevel);
            applyColourLayer(key, false);
            lit.insert(key);
        }
        else if (cf != Function::invalidId())
        {
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? m_startLevel : 1.0, 0, false);
            stopSlot("colx:" + key, true);   // R378: the fade's second colour, if the set went
            lit.insert(key);
        }
        else
            stopSlot("col:" + key, false);
        if (g.hasDimmer)
        {
            setDimmer(key, m_startLevel);      // the cast faders and MASTER ride on this
            lit.insert(key);
        }
    }

    // no beats while the room eats: keep the fades moving on the timer
    if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
        m_fadeTimer.start();

    m_cast = lit;
    m_baseCover.clear();                 // runde 260: only tick() says who covers the base
    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    stopEcho();
    m_report = tr("(start scene)  |  %1  |  master %2 %")
               .arg(colour.isEmpty() ? tr("(no colour)") : colour)
               .arg(int(m_master * 100));
    emit liveChanged();
}

bool TrackEngine::hold() const { return m_hold; }

void TrackEngine::setHold(bool on)
{
    if (on == m_hold)
        return;
    // The one free signal that is positive: freezing a look is asking it to
    // stay. Everything else the operator reaches for means "not this".
    m_hold = on;
    logSignal(on ? QStringLiteral("sig:hold") : QStringLiteral("sig:hold-off"));
    emit liveChanged();
}

int TrackEngine::calmBarsLeft() const
{
    return qMax(0, (m_calmUntil - m_lastBeat + 3) / 4);
}

bool TrackEngine::logEnabled() const { return m_logEnabled; }

void TrackEngine::setLogEnabled(bool on)
{
    m_logEnabled = on;
    QSettings().setValue(SETTINGS_ENGINE_LOG, on);
    if (on == false && m_log.isOpen())
        m_log.close();
    emit tableChanged();
}

QByteArray TrackEngine::logSettings() const
{
    QJsonObject stored;
    QSettings settings;
    foreach (const QString &key, settings.allKeys())
    {
        if (key.startsWith("trackengine/") || key.startsWith("trackmanager/"))
            stored.insert(key, QJsonValue::fromVariant(settings.value(key)));
    }
    QJsonObject live;
    live.insert("fullAuto", m_fullAuto);
    live.insert("master", m_master);
    live.insert("speed", m_speed);
    live.insert("hold", m_hold);
    live.insert("blackout", m_blackout);
    live.insert("accent", m_accent);
    live.insert("colourOverride", m_override);
    live.insert("colourOverrides", QJsonArray::fromStringList(m_overrideSet));   // runde 304
    live.insert("colourMode", m_colourMode);                                       // runde 370
    live.insert("roomAuto", m_roomAuto);
    live.insert("positionMode", m_positionMode);   // R378
    live.insert("rating", m_ratingOn);
    QJsonObject trims, disabled;
    foreach (const QString &group, m_groupOrder)
    {
        trims.insert(group, m_groupTrim.value(group, 1.0));
        disabled.insert(group, m_groupOff.contains(group));
    }
    live.insert("groupTrim", trims);
    live.insert("groupOff", disabled);
    QJsonObject context;
    context.insert("schema", 2);
    context.insert("stored", stored);
    context.insert("live", live);
    return QJsonDocument(context).toJson(QJsonDocument::Compact);
}

void TrackEngine::logBeat(const QString &state, int beat, qreal level, qreal energy, qreal sectionEnergy)
{
    if (m_logEnabled == false)
        return;

    // one file per date: a desk that runs past midnight starts a new one
    QString today = QDate::currentDate().toString("yyyyMMdd");
    if (m_log.isOpen() && m_log.fileName().contains(today) == false)
        m_log.close();
    if (m_log.isOpen() == false)
    {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                      + QDir::separator() + "QLC+";
        QDir().mkpath(dir);
        m_log.setFileName(dir + QDir::separator() + "tracklog-" + today + "-v2.csv");
        m_logSettingsLast.clear(); // first row of every file/session carries its snapshot
        bool fresh = m_log.exists() == false || m_log.size() == 0;
        if (m_log.open(QIODevice::Append | QIODevice::Text) == false)
        {
            // and SAY so: the LOG tile stayed ON with nothing written (runde 178)
            m_logEnabled = false;
            emit tableChanged();
            return;
        }
        if (fresh)
        {
            QTextStream head(&m_log);
            // funcs is APPENDED, never inserted: bane B's tracklog_report.py
            // reads the older columns by position and must keep working.
            head << "time,beat,state,cast,colour,level,energy,section_energy,master,moves,funcs,track,accent,event,build,settings_id,settings_json\n";
        }
    }

    // what was actually on stage, slot by slot - "efx:HEADS=1234". The slot
    // keeps which group and which job the function had, which is the whole
    // point: a rating has to be able to blame the right one later.
    QStringList running;
    for (QMap<QString, quint32>::const_iterator it = m_active.constBegin(); it != m_active.constEnd(); ++it)
        running << QString("%1=%2").arg(QString(it.key()).replace(',', ' ')).arg(it.value());
    running.sort();

    m_logBeatNo = beat;
    m_logLevel = level;
    m_logEnergy = energy;
    m_logSection = sectionEnergy;

    QStringList castSorted = m_cast.values();
    castSorted.sort();
    const QString build = QCoreApplication::applicationVersion()
                        + QStringLiteral(" / TRACK-r127 / " __DATE__ " " __TIME__);
    // Runde 221: every 16th beat, not every beat. logSettings() builds a
    // QSettings and walks allKeys() - on Windows the REGISTRY, with the big
    // seen/rating lists in it - and it ran on every beat and again on every
    // group-fader signal: 2-10 ms on the GUI thread each time. A changed
    // setting now reaches the log up to 16 beats late; the first row of a
    // file still carries its snapshot (m_logSettingsLast is empty there).
    const bool sample = m_logSettingsLast.isEmpty() || (beat % 16) == 0;
    const QByteArray currentSettings = sample ? logSettings() : m_logSettingsLast;
    QString snapshot;
    if (currentSettings != m_logSettingsLast)
    {
        m_logSettingsLast = currentSettings;
        m_logSettingsId = QString::fromLatin1(QCryptographicHash::hash(currentSettings, QCryptographicHash::Sha256).toHex());
        snapshot = QString::fromUtf8(currentSettings);
    }
    // Standard CSV quoting, including commas, quotes and any embedded newline.
    auto csv = [](QString value) {
        value.replace('"', QStringLiteral("\"\""));
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    };
    QTextStream out(&m_log);
    out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ','
        << beat << ',' << state << ','
        // group names cleaned of commas like every other name column (runde 178)
        << QString(castSorted.join('+')).replace(',', ' ').remove('"') << ',' << m_colour << ','   // runde 300: and quotes
        << QString::number(level, 'f', 2) << ','
        << QString::number(energy, 'f', 2) << ','
        << QString::number(sectionEnergy, 'f', 2) << ','
        << QString::number(m_master, 'f', 2) << ','
        << QString(m_lastMoves).replace(',', ';').remove('"') << ','
        << running.join(';') << ','
        // last, and appended like funcs was: bane B reads the older columns
        // by position. Commas and quotes out - the log is read with a plain
        // split(','), not a CSV parser, and a track called "Hello, Again"
        // would have shifted every column after it.
        //
        // ... and NEWLINES out, which the line above forgot. rekordbox hands
        // over titles with line breaks in them ("Snoh Aalegra\n - DO 4 LOVE -
        // Onderkoffer Remix\n(DJcity Intro)"), and one of those does not
        // shift a column - it splits the ROW, and every reader after it is
        // reading fields from the wrong place. In the log of 2026-09-17 it
        // tore 284 of 2717 rows into four lines each, and the report counted
        // event flags as section types.
        << QString(m_trackTitle).replace(',', ' ').remove('"')
               .replace('\n', ' ').replace('\r', ' ').simplified() << ','
        // runde 47, appended again: the accent ("Strobes All=white") and what
        // moved on this beat (section / turn / colour-on-turn / ...)
        << QString(m_logAccent).replace(',', ' ').remove('"') << ',' << m_logEvent << ',' << csv(build) << ','
        << m_logSettingsId << ',' << csv(snapshot) << '\n';
    out.flush();
    m_log.flush();                       // the report script reads while we play
}

void TrackEngine::release()
{
    // This fades the show out over a bar exactly as it did before round 162.
    // It touches nothing on the Virtual Console: the console is cleared once,
    // at SHOW ON (resetConsole), and never locked. release() is reached from
    // more than SHOW OFF - stopLook() calls it from applyLook() and
    // setRoleMode() with the show still on - so nothing console-related may
    // live here. (Round 162 had this function cut hard; that is gone too.)
    m_sequenceGroups.clear();
    m_restUntil = -1;
    m_calmUntil = 0;             // CALM goes with the show (stopAll() says so too; r199)
    m_forceNext = false;
    // the opening picture goes with everything else - and its tile with it:
    // the room went dark while START SCENE still showed lit, and the first
    // tap on it then turned "off" nothing (runde 176)
    if (m_startScene)
        setStartScene(false);
    if (m_testTimer.isActive())
        selfTest(); // cancel the test before releasing its output
    labShutdown(false);          // R410_LAB
    // AUTO went off: let everything fade out over a bar instead of clipping,
    // and let the dimmers fall back to the sliders. Positions stay where they
    // are - stopping a laser position is a move, and a slider may still have
    // the beam lit.
    // ... but a bar FIGURE stopping is a move, and an EFX has no fade-out:
    // the beams jumped back to the aim while their colour faded over a bar
    // (runde 220). Those bars go dark at once instead.
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &lg = m_groups.value(key);
        if (lg.lasers == false || lg.patternDevice || m_active.contains("efx:" + key) == false)
            continue;
        stopSlot("col:" + key, true);
        for (int i = 0; i < lg.parts.count(); i++)
            stopSlot(partSlot(key, i), true);
        // B22: a dimmer chase (mot:) is the only writer when it runs on the
        // bars, and the echo lights them too - both went soft further down,
        // so the beams jumped back to the aim still lit for up to 2 s
        stopSlot("mot:" + key, true);
        stopSlot("echo:" + key, true);
    }
    stopSweeps();
    m_strobeUntil = -1;
    // SHOW OFF hands the bars to the operator (busking, Light Rider) and a
    // four-bar hold may still be walking them home: whatever aim we remember
    // is not where the tilt is. Forget it, so the first aim after SHOW ON
    // takes the unknown-aim dark hold - as idle() and trackLoaded() do.
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &rg = m_groups.value(key);
        if (rg.lasers && rg.patternDevice == false)
            m_position.remove(key);
    }
    m_darkUntil.clear();              // AUTO is off: nothing is waiting to come back
    foreach (const QString &slot, m_active.keys())
    {
        // Both masks go: with AUTO off the group's channels belong to the
        // operator again, and that has to include the blackout.
        // Keeping the blackout mask was a trap. BLACKOUT lives on the Track
        // page; the Virtual Console's own blackout button is a different
        // mechanism entirely and does not clear it. So an operator who left
        // the Track page with it down had a rig that answered to a handful of
        // flash buttons and nothing else, with no route back from the pages
        // they actually use.
        if (slot.startsWith("off:") || slot.startsWith("black:"))
        {
            stopSlot(slot, true);
            continue;
        }
        // A static aim stays: stopping a laser position is a move in itself,
        // and a slider may still have the beam lit. An aim that MOVES - an
        // EFX or a chase on the bars, which is what a laser "position" often
        // is - has to go, or the bars sweep on after the show is stopped.
        // and the hardware strobe is cut, not faded: the soft stop scales
        // the intensity attribute, and a shutter-only scene has nothing for
        // it to scale - the rig would strobe on through the whole fade
        if (slot.startsWith("str:"))
        {
            stopSlot(slot, true);
            continue;
        }
        // A static aim of OURS stays: stopping a laser position is a move in
        // itself. A position of the OPERATOR'S does not - it may carry a
        // shutter or a dimmer of its own, and that would sit at full for the
        // rest of the night with AUTO off. idle() has had this guard all
        // along; release() was missing it.
        const TrackFuncInfo &pi = m_funcs.value(m_active.value(slot));
        bool stillAim = slot.startsWith("pos:")
                     && pi.type == int(Function::SceneType)
                     && pi.generated;
        if (stillAim == false)
            stopSlot(slot, false);
    }
    m_cast.clear();
    m_baseCover.clear();                 // runde 260: only tick() says who covers the base
    m_lastState.clear();
    m_autoStageKeys.clear();
    m_verdictAutoKeys.clear();
    m_verdictMs = -1;
    m_fillUntil = -1;
    m_fillLast = -8;
    m_lookState.clear();
    m_flash = false;
    // and the flag with them, or the next tick() would put the masks straight
    // back and the Track page would still show BLACKOUT lit
    m_blackout = false;
    clearMusicDark();                    // runde 356
    m_pulseDepth.clear();
    m_breathe.clear();
    m_flashHeld.clear();
    m_pulseTimer.stop();
    stopEcho();
    m_report = tr("(released)");
    if (m_fadeAttr.isEmpty() == false)
        m_fadeTimer.start();
    emit liveChanged();
}

bool TrackEngine::testing() const { return m_testTimer.isActive(); }

void TrackEngine::selfTest()
{
    if (m_testTimer.isActive())
    {
        m_testTimer.stop();
        testDark();
        m_testSteps.clear();
        m_testGroups.clear();
        m_testLabels.clear();
        m_report = tr("self test stopped");
        // The test darkened what it tested. Cut short - by a beat, or by its
        // tile - the opening picture stayed half dark. Put back on the next
        // turn of the event loop: stopAll() cancels a test on its way to
        // stopping everything and must not have the picture started under it,
        // and a destroyed engine takes the call with it (runde 185).
        if (m_startScene)
            QTimer::singleShot(0, this, [this]() {
                if (m_startScene && m_testTimer.isActive() == false)
                    startLook();
            });
        emit liveChanged();
        return;
    }
    if (m_doc == nullptr)
        return;
    // under BLACKOUT every step runs dark and reads as a broken lamp
    // (runde 202)
    if (m_blackout)
    {
        m_report = tr("self test: BLACKOUT is on");
        emit liveChanged();
        return;
    }
    // ... nor while the closing sequence has the room down (runde 213):
    // every lamp would read as broken. A hand on ENERGY lifts it.
    if (m_closingDim < 1.0)
    {
        m_report = tr("self test: closing - touch ENERGY first");
        emit liveChanged();
        return;
    }
    ensureTable();
    m_testSteps.clear();
    m_testGroups.clear();
    m_testLabels.clear();
    // the colours a rig is most likely to have, and the one that fails most
    // often (white: a lamp with no white channel and nothing learned sits it out)
    static const char *const testColours[] = { "red", "green", "blue", "white" };
    m_testSkipped.clear();
    // LASER SAFETY (runde 196): the test lit every group at full wherever it
    // pointed - and it is pressed right after QLC+ starts, when the bars'
    // tilt is whatever was left. The bars are sent home now, in the dark, and
    // tested LAST, after at least two other steps (four seconds) for the
    // motor; with no home aim, or nothing to test before them, they sit out.
    QStringList order = m_groupOrder;
    QStringList barsLast;
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &bg = m_groups.value(key);
        if (bg.lasers == false || bg.patternDevice || m_groupOff.contains(key))
            continue;
        order.removeAll(key);
        const quint32 home = homePosition(key);
        if (home == Function::invalidId())
        {
            m_testSkipped.append(key + tr(" (no home aim)"));
            continue;
        }
        // what RUNS, not what m_position remembers (release() stops an aim
        // and keeps the entry) - dark first, then home; and the entry is
        // forgotten, not written: the first beat after the test takes the
        // unknown-aim dark hold, as it must if the test is cut short while
        // the motor is still moving (runde 197)
        // (or a figure running on the home aim: it stops below, and the beams
        // jumped back lit - fejljagt 3)
        if (m_active.value("pos:" + key, Function::invalidId()) != home || m_active.contains("efx:" + key))
        {
            stopSlot("col:" + key, true);
            stopSlot("mot:" + key, true);
            stopSlot("efx:" + key, true);
            for (int i = 0; i < bg.parts.count(); i++)
                stopSlot(partSlot(key, i), true);
            m_cast.remove(key);
            run("pos:" + key, home, 1.0, 0, true);
        }
        // "home plus a figure" is the bars' everyday state: the figure goes
        // too, or the bars are tested at full while it swings (runde 213)
        stopSlot("efx:" + key, true);
        m_position.remove(key);
        barsLast.append(key);
    }
    order += barsLast;
    foreach (const QString &key, order)
    {
        if (m_groupOff.contains(key))
            continue;
        const TrackGroup &tg = m_groups.value(key);
        if (tg.lasers && tg.patternDevice == false && m_testSteps.count() < 2)
        {
            m_testSkipped.append(key + tr(" (no time to reach home)"));
            continue;
        }
        int before = m_testSteps.count();
        for (int i = 0; i < 4; i++)
        {
            QString colour = QString::fromLatin1(testColours[i]);
            quint32 fid = colourFunction(key, colour);
            if (fid == Function::invalidId())
                continue;
            m_testSteps.append(fid);
            m_testGroups.append(key);
            m_testLabels.append(key + " / " + colour);
        }
        // A group with no colour scene at all gets no steps - and a group
        // that is silently absent from the test reads exactly like a group
        // that passed it. The animation lasers are that group here: their
        // colour lives in their own pattern scenes, so ensureColourScenes()
        // skips them (patternDevice) and the operator has none either. Name
        // them at the end instead of leaving a hole. (2026-09-17.)
        if (m_testSteps.count() == before)
            m_testSkipped.append(key + tr(" (no colour scene)"));   // each skip says why (runde 197)
    }
    if (m_testSteps.isEmpty())
    {
        m_report = tr("self test: no colour scenes to run");
        emit liveChanged();
        return;
    }
    // the start picture steps aside for the test; idle() puts it back when
    // the manager next asks for it
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            stopSlot(slot, false);
    }
    m_testIndex = 0;
    m_testTimer.start();
    slotSelfTestStep();
}

void TrackEngine::stopEcho()
{
    m_echoTimer.stop();
    m_echoOffTimer.stop();
    if (m_echoKey.isEmpty() == false)
        stopSlot("echo:" + m_echoKey, true);    // B22: cut, as slotEchoOff() does (runde 295)
    m_echoKey.clear();
    m_echoFid = Function::invalidId();
}

void TrackEngine::slotEchoOn()
{
    // the stage went dark in the half beat since the hit: no echo
    // (nor between a project load and slotDocSettled() - runde 202)
    if (m_docTimer.isActive() || m_doc == nullptr || m_active.isEmpty() || m_echoFid == Function::invalidId()
        || m_echoKey.isEmpty() || m_groupOff.contains(m_echoKey)
        || m_blackout || m_startScene || m_lastBeat < m_calmUntil)
        return;
    run("echo:" + m_echoKey, m_echoFid, 1.0, 0, true);
    m_echoOffTimer.start(int(qMax(80.0, m_beatMs * 0.3)));
}

void TrackEngine::slotEchoOff()
{
    stopSlot("echo:" + m_echoKey, true);    // a third of a beat means cut: a bar colour does not fade
}

void TrackEngine::testDark()
{
    // the group the last step lit: colour slot and every dimmer part off,
    // the same way idle() clears a group
    if (m_testIndex <= 0 || m_testIndex > m_testGroups.count())
        return;
    QString key = m_testGroups.at(m_testIndex - 1);
    stopSlot("col:" + key, false);
    const TrackGroup &g = m_groups.value(key);
    for (int i = 0; i < g.parts.count(); i++)
        stopSlot(partSlot(key, i), false);
}

void TrackEngine::slotSelfTestStep()
{
    if (m_docTimer.isActive())          // see slotFadeTimer() (runde 202)
        return;
    testDark();
    // a group switched OFF during the test stays off: its step would light it
    // at full over the OFF mask - on the laser bars, the only blackout they
    // have (runde 202)
    while (m_testIndex < m_testSteps.count() && m_groupOff.contains(m_testGroups.at(m_testIndex)))
        m_testIndex++;
    if (m_testIndex >= m_testSteps.count())
    {
        selfTest();                       // the stopping half
        // the test darkened what it tested: the opening picture, if it is up,
        // comes back (runde 176)
        if (m_startScene)
            startLook();
        m_report = m_testSkipped.isEmpty()
                       ? tr("self test done")
                       : tr("self test done - NOT tested: %1")
                             .arg(m_testSkipped.join(", "));
        emit liveChanged();
        return;
    }
    // exactly the two things a beat does for a lit group: the colour scene
    // in the "col:" slot (MASTER and trim on top when it carries the
    // intensity) and every dimmer part at full through setDimmer(). If a
    // group stays dark here, it stays dark in a show too - and the report
    // line says which group and which colour.
    QString key = m_testGroups.at(m_testIndex);
    quint32 cf = m_testSteps.at(m_testIndex);
    run("col:" + key, cf, 1.0, 0, true);
    if (m_groups.value(key).hasDimmer)
        setDimmer(key, 1.0);
    m_report = tr("SELF TEST %1/%2: %3")
                   .arg(m_testIndex + 1).arg(m_testSteps.count()).arg(m_testLabels.at(m_testIndex));
    m_testIndex++;
    emit liveChanged();
}

void TrackEngine::idle()
{
    clearMusicDark();                    // runde 356: a stopped deck is not a silent beat
    m_sequenceGroups.clear();
    m_restUntil = -1;
    if (m_doc == nullptr)
        return;
    if (m_testTimer.isActive())   // SELF TEST owns the stage until it is done
        return;
    if (m_labActive)              // R410_LAB: and so does LASER LAB
        return;
    // the opening picture is up: nothing else runs - as in tick(). A stopped
    // deck (and the 30 s watchdog) called this and replaced the picture with
    // the idle look while the START SCENE tile stayed lit (runde 176).
    if (m_startScene)
    {
        startLook();
        return;
    }
    ensureTable();
    tickFades();

    QList<TrackFuncInfo *> list = candidates(ENGINE_ROLE_IDLE, QString());
    QString base = baseGroup();
    // no start scene: the base stands in its colour, still and dimmed - a
    // pause between tracks is not a blackout in a restaurant
    bool holdBase = list.isEmpty() && base.isEmpty() == false && m_groupOff.contains(base) == false;

    // everything from the track goes; the start scene(s) come on
    // ... the bars with a FIGURE dark first, as in release() (runde 220):
    // an EFX has no fade-out, and the beams jumped back to the aim while
    // their colour faded over a bar - idle() runs on every deck stop and
    // four seconds into a quiet link (fejljagt 3)
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &lg = m_groups.value(key);
        if (lg.lasers == false || lg.patternDevice || m_active.contains("efx:" + key) == false)
            continue;
        stopSlot("col:" + key, true);
        for (int i = 0; i < lg.parts.count(); i++)
            stopSlot(partSlot(key, i), true);
        // B22: a dimmer chase (mot:) is the only writer when it runs on the
        // bars, and the echo lights them too - both went soft further down,
        // so the beams jumped back to the aim still lit for up to 2 s
        stopSlot("mot:" + key, true);
        stopSlot("echo:" + key, true);
    }
    stopSweeps();
    m_strobeUntil = -1;
    // No section either. rate() reads an empty m_lastState as "nothing to
    // judge", and release() and trackLoaded() both clear it - this did not,
    // so a thumb on a paused deck (runEngine() idles when the player stops,
    // and after 30 s without Link) was filed under the last section of the
    // track that had ended, against the start scenes. tick() treats an
    // empty state as a fresh section on resume, which is what it is.
    m_lastState.clear();
    m_autoStageKeys.clear();
    m_verdictAutoKeys.clear();
    m_verdictMs = -1;
    m_fillUntil = -1;
    m_fillLast = -8;
    m_lookState.clear();
    applyGroupOff();                 // an off group stays off in the start look
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            continue;
        if (slot.startsWith("off:") || slot.startsWith("black:"))
            continue;                    // the masks we started four lines ago
        // a FLASH the operator is holding stays up (runde 246): idle() now
        // runs four seconds into a quiet link, not only after 30 s
        if (m_flash && (slot == QStringLiteral("flash") || slot.startsWith(QStringLiteral("flash:"))))
            continue;
        // ... and so do the dimmers under it (fejljagt 09-27): genFlash()
        // puts the lit groups' dimmer parts at full, the flash scenes carry
        // only colour, and tick() never restarts a held group's parts - the
        // held white stood at dimmer 0 until the button was let go
        if (m_flash && slot.startsWith(QStringLiteral("dim:")) && m_flashHeld.contains(slotGroup(slot)))
            continue;
        if (slot.startsWith("str:"))
        {
            stopSlot(slot, true);    // a shutter-only scene cannot be faded
            continue;
        }

        // our own aims stay, as in release(): stopping a laser position is a
        // move in itself, and a start scene started after this one wins the
        // aim anyway. A position of the OPERATOR'S may carry a shutter or a
        // dimmer of its own, and that would sit at full through the pause.
        if (slot.startsWith("pos:")
            && m_funcs.value(m_active.value(slot)).generated)
            continue;
        if (holdBase && (slot == "col:" + base || slot.startsWith("dim:" + base + "#")))
            continue;
        // an operator's aim stopped here is an aim nobody holds any more: the
        // tilt stays wherever the scene left it. Forgotten, so the first aim
        // after the pause goes through the unknown-aim dark hold - the bars
        // came back lit mid-swing (runde 193)
        // (the laser bars only: a moving head re-aimed lit on the resume beat,
        // under HOLD and at ENERGY 0 too - the bars go dark for theirs)
        if (slot.startsWith("pos:") && m_groups.value(slotGroup(slot)).lasers)
            m_position.remove(slotGroup(slot));
        stopSlot(slot, false);
    }

    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    stopEcho();

    foreach (TrackFuncInfo *info, list)
    {
        // The BARE level. run() puts MASTER on through slotScale(), and an
        // idle: slot has no group name in it, so it counts as carrying the
        // intensity itself - passing m_master here as well squared it. At
        // MASTER 50 % the between-tracks picture sat at 25 %, and there are
        // no beats between tracks to correct it.
        run("idle:" + QString::number(info->id), info->id,
            info->dimmer ? m_startLevel : 1.0, 0, false);
    }
    applyHeadHold(true);                 // R378_POSITIONS: after the START scene, so it wins the heads

    if (holdBase)
    {
        QString colour = m_colour.isEmpty() ? (m_palette.isEmpty() ? QString() : m_palette.first()) : m_colour;
        quint32 cf = colourFunction(base, colour);
        if (cf != Function::invalidId())
            run("col:" + base, cf, m_funcs.value(cf).dimmer ? 0.35 : 1.0, 0, false);
        if (m_groups.value(base).hasDimmer)
            setDimmer(base, 0.35);
        m_cast.clear();
        m_baseCover.clear();                 // runde 260: only tick() says who covers the base
        m_cast.insert(base);
    }
    else
        m_cast.clear();
    m_baseCover.clear();                     // runde 260 (braces: -Wmisleading-indentation, runde 289)

    // nothing plays, so no beats tick the fades: keep them moving on a timer
    if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
        m_fadeTimer.start();

    m_report = list.isEmpty() ? (holdBase ? tr("(idle - base held)") : tr("(idle - no start scene)"))
                              : tr("(start scene)");
    emit liveChanged();
}

QString TrackEngine::dropStyleName(int style)
{
    switch (style)
    {
        case 1: return tr("hard");
        case 2: return tr("wide");
        case 3: return tr("tight");
        case 4: return tr("heavy");
        case 5: return tr("nervous");
        default: return QString();
    }
}

int TrackEngine::keyBiasOf(const QString &key)
{
    // rekordbox writes the key one of three ways, depending on a preference:
    // classical ("Am", "F#m", "Bb"), Camelot ("8A" minor / "8B" major) or
    // Open Key ("1m" minor / "1d" major). Anything else, or nothing: unknown,
    // and the palette is drawn as it always was.
    QString k = key.trimmed();
    if (k.isEmpty())
        return -1;
    static const QRegularExpression camelot(QStringLiteral("^\\d{1,2}([ABab])$"));
    static const QRegularExpression openKey(QStringLiteral("^\\d{1,2}([mdMD])$"));
    static const QRegularExpression classical(QStringLiteral("^[A-Ga-g][#b]?(m|min|maj|M)?$"));
    QRegularExpressionMatch m = camelot.match(k);
    if (m.hasMatch())
        return m.captured(1).toUpper() == QStringLiteral("A") ? 0 : 1;
    m = openKey.match(k);
    if (m.hasMatch())
        return m.captured(1).toLower() == QStringLiteral("m") ? 0 : 1;
    m = classical.match(k);
    if (m.hasMatch())
    {
        QString q = m.captured(1);
        return (q == QStringLiteral("m") || q == QStringLiteral("min")) ? 0 : 1;
    }
    return -1;
}

void TrackEngine::resetConsole()
{
    labShutdown(false);                  // R410_LAB: SHOW ON - the lab gives the rig back
    // SHOW ON, once (TrackManager::setAutoRun). Tobias, 2026-09-22: "SHOW ON
    // skal ikke lukke for VC siderne, den skal blot soerge for at resette alt
    // paa VC saa naar auto-show starter, er vi sikre paa intet i VC'en kan
    // drille. Men naar showet ER i gang, skal det stadig vaere muligt at
    // buske. For at resette igen, maa man genstarte showet."
    //
    // Doc's list, walked here in the GUI thread - never the MasterTimer's own
    // m_functionList, which the timer thread changes without a lock.
    // isRunning(), startedByConsole() and stop() are each safe to call from
    // this thread: the Virtual Console's buttons call stop() from it all night.
    // A console function's children (a chaser's steps, a collection's members)
    // were started by that function and stop with it.
    if (m_doc == nullptr)
        return;
    foreach (Function *f, m_doc->functions())
    {
        if (f != nullptr && f->isRunning() && f->startedByConsole())
            f->stop(FunctionParent::master());
    }
    // ... a held Freeze or Kill button (runde 300, bane B's B18 hook): the
    // show could start black, or with our own layers frozen - Freeze pauses
    // TRACK's functions too, and setPart() only restarts a stopped one
    VCButton::releaseAllHolds();
    // ... and the Level sliders, which never start a function: each lets go
    // of its channels on its next tick (VCSlider::writeDMXLevel)
    m_doc->masterTimer()->resetConsole();
}

void TrackEngine::setIncomingProfile(const QString &title, const QString &state, qreal energy)
{
    m_incomingTitle = title;
    m_incomingState = state;
    m_incomingEnergy = qIsFinite(energy) ? qBound(-1.0, energy, 1.0) : -1.0;
    m_incomingAt = title.isEmpty() ? -1 : m_clock.elapsed();
}

void TrackEngine::setNextKey(const QString &key)
{
    m_nextKeyBias = keyBiasOf(key);
}

void TrackEngine::trackLoaded(const QString &title, const QString &key)
{
    // a NEXT pressed on the old track's last beat is about that track: kept,
    // the new track's first beat drew a fresh colour over the one the mix
    // handed over (runde 300, bane B's B19)
    m_forceNext = false;
    clearMusicDark();                    // runde 356
    newTrackMemory(title);               // runde 358: no loop; the drops of another track are forgotten
    // Stage time is counted every beat but only written when something else
    // triggers a save, and a whole night can pass without one. Once per track
    // bounds the loss to the track that was playing when the power went, and
    // costs a few kilobytes every four minutes.
    saveRoles();
    m_trackTitle = title;
    setIncomingProfile(QString(), QString(), -1.0);
    m_sequenceGroups.clear();
    m_restUntil = -1;
    // the key leans the palette: minor to the cold side, major to the warm
    // (runde 48). Unknown - not every track is key-analysed - leans nowhere.
    m_keyBias = keyBiasOf(key);
    // m_nextKeyBias is NOT cleared here: BLT sends "next" only when its title
    // changes, so a deck that was NEXT before the handover and still is lost
    // its key for good. TrackManager clears it when the next track is the one
    // that just started (runde 202)
    // the mix drew this track's colour when the mix began, and the base has
    // been standing in it for the mix's second half: it IS the room's colour
    // now, and colourBar -1 below holds it to the first break or drop
    // ... which holds only once the base has actually turned. A track that
    // lands before that - BLT now waits for "mix" before it hands over to a
    // deck that came up after being given MASTER (runde 203) - found a colour
    // drawn or not depending on whether the old track had ticked in those
    // 200 ms, and the room jumped to one the base had never shown (runde 204).
    // Four bars was not the test either: the turn comes after 4, 6 or 8 bars
    // by what the incoming track is doing. So: is the base's colour scene the
    // next colour (or the stand-in its wheel has for it)? (runde 205)
    bool adopted = false;
    const QString turnBase = baseGroup();
    const quint32 baseCol = turnBase.isEmpty() ? Function::invalidId()
                          : m_active.value(QStringLiteral("col:") + turnBase, Function::invalidId());
    // ... or its programme, when that wears the colour itself (a colour-named
    // AUTO Wash covers it, and tick() then stops the "col:" slot)
    const quint32 baseMot = turnBase.isEmpty() ? Function::invalidId()
                          : m_active.value(QStringLiteral("mot:") + turnBase, Function::invalidId());
    const QString turnWant = m_nextColour.isEmpty() ? QString() : colourForGroup(turnBase, m_nextColour);
    // R374_MIX_GLIDE: a glide under way is the room going to the next colour -
    // a quick cut (the old fader down at once) takes the colour with it
    const bool glided = m_mixGlide && m_mixGlideTo == m_nextColour && m_mixGlideP > 0.0
                     && (m_hold == false || m_mixGlideP >= 0.5);   // R375_GLIDE_HOLD: the nearer colour
    m_mixGlideEnd = -1;
    const bool baseTurned = m_nextColour.isEmpty() == false
        && (glided
            || (baseCol != Function::invalidId() && m_funcs.value(baseCol).colour == turnWant)
            || (baseMot != Function::invalidId() && m_funcs.value(baseMot).coversColour
                && m_funcs.value(baseMot).colour == turnWant));
    if (m_nextColour.isEmpty() == false && m_palette.contains(m_nextColour) && baseTurned)
    {
        m_leftColour = m_colour;
        m_colour = m_nextColour;
        adopted = true;
    }
    m_nextColour.clear();
    // R376_GLIDE_FINISH: adopted part way - the glide finishes on this track
    m_mixGlideFinish = adopted && glided && m_mixGlideP < 1.0;
    if (m_mixGlideFinish)
    {
        if (m_beatMs > 0.0)              // where the frame had it, then held to the first beat
            m_mixGlideP = qBound(0.0, m_mixGlideP + m_mixGlideRate
                                 * qBound(0.0, qreal(m_clock.elapsed() - m_beatStartMs) / m_beatMs, 1.0), 1.0);
        m_mixGlideBeat = -1;
        m_mixGlideRate = 0.0;            // held until this track's first beat: no step mid-beat
    }
    else
    {
        m_mixGlide = false;
        m_mixGlideKey.clear();
    }
    m_accentPick.clear();        // drawn for the last track's colour (runde 171)
    // positions are kept: a new track is not a reason to swing the lasers
    m_lastState.clear();
    m_autoStageKeys.clear();
    m_verdictAutoKeys.clear();
    m_verdictMs = -1;
    m_fillUntil = -1;
    m_fillLast = -8;
    m_lookState.clear();
    // ... but a hold still running is a motor still moving (fejljagt 3): its
    // aim is forgotten, so the new track's first beat takes the unknown-aim
    // hold instead of relighting the bars mid-travel
    for (QHash<QString, int>::const_iterator it = m_darkUntil.constBegin(); it != m_darkUntil.constEnd(); ++it)
    {
        if (it.value() >= m_lastBeat)
            m_position.remove(it.key());
    }
    m_darkUntil.clear();         // its beats belong to the track that just ended
    m_kickGone = 0;              // the new track is not mid-vocal
    m_curveBreak = false;        // runde 192: its own curves decide afresh
    m_curveGroove = false;
    m_curveTurnBeat = -100;
    m_kickBeat = -1;
    stopEcho();
    m_mixBeat = -1;              // a mix still on now is the mix INTO this track
    m_colourBar = -1;            // hold the colour until the first break or drop
    // ... and a colour the mix handed over counts as changed on beat 0, so
    // the two-bar floor keeps it through a landing IN a break or a drop:
    // there it was thrown away on the first beat, all 10 landings of that
    // kind on 19-20 Sep - the base turned for the mix, then turned again
    // (runde 188)
    m_colourSince = adopted ? 0 : -1;
    m_aimSince.clear();          // a fresh track aims where it likes
    m_castCursor++;
    // ... unless HOLD is on (runde 223): tick() redraws whatever is missing,
    // so clearing these under HOLD changed every pattern, figure and zoom on
    // the new track's first beat - the one thing HOLD promises not to do
    if (m_hold == false)
    {
        m_moves.clear();         // the new track draws its own moves
        m_sweep.clear();
    }
    m_dropStyle = 0;
    m_dropStyleDrawn = false;    // the new track's drop draws its own character (runde 282)
    // runde 327 (review): nor its kick - a drop-to-drop mix never had a tick
    // outside a drop, so the old track's lock and half-counted kicks stayed
    m_dropKickSum = 0.0;
    m_dropKickN = 0;
    m_dropKickLast = -1;
    m_dropKickLocked = false;
    m_strobeOnKick = true;
    m_dropFrom = -1;             // the settle clock is a beat of the track that ended (runde 266)
    m_dropLine = -1;             // runde 367
    m_dropCalm = false;
    // CALM counts beats of this track: carry only what is left of it
    m_calmUntil = m_calmUntil > m_lastBeat ? m_calmUntil - m_lastBeat : 0;
    m_lastBeat = 0;
    m_hitBeats.clear();
    // two more beat stamps of the track that ended: the laser-bar echo waited
    // for beat 604 of the new track after an echo on 600 of the old, and the
    // animation lasers' turns the same, per group (runde 168)
    m_echoBeat = -100;
    m_turnBeat.clear();
    m_starCeil = 0;
    // the cast size is hysteretic, so a peak-time track that ended on four
    // groups handed four to the next track's intro and took three sections
    // to come down again
    m_effects = 0;
    m_effectsBefore = 0;
    m_effectsBeat = -1;
    m_breakExtra = 0;
    m_buildDrawn = false;
    if (m_hold == false)         // as m_moves above (runde 223)
    {
        m_zoom.clear();
        m_zoomMode.clear();
    }
    m_floorRound = false;
    m_strobeSeen = -1;
    // the burst end is a beat number of THIS track: carrying it over would
    // hold the hardware strobe on for the whole of the next one
    m_strobeUntil = -1;
}

/*********************************************************************
 * Running functions
 *********************************************************************/

// THE PACE FOLLOWS A RUNNING CHASE (runde 280). The step length went in once,
// at the start (startFunction), and a chase kept it for as long as it held
// its slot: the quiet hour's half pace (divisionFor, on the bar line's
// m_dropShown) stuck to every chase already running when the ENERGY clock
// stepped past 30 % at 22:45 or the closing lid came down - the same
// programme walked at half (or double) its pace for the rest of the section -
// and so did the strobes' drop floor (500/1000) and FULL AUTO's support floor
// when the tier or the rhythm lead moved under a held programme. SPEED
// restarts every chase (setSpeed); this is the quiet version: the running
// step finishes, the NEXT one takes the new length (ChaserRunner reads
// overrideDuration() when it creates a step), so the chase does not jump back
// to step one. Chasers and sequences only: a scene or an EFX never reads the
// override, and a collection hands it to its members once, at their start.
//
// THE STEP LENGTH IN THE CHASE'S OWN UNIT (runde 281). The engine's division
// is always beats x 1000 (divisionFor snaps every chase to the beat grid,
// reading a Time chaser's milliseconds as beats at the current tempo). But
// ChaserRunner reads the override in the chaser's OWN tempo type
// (m_chaser->tempoType(), never the overrideTempoType start() is given): on
// a Beats chaser 1000 is one beat, on a Time chaser it is 1000 ms. 43 of the
// show's own chasers and two sequences are Time (no <Tempo> in the file =
// Time) - the strobe and laser chases, Light Rider FX - so wherever the
// engine ran one (plain AUTO, which picks the operator's programmes), "one
// beat a step" was one second: at 128 bpm 2.1 beats, off the grid, and the
// strobes' half-beat floor in a drop (500) came out as 500 ms, a whole beat.
// A Time chaser gets milliseconds at the current beat length instead.
// It is still not phase-locked (Time chasers count their own ms), but it
// walks at the pace the engine chose. Scenes/EFX/collections never get a
// division (divisionFor gives them 0, or they do not read the override).
static uint paceDuration(const Function *func, int division, qreal beatMs)
{
    if (func == nullptr || division <= 0)
        return Function::defaultSpeed();
    if (func->tempoType() == Function::Time && beatMs > 0.0
        && (func->type() == Function::ChaserType || func->type() == Function::SequenceType))
        return uint(qMax(1.0, std::round(qreal(division) * beatMs / 1000.0)));
    return uint(division);
}

static void followPace(Function *func, const QString &slot, int division, qreal beatMs)
{
    if (func == nullptr)
        return;
    // a head figure on pos: (runde 293): only when the engine paces it, and
    // its fade goes with the step so the motors keep gliding
    if (slot.startsWith(QStringLiteral("pos:")) && division > 0
        && (func->type() == Function::ChaserType || func->type() == Function::SequenceType))
    {
        const uint want = paceDuration(func, division, beatMs);
        if (func->overrideDuration() != want)
            func->setOverrideDuration(want);
        if (func->overrideFadeInSpeed() != want)
            func->setOverrideFadeInSpeed(want);
        if (func->overrideFadeOutSpeed() != want)
            func->setOverrideFadeOutSpeed(want);
        return;
    }
    if (slot.startsWith(QStringLiteral("mot:")) == false)
        return;
    if (func->type() != Function::ChaserType && func->type() != Function::SequenceType)
        return;
    // a Time chaser follows the tempo too: a drifting bpm moves its ms by one
    // or two, which only changes the length of the NEXT step (runde 281)
    const uint wantDuration = paceDuration(func, division, beatMs);
    if (func->overrideDuration() != wantDuration)
        func->setOverrideDuration(wantDuration);
}

void TrackEngine::run(const QString &slot, quint32 fid, qreal level, int division, bool hard)
{
    if (fid == Function::invalidId())
    {
        stopSlot(slot, hard);
        return;
    }

    level = qBound(0.0, level, 1.0);
    // a blackout: the function runs, at nothing
    qreal out = lightsOut() ? 0.0 : qBound(0.0, level * slotScale(slot, fid), 1.0);
    // A chase that OWNS the dimmers (runde 173/174). Its intensity scales
    // every QLCChannel::Intensity channel of its steps - the dimmer AND red,
    // green and blue - so when it paints the colour itself (the colour scene
    // steps aside for a programme that covers the group) the light is out
    // SQUARED: MASTER at half gave a quarter. The square root gives back the
    // level asked for. And the pulse goes on here too, as setPart() puts it on
    // the parts: without it every off-beat and every fader move wrote the
    // unpulsed level for a frame before the pulse timer pulled it down - a
    // strobe at 5 % jumping to full. The same two steps are in
    // slotPulseTimer() and reapplyLevels().
    if (slot.startsWith(QStringLiteral("mot:")) && lightsOut() == false)
    {
        const QString ownGroup = slotGroup(slot);
        if (m_motionDim.contains(ownGroup) && m_flashHeld.contains(ownGroup) == false)
            out = qBound(0.0, out * pulseFactor(ownGroup), 1.0);
        if (canOwnDimmers(m_funcs.value(fid), ownGroup == m_compositionBase)   // runde 231
            && m_funcs.value(fid).setsColour && m_funcs.value(fid).coversColour
            && m_groups.value(ownGroup).rgb)
            out = std::sqrt(out);
    }

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        if (func == nullptr)
        {
            // the function is gone (deleted, or the project was replaced)
            m_active.remove(slot);
            m_activeAttr.remove(slot);
            m_activeLevel.remove(slot);
            m_activeOut.remove(slot);
            return;
        }
        int attr = m_activeAttr.value(slot, -1);
        if (func->isRunning() == false || func->stopped())
        {
            // someone stopped it from the Virtual Console (or we did, this
            // very tick): it is still ours, so bring it back rather than
            // adjusting a dead function forever
            startFunction(func, division, slot.startsWith(QStringLiteral("pos:")));
            if (attr >= 0)
                func->releaseAttributeOverride(attr);
            attr = func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out);
            m_activeAttr.insert(slot, attr);
            m_activeOut.insert(slot, out);
        }
        else if (func != nullptr)
        {
            // requestAttributeOverride returns the existing ID (the attribute
            // is single-override) or a fresh one if a stop reset them - so it
            // heals a stale ID where adjustAttribute would fail silently
            m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
            m_activeOut.insert(slot, out);
            followPace(func, slot, division, m_beatMs);   // runde 280/281
        }
        m_activeLevel.insert(slot, level);
        return;
    }

    stopSlot(slot, hard);

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    // coming straight back to something still fading out: take it over
    if (m_fadeAttr.contains(fid))
    {
        func->releaseAttributeOverride(m_fadeAttr.take(fid));
        m_fadeLevel.remove(fid);
    }
    // not `else` (fejljagt 09-27): a function that ended by itself inside its
    // fade (a SingleShot chase, QLC+'s stop-all) was taken over and never
    // started again - the group stood dark until the slot changed
    if (func->isRunning() == false || func->stopped())
        startFunction(func, division, slot.startsWith(QStringLiteral("pos:")));
    else
        followPace(func, slot, division, m_beatMs);   // taken over from its fade, still on the old step (runde 280/281)

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, level);
    m_activeOut.insert(slot, out);
    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
}

int TrackEngine::figureBeats(quint32 fid) const
{
    const TrackFuncInfo &info = m_funcs.value(fid);
    if (info.type != int(Function::ChaserType) || info.beats <= 0.0
        || info.name.startsWith(QStringLiteral("AUTO ")) == false)
        return 0;
    // a CLIMB keeps its drawn tempo: its top lands on the drop only at it
    // (candidates' climb rule, runde 230/287; review 294)
    if (info.name.contains(QStringLiteral("climb"), Qt::CaseInsensitive))
        return 0;
    const qreal k = 1.5 - 0.75 * qBound(0.0, (m_faderNow - 0.30) / 0.70, 1.0);
    return qMax(2, int(qRound(info.beats * k)));
}

void TrackEngine::startFunction(Function *func, int division, bool glide)
{
    if (func == nullptr)
        return;
    // A stop asked for on this same turn - a fade that just ended, stopAll(),
    // a VC button, SHOW ON's console reset - has not reached the MasterTimer
    // yet. Started now, the timer runs postRun() and preRun() back to back,
    // and postRun() wiped the intensity override run() requests next: the
    // function came back at FULL, past MASTER, until something asked again.
    // The start picture with MASTER at 20 % went to the rider's full dimmer
    // and red when ENERGY moved with the deck stopped, for up to half a
    // minute (runde 337, Tobias: "har master-faderen nede, saa starter den
    // paa fuld dimmer"). preserveAttributes, as ChaserRunner does for its
    // overlap restarts; the engine releases its own override before every
    // stop, so nothing of ours outlives one.
    if (func->isRunning() && func->stopped())
        func->stop(FunctionParent::master(), true);
    // The division goes in as an overrideDuration, so Ableton Link stays the
    // only clock: we change how long a step lasts, never the timing source.
    // In the chaser's own unit (paceDuration, runde 281): a Beats chaser gets
    // beats x 1000, a Time chaser milliseconds at the current beat length -
    // ChaserRunner reads the override in the chaser's tempo type, whatever
    // tempo type start() is handed.
    const bool inMs = func->tempoType() == Function::Time
                   && (func->type() == Function::ChaserType || func->type() == Function::SequenceType);
    // a head figure (glide, runde 293): the fade is the step, as the
    // programme was drawn - the motors glide from aim to aim
    // runde 356: out of the dark (a silence, the black beat before a drop,
    // a stab) the new look starts without its fade-in. A fade starts from
    // nothing, and with the room black there is no old look under it to
    // carry the beat: in the harness every drop landed at a third and
    // reached full a beat late (93 of 255 on the landing, 218 on the next).
    const uint fadeIn = m_hardStart ? 0
                      : (division > 0 && glide) ? paceDuration(func, division, m_beatMs)
                      : Function::defaultSpeed();
    if (division > 0)
        func->start(m_doc->masterTimer(), FunctionParent::track(), 0,
                    fadeIn,
                    glide ? paceDuration(func, division, m_beatMs) : Function::defaultSpeed(),
                    paceDuration(func, division, m_beatMs),
                    inMs ? Function::Time : Function::Beats);
    else if (m_hardStart)
        func->start(m_doc->masterTimer(), FunctionParent::track(), 0, 0);
    else
        func->start(m_doc->masterTimer(), FunctionParent::track());
}

void TrackEngine::stopSlot(const QString &slot, bool hard)
{
    // R370: the fade's second colour goes wherever the group's colour goes
    if (slot.startsWith(QStringLiteral("col:")))
        stopSlot(QStringLiteral("colx:") + slot.mid(4), hard);
    quint32 fid = m_active.value(slot, Function::invalidId());
    if (fid == Function::invalidId())
        return;

    int attr = m_activeAttr.value(slot, -1);
    m_active.remove(slot);
    m_activeAttr.remove(slot);
    m_activeLevel.remove(slot);
    // the fade starts from what the light actually showed - trim, master,
    // pulse and blackout included - never from the bare level
    qreal level = m_activeOut.take(slot);

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    // the same function may be held by another slot (a collection shared by
    // two groups). The intensity attribute allows a single override, so the
    // other slot owns the same ID: leave it alone
    if (m_active.values().contains(fid))
        return;

    if (hard || attr < 0)
    {
        if (attr >= 0) func->releaseAttributeOverride(attr);
        func->stop(FunctionParent::master());
    }
    else
    {
        m_fadeAttr.insert(fid, attr);
        m_fadeLevel.insert(fid, level);
        // The timer was only ever started by setFullAuto, startLook, release
        // and idle - never by an ordinary soft stop in the middle of a track.
        // tickFades() steps 0.02 per CALL, so during playback the only caller
        // was tick(), once a beat: a "one second" fade took fifty beats, i.e.
        // twenty-three seconds at 128 bpm. Every group that dropped out of the
        // cast was still visibly lit twelve bars later.
        if (m_fadeTimer.isActive() == false)
            m_fadeTimer.start();
    }
}

void TrackEngine::tickFades()
{
    if (m_fadeAttr.isEmpty())
        return;

    // 20 ms a step. It used to be four steps of a quarter, 250 ms apart, which
    // is what "the fades chop" was: you could count them. Then a flat second
    // at every fader (runde 290, Tobias: "alt skal skalere efter energi-
    // slideren ... fades"): now four beats at the bottom of the slider down to
    // one at the top, within 0.4 - 2 s.
    const qreal fadeMs = qBound(400.0, (m_beatMs > 0.0 ? m_beatMs : 500.0)
                                       * (4.0 - 3.0 * qBound(0.0, m_faderNow, 1.0)), 2000.0);
    const qreal step = 20.0 / fadeMs;
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
                if (m_active.values().contains(fid) == false)
                    func->stop(FunctionParent::master());
            }
            m_fadeAttr.remove(fid);
            m_fadeLevel.remove(fid);
        }
        else
        {
            func->adjustAttribute(lightsOut() ? 0.0 : level, attr);
            m_fadeLevel.insert(fid, level);
        }
    }
}

void TrackEngine::setDimmer(const QString &group, qreal level)
{
    // the whole group at one level: every part the same
    const TrackGroup &g = m_groups.value(group);
    for (int i = 0; i < g.parts.count(); i++)
        setPart(group, i, level);
}

void TrackEngine::setPart(const QString &group, int index, qreal level)
{
    const TrackGroup &g = m_groups.value(group);
    if (index < 0 || index >= g.parts.count())
        return;
    quint32 fid = g.parts.at(index);
    if (fid == Function::invalidId())
        return;

    QString slot = partSlot(group, index);
    level = qBound(0.0, level, 1.0);
    // the level the beat sets already includes where the breath stands, so
    // an off-beat never bumps the light back up
    //
    // THE HELD FLASH BUTTON IGNORES THE GROUP'S TRIM (Tobias, 2026-09-22:
    // "flash knappen skal ogsaa override hvad end lysstyrken staar paa
    // strobe-lampe gruppen"). The trim is where the group sits all night;
    // the button is the one moment it should not. BLACKOUT still applies,
    // which is a safety. (MASTER did too until runde 200 - see below.)
    //
    // m_flash, not m_flashHeld alone: the set is shared with the engine's
    // OWN accent hits on a drop (tick(), genFlash(true, hue)), and those are
    // not what he asked to override. A group the operator has turned down
    // stays down through an automatic hit - that fader is how a group is
    // quietened for the night - and only his thumb on the button overrules
    // it. Caught reviewing runde 131 rather than on the rig.
    // ... and, since 2026-09-24, MASTER too: "FLASH skal ikke foelge master,
    // den skal altid vaere fuld styrke (med dens loft)" - the ceiling is in
    // the flash scene's white. BLACKOUT still wins (run() and the masks).
    const bool heldFlash = m_flash && m_flashHeld.contains(group);
    // runde 357: the strobes in the drop's landing burst pass MASTER and the
    // trim, as the held FLASH does - the one moment Tobias allows it
    const bool landFree = g.strobes && landBurstNow();
    qreal trim = (heldFlash || landFree) ? 1.0 : m_groupTrim.value(group, 1.0);
    qreal applied = qBound(0.0, level * pulseFactor(group) * trim * ((heldFlash || landFree) ? 1.0 : masterOut()), 1.0);
    // an animation laser's "dimmer" is a switch: on above a sliver, else off
    if (g.patternDevice)
        applied = applied > 0.10 ? 1.0 : 0.0;
    if (lightsOut())
        applied = 0.0;

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_activeAttr.value(slot, -1);
        if (func != nullptr && (func->isRunning() == false || func->stopped()))
        {
            // runde 343: a stop still pending (as startFunction, runde 337) -
            // restarted bare, postRun() wiped the level asked for below and
            // the part stood at FULL for a frame: every strobe at 255 at a
            // track change in headless, the bars too
            if (func->isRunning() && func->stopped())
                func->stop(FunctionParent::master(), true);
            func->start(m_doc->masterTimer(), FunctionParent::track());
            if (attr >= 0)
                func->releaseAttributeOverride(attr);
            attr = func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, applied);
            m_activeAttr.insert(slot, attr);
        }
        else if (func != nullptr)
            m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, applied));
        m_activeLevel.insert(slot, level);
        m_activeOut.insert(slot, applied);
        return;
    }

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    // back in the cast while still fading out: take the fade over, so two
    // overrides never multiply into a dip
    if (m_fadeAttr.contains(fid))
    {
        func->releaseAttributeOverride(m_fadeAttr.take(fid));
        m_fadeLevel.remove(fid);
    }
    // not `else` (fejljagt 09-27): a function that ended by itself inside its
    // fade (a SingleShot chase, QLC+'s stop-all) was taken over and never
    // started again - the group stood dark until the slot changed
    if (func->isRunning() == false || func->stopped())
    {
        // runde 343: see above - a part stopped and started on the same turn
        // flashed at full for one frame (the "puls op til 100 % et kort
        // oejeblik" Tobias saw on the strobes, 10-02)
        if (func->isRunning() && func->stopped())
            func->stop(FunctionParent::master(), true);
        func->start(m_doc->masterTimer(), FunctionParent::track());
    }

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, level);
    m_activeOut.insert(slot, applied);
    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, applied));
}

void TrackEngine::stopAll()
{
    m_beatWatch.stop();
    m_sequenceGroups.clear();
    m_restUntil = -1;
    if (m_testTimer.isActive())
        selfTest(); // no later test step may relight a stopped/replaced show
    labShutdown(false);          // R410_LAB
    foreach (const QString &slot, m_active.keys())
        stopSlot(slot, true);

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
    m_cast.clear();
    m_baseCover.clear();                 // runde 260: only tick() says who covers the base
    m_position.clear();
    m_lastState.clear();
    m_autoStageKeys.clear();
    m_verdictAutoKeys.clear();
    m_verdictMs = -1;
    m_fillUntil = -1;
    m_fillLast = -8;
    m_lookState.clear();
    m_moves.clear();
    m_sweep.clear();
    m_sweepShown.clear();
    m_moveHistory.clear();
    m_sweepHistory.clear();
    m_liveMove.clear();
    m_patterned.clear();          // m_liveMove's partner, and it was left behind
    m_moveLevel.clear();
    m_conflictBeats.clear();
    m_lastPan.clear();
    m_headMoveBeats.clear();
    m_zoom.clear();
    m_zoomMode.clear();
    m_floorRound = false;
    m_calmUntil = 0;              // CALM must not survive AUTO going off and on
    // NOT m_lastBeat: slotDocChanged() calls this mid-track now, and zeroing
    // the beat counter there would make a CALM pressed in that same beat
    // compare 5000 < 32 and silently do nothing. m_calmUntil = 0 above is
    // already the whole of what this line was for.
    m_dropStyle = 0;
    m_dropStyleDrawn = false;
    m_dropKickSum = 0.0;         // runde 327: the drop's kick is heard again
    m_dropKickN = 0;
    m_dropKickLast = -1;
    m_dropKickLocked = false;
    m_strobeOnKick = true;
    m_strobeUntil = -1;          // or the next tick walks straight back into a burst
    m_strobeRate = 0;
    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    stopEcho();
    m_flash = false;
    m_flashHeld.clear();         // release() clears it; without this the next
                                 // beat skips the whole out-of-cast teardown
    m_report = tr("(stopped)");
    emit liveChanged();
}

/* =====================================================================
 *  LASER LAB (runde 410) - appended to trackengine.cpp by
 *  feature_trackview_engine.py from track_runtime/tracklab.inc.cpp.
 *
 *  Tobias, 2026-10-09: "Ja vi skal have lavet en masse flere auto-programmer
 *  til laserne ... et system i SETUP hvor jeg kan koere programmerne igennem
 *  og paa en maade godkende de programmer jeg ser, som er fede (evt. med
 *  kommentar hvor jeg kan skrive/vaelge hvorhenne looket passer?)" - and,
 *  for the build, that the laser is the Yuer 6W 16-channel with no manual to
 *  be found. There is no DMX chart for it anywhere (the maker's page says
 *  "16P"), so the lab does not pretend to know what a value draws: the ATLAS
 *  walks Graphic Group, Group Selection and the built-in effects in steps,
 *  and the operator says what he sees. Every look he keeps becomes an AUTO
 *  programme of the show (labEnsureShow, from ensureTable()).
 *
 *  What the lab never does (REGLER, the laser rules): the height is the
 *  operator's - the most common vertical position in his own scenes, 118 on
 *  this rig - and so are the horizontal position and both X/Y rotations. The
 *  lab writes all four in every scene, always at his values, so nothing in
 *  it can tilt the beams towards the room. With no value of his to hold, the
 *  lab does not open. TOO LOW / AUDIENCE darkens the laser at once and files
 *  the look as never.
 *
 *  It owns the stage only with SHOW OFF, the way SELF TEST does: a beat, SHOW
 *  ON (resetConsole, the start picture), BLACKOUT, STOP ALL and a project
 *  load each close it (labShutdown).
 * ===================================================================== */

#include <QFile>

#define ENGINE_LAB_G_STEP     4       // Graphic Group: 64 values to look at
#define ENGINE_LAB_P_STEP     4       // Group Selection under a kept group: 63
#define ENGINE_LAB_F_STEP     8       // the built-in effects: 31
#define ENGINE_LAB_ONLY       6       // kept looks before the lab's own replace the borrowed busking scenes

namespace
{
struct EngineLabKey
{
    bool ok = false;
    bool fx = false;          // F<v>: a built-in effect
    int a = -1;               // G<a> / F<a>
    int b = -1;               // ... P<b>
    QString kind;             // "" or a variant: spin, turn, pump, wave, draw, swap
};

EngineLabKey engineLabParse(const QString &key)
{
    static const QRegularExpression re(QStringLiteral("^([GF])(\\d+)(?:P(\\d+))?(?:/([a-z]+))?$"));
    EngineLabKey k;
    const QRegularExpressionMatch m = re.match(key);
    if (m.hasMatch() == false)
        return k;
    k.fx = m.captured(1) == QStringLiteral("F");
    k.a = m.captured(2).toInt();
    k.b = m.captured(3).isEmpty() ? -1 : m.captured(3).toInt();
    k.kind = m.captured(4);
    if (k.a > 255 || k.b > 255 || (k.fx && k.b >= 0))
        return k;
    static const QStringList kinds = { "", "spin", "turn", "pump", "wave", "draw", "swap" };
    k.ok = kinds.contains(k.kind);
    return k;
}

const QStringList &engineLabKinds()
{
    static const QStringList kinds = { "spin", "turn", "pump", "wave", "draw", "swap" };
    return kinds;
}

int engineLabSteps(const QString &kind)
{
    if (kind == QStringLiteral("turn") || kind == QStringLiteral("draw"))
        return 4;
    if (kind == QStringLiteral("pump") || kind == QStringLiteral("swap"))
        return 2;
    return 1;
}

QString engineLabKindWord(const QString &kind)
{
    // the show's names: one word after the look's own key, so familyOf() reads
    // the key (each kept look is its own figure) and no tier, star or colour
    // word of the engine's language sneaks in
    if (kind == QStringLiteral("spin")) return QStringLiteral("Spin");
    if (kind == QStringLiteral("turn")) return QStringLiteral("Turn");
    if (kind == QStringLiteral("pump")) return QStringLiteral("Pump");
    if (kind == QStringLiteral("wave")) return QStringLiteral("Ripple");
    if (kind == QStringLiteral("draw")) return QStringLiteral("Draw");
    if (kind == QStringLiteral("swap")) return QStringLiteral("Swap");
    return QStringLiteral("Look");
}

QString engineLabKindText(const QString &kind)
{
    if (kind == QStringLiteral("spin")) return QStringLiteral("turning round its centre");
    if (kind == QStringLiteral("turn")) return QStringLiteral("a quarter turn on every beat");
    if (kind == QStringLiteral("pump")) return QStringLiteral("size pump on the beat");
    if (kind == QStringLiteral("wave")) return QStringLiteral("with the wave on");
    if (kind == QStringLiteral("draw")) return QStringLiteral("drawn in, a quarter a beat");
    if (kind == QStringLiteral("swap")) return QStringLiteral("the lamps take turns, a beat each");
    return QString();
}

QString engineLabRole(const QString &channelName)
{
    // By name, in this order: "Automatic Scaling of Patterns", "Pattern
    // Rotates around the center" and "Gradual Drawing of patterns" all say
    // "pattern" too, and "Dynamic Effects Speed" says "dynamic".
    const QString n = channelName.toLower();
    if (n.contains(QStringLiteral("dimmer"))) return QStringLiteral("dimmer");
    if (n.contains(QStringLiteral("color select")) || n.contains(QStringLiteral("colour select"))) return QStringLiteral("colour");
    if (n.contains(QStringLiteral("flow"))) return QStringLiteral("flow");
    if (n.contains(QStringLiteral("size"))) return QStringLiteral("size");
    if (n.contains(QStringLiteral("speed"))) return QStringLiteral("fxspeed");
    if (n.contains(QStringLiteral("graphic"))) return QStringLiteral("graphic");
    if (n.contains(QStringLiteral("scaling"))) return QStringLiteral("scale");
    if (n.contains(QStringLiteral("center")) || n.contains(QStringLiteral("centre"))) return QStringLiteral("spin");
    if (n.contains(QStringLiteral("x-axis")) || n.contains(QStringLiteral("x axis"))) return QStringLiteral("rotx");
    if (n.contains(QStringLiteral("y-axis")) || n.contains(QStringLiteral("y axis"))) return QStringLiteral("roty");
    if (n.contains(QStringLiteral("horizontal"))) return QStringLiteral("hpos");
    if (n.contains(QStringLiteral("vertical"))) return QStringLiteral("vpos");
    if (n.contains(QStringLiteral("wave"))) return QStringLiteral("wave");
    if (n.contains(QStringLiteral("draw"))) return QStringLiteral("draw");
    if (n.contains(QStringLiteral("group selection")) || n.contains(QStringLiteral("pattern"))) return QStringLiteral("pattern");
    if (n.contains(QStringLiteral("dynamic")) || n.contains(QStringLiteral("effect"))) return QStringLiteral("fx");
    return QString();
}

// the four the lab holds at the operator's own values, in every scene it writes
const QStringList &engineLabLocked()
{
    static const QStringList locked = { "vpos", "hpos", "rotx", "roty" };
    return locked;
}

int engineLabTier(const QJsonArray &sections)
{
    int tier = -2;
    for (const QJsonValue &v : sections)
    {
        const QString s = v.toString();
        int t = -1;
        if (s == QStringLiteral("break")) t = 0;
        else if (s == QStringLiteral("groove") || s == QStringLiteral("build")) t = 1;
        else if (s == QStringLiteral("drop")) t = 2;
        if (t < 0)
            return -1;
        if (tier == -2)
            tier = t;
        else if (tier != t)
            return -1;                       // two kinds of section: a look for any
    }
    return tier == -2 ? -1 : tier;
}

void engineLabWindow(const QJsonArray &energy, qreal &lo, qreal &hi)
{
    // the ENERGY tiles, as windows on the slider (the animation laser plays
    // from ENGINE_ANI_ON, 70 %): LOW to 80, MID 75-90, HIGH from 85, TOP
    // from 95. Several tiles: the span of them.
    lo = 0.0;
    hi = 1.0;
    if (energy.isEmpty())
        return;
    qreal l = 1.0, h = 0.0;
    for (const QJsonValue &v : energy)
    {
        const QString s = v.toString();
        qreal a = -1.0, b = -1.0;
        if (s == QStringLiteral("low")) { a = 0.0; b = 0.80; }
        else if (s == QStringLiteral("mid")) { a = 0.75; b = 0.90; }
        else if (s == QStringLiteral("high")) { a = 0.85; b = 1.0; }
        else if (s == QStringLiteral("top")) { a = 0.95; b = 1.0; }
        if (a < 0.0)
            continue;
        l = qMin(l, a);
        h = qMax(h, b);
    }
    if (l <= h)
    {
        lo = l;
        hi = h;
    }
}

QString engineLabPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                  + QDir::separator() + "QLC+";
    QDir().mkpath(dir);
    return dir + QDir::separator() + "laser-lab.json";
}
} // namespace

/* ---------------------------------------------------------------------
 *  the store: Documents/QLC+/laser-lab.json, written on every change
 * --------------------------------------------------------------------- */

void TrackEngine::labLoad()
{
    if (m_labLoaded)
        return;
    m_labLoaded = true;
    QFile f(engineLabPath());
    if (f.open(QIODevice::ReadOnly) == false)
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    m_labStore = root.value(QStringLiteral("looks")).toObject();
    const QJsonObject resume = root.value(QStringLiteral("resume")).toObject();
    m_labPass = qBound(0, resume.value(QStringLiteral("pass")).toInt(0), 2);
    m_labResumeKey = resume.value(QStringLiteral("key")).toString();
}

void TrackEngine::labSave()
{
    QJsonObject root;
    root.insert(QStringLiteral("about"), QStringLiteral("LASER LAB (Track page, SETUP): the operator's verdicts on the animation "
                                                        "laser's looks. v: 1 kept, -1 no, 2 same as the one before. "
                                                        "sec/en/ch: where it belongs. st: stars. Kept looks become AUTO programmes."));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("looks"), m_labStore);
    QJsonObject resume;
    resume.insert(QStringLiteral("pass"), m_labPass);
    resume.insert(QStringLiteral("key"), labCurrentKey());
    root.insert(QStringLiteral("resume"), resume);
    QSaveFile f(engineLabPath());
    if (f.open(QIODevice::WriteOnly) == false)
    {
        m_labMessage = tr("could not save laser-lab.json");
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (f.commit() == false)
        m_labMessage = tr("could not save laser-lab.json");
}

int TrackEngine::labVerdictOf(const QString &key) const
{
    return m_labStore.value(key).toObject().value(QStringLiteral("v")).toInt(0);
}

/* ---------------------------------------------------------------------
 *  the laser: which fixtures, which channel does what, the operator's values
 * --------------------------------------------------------------------- */

void TrackEngine::labScan()
{
    m_labFixtures.clear();
    m_labChan.clear();
    m_labBase.clear();
    m_labColourValue.clear();
    m_labColours.clear();
    m_labGroup.clear();
    if (m_doc == nullptr)
        return;
    QList<Fixture *> found;
    foreach (Fixture *fxi, m_doc->fixtures())
    {
        if (fxi == nullptr)
            continue;
        QHash<QString, quint32> roles;
        for (quint32 ch = 0; ch < fxi->channels(); ch++)
        {
            const QLCChannel *qch = fxi->channel(ch);
            if (qch == nullptr)
                continue;
            const QString r = engineLabRole(qch->name());
            if (r.isEmpty() == false && roles.contains(r) == false)
                roles.insert(r, ch);
        }
        // an animation laser: its pictures are Graphic Group x Group Selection
        if (roles.contains(QStringLiteral("graphic")) && roles.contains(QStringLiteral("pattern"))
            && roles.contains(QStringLiteral("dimmer")))
        {
            found.append(fxi);
            m_labChan.insert(fxi->id(), roles);
        }
    }
    if (found.isEmpty())
        return;
    std::sort(found.begin(), found.end(), [](Fixture *a, Fixture *b) {
        if (a->universe() != b->universe())
            return a->universe() < b->universe();
        return a->address() != b->address() ? a->address() < b->address() : a->id() < b->id();
    });
    m_labGroup = groupOfFixture(found.first()->id());
    foreach (Fixture *fxi, found)
    {
        // one group: the first one's (by address)
        if (groupOfFixture(fxi->id()) == m_labGroup)
            m_labFixtures.append(fxi->id());
        else
            m_labChan.remove(fxi->id());
    }
    if (m_labGroup.isEmpty())
        m_labGroup = found.first()->name();

    // the operator's own values: the most common one per channel over his
    // scenes on these fixtures - and per colour word, his colour values
    QHash<QString, QHash<int, int> > counts;
    QHash<QString, QHash<int, int> > colours;
    foreach (Function *func, m_doc->functions())
    {
        Scene *scene = qobject_cast<Scene *>(func);
        if (scene == nullptr || func->isVisible() == false)
            continue;
        const QString path = func->path(true);
        if (path.startsWith(ENGINE_LAB_PATH) || path.startsWith(ENGINE_LAB_STEP_PATH))
            continue;
        const QString colour = colourOf(func->name().toLower());
        foreach (const SceneValue &sv, scene->values())
        {
            QHash<quint32, QHash<QString, quint32> >::const_iterator rc = m_labChan.constFind(sv.fxi);
            if (rc == m_labChan.constEnd())
                continue;
            for (QHash<QString, quint32>::const_iterator it = rc->constBegin(); it != rc->constEnd(); ++it)
            {
                if (it.value() != sv.channel)
                    continue;
                counts[it.key()][int(sv.value)]++;
                if (it.key() == QStringLiteral("colour") && colour.isEmpty() == false)
                    colours[colour][int(sv.value)]++;
            }
        }
    }
    auto mode = [](const QHash<int, int> &c) -> int {
        int best = -1, bestN = 0;
        for (QHash<int, int>::const_iterator it = c.constBegin(); it != c.constEnd(); ++it)
        {
            if (it.value() > bestN || (it.value() == bestN && it.key() < best))
            {
                best = it.key();
                bestN = it.value();
            }
        }
        return best;
    };
    for (QHash<QString, QHash<int, int> >::const_iterator it = counts.constBegin(); it != counts.constEnd(); ++it)
        m_labBase.insert(it.key(), mode(it.value()));
    // the colours: his where his scenes say, else what this laser is known to
    // answer (white 0, red 15, blue 20, cyan 40 - read off PSMAIN 2026-10-09)
    static const QList<QPair<QString, int> > known = { { "white", 0 }, { "red", 15 }, { "blue", 20 }, { "cyan", 40 } };
    for (const QPair<QString, int> &k : known)
    {
        m_labColours.append(k.first);
        m_labColourValue.insert(k.first, colours.contains(k.first) ? mode(colours.value(k.first)) : k.second);
    }
    static const QStringList others = { "green", "magenta", "yellow", "orange" };
    for (const QString &c : others)
    {
        if (colours.contains(c))
        {
            m_labColours.append(c);
            m_labColourValue.insert(c, mode(colours.value(c)));
        }
    }
}

bool TrackEngine::labReady() const
{
    if (m_labFixtures.isEmpty())
        return false;
    // the height and the rest of the aim are his, or the lab does not run
    foreach (const QString &r, engineLabLocked())
    {
        foreach (quint32 fx, m_labFixtures)
        {
            if (m_labChan.value(fx).contains(r) && m_labBase.contains(r) == false)
                return false;
        }
    }
    return true;
}

QHash<QString, int> TrackEngine::labValues(const QString &key, int step) const
{
    // Every channel but the dimmer and the colour. What the atlas does not
    // scan starts where the operator's scenes have it (speed, size, scaling,
    // colour flow) or at nought (effects, wave, drawing, rotation) - and the
    // four locked ones are his, whatever the look.
    QHash<QString, int> v;
    static const QStringList fromBase = { "flow", "fxspeed", "size", "scale" };
    foreach (const QString &r, fromBase)
        v.insert(r, m_labBase.value(r, 0));
    static const QStringList nought = { "graphic", "pattern", "fx", "spin", "wave", "draw" };
    foreach (const QString &r, nought)
        v.insert(r, 0);
    const EngineLabKey k = engineLabParse(key);
    if (k.ok)
    {
        if (k.fx)
            v.insert(QStringLiteral("fx"), k.a);
        else
        {
            v.insert(QStringLiteral("graphic"), k.a);
            v.insert(QStringLiteral("pattern"), qMax(0, k.b));
        }
        const int s = qMax(0, step);
        if (k.kind == QStringLiteral("spin"))
            v.insert(QStringLiteral("spin"), 200);
        else if (k.kind == QStringLiteral("turn"))
            v.insert(QStringLiteral("spin"), (s % 4) * 32);
        else if (k.kind == QStringLiteral("pump"))
            v.insert(QStringLiteral("size"), (s % 2) == 0 ? 0 : 120);
        else if (k.kind == QStringLiteral("wave"))
            v.insert(QStringLiteral("wave"), 101);
        else if (k.kind == QStringLiteral("draw"))
            v.insert(QStringLiteral("draw"), (s % 4) * 64);
    }
    foreach (const QString &r, engineLabLocked())
        v.insert(r, m_labBase.value(r, 0));
    return v;
}

QList<SceneValue> TrackEngine::labSceneValues(const QString &key, const QString &colour, int step, int lamps) const
{
    QList<SceneValue> out;
    const EngineLabKey k = engineLabParse(key);
    const QHash<QString, int> v = labValues(key, step);
    const int n = int(m_labFixtures.count());
    for (int i = 0; i < n; i++)
    {
        const quint32 fx = m_labFixtures.at(i);
        bool lit = true;
        if (lamps == 1 && i != 0)
            lit = false;                     // LEFT: the first by address
        if (lamps == 2 && i != n - 1)
            lit = false;                     // RIGHT: the last
        if (k.kind == QStringLiteral("swap") && n >= 2)
            lit = lit && (i % 2) == (qMax(0, step) % 2);
        const QHash<QString, quint32> roles = m_labChan.value(fx);
        for (QHash<QString, quint32>::const_iterator it = roles.constBegin(); it != roles.constEnd(); ++it)
        {
            int val;
            if (it.key() == QStringLiteral("dimmer"))
                val = lit ? 255 : 0;
            else if (it.key() == QStringLiteral("colour"))
                val = m_labColourValue.value(colour, m_labColourValue.value(QStringLiteral("white"), 0));
            else
                val = v.value(it.key(), 0);
            out.append(SceneValue(fx, it.value(), uchar(qBound(0, val, 255))));
        }
    }
    return out;
}

/* ---------------------------------------------------------------------
 *  the lists: ATLAS, VARIANTS, APPROVED
 * --------------------------------------------------------------------- */

QStringList TrackEngine::labList(int pass) const
{
    QStringList out;
    if (pass == 0)
    {
        for (int g = 0; g <= 255; g += ENGINE_LAB_G_STEP)
            out.append(QStringLiteral("G") + QString::number(g));
        for (int f = ENGINE_LAB_F_STEP; f <= 255; f += ENGINE_LAB_F_STEP)
            out.append(QStringLiteral("F") + QString::number(f));
        // a group he kept: its patterns, after the first walk
        for (int g = 0; g <= 255; g += ENGINE_LAB_G_STEP)
        {
            const QString gk = QStringLiteral("G") + QString::number(g);
            if (labVerdictOf(gk) != 1)
                continue;
            for (int p = ENGINE_LAB_P_STEP; p <= 255; p += ENGINE_LAB_P_STEP)
                out.append(gk + QStringLiteral("P") + QString::number(p));
        }
        return out;
    }
    const QStringList atlas = labList(0);
    foreach (const QString &key, atlas)
    {
        const bool kept = labVerdictOf(key) == 1;
        if (pass == 2 && kept)
            out.append(key);
        if (kept == false && pass == 1)
            continue;
        foreach (const QString &kind, engineLabKinds())
        {
            if (kind == QStringLiteral("swap") && m_labFixtures.count() < 2)
                continue;
            const QString vk = key + QLatin1Char('/') + kind;
            if (pass == 1 || labVerdictOf(vk) == 1)
                out.append(vk);
        }
    }
    return out;
}

QString TrackEngine::labCurrentKey() const
{
    if (m_labIndex < 0 || m_labIndex >= m_labList.count())
        return QString();
    return m_labList.at(m_labIndex);
}

void TrackEngine::labRefresh()
{
    const QString cur = labCurrentKey();
    m_labList = labList(m_labPass);
    const int at = cur.isEmpty() ? -1 : int(m_labList.indexOf(cur));
    if (at >= 0)
        m_labIndex = at;
    m_labIndex = m_labList.isEmpty() ? 0 : qBound(0, m_labIndex, int(m_labList.count()) - 1);
}

int TrackEngine::labOrdinal(const QString &key) const
{
    // the atlas's own count: a value marked SAME is the picture before it,
    // so "G3" is the third DIFFERENT graphic group, whatever its value
    const EngineLabKey k = engineLabParse(key);
    if (k.ok == false)
        return 0;
    int n = 0;
    if (k.fx || k.b < 0)
    {
        const int first = k.fx ? ENGINE_LAB_F_STEP : 0;
        const int step = k.fx ? ENGINE_LAB_F_STEP : ENGINE_LAB_G_STEP;
        for (int v = first; v <= 255; v += step)
        {
            const QString e = (k.fx ? QStringLiteral("F") : QStringLiteral("G")) + QString::number(v);
            if (n == 0 || labVerdictOf(e) != 2)
                n++;
            if (v >= k.a)
                break;
        }
        return n;
    }
    n = 1;                                   // P0 is the group itself
    const QString gk = QStringLiteral("G") + QString::number(k.a);
    for (int p = ENGINE_LAB_P_STEP; p <= 255 && p <= k.b; p += ENGINE_LAB_P_STEP)
    {
        if (labVerdictOf(gk + QStringLiteral("P") + QString::number(p)) != 2)
            n++;
    }
    return n;
}

QString TrackEngine::labRange(const QString &key) const
{
    // the DMX values this picture covers: its own step, and every SAME after it
    const EngineLabKey k = engineLabParse(key);
    if (k.ok == false)
        return QString();
    const bool pat = k.fx == false && k.b >= 0;
    const int step = k.fx ? ENGINE_LAB_F_STEP : (pat ? ENGINE_LAB_P_STEP : ENGINE_LAB_G_STEP);
    const int from = pat ? k.b : k.a;
    int to = qMin(255, from + step - 1);
    for (int v = from + step; v <= 255; v += step)
    {
        const QString e = k.fx ? QStringLiteral("F") + QString::number(v)
                        : pat ? QStringLiteral("G") + QString::number(k.a) + QStringLiteral("P") + QString::number(v)
                              : QStringLiteral("G") + QString::number(v);
        if (labVerdictOf(e) != 2)
            break;
        to = qMin(255, v + step - 1);
    }
    return QString::number(from) + QChar(0x2013) + QString::number(to);
}

QString TrackEngine::labTitleOf(const QString &key) const
{
    const EngineLabKey k = engineLabParse(key);
    if (k.ok == false)
        return key;
    const QString base = key.section(QLatin1Char('/'), 0, 0);
    QString t;
    if (k.fx)
        t = QStringLiteral("FX") + QString::number(labOrdinal(base));
    else if (k.b < 0)
        t = QStringLiteral("G") + QString::number(labOrdinal(base));
    else
        t = QStringLiteral("G") + QString::number(labOrdinal(QStringLiteral("G") + QString::number(k.a)))
            + QStringLiteral(" · P") + QString::number(labOrdinal(base));
    if (k.kind.isEmpty() == false)
        t += QStringLiteral("  ·  ") + engineLabKindText(k.kind);
    return t;
}

QString TrackEngine::labDetailOf(const QString &key) const
{
    const EngineLabKey k = engineLabParse(key);
    if (k.ok == false)
        return QString();
    const QString base = key.section(QLatin1Char('/'), 0, 0);
    QString d;
    if (k.fx)
        d = tr("Built-in effect %1  ·  effect speed %2  ·  graphic group 0")
                .arg(labRange(base)).arg(m_labBase.value(QStringLiteral("fxspeed"), 0));
    else if (k.b < 0)
        d = tr("Graphic group %1  ·  pattern 0  ·  built-in effect off").arg(labRange(base));
    else
        d = tr("Graphic group %1  ·  pattern %2  ·  built-in effect off").arg(k.a).arg(labRange(base));
    if (k.kind == QStringLiteral("pump"))
        d += tr("  ·  size 0 → 120 → 0 every beat");
    else if (k.kind == QStringLiteral("turn"))
        d += tr("  ·  centre rotation 0 / 32 / 64 / 96 a beat each");
    else if (k.kind == QStringLiteral("spin"))
        d += tr("  ·  centre rotation 200");
    else if (k.kind == QStringLiteral("wave"))
        d += tr("  ·  wave 101");
    else if (k.kind == QStringLiteral("draw"))
        d += tr("  ·  drawing 0 / 64 / 128 / 192 a beat each");
    else if (k.kind == QStringLiteral("swap"))
        d += tr("  ·  one lamp a beat");
    if (m_labStore.value(key).toObject().value(QStringLiteral("slow")).toBool())
        d += tr("  ·  half pace");
    return d;
}

QString TrackEngine::labTagKey() const
{
    // WHERE IT BELONGS is for the look just kept (the strip has moved on by
    // then), or for the one playing when it is a kept one
    const QString cur = labCurrentKey();
    if (cur.isEmpty() == false && labVerdictOf(cur) == 1)
        return cur;
    if (m_labLastGood.isEmpty() == false && labVerdictOf(m_labLastGood) == 1)
        return m_labLastGood;
    return QString();
}

/* ---------------------------------------------------------------------
 *  on stage
 * --------------------------------------------------------------------- */

void TrackEngine::labShow()
{
    if (m_labActive == false || m_doc == nullptr || m_labGroup.isEmpty())
        return;
    const QString slot = QStringLiteral("lab:") + m_labGroup;
    const QString key = labCurrentKey();
    if (key.isEmpty() || m_labDark)
    {
        stopSlot(slot, true);
        return;
    }
    // A/B, as the colour chase does (chaseColourFunction): the scene on stage
    // keeps its values, the other is written and shown next
    const int other = 1 - m_labSide;
    const QString name = QStringLiteral("TRACK Lab: %1 %2").arg(m_labGroup, other == 0 ? QStringLiteral("A") : QStringLiteral("B"));
    Scene *scene = qobject_cast<Scene *>(m_doc->function(m_labScene[other]));
    if (scene != nullptr && scene->name() != name)
        scene = nullptr;                     // an id of another show
    if (scene == nullptr)
    {
        foreach (Function *func, m_doc->functions())
        {
            if (func != nullptr && func->name() == name)
                scene = qobject_cast<Scene *>(func);
        }
    }
    const QList<SceneValue> values = labSceneValues(key, m_labColour, m_labBeat, m_labLamps);
    if (scene != nullptr)
    {
        foreach (SceneValue old, scene->values())
            scene->unsetValue(old.fxi, old.channel);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
    }
    else
    {
        scene = new Scene(m_doc);
        scene->setName(name);
        scene->setVisible(false);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
        if (m_doc->addFunction(scene) == false)
        {
            delete scene;
            return;
        }
    }
    m_labScene[other] = scene->id();
    m_labSide = other;
    run(slot, scene->id(), 1.0, 0, true);
}

void TrackEngine::labShowCandidate()
{
    m_labBeat = 0;
    m_labBeatsShown = 0;
    labRestartTimers();
    labShow();
    emit labChanged();
}

void TrackEngine::labRestartTimers()
{
    m_labAutoTimer.stop();
    if (m_labActive == false)
    {
        m_labStepTimer.stop();
        return;
    }
    const qreal beat = (m_beatMs > 200.0 && m_beatMs < 2000.0) ? m_beatMs : 500.0;
    const bool slow = m_labTrySlow || m_labStore.value(labCurrentKey()).toObject().value(QStringLiteral("slow")).toBool();
    m_labStepTimer.start(int(beat * (slow ? 2.0 : 1.0)));
    if (m_labDark == false && m_labHold == false && (m_labAuto == 1 || m_labAuto == 2))
        m_labAutoTimer.start(m_labAuto == 1 ? 2000 : 4000);
}

void TrackEngine::labStepTick()
{
    if (m_labActive == false)
    {
        m_labStepTimer.stop();
        return;
    }
    m_labBeat++;
    if (m_labDark)
        return;
    if (engineLabSteps(labCurrentKey().section(QLatin1Char('/'), 1, 1)) > 1)
        labShow();
    if (m_labAuto == 3 && m_labHold == false && ++m_labBeatsShown >= 8)
        labNext();
}

void TrackEngine::labAutoTick()
{
    if (m_labActive && m_labDark == false && m_labHold == false)
        labNext();
}

bool TrackEngine::labOpen()
{
    if (m_labActive)
        return true;
    if (m_doc == nullptr || m_docTimer.isActive())
        return false;
    ensureTable();
    labLoad();
    labScan();
    m_labMessage.clear();
    QString refuse;
    if (m_labFixtures.isEmpty())
        refuse = tr("laser lab: no animation laser in this show");
    else if (labReady() == false)
        refuse = tr("laser lab: no height of yours to hold - make one scene with the laser where it belongs");
    else if (m_blackout)
        refuse = tr("laser lab: BLACKOUT is on");
    else if (m_groupOff.contains(m_labGroup))
        refuse = tr("laser lab: %1 is switched OFF").arg(m_labGroup);
    if (refuse.isEmpty() == false)
    {
        m_report = refuse;
        m_labMessage = refuse;
        emit liveChanged();
        emit labChanged();
        return false;
    }
    if (m_labWired == false)
    {
        m_labWired = true;
        m_labAutoTimer.setSingleShot(true);
        connect(&m_labAutoTimer, &QTimer::timeout, this, [this]() { labAutoTick(); });
        connect(&m_labStepTimer, &QTimer::timeout, this, [this]() { labStepTick(); });
    }
    if (m_testTimer.isActive())
        selfTest();
    if (m_startScene)
        setStartScene(false);
    // the laser is the lab's: whatever the engine still holds on it goes
    // (a position left running after SHOW OFF, a part, the start look)
    foreach (const QString &slot, m_active.keys())
    {
        if (slotGroup(slot) == m_labGroup || slot.startsWith(QStringLiteral("idle:")))
            stopSlot(slot, true);
    }
    m_cast.remove(m_labGroup);
    m_position.remove(m_labGroup);
    m_labActive = true;
    // R411_OPEN_DARK (Tobias 10-09: "Ja lav en play knap"): the lab opens with
    // the laser dark - ▶ PLAY lights it, and nothing else does (STOP and TOO
    // LOW put it back to dark the same way)
    m_labDark = true;
    m_labHold = false;
    m_labTrySlow = false;
    if (m_labColours.contains(m_labColour) == false)
        m_labColour = QStringLiteral("white");
    m_labIndex = 0;
    m_labList = labList(m_labPass);
    // where he stopped last time, or the first one not looked at yet
    int at = m_labResumeKey.isEmpty() ? -1 : int(m_labList.indexOf(m_labResumeKey));
    if (at < 0)
    {
        for (int i = 0; i < m_labList.count(); i++)
        {
            if (labVerdictOf(m_labList.at(i)) == 0)
            {
                at = i;
                break;
            }
        }
    }
    m_labIndex = qMax(0, at);
    m_report = tr("LASER LAB");
    m_labMessage = tr("The laser is dark - \u25b6 PLAY lights it");
    logSignal(QStringLiteral("sig:lab-open"));
    emit liveChanged();
    labShowCandidate();
    return true;
}

void TrackEngine::labClose()
{
    labShutdown(true);
}

void TrackEngine::labShutdown(bool rebuildNow)
{
    if (m_labActive == false)
        return;
    m_labActive = false;
    m_labAutoTimer.stop();
    m_labStepTimer.stop();
    if (m_labGroup.isEmpty() == false)
        stopSlot(QStringLiteral("lab:") + m_labGroup, true);
    m_labResumeKey = labCurrentKey();
    labSave();
    logSignal(QStringLiteral("sig:lab-close"));
    if (m_labShowDirty)
    {
        // the kept looks into the show: the next table build writes them
        m_labShowDirty = false;
        if (rebuildNow)
            rebuild();
        else
        {
            saveRoles();
            m_dirty = true;
        }
    }
    m_labMessage.clear();
    emit labChanged();
}

void TrackEngine::labSetPass(int pass)
{
    if (m_labActive == false)
        return;
    pass = qBound(0, pass, 2);
    if (pass != m_labPass)
    {
        m_labPass = pass;
        m_labList = labList(pass);
        m_labIndex = 0;
        if (pass != 2)
        {
            for (int i = 0; i < m_labList.count(); i++)
            {
                if (labVerdictOf(m_labList.at(i)) == 0)
                {
                    m_labIndex = i;
                    break;
                }
            }
        }
    }
    m_labTrySlow = false;              // R411_OPEN_DARK: the dark stays until PLAY
    m_labMessage = m_labList.isEmpty()
                       ? (pass == 1 ? tr("No kept looks yet - keep some in the ATLAS first")
                                    : tr("Nothing kept yet"))
                       : (m_labDark ? tr("The laser is dark - \u25b6 PLAY lights it") : QString());
    labShowCandidate();
}

void TrackEngine::labGoto(int index)
{
    if (m_labActive == false || m_labList.isEmpty())
        return;
    m_labIndex = qBound(0, index, int(m_labList.count()) - 1);
    m_labTrySlow = false;              // R411_OPEN_DARK: the dark stays until PLAY
    m_labMessage = m_labDark ? tr("The laser is dark - \u25b6 PLAY lights it") : QString();
    labShowCandidate();
}

void TrackEngine::labNext()
{
    if (m_labActive == false)
        return;
    if (m_labIndex + 1 >= m_labList.count())
    {
        m_labAutoTimer.stop();
        m_labMessage = tr("The end of the list");
        emit labChanged();
        return;
    }
    labGoto(m_labIndex + 1);
}

void TrackEngine::labBack()
{
    if (m_labActive && m_labIndex > 0)
        labGoto(m_labIndex - 1);
}

void TrackEngine::labDecide(int verdict)
{
    if (m_labActive == false)
        return;
    const QString key = labCurrentKey();
    const EngineLabKey k = engineLabParse(key);
    if (k.ok == false || (verdict != 1 && verdict != -1 && verdict != 2))
        return;
    if (verdict == 2)
    {
        // SAME AS LAST is the atlas's, and needs a picture before it
        const bool first = k.kind.isEmpty() == false
                           || (k.b < 0 && k.a == (k.fx ? ENGINE_LAB_F_STEP : 0));
        if (first)
            return;
    }
    QJsonObject o = m_labStore.value(key).toObject();
    const bool wasKept = o.value(QStringLiteral("v")).toInt(0) == 1;
    o.insert(QStringLiteral("v"), verdict);
    o.insert(QStringLiteral("t"), QDateTime::currentDateTime().toString(Qt::ISODate));
    if (verdict == 1)
    {
        o.remove(QStringLiteral("ban"));
        if (m_labTrySlow && engineLabSteps(k.kind) > 1)
            o.insert(QStringLiteral("slow"), true);
        m_labLastGood = key;
    }
    m_labStore.insert(key, o);
    if (wasKept || verdict == 1)
        m_labShowDirty = true;
    labSave();
    logSignal(QStringLiteral("sig:lab:%1=%2").arg(key).arg(verdict));
    labRefresh();
    if (m_labPass == 2)
    {
        m_labTrySlow = false;
        labShowCandidate();
        return;
    }
    labNext();
}

void TrackEngine::labToggleTag(const QString &kind, const QString &tag)
{
    const QString key = labTagKey();
    if (m_labActive == false)
        return;
    if (key.isEmpty())
    {
        m_labMessage = tr("Keep a look first (✓ GOOD) - the tags are for it");
        emit labChanged();
        return;
    }
    static const QStringList kinds = { "sec", "en", "ch" };
    if (kinds.contains(kind) == false || tag.isEmpty())
        return;
    QJsonObject o = m_labStore.value(key).toObject();
    QJsonArray list = o.value(kind).toArray();
    QJsonArray out;
    bool had = false;
    for (const QJsonValue &v : list)
    {
        if (v.toString() == tag)
            had = true;
        else if (kind != QStringLiteral("sec") || tag != QStringLiteral("any"))
            out.append(v);
    }
    // ANYWHERE is no section at all
    if (had == false && tag != QStringLiteral("any"))
        out.append(tag);
    o.insert(kind, out);
    m_labStore.insert(key, o);
    m_labShowDirty = true;
    m_labMessage.clear();
    labSave();
    emit labChanged();
}

void TrackEngine::labSetStars(int stars)
{
    const QString key = labTagKey();
    if (m_labActive == false || key.isEmpty())
        return;
    QJsonObject o = m_labStore.value(key).toObject();
    const int now = o.value(QStringLiteral("st")).toInt(0);
    o.insert(QStringLiteral("st"), now == stars ? 0 : qBound(1, stars, 3));
    m_labStore.insert(key, o);
    m_labShowDirty = true;
    labSave();
    emit labChanged();
}

void TrackEngine::labSetNote(const QString &note)
{
    const QString key = labTagKey();
    if (m_labActive == false || key.isEmpty())
        return;
    QJsonObject o = m_labStore.value(key).toObject();
    const QString text = note.trimmed().left(200);
    if (o.value(QStringLiteral("note")).toString() == text)
        return;
    o.insert(QStringLiteral("note"), text);
    m_labStore.insert(key, o);
    labSave();
    emit labChanged();
}

void TrackEngine::labSetAuto(int mode)
{
    m_labAuto = qBound(0, mode, 3);
    m_labBeatsShown = 0;
    labRestartTimers();
    emit labChanged();
}

void TrackEngine::labSetColour(const QString &colour)
{
    if (m_labColours.contains(colour) == false)
        return;
    m_labColour = colour;
    labShow();
    emit labChanged();
}

void TrackEngine::labSetLamps(int lamps)
{
    m_labLamps = qBound(0, lamps, 2);
    labShow();
    emit labChanged();
}

void TrackEngine::labToggleHold()
{
    m_labHold = !m_labHold;
    m_labBeatsShown = 0;
    labRestartTimers();
    emit labChanged();
}

void TrackEngine::labToggleSlower()
{
    if (m_labActive == false || m_labPass == 0)
        return;
    m_labTrySlow = !m_labTrySlow;
    labRestartTimers();
    emit labChanged();
}

void TrackEngine::labStop()
{
    if (m_labActive == false)
        return;
    m_labDark = !m_labDark;                // STOP, and R411_OPEN_DARK's PLAY
    m_labMessage = m_labDark ? tr("Stopped - \u25b6 PLAY lights it again") : QString();
    labRestartTimers();
    labShow();
    emit labChanged();
}

void TrackEngine::labBan()
{
    // TOO LOW / AUDIENCE: dark first, then filed as never
    if (m_labActive == false)
        return;
    m_labDark = true;
    labShow();
    m_labAutoTimer.stop();
    const QString key = labCurrentKey();
    if (key.isEmpty() == false)
    {
        QJsonObject o = m_labStore.value(key).toObject();
        if (o.value(QStringLiteral("v")).toInt(0) == 1)
            m_labShowDirty = true;
        o.insert(QStringLiteral("v"), -1);
        o.insert(QStringLiteral("ban"), true);
        o.insert(QStringLiteral("t"), QDateTime::currentDateTime().toString(Qt::ISODate));
        m_labStore.insert(key, o);
        labSave();
        logSignal(QStringLiteral("sig:lab-ban:") + key);
    }
    // R411_OPEN_DARK: on to the next one, still dark - PLAY lights it
    labRefresh();
    if (m_labIndex + 1 < m_labList.count())
        m_labIndex++;
    m_labBeat = 0;
    m_labBeatsShown = 0;
    labRestartTimers();
    m_labMessage = tr("Never in the show. The laser is dark - \u25b6 PLAY goes on with the next.");
    emit labChanged();
}

void TrackEngine::labRemove(const QString &key)
{
    if (m_labStore.contains(key) == false)
        return;
    QJsonObject o = m_labStore.value(key).toObject();
    if (o.value(QStringLiteral("v")).toInt(0) != 1)
        return;
    o.insert(QStringLiteral("v"), 0);
    m_labStore.insert(key, o);
    m_labShowDirty = true;
    labSave();
    if (m_labActive)
    {
        labRefresh();
        if (m_labPass == 2)
            labShowCandidate();
    }
    emit labChanged();
}

/* ---------------------------------------------------------------------
 *  for QML
 * --------------------------------------------------------------------- */

bool TrackEngine::labAvailable() const
{
    // a lamp with a Graphic Group and a Group Selection channel - asked before
    // the table is built too (the SETUP tile), so read off the fixtures
    if (m_labFixtures.isEmpty() == false)
        return true;
    if (m_doc == nullptr)
        return false;
    foreach (Fixture *fxi, m_doc->fixtures())
    {
        if (fxi == nullptr)
            continue;
        bool graphic = false, pattern = false;
        for (quint32 ch = 0; ch < fxi->channels(); ch++)
        {
            const QLCChannel *qch = fxi->channel(ch);
            const QString r = qch != nullptr ? engineLabRole(qch->name()) : QString();
            graphic = graphic || r == QStringLiteral("graphic");
            pattern = pattern || r == QStringLiteral("pattern");
        }
        if (graphic && pattern)
            return true;
    }
    return false;
}

QString TrackEngine::labTitle() const { return labTitleOf(labCurrentKey()); }
QString TrackEngine::labDetail() const { return labDetailOf(labCurrentKey()); }

QStringList TrackEngine::labLocked() const
{
    const EngineLabKey k = engineLabParse(labCurrentKey());
    QStringList out;
    out.append(m_labColour.toUpper());
    if (m_labBase.contains(QStringLiteral("vpos")))
        out.append(tr("HEIGHT %1 · LOCKED").arg(m_labBase.value(QStringLiteral("vpos"))));
    out.append(tr("NO X/Y TILT"));
    if (k.kind == QStringLiteral("spin") || k.kind == QStringLiteral("turn"))
        out.append(tr("TURNS IN PLACE"));
    else
        out.append(tr("NO ROTATION"));
    if (engineLabSteps(k.kind) > 1)
        out.append(tr("ON THE BEAT"));
    return out;
}

QVariantList TrackEngine::labStrip() const
{
    QVariantList out;
    out.reserve(m_labList.count());
    foreach (const QString &key, m_labList)
        out.append(labVerdictOf(key));
    return out;
}

int TrackEngine::labCountOf(int verdict) const
{
    int n = 0;
    foreach (const QString &key, m_labList)
        n += labVerdictOf(key) == verdict ? 1 : 0;
    return n;
}

int TrackEngine::labApprovedCount() const
{
    int n = 0;
    for (QJsonObject::const_iterator it = m_labStore.constBegin(); it != m_labStore.constEnd(); ++it)
        n += (it.value().toObject().value(QStringLiteral("v")).toInt(0) == 1 && engineLabParse(it.key()).ok) ? 1 : 0;
    return n;
}

QVariantList TrackEngine::labApprovedList() const
{
    QVariantList out;
    foreach (const QString &key, labList(2))
    {
        const QJsonObject o = m_labStore.value(key).toObject();
        QVariantMap m;
        m.insert(QStringLiteral("key"), key);
        m.insert(QStringLiteral("title"), labTitleOf(key));
        m.insert(QStringLiteral("sections"), o.value(QStringLiteral("sec")).toArray().toVariantList());
        m.insert(QStringLiteral("energy"), o.value(QStringLiteral("en")).toArray().toVariantList());
        m.insert(QStringLiteral("character"), o.value(QStringLiteral("ch")).toArray().toVariantList());
        m.insert(QStringLiteral("stars"), o.value(QStringLiteral("st")).toInt(0));
        m.insert(QStringLiteral("note"), o.value(QStringLiteral("note")).toString());
        out.append(m);
    }
    return out;
}

QString TrackEngine::labTagTitle() const
{
    const QString key = labTagKey();
    return key.isEmpty() ? QString() : labTitleOf(key);
}

QStringList TrackEngine::labTagList(const QString &kind) const
{
    QStringList out;
    const QJsonArray a = m_labStore.value(labTagKey()).toObject().value(kind).toArray();
    for (const QJsonValue &v : a)
        out.append(v.toString());
    return out;
}

QStringList TrackEngine::labSections() const { return labTagList(QStringLiteral("sec")); }
QStringList TrackEngine::labEnergy() const { return labTagList(QStringLiteral("en")); }
QStringList TrackEngine::labCharacter() const { return labTagList(QStringLiteral("ch")); }

int TrackEngine::labStars() const
{
    return m_labStore.value(labTagKey()).toObject().value(QStringLiteral("st")).toInt(0);
}

QString TrackEngine::labNote() const
{
    return m_labStore.value(labTagKey()).toObject().value(QStringLiteral("note")).toString();
}

bool TrackEngine::labSlower() const
{
    return m_labTrySlow || m_labStore.value(labCurrentKey()).toObject().value(QStringLiteral("slow")).toBool();
}

/* ---------------------------------------------------------------------
 *  into the show: from ensureTable(), before the table reads the functions
 * --------------------------------------------------------------------- */

Scene *TrackEngine::labUpsertScene(QHash<QString, Function *> &mine, const QString &name,
                                   const QString &path, const QList<SceneValue> &values)
{
    Scene *scene = qobject_cast<Scene *>(mine.value(name));
    if (scene == nullptr)
    {
        scene = new Scene(m_doc);
        scene->setName(name);
        scene->setPath(path);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
        if (m_doc->addFunction(scene) == false)
        {
            delete scene;
            return nullptr;
        }
        mine.insert(name, scene);
        return scene;
    }
    // written again only when it changed: an unchanged show stays unmodified
    QMap<QPair<quint32, quint32>, uchar> want, have;
    foreach (const SceneValue &sv, values)
        want.insert(qMakePair(sv.fxi, sv.channel), sv.value);
    foreach (const SceneValue &sv, scene->values())
        have.insert(qMakePair(sv.fxi, sv.channel), sv.value);
    if (want != have)
    {
        foreach (SceneValue old, scene->values())
            scene->unsetValue(old.fxi, old.channel);
        foreach (SceneValue sv, values)
            scene->setValue(sv);
    }
    if (scene->isVisible() == false)
        scene->setVisible(true);
    return scene;
}

void TrackEngine::labEnsureShow()
{
    // Every kept look, in each colour this laser has, as a visible AUTO
    // programme in "AUTO Programs/Laser Lab" - a scene, or for a variant on
    // the beat a Beats-tempo chaser of steps in the step folder (visible,
    // like gen_programs' steps: QLC+ saves a hidden scene as noughts). Found
    // again by name, rewritten from laser-lab.json on every build. A look no
    // longer kept stays in the file and out of the table (m_labLive).
    m_labLive.clear();
    m_labFuncKey.clear();
    m_labOnlyGroups.clear();
    labLoad();
    labScan();
    if (m_doc == nullptr || labReady() == false)
        return;
    QHash<QString, Function *> mine;
    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr)
            continue;
        const QString path = func->path(true);
        if (path == ENGINE_LAB_PATH || path == ENGINE_LAB_STEP_PATH)
            mine.insert(func->name(), func);
    }
    QSet<QString> bases;
    foreach (const QString &key, labList(2))
    {
        const EngineLabKey k = engineLabParse(key);
        const QJsonObject o = m_labStore.value(key).toObject();
        if (k.ok == false || o.value(QStringLiteral("ban")).toBool())
            continue;
        const int steps = engineLabSteps(k.kind);
        const bool slow = o.value(QStringLiteral("slow")).toBool();
        const QString look = key.section(QLatin1Char('/'), 0, 0) + QLatin1Char(' ') + engineLabKindWord(k.kind);
        foreach (const QString &colour, m_labColours)
        {
            QString cap = colour;
            cap[0] = cap.at(0).toUpper();
            const QString name = QStringLiteral("AUTO ") + look + QLatin1Char(' ') + cap;
            Function *made = nullptr;
            if (steps == 1)
                made = labUpsertScene(mine, name, ENGINE_LAB_PATH, labSceneValues(key, colour, 0, 0));
            else
            {
                QList<quint32> fids;
                for (int s = 0; s < steps; s++)
                {
                    Scene *st = labUpsertScene(mine, name + QLatin1Char(' ') + QString::number(s + 1),
                                               ENGINE_LAB_STEP_PATH, labSceneValues(key, colour, s, 0));
                    if (st != nullptr)
                        fids.append(st->id());
                }
                if (fids.count() != steps)
                    continue;
                const uint dur = slow ? 2000 : 1000;     // beats x 1000: one beat a step, or two
                Chaser *chaser = qobject_cast<Chaser *>(mine.value(name));
                if (chaser == nullptr)
                {
                    chaser = new Chaser(m_doc);
                    chaser->setName(name);
                    chaser->setPath(ENGINE_LAB_PATH);
                    chaser->setTempoType(Function::Beats);
                    chaser->setRunOrder(Function::Loop);
                    chaser->setFadeInMode(Chaser::Common);
                    chaser->setFadeOutMode(Chaser::Common);
                    chaser->setDurationMode(Chaser::Common);
                    chaser->setFadeInSpeed(0);
                    chaser->setFadeOutSpeed(0);
                    chaser->setDuration(dur);
                    foreach (quint32 fid, fids)
                        chaser->addStep(ChaserStep(fid, 0, dur, 0));
                    if (m_doc->addFunction(chaser) == false)
                    {
                        delete chaser;
                        continue;
                    }
                    mine.insert(name, chaser);
                }
                else
                {
                    QList<quint32> have;
                    foreach (const ChaserStep &cs, chaser->steps())
                        have.append(cs.fid);
                    if (have != fids)
                    {
                        while (chaser->stepsCount() > 0)
                            chaser->removeStep(0);
                        foreach (quint32 fid, fids)
                            chaser->addStep(ChaserStep(fid, 0, dur, 0));
                    }
                    if (chaser->duration() != dur)
                        chaser->setDuration(dur);
                    if (chaser->isVisible() == false)
                        chaser->setVisible(true);
                }
                made = chaser;
            }
            if (made != nullptr)
            {
                m_labLive.insert(made->id());
                m_labFuncKey.insert(made->id(), key);
                bases.insert(key.section(QLatin1Char('/'), 0, 0));
            }
        }
    }
    // R410_LAB_ONLY: with enough of his own, the laser plays his kept looks
    // and no longer borrows the busking scenes (motionFor)
    if (bases.count() >= ENGINE_LAB_ONLY)
        m_labOnlyGroups.insert(m_labGroup);
    emit labChanged();                   // the SETUP tile: there is a laser for the lab
}

void TrackEngine::labApplyTags()
{
    // after the roles: what he said, over what the names would say
    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
    {
        TrackFuncInfo &info = it.value();
        QHash<quint32, QString>::const_iterator kc = m_labFuncKey.constFind(info.id);
        if (kc == m_labFuncKey.constEnd())
            continue;
        const QJsonObject o = m_labStore.value(kc.value()).toObject();
        info.lab = true;
        info.guess = ENGINE_ROLE_MOTION;
        info.role = ENGINE_ROLE_MOTION;
        info.tier = engineLabTier(o.value(QStringLiteral("sec")).toArray());
        const int st = o.value(QStringLiteral("st")).toInt(0);
        info.stars = info.starsGuess = st > 0 ? qBound(1, st, 3) : 1;   // a plain GOOD: anywhere, one star
        engineLabWindow(o.value(QStringLiteral("en")).toArray(), info.labMin, info.labMax);
        info.labCalm = o.value(QStringLiteral("ch")).toArray().contains(QJsonValue(QStringLiteral("calm")));
    }
}
