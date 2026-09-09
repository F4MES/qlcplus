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

#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QTime>
#include <QTextStream>
#include <QPair>
#include <QDir>
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
#include "efxfixture.h"
#include "function.h"
#include "fixture.h"
#include "chaser.h"
#include "scene.h"
#include "efx.h"
#include "doc.h"

#define ENGINE_INTENSITY_ATTR 0
#define ENGINE_DIMMER_PREFIX  QStringLiteral("TRACK Dimmer: ")
#define ENGINE_COLOUR_PREFIX  QStringLiteral("TRACK Colour: ")
#define ENGINE_POS_PREFIX     QStringLiteral("TRACK Pos: ")
#define ENGINE_HOME_PREFIX    QStringLiteral("TRACK Home: ")
#define ENGINE_HOMEB_PREFIX   QStringLiteral("TRACK Home B: ")
#define ENGINE_EFX_PREFIX     QStringLiteral("TRACK EFX: ")
#define ENGINE_ZOOM_PREFIX    QStringLiteral("TRACK Zoom: ")
#define ENGINE_STROBE_PREFIX  QStringLiteral("TRACK Strobe: ")
#define ENGINE_OFF_PREFIX     QStringLiteral("TRACK Off: ")
// How many strobe rates ensureStrobeScenes() builds per group. driveStrobe()
// draws an index in this range, so the two must never disagree.
#define ENGINE_STROBE_RATES   6

/* colours the house does not like: never in the palette, never as an accent,
 * never generated - even when a scene of that colour exists */
static bool engineBannedColour(const QString &colour)
{
    return colour == QLatin1String("yellow");
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
    , m_colourCursor(0)
    , m_colourBar(-1)
    , m_colourSince(-1)
    , m_holdNow(32)
    , m_castCursor(0)
    , m_motionCursor(0)
    , m_master(1.0)
    , m_blackout(false)
    , m_mixing(false)
    , m_speed(0)
    , m_flash(false)
    , m_effects(0)
    , m_effectsBefore(0)
    , m_starCeil(0)
    , m_lastBeat(0)
    , m_calmUntil(0)
    , m_logEnabled(true)
    , m_dropStyle(0)
    , m_strobeUntil(-1)
    , m_strobeSeen(-1)
    , m_strobeRate(0)
    , m_beatMs(500.0)
    , m_beatStartMs(0)
    , m_beatIndex(0)
    , m_room(2)
    , m_roomAuto(true)
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
    m_docTimer.setSingleShot(true);
    m_docTimer.setInterval(0);         // the next turn of the event loop
    connect(&m_docTimer, SIGNAL(timeout()), this, SLOT(slotDocSettled()));

    m_fadeTimer.setInterval(20);       // 50 a second: a fade, not a staircase
    connect(&m_fadeTimer, SIGNAL(timeout()), this, SLOT(slotFadeTimer()));
    m_clock.start();
    m_pulseTimer.setInterval(20);      // the breath is a slow sine: 25 Hz showed
    connect(&m_pulseTimer, SIGNAL(timeout()), this, SLOT(slotPulseTimer()));
    // MASTER is deliberately not restored: a night that starts at 40 %
    // because someone dimmed last time is worse than one that starts bright
    m_master = 1.0;
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt();
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_fullAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
    // NOT per show here: the constructor runs before any workspace is loaded,
    // so the fingerprint would be empty. loadRoles() re-reads it per show once
    // the table is built; this is only the starting point.
    foreach (QString key, settings.value(SETTINGS_ENGINE_GROUPOFF, QString())
                                  .toString().split(';', Qt::SkipEmptyParts))
        m_groupOff.insert(key);

    if (m_doc != nullptr)
    {
        connect(m_doc, SIGNAL(loaded()), this, SLOT(slotDocChanged()));
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
    tickFades();
    if (m_fadeAttr.isEmpty())
        m_fadeTimer.stop();
}

void TrackEngine::slotDocChanged()
{
    // Loading a project emits functionRemoved once per function, fixtureRemoved
    // once per fixture and fixtureGroupRemoved once per group, and then the
    // same again on the way in - hundreds of signals for one event. Tearing the
    // show down and rebuilding the table on each of them is not only wasted
    // work: ensureTable() adds hidden scenes, and Doc::clearContents() iterates
    // a SNAPSHOT of its function list, so scenes added in that window survive
    // the clear and leak into the next project holding values for fixtures that
    // are about to be deleted. One rebuild once the storm has passed.
    m_dirty = true;
    if (m_docTimer.isActive() == false)
        m_docTimer.start();
}

void TrackEngine::slotDocSettled()
{
    m_dirty = true;
    m_position.clear();
    m_moves.clear();
    m_sweep.clear();
    m_sweepShown.clear();
    m_sweepFunc.clear();
    m_splitScenes.clear();
    m_zoomScenes.clear();
    m_strobeScenes.clear();
    m_offScenes.clear();
    m_strobeUntil = -1;
    m_strobeSeen = -1;
    m_strobeRate = 0;
    // setHaze/setFan early-return on an unchanged value, so a stale reading
    // here left the slider dead until it was moved somewhere else first
    m_haze = 0.0;
    m_fan = 0.0;
    m_flash = false;
    m_zoom.clear();
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
    m_pulseTimer.stop();
    m_fadeTimer.stop();
    // every 'live' property (cast, report, warnings, colour, trims) notifies
    // on liveChanged: without it they keep showing the last project's state
    // until a beat happens to arrive
    m_report.clear();
    m_warnings.clear();
    emit tableChanged();
    emit liveChanged();
}

void TrackEngine::slotPulseTimer()
{
    // between two beats: let every breathing group's dimmers fall back from
    // the level the beat set, so the light pumps with the kick
    bool any = false;
    qint64 now = m_clock.elapsed();
    foreach (const QString &key, m_cast)
    {
        // a held flash is full: nothing steps or pulses under it
        if (m_flashHeld.contains(key))
            continue;
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
            if (mv.pulse > 0.0 && sub != m_subStepSeen.value(key, 0))
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
        if (m_pulseDepth.value(key, 0.0) <= 0.0 && m_breathe.value(key, 0) <= 0)
            continue;
        any = true;
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
                qreal out = m_blackout ? 0.0 : qBound(0.0, m_activeLevel.value(slot, 0.0) * f * m_groupTrim.value(key, 1.0) * m_master, 1.0);
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
            // short words must stand alone, so "up" does not match "group"
            QRegularExpression re(QStringLiteral("(^|[^a-z\\x{00e6}\\x{00f8}\\x{00e5}])%1([^a-z\\x{00e6}\\x{00f8}\\x{00e5}]|$)").arg(w));
            if (re.match(text).hasMatch())
                return true;
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
    QRegularExpression suffix(QStringLiteral("[a-z]([BRWG])\\s*$"));
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
            foreach (SceneValue sv, scene->values())
                out.insert(sv.fxi);
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
                    allPos = false;
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

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
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
            if (hollow != nullptr && hollow->values().isEmpty())
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

        QSet<quint32> touched = fixturesOf(func, 0);
        info.fixtureCount = touched.count();
        foreach (quint32 fid, touched)
        {
            QString key = groupOfFixture(fid);
            if (key.isEmpty() == false)
                info.groups.insert(key);
        }

        // A scene that carries the master dimmer itself cannot be dimmed by
        // the group dimmer (HTP: the higher value wins), so the engine has to
        // scale such a scene through its own intensity attribute instead.
        Scene *scene = qobject_cast<Scene *>(func);
        if (scene != nullptr)
        {
            foreach (SceneValue sv, scene->values())
            {
                Fixture *fxi = m_doc->fixture(sv.fxi);
                if (fxi != nullptr && sv.channel == dimmerChannel(fxi) && sv.value > 0)
                    info.dimmer = true;
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

    ensureColourScenes();
    ensureStrobeScenes();
    ensureOffScenes();
    learnHome();
    ensurePositionScenes();
    ensureSweeps();
    ensureZoomScenes();
    ensureDimmerScenes();
    ensureAtmosScenes();

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
    QString n = info.name.toLower();

    if (info.type == int(Function::SceneType) || info.type == int(Function::CollectionType))
        return 1;                                   // a look, not a movement
    if (hasWord(n, hot))
        return 3;
    if (hasWord(n, cool))
        return 1;
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
        return 0;
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
            foreach (SceneValue sv, scene->values())
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
                else if (constant == false && coloursHere >= 2
                         && qch->group() != QLCChannel::Pan && qch->group() != QLCChannel::Tilt)
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
                if (qch != nullptr && qch->name().contains(QRegularExpression(QStringLiteral("(colou?r|eye)\\s*\\d+"), QRegularExpression::CaseInsensitiveOption)))
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
    // Three hidden zoom scenes per group of moving heads - narrow, mid,
    // wide - so the beam is a move of its own: tight beams for big figures
    // in a drop, a wide wash in a break, wide for a bar when a drop lands.
    static const char *names[3] = { "Narrow", "Mid", "Wide" };
    static const int levels[3] = { 40, 130, 225 };

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
        for (int z = 0; z < 3; z++)
        {
            QList<SceneValue> values;
            foreach (quint32 fid, g.fixtures)
            {
                Fixture *fxi = m_doc->fixture(fid);
                if (fxi == nullptr)
                    continue;
                for (quint32 ch = 0; ch < fxi->channels(); ch++)
                {
                    const QLCChannel *qch = fxi->channel(ch);
                    if (qch == nullptr)
                        continue;
                    if (qch->preset() == QLCChannel::BeamZoomSmallBig)
                        values.append(SceneValue(fid, ch, uchar(levels[z])));
                    else if (qch->preset() == QLCChannel::BeamZoomBigSmall)
                        values.append(SceneValue(fid, ch, uchar(255 - levels[z])));
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
            ids.append(scene->id());
        }
        if (ids.count() == 3)
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

        // the heads it drives: every pan/tilt head of every fixture in the group
        QList<QPair<quint32, int> > want;
        foreach (quint32 fid, g.fixtures)
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
        efx->setXOffset(127);
        efx->setYOffset(127);
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
            quint32 fid = colourFunction(key, hue);
            if (fid == Function::invalidId())
                fid = colourFunction(key, QStringLiteral("white"));
            if (fid != Function::invalidId())
            {
                // Colour channels are HTP, so a red accent over a running
                // blue base would mix to magenta. White never showed this
                // because 255,255,255 wins every channel. The colour scene
                // steps aside for the flash and comes back on the next beat.
                if (hue != QStringLiteral("white"))
                    stopSlot("col:" + key, true);
                run("flash:" + key, fid, 1.0, 0, true);
            }
            // full means full: no pulse or breath on the flash itself
            qreal keepDepth = m_pulseDepth.value(key, 0.0);
            int keepBreath = m_breathe.value(key, 0);
            m_pulseDepth.insert(key, 0.0);
            m_breathe.insert(key, 0);
            setDimmer(key, 1.0);
            m_pulseDepth.insert(key, keepDepth);
            m_breathe.insert(key, keepBreath);
            m_flashHeld.insert(key);
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
        { "cyan", 0, 255, 255, 0 },    { "magenta", 255, 0, 255, 0 }, { "yellow", 255, 255, 0, 0 },
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
                if (isWhite && wcs.isEmpty() == false)
                {
                    // A real white channel: use it alone. R+G+B on top only
                    // makes it colder and dirtier.
                    // And on a strobe it is capped: those three lamps at a
                    // full white channel are painful to stand in front of.
                    foreach (quint32 c, wcs) values.append(SceneValue(fid, c, uchar(g.strobes ? 153 : 255)));
                    foreach (quint32 c, rcs) values.append(SceneValue(fid, c, uchar(0)));
                    foreach (quint32 c, gcs) values.append(SceneValue(fid, c, uchar(0)));
                    foreach (quint32 c, bcs) values.append(SceneValue(fid, c, uchar(0)));
                    coloured = true;
                }
                else if (isWhite && g.colourValue.contains(fid) == false)
                {
                    // no white channel and nothing learned from a scene of the
                    // operator's: this fixture sits the white out rather than
                    // faking one by driving red, green and blue to full
                    continue;
                }
                else if (sw != nullptr && rcs.isEmpty() == false && gcs.isEmpty() == false && bcs.isEmpty() == false)
                {
                    foreach (quint32 c, rcs) values.append(SceneValue(fid, c, uchar(sw->r)));
                    foreach (quint32 c, gcs) values.append(SceneValue(fid, c, uchar(sw->g)));
                    foreach (quint32 c, bcs) values.append(SceneValue(fid, c, uchar(sw->b)));
                    foreach (quint32 c, wcs) values.append(SceneValue(fid, c, uchar(sw->w)));
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
                // and an open shutter, when the definition says which value that is
                for (quint32 i = 0; i < fxi->channels(); i++)
                {
                    const QLCChannel *qch = fxi->channel(i);
                    if (qch == nullptr || qch->group() != QLCChannel::Shutter || base.contains(i))
                        continue;
                    foreach (QLCCapability *cap, qch->capabilities())
                    {
                        if (cap != nullptr && cap->preset() == QLCCapability::ShutterOpen)
                        {
                            values.append(SceneValue(fid, i, uchar(cap->min())));
                            break;
                        }
                    }
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
        QString black = "black:" + key;
        if (m_blackout && fid != Function::invalidId())
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
        Function *func = fid != Function::invalidId() ? m_doc->function(fid) : nullptr;
        if (func != nullptr && func->isRunning())
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

    // The DJ scrubbed backwards. A burst is at most four beats, so any
    // backwards jump can leave an end beat in the future - and then
    // "beat <= m_strobeUntil" stays true for the whole length of the jump and
    // the strobe runs solid. It cannot be checked against m_lastBeat: tick()
    // has already set that to this beat by the time we get here.
    if (m_strobeSeen >= 0 && beat < m_strobeSeen)
        m_strobeUntil = -1;
    m_strobeSeen = beat;

    if (quiet)
    {
        m_strobeUntil = -1;
    }
    else if (beat > m_strobeUntil)
    {
        int want = -1, beats = 2;
        // Which rate is mostly a DRAW, not a function of the energy: every one
        // of them is inside the usable band, so what the energy buys is how
        // often the strobe comes and how long it stays, not how fast it runs.
        // Drawing it means two bursts in a row are never quite the same.
        int drawn = int(rng->bounded(rateCount));
        // The riser starts earlier in the build the higher the fader is: at a
        // quarter it only arrives in the last eighth, at the top it runs the
        // last third of the build - and there it does climb, because a riser
        // that speeds up is the whole point of a riser.
        qreal riserFrom = 0.90 - 0.30 * w;
        if (isBuild && prog > riserFrom && e > 0.18)
        {
            qreal into = qBound(0.0, (prog - riserFrom) / qMax(0.02, 1.0 - riserFrom), 1.0);
            want = int(qRound(into * qreal(rateCount - 1)));
            beats = 1;
        }
        else if (isDrop && bar == 0 && beatInBar == 0 && e > 0.25)
        {
            want = drawn;                                // the drop lands
            beats = 1 + int(qRound(2.0 * w));
        }
        else if (isDrop && w > 0.0 && beatInBar == 0 && roll(0.05 + 0.70 * w))
        {
            want = drawn;                                // and again, more of it
            beats = 1 + int(qRound(3.0 * w));
        }
        else if (isDrop == false && isBuild == false && w > 0.0
                 && (bar % qMax(2, 10 - int(qRound(8.0 * w)))) == 0
                 && beatInBar == 3 && roll(0.10 + 0.50 * w))
        {
            want = drawn;                                // a groove gets a taste
            beats = 1;
        }
        if (want >= 0)
        {
            m_strobeRate = want;
            m_strobeUntil = beat + beats - 1;
        }
    }

    bool on = beat <= m_strobeUntil;
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
        bool joins = cast.contains(key) && (g.strobes || w > 0.55);
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
        int r = qBound(0, g.strobes ? m_strobeRate : qMax(0, m_strobeRate - 2),
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
    }
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

    // stop() is deferred to the MasterTimer thread, so a scene that has just
    // been told to stop still says it is running. Push the slider back up
    // inside that tick and the start would be skipped and the queued stop
    // would land: the hazer sits off with the slider up.
    if (value > 0 && (scene->isRunning() == false || scene->stopped()))
        scene->start(m_doc->masterTimer(), FunctionParent::master());
    else if (value == 0 && scene->stopped() == false)
        scene->stop(FunctionParent::master());   // ... and the same the other way
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
}

void TrackEngine::rebuild()
{
    m_dirty = true;
    ensureTable();
    emit tableChanged();
}

QVariantList TrackEngine::table()
{
    ensureTable();

    QVariantList list;
    QList<TrackFuncInfo> rows = m_funcs.values();
    std::sort(rows.begin(), rows.end(), [](const TrackFuncInfo &a, const TrackFuncInfo &b) {
        bool ha = a.junk || a.step || a.groups.isEmpty();
        bool hb = b.junk || b.step || b.groups.isEmpty();
        if (ha != hb) return hb;                 // the usable looks first
        // the same group name the row shows: a QSet iterates in hash order
        QStringList la = a.groups.values(); la.sort();
        QStringList lb = b.groups.values(); lb.sort();
        QString ga = la.isEmpty() ? QString() : la.first();
        QString gb = lb.isEmpty() ? QString() : lb.first();
        if (ga != gb) return ga < gb;
        return a.name.toLower() < b.name.toLower();
    });

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
        if (hidden && m_showAll == false && info.role < 0)
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
    if (m_funcs.contains(fid) == false)
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
    if (m_funcs.contains(fid) == false)
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
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        int colours = 0, motions = 0;
        for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        {
            if (it.value().groups.contains(key) == false) continue;
            if (it.value().role == ENGINE_ROLE_COLOR) colours++;
            if (it.value().role == ENGINE_ROLE_MOTION) motions++;
        }
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
        row.insert("base", key == baseGroup());
        list.append(row);
    }
    return list;
}

void TrackEngine::setGroupEnabled(QString key, bool enable)
{
    // Already precise: it names the group, so it blames exactly the program
    // that was on it. No tapping required.
    if (enable != (m_groupOff.contains(key) == false))
        logSignal((enable ? QStringLiteral("sig:group-on:") : QStringLiteral("sig:group-off:")) + key);
    if (enable) m_groupOff.remove(key); else m_groupOff.insert(key);
    saveRoles();
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
            m_pulseDepth.remove(key);
            m_breathe.remove(key);
        }
        applyGroupOff();
    }
    if (m_startScene)
        startLook();                     // the opening picture follows the switches
    emit tableChanged();
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
    // A fader is dragged, not tapped, so this would write a line per frame.
    // One per group per logged beat is enough to see the gesture and where it
    // ended, and the beat lines around it carry the rest.
    if (m_trimLogged.value(key, -1) != m_logBeatNo)
    {
        m_trimLogged.insert(key, m_logBeatNo);
        logSignal(QStringLiteral("sig:trim:") + key
                  + QLatin1Char('=') + QString::number(level, 'f', 2));
    }
    m_groupTrim.insert(key, level);

    // straight onto everything that is lit for this group - the dimmer parts
    // and the colour scene alike - without waiting for the beat
    reapplyLevels();
    emit liveChanged();
}

QString TrackEngine::baseGroup() const
{
    if (m_base.isEmpty() == false && m_groups.contains(m_base) && m_groupOff.contains(m_base) == false)
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
    ensureTable();
    // ON -> BASE -> OFF -> ON. The base may also be picked automatically when
    // none is set; the first tap on that one pins it, so the cycle carries on
    // from there instead of flipping between BASE and OFF forever
    if (m_groupOff.contains(key))
    {
        m_groupOff.remove(key);                 // OFF -> ON
        if (m_base == key) m_base.clear();
    }
    else if (m_base == key)
    {
        m_groupOff.insert(key);                 // BASE -> OFF
        m_base.clear();
    }
    else
    {
        m_base = key;                           // ON (or the automatic base) -> BASE
    }
    QSettings().setValue(SETTINGS_ENGINE_BASE, m_base);
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
    QSettings().setValue(SETTINGS_ENGINE_FULLAUTO, m_fullAuto);
    // whatever runs now may be a function that is no longer allowed
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:") == false)
            stopSlot(slot, false);
    }
    m_position.clear();
    m_moves.clear();
    m_dirty = true;
    // the soft stops are stepped down by the fade timer, and no beat may come
    if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
        m_fadeTimer.start();
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
    if (colour == m_override)
        return;
    logSignal(colour.isEmpty() ? QStringLiteral("sig:colour-auto")
                               : QStringLiteral("sig:colour:") + colour);
    m_override = colour;
    m_startColour = false;               // a tile the DJ tapped is theirs
    if (colour.isEmpty() == false)
        m_colour = colour;
    else if (engineBannedColour(m_colour))
        m_colour = m_palette.isEmpty() ? QString() : m_palette.first();
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
    logSignal(on ? QStringLiteral("sig:blackout") : QStringLiteral("sig:blackout-off"));
    m_blackout = on;

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
    QSettings settings;
    QJsonObject obj;
    foreach (const QString &key, settings.allKeys())
    {
        if (key.startsWith(QStringLiteral("trackengine/")) == false && key.startsWith(QStringLiteral("trackmanager/")) == false)
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
    QFile file(engineSettingsPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) == false)
        return tr("could not write %1").arg(file.fileName());
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();
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
        settings.setValue(key, v);
        n++;
    }
    // take them on board: roles, stars and options are read in ensureTable
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt();
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_logEnabled = settings.value(SETTINGS_ENGINE_LOG, true).toBool();
    bool wantAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
    if (wantAuto != m_fullAuto)
    {
        m_fullAuto = !wantAuto;            // let the setter do its teardown
        setFullAuto(wantAuto);
    }
    m_groupOff.clear();
    foreach (QString key, settings.value(SETTINGS_ENGINE_GROUPOFF, QString()).toString().split(';', Qt::SkipEmptyParts))
        m_groupOff.insert(key);
    m_dirty = true;
    m_moves.clear();
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
            stopSlot(slot, true);
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
        quint32 fid = flashFunction(strobeGroups, "white");
        if (fid == Function::invalidId())
            fid = flashFunction(allOn, "white");
        if (fid != Function::invalidId())
            run("flash", fid, 1.0, 0, true);
        else
            genFlash(true);
    }
    else
    {
        stopSlot("flash", true);
        genFlash(false);
    }
    emit liveChanged();
}

QString TrackEngine::report() const { return m_report; }

/*********************************************************************
 * Choosing
 *********************************************************************/

QList<TrackFuncInfo *> TrackEngine::candidates(int role, const QString &group) const
{
    QList<TrackFuncInfo *> out;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.role != role)
            continue;
        // Hard, and hard on purpose: no weighting, no floor, no "unless
        // nothing else is left". The operator said never.
        if (info.banned)
            continue;
        // Per-group slots must never start a whole-room snapshot. Its other
        // groups would bypass cast, colour and intensity decisions. Such
        // looks remain available as START scenes and on the Virtual Console.
        if (group.isEmpty() == false
            && (info.groups.contains(group) == false || info.groups.count() != 1))
            continue;
        if (m_doc->function(info.id) == nullptr)
            continue;
        if (userAllowed(info, group) == false)
            continue;
        out.append(const_cast<TrackFuncInfo *>(&info));
    }
    std::sort(out.begin(), out.end(), [](TrackFuncInfo *a, TrackFuncInfo *b) { return a->id < b->id; });
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
        // a learned colour channel is this group's colour, whatever the
        // definition calls it
        if (g.colourValue.value(sv.fxi).contains(sv.channel))
            return true;
    }
    return false;
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
    // a colourless look for this group (the group has no named colours)
    foreach (TrackFuncInfo *info, list)
    {
        if (info->groups.count() == 1 && info->colour.isEmpty())
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

quint32 TrackEngine::motionFor(const QString &group, const QString &colour,
                               const QSet<QString> &cast, int cursor, int tier,
                               qreal bpm, int division, bool staticOnly, int maxStars) const
{
    Q_UNUSED(bpm)
    Q_UNUSED(division)
    QList<TrackFuncInfo *> all = candidates(ENGINE_ROLE_MOTION, group);
    QList<TrackFuncInfo *> ok;
    foreach (TrackFuncInfo *info, all)
    {
        // a static pattern scene is a look and may show in any section; a
        // chase or EFX is movement and belongs to drops and builds
        if (staticOnly && info->type != int(Function::SceneType))
            continue;
        // energy stars: a three-star chase waits for a full-energy drop
        if (qMax(1, info->stars) > maxStars)
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
        if (info->colour.isEmpty() || info->colour == colour)
            ok.append(info);
    }
    if (ok.isEmpty())
        return Function::invalidId();

    // the pattern made in this colour beats the colourless one, which would
    // otherwise overwrite the palette with its own colour channel
    QList<TrackFuncInfo *> exact;
    foreach (TrackFuncInfo *info, ok)
    {
        if (info->colour == colour)
            exact.append(info);
    }
    if (exact.isEmpty() == false)
        ok = exact;

    // this tier's motions first
    QList<TrackFuncInfo *> tagged;
    foreach (TrackFuncInfo *info, ok)
    {
        if (info->tier == tier)
            tagged.append(info);
    }
    if (tagged.isEmpty() == false)
        ok = tagged;

    // of what is allowed, the hottest: a drop at full energy takes the
    // three-star chases, not the one-star ones it could also have had
    int top = 0;
    foreach (TrackFuncInfo *info, ok)
        top = qMax(top, qMax(1, info->stars));
    QList<TrackFuncInfo *> hot;
    foreach (TrackFuncInfo *info, ok)
    {
        if (qMax(1, info->stars) == top)
            hot.append(info);
    }
    if (hot.isEmpty() == false)
        ok = hot;

    return pickWeighted(ok, cursor);
}

int TrackEngine::tierOf(const QString &text) const
{
    static const QStringList breakWords  = { "break", "slow", "center", "centre", "calm", "low" };
    static const QStringList grooveWords = { "fan", "groove", "medium", "normal" };
    static const QStringList dropWords   = { "drop", "cross", "high", "wide", "eight", "fast" };
    if (hasWord(text, dropWords))   return 2;
    if (hasWord(text, breakWords))  return 0;
    if (hasWord(text, grooveWords)) return 1;
    return -1;
}

quint32 TrackEngine::homePosition(const QString &group) const
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
    return best != nullptr ? best->id : Function::invalidId();
}

quint32 TrackEngine::positionFunction(const QString &group, int cursor, int tier) const
{
    QList<TrackFuncInfo *> all = candidates(ENGINE_ROLE_POSITION, group);
    bool lasers = m_groups.value(group).lasers;

    QList<TrackFuncInfo *> safe;
    foreach (TrackFuncInfo *info, all)
    {
        // a laser sweep runs on its own only when its name says it stays low -
        // the rest are there for the operator to choose by hand
        // "low" as a WORD: "Slow" and "Yellow" are not a promise to stay low
        if (lasers && (info->sweep || info->type == int(Function::ChaserType))
            && hasWord(info->name.toLower(), QStringList() << "low") == false)
            continue;
        safe.append(info);
    }
    if (safe.isEmpty())
        return Function::invalidId();

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
    return pickWeighted(pool, cursor);
}

QString TrackEngine::accentFor(const QString &colour) const
{
    // pairs that sit well together - what the hands would pick
    static const QMap<QString, QStringList> pairs =
    {
        { "blue",    { "white", "cyan" } },
        { "red",     { "amber", "white" } },
        { "cyan",    { "magenta", "white" } },
        { "green",   { "white" } },
        { "magenta", { "blue", "white" } },
        { "white",   { "blue", "cyan" } },
        { "yellow",  { "amber", "white" } },
        { "orange",  { "amber", "red" } },
        { "amber",   { "red", "white" } },
        { "uv",      { "white" } },
    };
    // of the partners the palette has, one at random - the same pair every
    // drop would be a habit, not a choice
    QStringList have;
    foreach (const QString &p, pairs.value(colour))
    {
        if (m_palette.contains(p) && engineBannedColour(p) == false && p != colour)
            have << p;
    }
    if (have.isEmpty())
        return QString();
    return have.at(int(QRandomGenerator::global()->bounded(have.count())));
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
        bool inside = info->groups.isEmpty() == false;
        foreach (const QString &g, info->groups)
        {
                if (cast.contains(g) == false)
                    inside = false;
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
                       qreal kick, qreal high)
{
    if (m_doc == nullptr)
        return;
    if (m_startScene)            // the opening picture is up: nothing else runs
        return;
    ensureTable();
    tickFades();
    m_lastBeat = beat;

    // the clock moves the ENERGY slider (through roomChanged), so the time
    // of night is already in the energy that arrives here
    announceRoom();

    // NEXT: treat this beat as a fresh section with a fresh colour
    bool forceNext = m_forceNext;
    m_forceNext = false;
    // ENERGY at zero is the restaurant: the base stands in its colour and
    // nothing moves or changes - no pulse, no patterns, no colour rotation,
    // no positions. A hold the slider imposes.
    bool still = energy < 0.03;
    QRandomGenerator *rng = QRandomGenerator::global();
    bool hold = (m_hold || still) && forceNext == false;      // NEXT breaks a hold for one beat
    if (forceNext)
    {
        sectionChanged = true;
        m_moves.clear();
        m_sweep.clear();
    }

    // a track is playing: the start scene steps aside
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            stopSlot(slot, false);
    }

    // Rekordbox' own phrase analysis, when the track has one, hands us two
    // more section types than our own analysis produces. Both behave like a
    // break - quiet, one figure moving - but an intro is the room before the
    // track has started and an outro is it letting go, so both sit a little
    // lower and neither gets the hardware strobe.
    bool isIntro = (state == QStringLiteral("intro"));
    bool isOutro = (state == QStringLiteral("outro"));
    bool isBreak = (state == QStringLiteral("break")) || isIntro || isOutro;
    bool isBuild = (state == QStringLiteral("build"));
    bool isDrop  = (state == QStringLiteral("drop"));
    int tier = isBreak ? 0 : (isDrop ? 2 : 1);
    bool isCalm = beat < m_calmUntil;
    // a mix: two decks on air. Whatever the analysis says, this is groove
    // at most - the drop belongs to the outgoing track
    if (m_mixing && isBreak == false)
    {
        isDrop = false;
        isBuild = false;
        tier = 1;
    }

    // a flag edited to sit ahead of us, or a jump back in the track, makes
    // beat - secStart negative: bar and beatInBar go with it, every downbeat
    // test stops firing and patternMask() picks a negative step, which leaves
    // a chase dark. Clamp once, here, and everything downstream is safe.
    secStart = qMin(secStart, beat);
    int len = qMax(1, secEnd - secStart);
    qreal prog = qBound(0.0, qreal(beat - secStart) / qreal(len), 1.0);
    int bar = (beat - secStart) / 4;
    int beatInBar = (beat - secStart) % 4;

    // the drop is one bar away: pull the cast in now so the hit lands lit
    bool preDrop = isDrop == false && m_mixing == false && hold == false
                && nextState == QStringLiteral("drop")
                && beatsToNext > 0 && beatsToNext <= 4;

    /* ---- palette: one colour, changed rarely. A fresh track keeps the colour
     *      it arrived with until its first break or drop. ---- */
    // the hold is counted from the last change and varies around the SETUP
    // value (x0.5, x0.75, x1, x1.5), always ending on a bar line - so the
    // colour does not change on the same beat of every track
    bool holdUp = m_colourSince >= 0 && beatInBar == 0
               && beat - m_colourSince >= qMax(4, m_holdNow * 4);
    bool changeColour;
    if (m_colour.isEmpty())
        changeColour = true;
    else if (m_colourBar < 0)
        changeColour = sectionChanged && (isBreak || isDrop);
    else
        changeColour = (sectionChanged && (isBreak || isDrop)) || holdUp;
    if (isCalm)
        changeColour = m_colour.isEmpty();
    if (forceNext)
        changeColour = true;
    if ((hold || m_mixing) && m_colour.isEmpty() == false)
        changeColour = false;
    if (changeColour || m_colourBar >= 0)
        m_colourBar = 0;
    if (changeColour)
    {
        m_colourSince = beat;
        static const qreal stretch[4] = { 0.5, 0.75, 1.0, 1.5 };
        m_holdNow = qMax(4, int(qRound(m_holdBars * stretch[rng->bounded(4)])));
    }

    if (engineBannedColour(m_override))
        m_override.clear();
    if (m_override.isEmpty() == false)
        m_colour = m_override;
    else if (changeColour && m_palette.isEmpty() == false)
    {
        // Drawn, not counted through. Round-robin means the same order every
        // night, and white sat in the rotation like a colour - it is not one,
        // it is a punctuation mark. It comes up about one change in six now,
        // and the flash still reaches for it whenever it likes.
        m_colourCursor++;
        QStringList pool;
        bool allowWhite = rng->bounded(10) == 0;   // white is punctuation
        foreach (const QString &c, m_palette)
        {
            if (c == m_colour)
                continue;
            if (allowWhite == false && c == QStringLiteral("white"))
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
        m_colour = pool.at(int(rng->bounded(pool.count())));
    }
    else if (m_colour.isEmpty() && m_palette.isEmpty() == false)
        m_colour = m_palette.first();

    /* ---- eligible groups: enabled, and with a colour to take ---- */
    QStringList eligible;
    foreach (const QString &key, m_groupOrder)
    {
        if (m_groupOff.contains(key))
            continue;
        if (candidates(ENGINE_ROLE_COLOR, key).isEmpty())
            continue;
        eligible.append(key);
    }

    /* ---- cast: the base group is always lit; effects are added on top as
     *      the evening's energy rises. Decided once per section, and never
     *      more than one step from the last section. ---- */
    if (sectionChanged && hold == false)
    {
        // a random stride, so the rotation of groups, looks and positions
        // does not fall into the same order night after night
        if (m_lastState.isEmpty() == false)
            m_castCursor += 1 + int(rng->bounded(2));
        m_motionCursor += 1 + int(rng->bounded(3));
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
    auto effectsFor = [&energy, rng](bool drop, bool brk) {
        // a break used to empty the room down to the base. Late in the night
        // it keeps one group as well - quieter than a groove, not dark.
        // A break is a quiet section, not an empty one. It always keeps one
        // group besides the base, and from half a fader upwards it keeps two -
        // so the ENERGY slider is felt in a break as well, which it was not.
        if (brk)
            return 1 + (energy > 0.50 && rng->bounded(3) > 0 ? 1 : 0);
        // The top of the ENERGY fader has to mean something: at full it is
        // three groups on a drop and two in a groove, not two and one.
        // four groups on a drop at the stop, three in a groove: the fader's
        // last quarter has to add rig, not just brightness
        qreal want = drop ? 3.0 * qBound(0.0, (energy - 0.15) / 0.75, 1.0)
                          : 2.0 * qBound(0.0, (energy - 0.25) / 0.65, 1.0);
        want += 1.0 * qBound(0.0, (energy - 0.80) / 0.20, 1.0);
        int whole = int(want);
        qreal frac = want - whole;
        return whole + (rng->bounded(1000) < int(frac * 1000.0) ? 1 : 0);
    };
    if ((sectionChanged || m_lastState.isEmpty()) && hold == false)
    {
        m_effectsBefore = m_effects;
        int want = effectsFor(isDrop, isBreak);
        m_effects = qBound(m_effects - 1, want, m_effects + 1);
    }
    // Every re-pick below sits behind `hold == false`, so this is exactly the
    // moment the look on stage may change. A verdict belongs in the section
    // the look was CHOSEN for, not the one the track happens to have reached:
    // freeze a drop look, let it ride into the break, thumb it up, and without
    // this the drop look collects credit under "break" - and then gets
    // favoured in breaks, where nothing ever chose it. HOLD is the button you
    // press when you like what you see, so this is not a corner case.
    if (hold == false)
        m_lookState = state;
    m_lastState = state;

    int effects = m_effects;
    // a build is the room filling up: it never has fewer groups than the
    // section before it, and past the middle it reaches for one more
    if (isBuild)
    {
        effects = qMax(effects, m_effectsBefore);
        if (prog > 0.5 && energy > 0.30)
            effects = qMax(effects, m_effectsBefore + 1);
    }
    if (preDrop)     // no dice here: four beats of joining and leaving would flicker
        effects = qMax(effects, int(qRound(3.0 * qBound(0.0, (energy - 0.15) / 0.75, 1.0))));
    if (isCalm || still)
        effects = 0;
    if (base.isEmpty())
        effects = qMax(effects, 1);                 // no base: something must show

    QStringList pool;
    foreach (const QString &key, eligible)
    {
        if (key != base)
            pool.append(key);
    }

    QSet<QString> castSet;
    // the base is in the cast whatever happens - the light the room stands on
    if (base.isEmpty() == false)
        castSet.insert(base);
    if (silent == false)
    {
        int n = pool.count();
        for (int i = 0; i < qMin(effects, n); i++)
            castSet.insert(pool.at((m_castCursor + i) % n));
        // strobes carry a drop; swap one in when the energy allows effects
        if ((isDrop || preDrop) && effects > 0 && isCalm == false && hold == false)
        {
            foreach (const QString &key, pool)
            {
                if (m_groups.value(key).strobes && castSet.contains(key) == false && castSet.count() <= 3)
                    castSet.insert(key);
            }
        }
        while (castSet.count() > 3)
        {
            QStringList sorted = castSet.values(); sorted.sort();
            for (int i = sorted.count() - 1; i >= 0; i--)
                if (sorted.at(i) != base) { castSet.remove(sorted.at(i)); break; }
        }
    }

    /* ---- accent: a partner colour on one effect group in drops ---- */
    QString accentColour;
    if (m_accent && isDrop && isCalm == false && castSet.count() >= 2 && m_override.isEmpty())
    {
        if (sectionChanged || m_accentPick.isEmpty() || m_palette.contains(m_accentPick) == false)
            m_accentPick = accentFor(m_colour);
        accentColour = m_accentPick;
    }

    bool hard = sectionChanged && isDrop;
    QStringList castSorted = castSet.values();
    castSorted.sort();
    QString accentGroup;
    foreach (const QString &key, castSorted)
    {
        if (key != base)
            accentGroup = key;                   // the last effect group takes the accent
    }

    /* ---- the drop's character: one draw that leans every group's dice
     *      the same way, so a drop is one idea and the next drop another.
     *      hard = strobing, sparkling, fast; wide = full, slow trades, big
     *      figures; tight = chases and lines ---- */
    if (isDrop && (sectionChanged || m_dropStyle == 0) && hold == false)
    {
        QList<int> styles = { 2, 3, 2, 3, 0 };
        if (energy > 0.45)
            styles << 1 << 1;
        if (energy > 0.7)
            styles << 1;
        m_dropStyle = styles.at(int(rng->bounded(styles.count())));
    }
    else if (isDrop == false)
        m_dropStyle = 0;

    /* ---- moves: every group in the cast draws how it moves this section.
     *      Long sections redraw every 16 bars, half the time. A pattern the
     *      group ran in its last two sections is not drawn again if the
     *      dice can help it. ---- */
    bool redraw = hold == false
               && (sectionChanged || m_moves.isEmpty()
                   || (bar > 0 && bar % 8 == 0 && beatInBar == 0 && rng->bounded(3) > 0));
    foreach (const QString &key, castSorted)
    {
        if (redraw == false && m_moves.contains(key))
            continue;
        QList<int> history = m_moveHistory.value(key);
        TrackMove fresh = drawMove(key, tier, isBuild, energy, key == base, prog);
        for (int attempt = 0; attempt < 4 && fresh.pattern != ENGINE_PAT_STATIC && history.contains(fresh.pattern); attempt++)
            fresh = drawMove(key, tier, isBuild, energy, key == base, prog);
        m_moves.insert(key, fresh);
        if (fresh.pattern != ENGINE_PAT_STATIC)
        {
            history.append(fresh.pattern);
            while (history.count() > 2)
                history.removeFirst();
            m_moveHistory.insert(key, history);
        }
    }

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
        if (g.lasers && hold == false)
        {
            bool mayRoam = energy >= 0.60 && isBreak == false && isBuild == false
                        && isCalm == false && still == false;
            quint32 home = mayRoam ? Function::invalidId() : homePosition(key);
            if (home != Function::invalidId())
            {
                if (m_position.value(key, Function::invalidId()) != home)
                {
                    // A beam that swings while it is lit is the one thing
                    // these fixtures must never do: the group goes dark for
                    // the beat it moves on, exactly as the generic path does.
                    if (inCast)
                        darkGroups.insert(key);
                    m_position.insert(key, home);
                }
                run("pos:" + key, home, 1.0, 0, true);
                continue;
            }
        }

        quint32 want = m_position.value(key, Function::invalidId());
        if (want == Function::invalidId() || (mayMove && sectionChanged))
        {
            quint32 np = positionFunction(key, m_castCursor, tier);
            if (np != want && (mayMove || want == Function::invalidId()))
            {
                if (inCast && g.lasers)
                    darkGroups.insert(key);
                want = np;
                m_position.insert(key, want);
                m_headMoveBeats.insert(key, -8);     // our own move: grace before the Light Rider check
            }
        }
        // heads without a sweep of the user's (or in FULL AUTO): walk through
        // the positions every four bars in the groove, every two in a drop -
        // the pan/tilt speed channel turns each step into a slow sweep
        int walkBars = qMax(1, (isDrop ? 2 : 4) * (m_speed < 0 ? 2 : 1) / (m_speed > 0 ? 2 : 1));
        if (g.heads && inCast && hold == false && isBreak == false
            && (m_fullAuto || (m_moves.value(key).ownChaser
                               && candidates(ENGINE_ROLE_MOTION, key).isEmpty()))
            && beatInBar == 0 && bar > 0 && (bar % walkBars) == 0)
        {
            // one step per walk, through the tier's own pool
            quint32 np = positionFunction(key, m_castCursor + bar / walkBars, tier);
            if (np != Function::invalidId() && np != want)
            {
                want = np;
                m_position.insert(key, want);
                m_headMoveBeats.insert(key, -8);
            }
        }
        if (want != Function::invalidId())
            run("pos:" + key, want, 1.0, 0, true);
    }

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

    /* ---- levels: the build climbs like a snare roll, not a straight line ---- */
    // The build used to start at 0.40 and only reach 0.55 at the halfway
    // mark - below the groove it came out of, so it read as the light going
    // DOWN. It starts at the groove's level now and climbs from there.
    // Every tier is also opened up by the energy: at a full fader a break is
    // as bright as a groove used to be, and a groove is nearly a drop.
    qreal eNow = qBound(0.0, energy, 1.0);
    qreal tierLevel = isBreak ? (0.45 + 0.35 * eNow)
                    : isBuild ? (0.65 + 0.35 * prog * prog + 0.15 * eNow)
                    : isDrop  ? 1.0
                              : (0.60 + 0.35 * eNow);
    if (preDrop)
        tierLevel = qMax(tierLevel, 0.60);
    if (isIntro || isOutro)
        tierLevel *= 0.80;               // the ends of a track are not the middle
    if (isCalm)
        tierLevel = qMin(tierLevel, 0.55);
    // The energy is already inside tierLevel above, per section type; here it
    // only keeps a very quiet room from running at full. The LEVEL slider is
    // a straight brightness trim.
    // 45 % at the bottom of the fader, 100 % at the top - a real spread, and
    // linear the whole way so every ten per cent is visible. The tier decides
    // the shape (a break dips, a drop is full); this decides how loud the
    // whole picture is.
    qreal level = qBound(0.0, tierLevel, 1.0) * (0.45 + 0.55 * eNow)
                * qBound(0.0, levelScale, 1.0);

    // how hot a chase may be right now: the stars a motion needs. Drawn once
    // per section from ramps of the energy, not read off a step
    if (redraw || m_starCeil <= 0)
    {
        qreal p2 = isDrop ? qBound(0.0, (energy - 0.15) / 0.35, 1.0) : qBound(0.0, (energy - 0.30) / 0.35, 1.0);
        qreal p3 = isDrop ? qBound(0.0, (energy - 0.45) / 0.35, 1.0) : qBound(0.0, (energy - 0.60) / 0.35, 1.0);
        m_starCeil = 1;
        if (rng->bounded(1000) < int(p2 * 1000.0))
        {
            m_starCeil = 2;
            if (rng->bounded(1000) < int(p3 * 1000.0))
                m_starCeil = 3;
        }
    }
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
        bool userMoves = mf != Function::invalidId() && m_funcs.value(mf).type != int(Function::SceneType);
        bool aimed = m_active.contains("pos:" + key);
        // HOLD freezes the figure rather than stopping it; STILL, CALM and a
        // blackout do stop it
        bool wanted = castSet.contains(key) && aimed && userMoves == false && darkGroups.contains(key) == false
                   && isCalm == false && still == false && m_blackout == false
                   && (g.lasers == false || m_fullAuto);
        if (wanted == false)
        {
            if (m_active.contains(slot))
                stopSlot(slot, true);
            m_sweep.remove(key);
            continue;
        }
        // the build tightens its figure ONCE, on the first downbeat past the
        // middle: "beats > 4" as the test redrew it every bar from there on
        // (a halved build figure is 5-14 beats), so the heads jumped to a new
        // figure every bar through the back half of every build
        qreal prevProg = qreal(beat - 4 - secStart) / qreal(len);
        bool fresh = redraw || m_sweep.contains(key) == false
                  || (isBuild && prog > 0.5 && prevProg <= 0.5 && beatInBar == 0 && m_sweep.value(key).shape >= 0);
        if (fresh)
        {
            QList<int> history = m_sweepHistory.value(key);
            // how many heads the figure spans: the EFX holds one entry per
            // pan/tilt head, and a four-eye bar is one fixture with four of
            // them. Counting fixtures gave a bar no spread at all and gave
            // two four-eyes a fan of 180 degrees over eight heads.
            EFX *sweepEfx = qobject_cast<EFX *>(m_doc->function(m_sweepFunc.value(key)));
            int sweepHeads = sweepEfx != nullptr ? sweepEfx->fixtures().count() : g.fixtures.count();
            TrackSweep sw = drawSweep(tier, isBuild, prog, energy, sweepHeads, g.lasers);
            for (int attempt = 0; attempt < 4 && sw.shape >= 0 && history.contains(sw.shape); attempt++)
                sw = drawSweep(tier, isBuild, prog, energy, sweepHeads, g.lasers);
            m_sweep.insert(key, sw);
            if (sw.shape >= 0)
            {
                history.append(sw.shape);
                while (history.count() > 2)
                    history.removeFirst();
                m_sweepHistory.insert(key, history);
            }
        }
        applySweep(key, m_sweep.value(key), bpm);
    }

    /* ---- zoom: a move of its own on the heads. Wide in a break, mid in
     *      the groove, by the drop's character in a drop, tightening through
     *      a build - and wide for the first bar when a drop lands ---- */
    foreach (const QString &key, m_zoomScenes.keys())
    {
        QString slot = "zoom:" + key;
        quint32 posId = m_active.value("pos:" + key, Function::invalidId());
        // only over our own positions: a position of the user's may set its own zoom
        bool ours = posId != Function::invalidId() && m_funcs.value(posId).generated;
        if (castSet.contains(key) == false || ours == false || m_blackout)
        {
            if (m_active.contains(slot))
                stopSlot(slot, true);
            m_zoom.remove(key);
            continue;
        }
        int want = m_zoom.value(key, -1);
        bool pickZoom = want < 0 || (hold == false && (redraw
                     || (isDrop && bar == 1 && beatInBar == 0)
                     || (isBuild && beatInBar == 0 && ((prog > 0.5 && want != 0) || (prog <= 0.5 && want == 0)))));
        if (isDrop && bar == 0 && hold == false)
            want = 2;                                        // the landing: everything wide
        else if (pickZoom)
        {
            if (isBreak)       want = rng->bounded(4) == 0 ? 1 : 2;
            else if (isBuild)  want = prog > 0.5 ? 0 : 1;
            else if (isDrop)   want = m_dropStyle == 2 ? 2 : (m_dropStyle == 3 ? 0 : (m_dropStyle == 1 ? int(rng->bounded(2)) : int(rng->bounded(3))));
            else               want = rng->bounded(3) == 0 ? 2 : 1;
        }
        m_zoom.insert(key, want);
        run(slot, m_zoomScenes.value(key).at(qBound(0, want, 2)), 1.0, 0, true);
    }

    /* ---- the phrase: bars 7 and 8 of every eight turn around - the
     *      effects halve their step, and on a hot night the last beat goes
     *      dark every other phrase, so the one after lands. Bar 1 of a
     *      drop is the landing: every effect lit and still for a bar. ---- */
    int phraseBar = bar % 8;
    bool turnaround = hold == false && isCalm == false && still == false && bar >= 6
                   && (phraseBar == 6 || phraseBar == 7)
                   && (isDrop || (tier == 1 && energy > 0.5));
    bool landing = isDrop && bar == 0 && isCalm == false;
    if (turnaround && phraseBar == 7 && beatInBar == 3 && energy > 0.6 && ((bar / 8) % 2) == 0)
    {
        foreach (const QString &key, castSet)
        {
            if (key != base)
                darkGroups.insert(key);
        }
    }

    // this beat's clock: the pulse timer measures its breath against it
    if (bpm > 0.0)
        m_beatMs = 60000.0 / bpm;
    m_beatStartMs = m_clock.elapsed();
    m_beatIndex = beat - secStart;
    bool anyPulse = false;
    bool moveHit = false;

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
            stopSlot("col:" + key, g.lasers);
            stopSlot("mot:" + key, false);
            for (int i = 0; i < g.parts.count(); i++)
                stopSlot(partSlot(key, i), g.lasers);    // effects fade out over a bar
            m_pulseDepth.remove(key);
            m_breathe.remove(key);
            continue;
        }

        TrackMove mv = m_moves.value(key);
        if (isCalm || still)
            mv = TrackMove();
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
                mv.stepBeats = prog < 0.30 ? 2 : 1;
                qreal sub = 1.0 + (reach - 1.0) * qBound(0.0, (prog - 0.30) / 0.70, 1.0);
                int subs = sub >= 3.0 ? 4 : (sub >= 1.6 ? 2 : 1);
                mv.subSteps = g.lasers ? 1 : qMin(g.strobes ? 2 : 4, subs);
                if (g.strobes == false)
                    mv.pulseOn = (prog < 0.20 && energy < 0.5) ? 1 : 0;
            }
            else
            {
                // the roll: steps halve as the build climbs, the pulse deepens
                mv.stepBeats = qMax(1, mv.stepBeats >> qBound(0, int(prog * 3.0), 2));
                mv.pulse *= 0.5 + 0.5 * prog;
            }
            // the strobes are handed over to the beat as the build runs out
            if (g.strobes)
                mv.pulseOn = prog > 0.60 ? 0 : 1;
        }
        // the turnaround: bars 7-8 of an eight-bar phrase move twice as
        // fast, the way a drummer fills into the next phrase - down to
        // eighths and sixteenths when it is hot. The landing bar stands still
        else if (key != base && landing)
            mv.pattern = ENGINE_PAT_STATIC;
        else if (key != base && turnaround && mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL)
        {
            if (mv.stepBeats > 1)
                mv.stepBeats = qMax(1, mv.stepBeats / 2);
            else if (mv.subSteps < 4 && energy > 0.55 && g.lasers == false)
                mv.subSteps *= 2;      // the bars never step between the beats
        }
        // a hats-only passage (highs up, no kick) sparkles rather than sits
        if (high > 0.65 && kick >= 0.0 && kick < 0.35 && mv.pattern == ENGINE_PAT_STATIC
            && key != base && isCalm == false && still == false && g.parts.count() >= 3)
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
        else if (m_speed > 0)
        {
            if (mv.stepBeats > 1) mv.stepBeats = qMax(1, mv.stepBeats / 2);
            else if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL
                     && g.lasers == false) mv.subSteps = qMin(4, mv.subSteps * 2);
        }

        QString colour = m_colour;
        quint32 splitScene = Function::invalidId();
        if (accentColour.isEmpty() == false && key == accentGroup)
        {
            // the accent either holds, or trades places with the palette
            // colour every few bars - never a third colour
            colour = accentColour;
            if (mv.colourBars > 0 && ((bar / mv.colourBars) % 2) == 1)
                colour = m_colour;
            // on a per-eye lamp the two colours share the bar, and swap eyes
            // every second bar
            if (g.perEye && m_fullAuto)
            {
                bool swap = ((bar / 2) % 2) == 1;
                splitScene = splitColourFunction(key, swap ? accentColour : m_colour, swap ? m_colour : accentColour);
            }
        }

        // the base is the light the room stands on: brighter than the
        // effects in a break, where it is often the only thing lit
        qreal groupLevel = qBound(0.0, level * ((isBreak && key == base) ? 1.4 : 1.0), 1.0);
        qreal gl = darkGroups.contains(key) ? 0.0 : groupLevel * m_groupTrim.value(key, 1.0) * m_master;
        // run() puts MASTER and the trim on for us now, so the colour scene
        // gets the bare level - or the two would multiply
        qreal glBase = darkGroups.contains(key) ? 0.0 : groupLevel;
        quint32 cf = splitScene != Function::invalidId() ? splitScene : colourFunction(key, colour);
        // colour scenes swap hard: a soft fade left the old colour adding up
        // with the new one on RGB fixtures for a bar - a blend nobody asked for
        if (cf != Function::invalidId())
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? glBase : 1.0, 0, true);
        else
            stopSlot("col:" + key, false);

        // motion: real movement (chases, EFX) in drops, the climbing half of
        // a build, and on the base from the groove onward. Static pattern
        // scenes are looks and may show in any section. Never while calm.
        // the move drew whether this group runs one of the user's own chases
        // or EFX (never in a break, only the climbing half of a build); the
        // base may reach one star higher, it is what carries the room
        bool moving = mv.ownChaser && isBreak == false && still == false && (isBuild == false || prog > 0.5);
        int stars = qMin(3, maxStars + (key == base ? 1 : 0));
        quint32 mf = Function::invalidId();
        if (isCalm == false)
        {
            mf = motionFor(key, colour, castSet, m_motionCursor, tier, bpm, division, moving == false, stars);
            if (mf == Function::invalidId() && moving)
                mf = motionFor(key, colour, castSet, m_motionCursor, tier, bpm, division, true, stars);
        }
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
                run("mot:" + key, mf, mi.dimmer ? gl : 1.0, divisionFor(mi, bpm, division), hard);
        }
        else
            stopSlot("mot:" + key, false);

        if (g.hasDimmer)
        {
            // the pulse: on its beats the dimmers jump to the level and fall
            // back until the next one
            bool pulseBeat = mv.pulseOn == 0
                          || (mv.pulseOn == 1 && (beatInBar == 0 || beatInBar == 2))
                          || (mv.pulseOn == 2 && (beatInBar == 1 || beatInBar == 3))
                          || (mv.pulseOn == 3 && beatInBar == 0);
            qreal depth = darkGroups.contains(key) ? 0.0 : mv.pulse;
            // the kick the analysis heard on this beat: no kick, no pulse;
            // a soft kick, a soft pulse. The kick scales the HIT, never the
            // depth: depth is how far the light falls between two beats, so
            // scaling it down for a soft kick RAISED the floor - a bare strobe
            // chase sat half-lit through every kickless passage
            qreal strength = 1.0;
            if (kick >= 0.0)
            {
                if (kick < 0.20)
                    pulseBeat = false;
                strength = 0.5 + 0.5 * qBound(0.0, kick, 1.0);
            }
            // A fixture whose dimmer is a switch is driven by a square gate,
            // and a square gate stays SHUT until something re-opens it. On a
            // beat that carries no pulse - an off-beat, or one the analysis
            // heard no kick on - it would sit dark until the next beat that
            // does, which on a quiet passage is the whole section. For those,
            // "no pulse this beat" has to mean "no gate this beat".
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

            // a real chase or EFX of theirs is the movement; the generated
            // pattern only runs when the look is static. A colour scene that
            // sets the dimmers itself hides the parts (HTP), so no pattern
            bool patterned = (mf == Function::invalidId() || m_funcs.value(mf).type == int(Function::SceneType))
                          && (cf == Function::invalidId() || m_funcs.value(cf).dimmer == false)
                          && (mf == Function::invalidId() || m_funcs.value(mf).dimmer == false);
            m_patterned.insert(key, patterned);
            m_liveMove.insert(key, mv);                  // the shaped move, for the sub-beat steps
            if ((m_flash && m_flashHeld.contains(key)) == false)
                applyMove(key, darkGroups.contains(key) ? 0.0 : groupLevel, beat, secStart, prog, mv, patterned);
            if (mv.flashBar && beatInBar == 0 && (bar % 2) == 1)
                moveHit = true;
        }
    }

    if (anyPulse && m_pulseTimer.isActive() == false)
    {
        m_pulseTimer.start();
    }
    else if (anyPulse == false && m_pulseTimer.isActive())
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
    driveStrobe(castSet, beat, energy, isDrop, isBuild, prog, bar, beatInBar,
                isCalm || still || m_mixing || m_flash || m_blackout
                || isIntro || isOutro);       // nobody strobes an intro

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
    // bottom of the fader to twelve at the top, and the shortest gap between
    // two of them slides from eight beats down to one.
    int hitCeil = 2 + int(qRound(10.0 * eNow * eNow));
    int hitGap = qMax(1, int(qRound(8.0 - 7.0 * eNow)));
    bool crowded = m_hitBeats.count() >= hitCeil
                || (m_hitBeats.isEmpty() == false && beat - m_hitBeats.last() < hitGap);
    bool hit = isCalm == false && still == false && m_mixing == false
            && ((isBuild && prog > 0.82 && crowded == false)
                || (isDrop && bar == 0 && beatInBar < 2)
                || (moveHit && crowded == false));
    if (m_flash == false)
    {
        if (hit)
        {
            m_hitBeats.append(beat);
            // white on the downbeat of a drop - that is the one moment it
            // reads as a punch rather than as a lamp somebody forgot to
            // colour. Everywhere else the accent is in the room's colour.
            QString hue = (isDrop && bar == 0 && beatInBar == 0) ? QStringLiteral("white") : m_colour;
            quint32 ff = flashFunction(castSet, hue);
            if (ff != Function::invalidId())
                run("flash", ff, 1.0, 0, true);
            else
                genFlash(true, hue);
        }
        else
        {
            stopSlot("flash", true);
            if (m_flashHeld.isEmpty() == false)
                genFlash(false);
        }
    }

    checkConflicts(castSet);

    QStringList moveNames;
    foreach (const QString &key, castSorted)
    {
        QString mn = isCalm ? QString() : moveName(m_moves.value(key));
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

    logBeat(state, beat, level, energy, sectionEnergy);
    m_report = QString("%1  |  %2%3  |  %4%5%6%7")
        .arg(moveNames.isEmpty() ? (silent ? tr("(silence)") : tr("(no groups)"))
                                 : moveNames.join(" + "))
        .arg(m_colour.isEmpty() ? tr("(no colour)") : m_colour)
        .arg(accentColour.isEmpty() ? QString() : QString(" + %1").arg(accentColour))
        .arg(state + (isDrop && m_dropStyle > 0 ? QString(" %1 %2").arg(QChar(0xb7)).arg(m_dropStyle == 1 ? tr("hard") : (m_dropStyle == 2 ? tr("wide") : tr("tight"))) : QString())
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
        mv.pulse = 0.35 + 0.55 * e;
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
    // below three fifths of the fader - so what is left here is how hard they
    // work: at the bottom they simply stand lit, and from there the fader
    // adds a chase down the row, then a blink on the beat, then both at once.
    // Nothing about this changes their aim.
    if (g.lasers && g.parts.count() >= 2)
    {
        qreal busy = qBound(0.0, (e - 0.12) / 0.78, 1.0);
        mv.breatheBars = 0;
        mv.texture = 0.0;
        mv.ownChaser = tier > 0 && chance(0.25 + 0.45 * busy);
        if (chance(busy))
        {
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG,
                                ENGINE_PAT_ODDEVEN, ENGINE_PAT_CHASE });
            // eight beats a step at the bottom, one at the top - and the
            // steps in between are really in between
            mv.stepBeats = qMax(1, int(qRound(8.0 - 7.0 * busy)));
            mv.bare = chance(0.35 + 0.45 * busy);
        }
        else
        {
            mv.pattern = ENGINE_PAT_STATIC;
            mv.stepBeats = 8;
        }
        mv.subSteps = 1;                       // never between the beats: too twitchy
        mv.pulse = busy < 0.15 ? 0.0 : 0.15 + 0.75 * busy;
        mv.pulseOn = chance(busy * busy) ? 0 : (chance(busy) ? pick({ 1, 2 }) : 3);
        mv.colourBars = chance(0.5 * busy) ? pick({ 2, 4, 4 }) : 0;
        mv.flashBar = tier == 2 && chance(0.5 * busy);
        return mv;
    }

    // A strobe is not a light source, it is a rhythm instrument. It belongs
    // ON the beat and dark between the beats - standing lit with a wobble on
    // top is what made them read as ugly floodlights. So: a deep pulse (full
    // on the beat, gone well before the next one), a still picture underneath
    // so nothing fights the blink, and the energy decides how often it lands.
    if (g.strobes)
    {
        // Not always a picture behind them. Four times in ten the group runs
        // one lamp at a time with nothing lit in between - a blink walking
        // down the row rather than a lit bank that flickers.
        mv.bare = g.parts.count() >= 2 && chance(0.40);
        if (mv.bare)
        {
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG, ENGINE_PAT_CHASE });
            mv.stepBeats = 1;
        }
        else
        {
            mv.pattern = e > 0.60 && g.parts.count() >= 2
                       ? pick({ ENGINE_PAT_STATIC, ENGINE_PAT_ODDEVEN, ENGINE_PAT_STATIC })
                       : ENGINE_PAT_STATIC;
            mv.stepBeats = 4;
        }
        mv.subSteps = 1;
        mv.breatheBars = 0;
        mv.texture = 0.0;
        mv.colourBars = 0;
        mv.ownChaser = false;
        // near-total at the bottom of the fader, total at the top: either way
        // there is nothing between the blinks
        mv.pulse = 0.85 + 0.15 * e;
        // the downbeat while the room is quiet, the backbeat in between,
        // every beat once it is going. A build hands them over to the beat as
        // it runs out; a break gets the downbeat and nothing else.
        if (tier == 0)
            mv.pulseOn = 3;
        else if (build)
            mv.pulseOn = prog > 0.60 ? 0 : 1;
        else
        {
            // A ramp, drawn: at the bottom of the fader it is the downbeat
            // nearly every time, at the top it is every beat nearly every
            // time, and the middle really is the middle - which a pair of
            // thresholds could never be.
            qreal often = qBound(0.0, (e - 0.20) / 0.65, 1.0);
            mv.pulseOn = chance(often * often) ? 0
                       : (chance(often) ? pick({ 1, 2 }) : 3);
        }
        mv.flashBar = tier == 2 && e > 0.75 && chance(0.4);
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
        if (chance(0.15 + 0.75 * e))
            mv.pulse = 0.05 + 0.55 * e;      // a heartbeat, from a hint to a real one
        mv.texture = 0.15;
        mv.ownChaser = false;            // a break moves on the engine's figure

        // Nearly half the time a break is ONE lamp at a time on the beat and
        // nothing else at all - no picture behind it, no floor to fall back
        // to. The quietest thing the rig can do that is still on the music,
        // and the heads slowly hand the beat to each other down the row.
        if (g.parts.count() >= 2 && chance(0.15 + 0.60 * e))
        {
            mv.bare = true;
            mv.pattern = pick({ ENGINE_PAT_CHASE, ENGINE_PAT_CHASE, ENGINE_PAT_PINGPONG });
            // the lamp hands the beat on faster the higher the fader is -
            // four beats each at the bottom, every beat at the top
            mv.stepBeats = qMax(1, int(qRound(4.0 - 3.0 * e)));
            mv.subSteps = 1;
            mv.pulse = 1.0;                          // full on the beat, dark between
            mv.pulseOn = chance(e * e) ? 0 : (chance(e) ? 1 : 3);
            mv.breatheBars = 0;
            mv.texture = 0.0;
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
        mv.pulse = (0.10 + 0.30 * prog) * (0.4 + 0.6 * e);
        mv.pulseOn = pick({ 0, 0, 2 });
        // A chase of the operator's switches the generated figure OFF - and a
        // build was drawing one half the time, so half of all builds stood
        // completely still. The engine's own figure is what a build needs.
        mv.ownChaser = rng->bounded(5) == 0;
        if (isBase)
        {
            // the base carries the build: a fill that grows across the heads,
            // stepping faster as the section runs out. It used to be pinned
            // to STATIC, which left the build as a slow brightness ramp on a
            // still picture - and the base is usually the only group lit.
            mv.pattern = ENGINE_PAT_FILL;
            mv.stepBeats = prog > 0.6 ? 1 : 2;
            mv.subSteps = 1;
            mv.pulse = qMin(mv.pulse, 0.30);
        }

        // The build's own shape: the same bare blink as the break, handed
        // round faster and faster the closer the drop gets. Nothing lit in
        // between, so the acceleration is the only thing in the room and you
        // cannot miss where it is going.
        // gated, or every build in the set is the same one: past the middle
        // it is nearly always this, early on it is often the fill above
        if (g.parts.count() >= 2 && chance(0.35 + 0.55 * prog))
        {
            mv.bare = true;
            mv.ownChaser = false;        // a chase of theirs would swallow the pattern
            mv.pattern = ENGINE_PAT_CHASE;
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
    if (isBase)
        mv.ownChaser = rng->bounded(10) < 7;
    else if (tier == 2)
        mv.ownChaser = rng->bounded(10) < 6;
    else
        mv.ownChaser = rng->bounded(1000) < int(300.0 * qBound(0.0, (e - 0.25) / 0.5, 1.0));

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
        mv.pulse = 0.45 * ramp(e, 0.08, 1.00) * (0.6 + 0.4 * rng->bounded(1000) / 1000.0);
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
        qreal wild = ramp(e, 0.05, 1.00);        // the whole fader, not a slice
        QList<int> menu = { ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES, ENGINE_PAT_STATIC };
        if (chance(ramp(e, 0.05, 0.55)))
            menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG;
        if (chance(ramp(e, 0.35, 1.00)))
            menu << ENGINE_PAT_SPARKLE << ENGINE_PAT_CHASE;
        if (m_dropStyle == 1)       menu << ENGINE_PAT_SPARKLE << ENGINE_PAT_SPARKLE << ENGINE_PAT_ODDEVEN;
        else if (m_dropStyle == 2)  menu << ENGINE_PAT_STATIC << ENGINE_PAT_HALVES << ENGINE_PAT_STATIC;
        else if (m_dropStyle == 3)  menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG << ENGINE_PAT_CHASE;
        mv.pattern = pick(menu);
        mv.stepBeats = chance(wild) ? pick({ 1, 1, 2 }) : pick({ 2, 4 });
        if (m_dropStyle == 2)
            mv.stepBeats = qMax(mv.stepBeats, 2);
        if (m_dropStyle == 3)
            mv.stepBeats = 1;
        // eighths and sixteenths: the fast patterns run between the beats
        // when it is hot - what makes a drop roll instead of tick
        bool fast = mv.pattern == ENGINE_PAT_CHASE || mv.pattern == ENGINE_PAT_PINGPONG
                 || mv.pattern == ENGINE_PAT_ODDEVEN || mv.pattern == ENGINE_PAT_SPARKLE;
        if (fast && mv.stepBeats == 1 && chance((m_dropStyle == 2 ? 0.2 : 0.6) * ramp(e, 0.45, 0.95)))
            mv.subSteps = m_dropStyle == 1 ? pick({ 2, 4, 4 }) : pick({ 2, 2, 4 });
        mv.pulse = 0.25 + 0.40 * wild * (0.7 + 0.3 * rng->bounded(1000) / 1000.0);
        if (m_dropStyle == 2)
            mv.pulse *= 0.6;
        mv.pulseOn = chance(0.7) ? 0 : pick({ 1, 2 });
        if (chance((m_dropStyle == 2 ? 0.8 : 0.5) * ramp(e, 0.30, 0.90)))
            mv.colourBars = pick({ 1, 2, 4 });
        mv.flashBar = chance((m_dropStyle == 1 ? 0.7 : 0.35) * ramp(e, 0.20, 1.00));
    }

    // texture: the groove and the break spread the lit fixtures a little
    mv.texture = tier == 1 ? 0.25 : (tier == 0 ? 0.15 : 0.0);

    if (isBase)
    {
        // the heads: slow trades only, a gentle breath, the palette colour
        if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_ODDEVEN && mv.pattern != ENGINE_PAT_HALVES)
            mv.pattern = ENGINE_PAT_HALVES;
        mv.stepBeats = qMax(mv.stepBeats, tier == 2 ? 2 : 4);
        mv.subSteps = 1;
        mv.pulse = qMin(mv.pulse, 0.30);
        mv.colourBars = 0;
        mv.flashBar = false;
    }
    // A drop may drop the backdrop too: a random handful of lamps hits each
    // beat and there is nothing lit in between. On the heads that reads as
    // the room being punched rather than washed, and it is the one place
    // where SPARKLE - a fresh random set every step - belongs.
    // never on the base: that group is the light the room stands on, and the
    // block just above spends fifteen lines saying so
    if (tier == 2 && isBase == false && g.parts.count() >= 2 && chance(0.10 + 0.55 * e))
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
        if (chance(0.55 + 0.40 * fest))
        {
            // a generated pattern only reaches the rig when the group is not
            // already running one of the user's chases - and the base runs
            // one seven times in ten, so without this the top of the fader
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
            int lit = step % n;
            int tail = (lit + n - 1) % n;
            for (int i = 0; i < n; i++)
                mask[i] = i == lit ? 1.0 : (i == tail && move.bare == false ? 0.3 : dim);
        }
        break;
        case ENGINE_PAT_PINGPONG:
        {
            int period = qMax(1, 2 * n - 2);
            int idx = step % period;
            if (idx >= n)
                idx = period - idx;
            for (int i = 0; i < n; i++)
                mask[i] = i == idx ? 1.0 : dim;
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
    return mask;
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
    if (slot.startsWith(QStringLiteral("col:")) == false
        && slot.startsWith(QStringLiteral("idle:")) == false)
        return 1.0;
    QString group = slotGroup(slot);
    const TrackGroup &g = m_groups.value(group);
    bool carries = m_funcs.value(fid).dimmer || g.hasDimmer == false || g.parts.isEmpty();
    if (carries == false)
        return 1.0;
    return m_master * (group.isEmpty() ? 1.0 : m_groupTrim.value(group, 1.0));
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
            out *= pulseFactor(group) * m_groupTrim.value(group, 1.0) * m_master;
            // and the same on/off squaring setPart() does: an animation
            // laser's dimmer is a switch, and a fraction written to it is
            // rounded by the fixture in a way nobody can predict
            if (m_groups.value(group).patternDevice)
                out = out > 0.10 ? 1.0 : 0.0;
        }
        else
        {
            out *= slotScale(slot, fid);
        }
        out = m_blackout ? 0.0 : qBound(0.0, out, 1.0);
        m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
        m_activeOut.insert(slot, out);
    }
}

qreal TrackEngine::pulseFactor(const QString &group) const
{
    qreal factor = 1.0;
    qint64 now = m_clock.elapsed();

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
        qreal pos = (qreal(m_beatIndex) + within) / (qreal(bars) * 4.0);
        factor *= 0.70 + 0.30 * (0.5 + 0.5 * std::sin(pos * 6.283185307179586));
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
    if (move.bare)
        s += " bare";
    if (move.flashBar)
        s += " hits";
    return s.isEmpty() ? QString() : QString("(%1)").arg(s);
}

/*********************************************************************
 * Warnings, calm, log
 *********************************************************************/


TrackSweep TrackEngine::drawSweep(int tier, bool build, qreal prog, qreal energy, int heads, bool laser) const
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
    qreal moveP = tier == 0 ? 0.80 + 0.20 * e : (tier == 2 ? 0.90 + 0.10 * e : 0.85 + 0.15 * e);
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

    // the shape: a gentle few at rest, the whole menu when it is hot
    QList<int> shapes;
    shapes << int(EFX::Circle) << int(EFX::Line) << int(EFX::Eight);
    if (tier > 0 && (e > 0.30 || tier == 2))
        shapes << int(EFX::Leaf) << int(EFX::Diamond) << int(EFX::Line2) << int(EFX::Circle);
    if (tier > 0 && (e > 0.55 || tier == 2))
        shapes << int(EFX::Lissajous) << int(EFX::Square) << int(EFX::Lissajous) << int(EFX::Eight);
    sw.shape = pick(shapes);
    if (sw.shape == int(EFX::Lissajous))
    {
        sw.fx = pick(QList<int>() << 1 << 2 << 3);
        sw.fy = pick(QList<int>() << 2 << 3 << 4);
        if (sw.fx == sw.fy)
            sw.fy++;
    }

    // size: the energy sets the ceiling, the dice the figure. Tilt has less
    // room than pan, and a break barely stirs
    // a break's figure is wide enough to see but takes half a minute to walk
    // it; that is the whole point of a break
    // A break's figure is BIG - the room is quiet, so the one thing moving
    // has all the attention and it may as well travel. It is the pace that
    // makes a break a break, not the size.
    qreal reach = tier == 0 ? (30.0 + 22.0 * e)
                : (tier == 2 ? 24.0 + 18.0 * e : 16.0 + 14.0 * e);
    if (build)
        reach = 26.0 + 30.0 * prog;
    qreal size = reach * (0.5 + 0.5 * rng->generateDouble());
    // pan has the whole room, tilt has the floor: the heads hang from the
    // ceiling and a figure must not climb the walls
    sw.width = qBound(6, int(size), 127);
    // 4 units of tilt is eight degrees - a circle that reads as a flat line
    sw.height = qBound(10, int(size * (0.45 + 0.55 * rng->generateDouble())), 28);
    if (sw.shape == int(EFX::Line) || sw.shape == int(EFX::Line2))
        sw.rotation = pick(QList<int>() << 0 << 0 << 90 << 30 << 150 << 60 << 120);
    else
        sw.rotation = chance(0.4) ? int(rng->bounded(360)) : 0;

    // Tempo: beats per figure. The ENERGY fader is the pace as well as the
    // size now - a straight slide between the slowest this room allows and
    // the quickest, in every section type, so 100 % feels different from
    // 50 % everywhere and not only in a drop. Nothing here is fast: even the
    // top of a drop is six beats for a whole circle.
    auto beatsFor = [rng, e](int slow, int quick) {
        qreal f = qreal(slow) + (qreal(quick) - qreal(slow)) * e;
        f *= 0.85 + 0.30 * rng->generateDouble();      // the dice, but not much
        return qMax(3, int(qRound(f)));
    };
    if (tier == 0)
        sw.beats = beatsFor(48, 20);      // a break: 22 s down to 9 s a figure
    else if (tier == 2)
        sw.beats = beatsFor(24, 6);       // a drop: 11 s down to under 3
    else
        sw.beats = beatsFor(36, 10);      // a groove: in between
    if (build)
        sw.beats = beatsFor(28, 8) / (prog > 0.5 ? 2 : 1);

    // how the heads relate: in unison, as a wave, one after another,
    // mirrored, or fanned out around the figure
    if (heads >= 2)
    {
        int rel = tier == 2 ? pick(QList<int>() << 0 << 1 << 1 << 2 << 3 << 4)
                            : pick(QList<int>() << 0 << 0 << 1 << 3 << 4);
        if (rel == 1)      sw.spread = 1;
        else if (rel == 2) sw.spread = 2;
        else if (rel == 3) sw.mirror = true;
        else if (rel == 4) sw.fan = 360 / heads;
    }

    // the drop's character: hard = big and fast, wide = big and slow,
    // tight = small quick lines and eights
    if (tier == 2 && m_dropStyle == 1)
    {
        sw.width = qBound(6, int(sw.width * 1.3), 127);
        sw.beats = qMax(8, sw.beats);
    }
    else if (tier == 2 && m_dropStyle == 2)
    {
        sw.width = qBound(6, int(sw.width * 1.3), 127);
        sw.height = qBound(4, int(sw.height * 1.3), 28);
        sw.beats = qMax(sw.beats, 8);
    }
    else if (tier == 2 && m_dropStyle == 3)
    {
        if (sw.shape != int(EFX::Line) && sw.shape != int(EFX::Eight) && sw.shape != int(EFX::Line2))
            sw.shape = chance(0.5) ? int(EFX::Line) : int(EFX::Eight);
        sw.width = qBound(6, int(sw.width * 0.6), 60);
        sw.height = qBound(10, int(sw.height * 0.6), 28);
        sw.beats = qMax(8, sw.beats);
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
        qreal lw = qBound(0.0, (e - 0.60) / 0.40, 1.0);
        if (lw <= 0.0 || tier == 0 || build)
        {
            sw.shape = -1;
            sw.dx = 0;
            sw.dy = 0;
            return sw;
        }
        bool wide = tier == 2 && chance(0.15 + 0.35 * lw);
        // The ordinary figure is 12-42 units (5-18 degrees); a "wide" one on a
        // drop reaches 34-85, which is a third of the travel. That is
        // deliberate - the bars are meant to take the whole wall now and then
        // when it lands - and 85 is the agreed ceiling for the mirrors. The
        // fader decides where inside each band this sits.
        sw.height = wide ? int(34 + 40 * lw) + int(rng->bounded(12))
                         : int(12 + 30 * lw) + int(rng->bounded(10));
        // FIXED, whatever the energy says. A figure takes 13 seconds at 128
        // bpm and a wide one 19, and that is the whole range there is.
        sw.beats = wide ? 40 : 28;
        sw.dx = 0;
        sw.dy = int(rng->bounded(9)) - 4;          // barely off the aim
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
            int spread = int(qRound(60.0 + 120.0 * lw));
            sw.fan = qBound(60, spread, 180);
        }
    }
    return sw;
}

void TrackEngine::applySweep(const QString &group, const TrackSweep &sw, qreal bpm)
{
    quint32 fid = m_sweepFunc.value(group, Function::invalidId());
    EFX *efx = m_doc ? qobject_cast<EFX *>(m_doc->function(fid)) : nullptr;
    if (efx == nullptr)
        return;
    QString slot = "efx:" + group;
    if (sw.shape < 0 && sw.dx == 0 && sw.dy == 0)
    {
        if (m_active.contains(slot))
            stopSlot(slot, true);
        return;
    }

    // the EFX counts milliseconds, the music beats: one figure = beats x the
    // DJ's beat, halved or doubled by the SPEED tiles
    qreal beatMs = bpm > 0.0 ? 60000.0 / bpm : 468.75;
    int beats = sw.beats;
    if (m_speed < 0)
        beats *= 2;
    else if (m_speed > 0)
        beats = qMax(1, beats / 2);
    uint ms = uint(qMax(250.0, beats * beatMs));

    bool running = m_active.contains(slot) && m_active.value(slot) == fid
                && efx->isRunning() && efx->stopped() == false;
    if (running && m_sweepShown.value(group) == sw)
    {
        // the pitch fader drifts the clock: keep the figure on the beat
        if (qAbs(int(efx->duration()) - int(ms)) > int(ms / 50))
            efx->setDuration(ms);
        return;
    }
    // reconfigured live: a stop and a start in the same tick would leave the
    // EFX stopped (stop() only asks; the timer thread does it later)

    // no figure but a jitter: a figure of size zero is a still point off the aim
    efx->setAlgorithm(sw.shape < 0 ? EFX::Circle : EFX::Algorithm(sw.shape));
    efx->setWidth(sw.shape < 0 ? 0 : sw.width);
    efx->setHeight(sw.shape < 0 ? 0 : sw.height);
    efx->setRotation(sw.rotation);
    efx->setXOffset(qBound(0, 127 + sw.dx, 255));
    efx->setYOffset(qBound(0, 127 + sw.dy, 255));
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
        ef->setStartOffset((sw.fan * i) % 360);
        i++;
    }
    m_sweepShown.insert(group, sw);
    run(slot, fid, 1.0, 0, true);
}

QString TrackEngine::sweepName(const TrackSweep &sw) const
{
    if (sw.shape < 0)
        return QString();
    QString shape = EFX::algorithmToString(EFX::Algorithm(sw.shape)).toLower();
    QString rel = sw.spread == 1 ? "~" : (sw.spread == 2 ? ">" : (sw.mirror ? "><" : (sw.fan ? "*" : "")));
    return QString("(%1%2 %3 %4b)").arg(shape).arg(rel).arg(sw.width).arg(sw.beats);
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
    int sharedLooks = 0;
    foreach (const TrackFuncInfo &info, m_funcs)
        if (info.role >= 0 && info.role != ENGINE_ROLE_IDLE && info.groups.count() > 1)
            sharedLooks++;
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
                     || m_active.contains("efx:" + key);
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
    emit liveChanged();
}

void TrackEngine::next()
{
    // The strongest free signal there is: whatever was on stage, he did not
    // want it. How long it had been up is in the log already - the beat lines
    // before this one say when funcs last changed.
    logSignal(QStringLiteral("sig:next"));
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
    const QString &sect = m_lookState.isEmpty() ? m_lastState : m_lookState;
    QString mark = QString(tag).replace(',', ' ');
    if (sect.isEmpty() == false)
        mark += QLatin1Char('@') + QString(sect).replace(',', ' ');
    logBeat(mark, m_logBeatNo, m_logLevel, m_logEnergy, m_logSection);
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

quint32 TrackEngine::pickWeighted(const QList<TrackFuncInfo *> &ok, int cursor) const
{
    if (ok.isEmpty())
        return Function::invalidId();
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

void TrackEngine::markVerdictPoint()
{
    m_verdictActive = m_active;
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
        quint32 fid = stage.value("col:" + key, Function::invalidId());
        if (fid == Function::invalidId())
            fid = stage.value("mot:" + key, Function::invalidId());
        if (fid == Function::invalidId())
            fid = stage.value("efx:" + key, Function::invalidId());
        if (fid == Function::invalidId() || m_funcs.contains(fid) == false)
            continue;
        const TrackFuncInfo &info = m_funcs.value(fid);
        if (info.generated)
            continue;                  // ours, and rebuilt with the table
        QVariantMap row;
        row.insert("group", key);
        row.insert("name", info.name);
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
    bool touched = false;
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
    bool touched = false;
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
}

int TrackEngine::room() const { return m_room; }

void TrackEngine::setRoom(int room)
{
    // a hand on the dial ends the automatic evening
    m_roomAuto = false;
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

bool TrackEngine::roomAuto() const { return m_roomAuto; }

void TrackEngine::setRoomAuto(bool on)
{
    if (on == m_roomAuto)
        return;
    m_roomAuto = on;
    if (on)
    {
        m_roomSent = -1;             // hand the clock's value over right away
        announceRoom();
    }
    emit liveChanged();
}

int TrackEngine::roomPercent() const { return m_roomSent; }

int TrackEngine::clockPercent() const
{
    // anchor points through the night, minutes past 21:00 -> percent. A
    // restaurant: still until 22:00, then a slow creep - the DJ pushes the
    // slider when the floor actually opens, somewhere between 23:00 and 01:00
    static const int anchor[][2] = { { 0, 0 }, { 60, 0 }, { 120, 20 }, { 180, 45 }, { 240, 70 }, { 300, 85 }, { 420, 85 }, { 480, 0 } };
    QTime now = QTime::currentTime();
    int minutes = now.hour() * 60 + now.minute() - 21 * 60;
    if (minutes < 0)
        minutes += 24 * 60;          // past midnight
    if (minutes >= 480)
        return 0;                    // 05:00 - 21:00: a restaurant, still
    for (int i = 1; i < 8; i++)
    {
        if (minutes <= anchor[i][0])
        {
            int span = anchor[i][0] - anchor[i - 1][0];
            qreal f = span > 0 ? qreal(minutes - anchor[i - 1][0]) / qreal(span) : 1.0;
            return int(qRound(anchor[i - 1][1] + f * (anchor[i][1] - anchor[i - 1][1])));
        }
    }
    return 0;
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

int TrackEngine::roomByClock() const
{
    // a club night, roughly: doors and a thin floor, warming up, full,
    // peak after midnight; the morning after is empty again
    QTime now = QTime::currentTime();
    int minutes = now.hour() * 60 + now.minute();
    if (minutes >= 5 * 60 && minutes < 21 * 60 + 30)
        return 0;                                    // 05:00 - 21:30 empty
    if (minutes >= 21 * 60 + 30 && minutes < 23 * 60)
        return 1;                                    // 21:30 - 23:00 warming
    if (minutes >= 23 * 60 || minutes < 30)
        return 2;                                    // 23:00 - 00:30 full
    return 3;                                        // 00:30 - 05:00 peak
}

bool TrackEngine::startScene() const { return m_startScene; }

void TrackEngine::setStartScene(bool on)
{
    if (on == m_startScene)
        return;
    m_startScene = on;
    if (on)
    {
        // it has to stand in some colour: red unless the DJ has already
        // picked one - and the tile lights up, so it is clear which it is
        if (m_override.isEmpty())
        {
            QString want = m_palette.contains(QStringLiteral("red"))
                           ? QStringLiteral("red")
                           : (m_palette.isEmpty() ? QString() : m_palette.first());
            if (want.isEmpty() == false)
            {
                m_override = want;
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
            m_override.clear();
        m_startColour = false;
        foreach (const QString &slot, m_active.keys())
            stopSlot(slot, false);
        m_cast.clear();
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
    // The evening's opening picture: the IDLE functions hold the aim (the
    // START scene from the rider carries pan/tilt/zoom only), and the engine
    // lights every group that is on, in one colour, standing still. The
    // colour tiles, MASTER and the cast faders all work on it.
    if (m_doc == nullptr)
        return;
    ensureTable();
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

    // the aim, from the start scene(s)
    foreach (TrackFuncInfo *info, idles)
        // the bare level: run() puts MASTER on through slotScale(), and an
        run("idle:" + QString::number(info->id), info->id,
            info->dimmer ? m_startLevel : 1.0, 0, false);

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
        if (cf != Function::invalidId())
        {
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? m_startLevel : 1.0, 0, false);
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
    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    m_report = tr("(start scene)  |  %1  |  master %2 %%")
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
    logSignal(on ? QStringLiteral("sig:hold") : QStringLiteral("sig:hold-off"));
    m_hold = on;
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
        m_log.setFileName(dir + QDir::separator() + "tracklog-" + today + ".csv");
        bool fresh = m_log.exists() == false || m_log.size() == 0;
        if (m_log.open(QIODevice::Append | QIODevice::Text) == false)
        {
            m_logEnabled = false;
            return;
        }
        if (fresh)
        {
            QTextStream head(&m_log);
            // funcs is APPENDED, never inserted: bane B's tracklog_report.py
            // reads the older columns by position and must keep working.
            head << "time,beat,state,cast,colour,level,energy,section_energy,master,moves,funcs,track\n";
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
    QTextStream out(&m_log);
    out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ','
        << beat << ',' << state << ','
        << castSorted.join('+') << ',' << m_colour << ','
        << QString::number(level, 'f', 2) << ','
        << QString::number(energy, 'f', 2) << ','
        << QString::number(sectionEnergy, 'f', 2) << ','
        << QString::number(m_master, 'f', 2) << ','
        << QString(m_lastMoves).replace(',', ';') << ','
        << running.join(';') << ','
        // last, and appended like funcs was: bane B reads the older columns
        // by position. Commas and quotes out - the log is read with a plain
        // split(','), not a CSV parser, and a track called "Hello, Again"
        // would have shifted every column after it.
        << QString(m_trackTitle).replace(',', ' ').remove('"') << '\n';
    out.flush();
    m_log.flush();                       // the report script reads while we play
}

void TrackEngine::release()
{
    // AUTO went off: let everything fade out over a bar instead of clipping,
    // and let the dimmers fall back to the sliders. Positions stay where they
    // are - stopping a laser position is a move, and a slider may still have
    // the beam lit.
    stopSweeps();
    m_strobeUntil = -1;
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
    m_lastState.clear();
    m_lookState.clear();
    m_flash = false;
    // and the flag with them, or the next tick() would put the masks straight
    // back and the Track page would still show BLACKOUT lit
    m_blackout = false;
    m_pulseDepth.clear();
    m_breathe.clear();
    m_flashHeld.clear();
    m_pulseTimer.stop();
    m_report = tr("(released)");
    if (m_fadeAttr.isEmpty() == false)
        m_fadeTimer.start();
    emit liveChanged();
}

void TrackEngine::idle()
{
    if (m_doc == nullptr)
        return;
    ensureTable();
    tickFades();

    QList<TrackFuncInfo *> list = candidates(ENGINE_ROLE_IDLE, QString());
    QString base = baseGroup();
    // no start scene: the base stands in its colour, still and dimmed - a
    // pause between tracks is not a blackout in a restaurant
    bool holdBase = list.isEmpty() && base.isEmpty() == false && m_groupOff.contains(base) == false;

    // everything from the track goes; the start scene(s) come on
    stopSweeps();
    m_strobeUntil = -1;
    applyGroupOff();                 // an off group stays off in the start look
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            continue;
        if (slot.startsWith("off:") || slot.startsWith("black:"))
            continue;                    // the masks we started four lines ago
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
        stopSlot(slot, false);
    }

    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();

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

    if (holdBase)
    {
        QString colour = m_colour.isEmpty() ? (m_palette.isEmpty() ? QString() : m_palette.first()) : m_colour;
        quint32 cf = colourFunction(base, colour);
        if (cf != Function::invalidId())
            run("col:" + base, cf, m_funcs.value(cf).dimmer ? 0.35 : 1.0, 0, false);
        if (m_groups.value(base).hasDimmer)
            setDimmer(base, 0.35);
        m_cast.clear();
        m_cast.insert(base);
    }
    else
        m_cast.clear();

    // nothing plays, so no beats tick the fades: keep them moving on a timer
    if (m_fadeAttr.isEmpty() == false && m_fadeTimer.isActive() == false)
        m_fadeTimer.start();

    m_report = list.isEmpty() ? (holdBase ? tr("(idle - base held)") : tr("(idle - no start scene)"))
                              : tr("(start scene)");
    emit liveChanged();
}

void TrackEngine::trackLoaded(const QString &title)
{
    // Stage time is counted every beat but only written when something else
    // triggers a save, and a whole night can pass without one. Once per track
    // bounds the loss to the track that was playing when the power went, and
    // costs a few kilobytes every four minutes.
    saveRoles();
    m_trackTitle = title;
    // positions are kept: a new track is not a reason to swing the lasers
    m_lastState.clear();
    m_lookState.clear();
    m_colourBar = -1;            // hold the colour until the first break or drop
    m_colourSince = -1;
    m_castCursor++;
    m_moves.clear();             // the new track draws its own moves
    m_sweep.clear();
    m_dropStyle = 0;
    // CALM counts beats of this track: carry only what is left of it
    m_calmUntil = m_calmUntil > m_lastBeat ? m_calmUntil - m_lastBeat : 0;
    m_lastBeat = 0;
    m_hitBeats.clear();
    m_starCeil = 0;
    // the cast size is hysteretic, so a peak-time track that ended on four
    // groups handed four to the next track's intro and took three sections
    // to come down again
    m_effects = 0;
    m_effectsBefore = 0;
    m_zoom.clear();
    m_strobeSeen = -1;
    // the burst end is a beat number of THIS track: carrying it over would
    // hold the hardware strobe on for the whole of the next one
    m_strobeUntil = -1;
}

/*********************************************************************
 * Running functions
 *********************************************************************/

void TrackEngine::run(const QString &slot, quint32 fid, qreal level, int division, bool hard)
{
    if (fid == Function::invalidId())
    {
        stopSlot(slot, hard);
        return;
    }

    level = qBound(0.0, level, 1.0);
    // a blackout: the function runs, at nothing
    qreal out = m_blackout ? 0.0 : qBound(0.0, level * slotScale(slot, fid), 1.0);

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
            startFunction(func, division);
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
    else if (func->isRunning() == false || func->stopped())
        startFunction(func, division);

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, level);
    m_activeOut.insert(slot, out);
    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
}

void TrackEngine::startFunction(Function *func, int division)
{
    if (func == nullptr)
        return;
    // The division goes in as an overrideDuration with tempo type Beats, so
    // Ableton Link stays the only clock: we change how long a step lasts,
    // never the timing source.
    if (division > 0)
        func->start(m_doc->masterTimer(), FunctionParent::master(), 0,
                    Function::defaultSpeed(), Function::defaultSpeed(),
                    uint(division), Function::Beats);
    else
        func->start(m_doc->masterTimer(), FunctionParent::master());
}

void TrackEngine::stopSlot(const QString &slot, bool hard)
{
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

    // 20 ms x 0.02 = one second from full to silent, in fifty steps. It used
    // to be four steps of a quarter, 250 ms apart, which is what "the fades
    // chop" was: you could count them.
    const qreal step = 0.02;
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
            func->adjustAttribute(m_blackout ? 0.0 : level, attr);
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
    qreal applied = qBound(0.0, level * pulseFactor(group) * m_groupTrim.value(group, 1.0) * m_master, 1.0);
    // an animation laser's "dimmer" is a switch: on above a sliver, else off
    if (g.patternDevice)
        applied = applied > 0.10 ? 1.0 : 0.0;
    if (m_blackout)
        applied = 0.0;

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_activeAttr.value(slot, -1);
        if (func != nullptr && (func->isRunning() == false || func->stopped()))
        {
            func->start(m_doc->masterTimer(), FunctionParent::master());
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
    else if (func->isRunning() == false || func->stopped())
        func->start(m_doc->masterTimer(), FunctionParent::master());

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, level);
    m_activeOut.insert(slot, applied);
    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, applied));
}

void TrackEngine::stopAll()
{
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
    m_position.clear();
    m_lastState.clear();
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
    m_calmUntil = 0;              // CALM must not survive AUTO going off and on
    // NOT m_lastBeat: slotDocChanged() calls this mid-track now, and zeroing
    // the beat counter there would make a CALM pressed in that same beat
    // compare 5000 < 32 and silently do nothing. m_calmUntil = 0 above is
    // already the whole of what this line was for.
    m_dropStyle = 0;
    m_strobeUntil = -1;          // or the next tick walks straight back into a burst
    m_strobeRate = 0;
    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    m_flash = false;
    m_flashHeld.clear();         // release() clears it; without this the next
                                 // beat skips the whole out-of-cast teardown
    m_report = tr("(stopped)");
    emit liveChanged();
}
