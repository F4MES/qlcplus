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
#define ENGINE_EFX_PREFIX     QStringLiteral("TRACK EFX: ")
#define ENGINE_ZOOM_PREFIX    QStringLiteral("TRACK Zoom: ")

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
    , m_starCeil(0)
    , m_lastBeat(0)
    , m_calmUntil(0)
    , m_logEnabled(true)
    , m_dropStyle(0)
    , m_beatMs(500.0)
    , m_beatStartMs(0)
    , m_beatIndex(0)
    , m_room(2)
    , m_roomAuto(true)
    , m_roomSent(-1)
    , m_fullAuto(false)
    , m_hold(false)
    , m_forceNext(false)
{
    QSettings settings;
    m_logEnabled = settings.value(SETTINGS_ENGINE_LOG, true).toBool();
    m_fadeTimer.setInterval(250);
    connect(&m_fadeTimer, SIGNAL(timeout()), this, SLOT(slotFadeTimer()));
    m_clock.start();
    m_pulseTimer.setInterval(40);
    connect(&m_pulseTimer, SIGNAL(timeout()), this, SLOT(slotPulseTimer()));
    // MASTER is deliberately not restored: a night that starts at 40 %
    // because someone dimmed last time is worse than one that starts bright
    m_master = 1.0;
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt();
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_fullAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
    foreach (QString key, settings.value(SETTINGS_ENGINE_GROUPOFF, QString())
                                  .toString().split(';', Qt::SkipEmptyParts))
        m_groupOff.insert(key);

    if (m_doc != nullptr)
    {
        connect(m_doc, SIGNAL(loaded()), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(cleared()), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(functionRemoved(quint32)), this, SLOT(slotDocChanged()));
        connect(m_doc, SIGNAL(fixtureRemoved(quint32)), this, SLOT(slotDocChanged()));
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
    m_dirty = true;
    m_position.clear();
    m_moves.clear();
    m_sweep.clear();
    emit tableChanged();
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
            int step = m_beatIndex * mv.subSteps + int(within * mv.subSteps) + mv.phase;
            QVector<qreal> mask = patternMask(key, mv, step, 1.0);
            qreal level = m_moveLevel.value(key, 0.0);
            for (int i = 0; i < mask.count(); i++)
                setPart(key, i, level * mask.at(i));
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
        { "yellow",  { "yellow", "gul" } },
        { "cyan",    { "cyan" } },
        { "green",   { "green", "grøn", "groen" } },
        { "blue",    { "blue", "blå", "blaa" } },
        { "red",     { "red", "rød", "roed" } },
        { "white",   { "white", "hvid" } },
    };

    for (int i = 0; i < table.count(); i++)
        foreach (const QString &w, table.at(i).second)
            if (t.contains(w))
                return table.at(i).first;

    // the two that are too short to trust inside other words
    if (hasWord(t, QStringList() << "uv")) return "uv";
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
                if (ef != nullptr)
                    out.insert(ef->head().fxi);
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
        if (m_groups.contains(g) == false || m_groups.value(g).lasers == false)
            laserOnly = false;

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
    if (m_dirty == false || m_doc == nullptr)
        return;
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
            // pan and tilt on a non-laser: a moving head
            bool pan = false, tilt = false;
            for (quint32 ch = 0; ch < fxi->channels(); ch++)
            {
                const QLCChannel *qch = fxi->channel(ch);
                if (qch == nullptr) continue;
                if (qch->group() == QLCChannel::Pan)  pan = true;
                if (qch->group() == QLCChannel::Tilt) tilt = true;
            }
            g.heads = pan && tilt && g.lasers == false;
            m_groups.insert(key, g);
            m_groupOrder.append(key);
        }
        m_groups[key].fixtures.append(fxi->id());
    }

    /* ---- functions ---- */
    QSet<quint32> steps;
    foreach (Function *func, m_doc->functions())
    {
        Chaser *chaser = qobject_cast<Chaser *>(func);
        if (chaser != nullptr)
        {
            foreach (ChaserStep step, chaser->steps())
                steps.insert(step.fid);
        }
    }

    static const QStringList junkWords = { "blackout", "reset", "new scene", "new chaser",
                                           "new sequence", "new rgb", "copy", "filler",
                                           "speedtest", "fractest", "for advanced", "off" };

    QHash<quint32, TrackFuncInfo> old = m_funcs;
    m_funcs.clear();

    foreach (Function *func, m_doc->functions())
    {
        if (func == nullptr || func->isVisible() == false)
            continue;
        if (func->name().startsWith(ENGINE_DIMMER_PREFIX)
            || func->name() == ENGINE_HAZE_SCENE || func->name() == ENGINE_FAN_SCENE)
            continue;

        Function::Type t = func->type();
        if (t != Function::SceneType && t != Function::ChaserType &&
            t != Function::EFXType && t != Function::RGBMatrixType &&
            t != Function::CollectionType && t != Function::SequenceType)
            continue;

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
        if (info.role != ENGINE_ROLE_COLOR || info.colour.isEmpty())
            continue;
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
                foreach (const QString &col, cit.value().keys())
                    coverage[col].insert(g.key);
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
    ensurePositionScenes();
    ensureSweeps();
    ensureZoomScenes();
    ensureDimmerScenes();
    ensureAtmosScenes();

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
            bool r = false, gr = false, b = false;
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
            }
            if (r && gr && b)
                g.rgb = true;
            else if (effects >= 3)
                g.patternDevice = true;
        }

        // the user's colour scenes of exactly this group
        QMap<quint32, QMap<quint32, QList<uchar> > > seen;             // fixture -> channel -> values
        QMap<quint32, QMap<quint32, QMap<QString, uchar> > > byColour;  // fixture -> channel -> colour -> value
        int scenes = 0;
        for (QHash<quint32, TrackFuncInfo>::const_iterator fit = m_funcs.constBegin(); fit != m_funcs.constEnd(); ++fit)
        {
            const TrackFuncInfo &info = fit.value();
            if (info.role != ENGINE_ROLE_COLOR || info.generated || info.colour.isEmpty()
                || info.type != int(Function::SceneType)
                || info.groups.count() != 1 || info.groups.contains(g.key) == false)
                continue;
            Scene *scene = qobject_cast<Scene *>(m_doc->function(info.id));
            if (scene == nullptr)
                continue;
            scenes++;
            foreach (SceneValue sv, scene->values())
            {
                seen[sv.fxi][sv.channel].append(sv.value);
                byColour[sv.fxi][sv.channel].insert(info.colour, sv.value);
            }
        }
        if (scenes < 2)
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

                bool constant = vals.count() == scenes;
                for (int i = 1; i < vals.count() && constant; i++)
                    if (vals.at(i) != vals.first())
                        constant = false;

                if (constant)
                    g.baseValue[fid].insert(ch, vals.first());
                else if (qch->group() != QLCChannel::Pan && qch->group() != QLCChannel::Tilt)
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
        if (func != nullptr && func->name() == name)
            scene = qobject_cast<Scene *>(func);
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
        if (func != nullptr && func->name().startsWith(ENGINE_POS_PREFIX))
            existing.insert(func->name().mid(ENGINE_POS_PREFIX.length()), func->id());

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
                int panVal = qBound(0, int(qRound(128.0 + defs[d].slope * off + defs[d].panOff
                                                  + (defs[d].split ? (off < 0 ? -defs[d].split : defs[d].split) : 0))), 255);
                int tiltVal = qBound(80, int(qRound(defs[d].tilt + ((i % 2) ? defs[d].zig : -defs[d].zig) / 2.0
                                                    + defs[d].tiltSlope * off)), 176);
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
        if (func != nullptr && func->name().startsWith(ENGINE_ZOOM_PREFIX))
            existing.insert(func->name().mid(ENGINE_ZOOM_PREFIX.length()), func->id());

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
        if (func != nullptr && func->type() == Function::EFXType && func->name().startsWith(ENGINE_EFX_PREFIX))
            existing.insert(func->name().mid(ENGINE_EFX_PREFIX.length()), func->id());

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

bool TrackEngine::userAllowed(const TrackFuncInfo &info) const
{
    // In FULL AUTO the user's functions step aside wherever the engine can
    // make its own: only pattern devices keep theirs, and laser positions
    // stay in the user's hands - a generated tilt is not a safe tilt.
    if (m_fullAuto == false || info.generated || info.role == ENGINE_ROLE_IDLE)
        return true;
    if (info.groups.isEmpty())
        return true;
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

void TrackEngine::genFlash(bool on)
{
    // the flash without a scene: the strobe groups go white with every part
    // at full, whatever the cast is doing; off again, the parts of groups
    // outside the cast are cut hard, the rest fall back on the next beat
    if (on)
    {
        foreach (const QString &key, m_groupOrder)
        {
            const TrackGroup &g = m_groups.value(key);
            if (g.strobes == false || g.generatable() == false || m_groupOff.contains(key))
                continue;
            quint32 white = colourFunction(key, "white");
            if (white != Function::invalidId())
                run("flash:" + key, white, 1.0, 0, true);
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
        if (func != nullptr && func->name().startsWith(ENGINE_COLOUR_PREFIX))
            existing.insert(func->name().mid(ENGINE_COLOUR_PREFIX.length()), func->id());

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
                quint32 rc = QLCChannel::invalid(), gc = QLCChannel::invalid(),
                        bc = QLCChannel::invalid(), wc = QLCChannel::invalid();
                for (quint32 i = 0; i < fxi->channels(); i++)
                {
                    const QLCChannel *qch = fxi->channel(i);
                    if (qch == nullptr || qch->group() != QLCChannel::Intensity)
                        continue;
                    if (qch->colour() == QLCChannel::Red && rc == QLCChannel::invalid()) rc = i;
                    if (qch->colour() == QLCChannel::Green && gc == QLCChannel::invalid()) gc = i;
                    if (qch->colour() == QLCChannel::Blue && bc == QLCChannel::invalid()) bc = i;
                    if (qch->colour() == QLCChannel::White && wc == QLCChannel::invalid()) wc = i;
                }

                bool coloured = false;
                if (sw != nullptr && rc != QLCChannel::invalid() && gc != QLCChannel::invalid() && bc != QLCChannel::invalid())
                {
                    values.append(SceneValue(fid, rc, uchar(sw->r)));
                    values.append(SceneValue(fid, gc, uchar(sw->g)));
                    values.append(SceneValue(fid, bc, uchar(sw->b)));
                    if (wc != QLCChannel::invalid())
                        values.append(SceneValue(fid, wc, uchar(sw->w)));
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
        if (func != nullptr && func->name().startsWith(ENGINE_DIMMER_PREFIX))
            existing.insert(func->name().mid(ENGINE_DIMMER_PREFIX.length()), func->id());

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
            {
                g.parts.append(Function::invalidId());
                continue;
            }
            SceneValue sv(fxi->id(), ch, 255);
            QString name = QString("%1 #%2").arg(g.key).arg(i + 1);

            Scene *scene = nullptr;
            if (existing.contains(name))
                scene = qobject_cast<Scene *>(m_doc->function(existing.value(name)));
            if (scene != nullptr)
            {
                // the fixture behind this part may have changed: hold only it
                foreach (SceneValue old, scene->values())
                    if (old.fxi != sv.fxi || old.channel != sv.channel)
                        scene->unsetValue(old.fxi, old.channel);
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
                g.hasDimmer = true;
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
        scene->setValue(channels.at(i).first, channels.at(i).second, value);

    if (value > 0 && scene->isRunning() == false)
        scene->start(m_doc->masterTimer(), FunctionParent::master());
    else if (value == 0 && scene->isRunning())
        scene->stop(FunctionParent::master());
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
    QString stored = QSettings().value(SETTINGS_ENGINE_ROLES, QString()).toString();
    foreach (QString entry, stored.split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 2)
            continue;
        quint32 fid = parts.at(0).toUInt();
        if (m_funcs.contains(fid))
            m_funcs[fid].role = parts.at(1).toInt();
    }

    QString stars = QSettings().value(SETTINGS_ENGINE_STARS, QString()).toString();
    foreach (QString entry, stars.split(';', Qt::SkipEmptyParts))
    {
        QStringList parts = entry.split(':');
        if (parts.count() != 2)
            continue;
        quint32 fid = parts.at(0).toUInt();
        if (m_funcs.contains(fid))
            m_funcs[fid].stars = qBound(1, parts.at(1).toInt(), 3);
    }
}

void TrackEngine::saveRoles()
{
    QStringList entries;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        if (it.value().role != it.value().guess || it.value().step)
            entries << QString("%1:%2").arg(it.key()).arg(it.value().role);
    QSettings().setValue(SETTINGS_ENGINE_ROLES, entries.join(';'));
    QSettings().setValue(SETTINGS_ENGINE_GROUPOFF, QStringList(m_groupOff.values()).join(';'));

    QStringList stars;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        if (it.value().stars != it.value().starsGuess && it.value().generated == false)
            stars << QString("%1:%2").arg(it.key()).arg(it.value().stars);
    QSettings().setValue(SETTINGS_ENGINE_STARS, stars.join(';'));
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
        QString ga = a.groups.isEmpty() ? QString() : *a.groups.constBegin();
        QString gb = b.groups.isEmpty() ? QString() : *b.groups.constBegin();
        if (ga != gb) return ga < gb;
        return a.name.toLower() < b.name.toLower();
    });

    foreach (const TrackFuncInfo &info, rows)
    {
        bool hidden = info.junk || info.step || info.groups.isEmpty() || info.generated;
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
    ensureTable();
    if (force)
    {
        QSettings().remove(SETTINGS_ENGINE_ROLES);
        for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
            it.value().role = it.value().step ? -1 : it.value().guess;
        saveRoles();
    }
    m_dirty = true;
    ensureTable();
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
        row.insert("heads", g.heads);
        row.insert("base", key == baseGroup());
        list.append(row);
    }
    return list;
}

void TrackEngine::setGroupEnabled(QString key, bool enable)
{
    if (enable) m_groupOff.remove(key); else m_groupOff.insert(key);
    saveRoles();
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
    m_groupTrim.insert(key, level);

    // straight onto the lit parts, no waiting for the beat
    const TrackGroup &g = m_groups.value(key);
    for (int i = 0; i < g.parts.count(); i++)
    {
        QString slot = partSlot(key, i);
        if (m_active.contains(slot) == false)
            continue;
        Function *func = m_doc->function(m_active.value(slot));
        if (func != nullptr)
        {
            qreal out = m_blackout ? 0.0 : qBound(0.0, m_activeLevel.value(slot, 1.0) * pulseFactor(key) * level * m_master, 1.0);
            m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
            m_activeOut.insert(slot, out);
        }
    }
    emit liveChanged();
}

QString TrackEngine::baseGroup() const
{
    if (m_base.isEmpty() == false && m_groups.contains(m_base) && m_groupOff.contains(m_base) == false)
        return m_base;
    // automatic: the moving heads, if there are any with a colour to show
    foreach (const QString &key, m_groupOrder)
        if (m_groups.value(key).heads && m_groupOff.contains(key) == false
            && candidates(ENGINE_ROLE_COLOR, key).isEmpty() == false)
            return key;
    return QString();
}

void TrackEngine::cycleGroup(QString key)
{
    ensureTable();
    if (m_groupOff.contains(key))
    {
        m_groupOff.remove(key);                 // OFF -> ON
        if (m_base == key) m_base.clear();
    }
    else if (baseGroup() == key)
    {
        m_groupOff.insert(key);                 // BASE -> OFF
        if (m_base == key) m_base.clear();
    }
    else
    {
        m_base = key;                           // ON -> BASE
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
        if (slot.startsWith("idle:") == false)
            stopSlot(slot, false);
    m_position.clear();
    m_moves.clear();
    m_dirty = true;
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
    if (colour == m_override)
        return;
    m_override = colour;
    if (colour.isEmpty() == false)
        m_colour = colour;
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
    if (qFuzzyCompare(level, m_master))
        return;
    m_master = level;

    // re-apply to whatever is lit right now, where the breath stands
    foreach (const QString &slot, m_active.keys())
        if (slot.startsWith("dim:"))
        {
            Function *func = m_doc->function(m_active.value(slot));
            int hash = slot.lastIndexOf('#');
            QString group = hash > 4 ? slot.mid(4, hash - 4) : QString();
            if (func != nullptr)
            {
                qreal out = m_blackout ? 0.0 : qBound(0.0, m_activeLevel.value(slot, 1.0) * pulseFactor(group) * m_groupTrim.value(group, 1.0) * m_master, 1.0);
                m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, out));
                m_activeOut.insert(slot, out);
            }
        }
    emit liveChanged();
}

bool TrackEngine::blackout() const { return m_blackout; }

void TrackEngine::setBlackout(bool on)
{
    if (on == m_blackout)
        return;
    m_blackout = on;
    // straight onto everything that runs: parts and functions alike
    foreach (const QString &slot, m_active.keys())
    {
        Function *func = m_doc->function(m_active.value(slot));
        if (func == nullptr)
            continue;
        qreal level = m_activeLevel.value(slot, 1.0);
        if (slot.startsWith("dim:"))
        {
            int hash = slot.lastIndexOf('#');
            QString group = hash > 4 ? slot.mid(4, hash - 4) : QString();
            level = qBound(0.0, level * pulseFactor(group) * m_groupTrim.value(group, 1.0) * m_master, 1.0);
        }
        m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, m_blackout ? 0.0 : level));
        m_activeOut.insert(slot, m_blackout ? 0.0 : level);
    }
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
        settings.setValue(key, obj.value(key).toVariant());
        n++;
    }
    // take them on board: roles, stars and options are read in ensureTable
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt();
    m_base = settings.value(SETTINGS_ENGINE_BASE, QString()).toString();
    m_fullAuto = settings.value(SETTINGS_ENGINE_FULLAUTO, false).toBool();
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
        if (slot.startsWith("mot:"))
            stopSlot(slot, true);
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
        QSet<QString> strobeGroups;
        foreach (const QString &key, m_groupOrder)
            if (m_groups.value(key).strobes)
                strobeGroups.insert(key);
        quint32 fid = flashFunction(strobeGroups, "white");
        if (fid == Function::invalidId())
            fid = flashFunction(QSet<QString>(m_groupOrder.begin(), m_groupOrder.end()), "white");
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
        if (group.isEmpty() == false && info.groups.contains(group) == false)
            continue;
        if (m_doc->function(info.id) == nullptr)
            continue;
        if (userAllowed(info) == false)
            continue;
        out.append(const_cast<TrackFuncInfo *>(&info));
    }
    std::sort(out.begin(), out.end(), [](TrackFuncInfo *a, TrackFuncInfo *b) { return a->id < b->id; });
    return out;
}

quint32 TrackEngine::colourFunction(const QString &group, const QString &colour) const
{
    QList<TrackFuncInfo *> list = candidates(ENGINE_ROLE_COLOR, group);

    // exactly this group, exactly this colour - and of those, the one that
    // lights the most fixtures ("LaserCyan", not "1onCYAN")
    TrackFuncInfo *best = nullptr;
    foreach (TrackFuncInfo *info, list)
        if (info->groups.count() == 1 && info->colour == colour
            && (best == nullptr || info->fixtureCount > best->fixtureCount
                || (info->fixtureCount == best->fixtureCount && best->generated && info->generated == false)))
            best = info;
    if (best != nullptr)
        return best->id;
    // anything of this colour that includes the group (a collection)
    foreach (TrackFuncInfo *info, list)
        if (info->colour == colour)
            return info->id;
    // a colourless look for this group (the group has no named colours)
    foreach (TrackFuncInfo *info, list)
        if (info->groups.count() == 1 && info->colour.isEmpty())
            return info->id;
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
            if (cast.contains(g) == false)
                inside = false;
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
        if (info->colour == colour)
            exact.append(info);
    if (exact.isEmpty() == false)
        ok = exact;

    // this tier's motions first
    QList<TrackFuncInfo *> tagged;
    foreach (TrackFuncInfo *info, ok)
        if (info->tier == tier)
            tagged.append(info);
    if (tagged.isEmpty() == false)
        ok = tagged;

    // of what is allowed, the hottest: a drop at full energy takes the
    // three-star chases, not the one-star ones it could also have had
    int top = 0;
    foreach (TrackFuncInfo *info, ok)
        top = qMax(top, qMax(1, info->stars));
    QList<TrackFuncInfo *> hot;
    foreach (TrackFuncInfo *info, ok)
        if (qMax(1, info->stars) == top)
            hot.append(info);
    if (hot.isEmpty() == false)
        ok = hot;

    return ok.at(qAbs(cursor) % ok.count())->id;
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

quint32 TrackEngine::positionFunction(const QString &group, int cursor, int tier) const
{
    QList<TrackFuncInfo *> all = candidates(ENGINE_ROLE_POSITION, group);
    bool lasers = m_groups.value(group).lasers;

    QList<TrackFuncInfo *> safe;
    foreach (TrackFuncInfo *info, all)
    {
        // a laser sweep runs on its own only when its name says it stays low -
        // the rest are there for the operator to choose by hand
        if (lasers && (info->sweep || info->type == int(Function::ChaserType))
            && info->name.toLower().contains("low") == false)
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
    return pool.at(qAbs(cursor) % pool.count())->id;
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
        if (m_palette.contains(p) && engineBannedColour(p) == false && p != colour)
            have << p;
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
            if (cast.contains(g) == false)
                inside = false;
        if (inside)
            ok.append(info);
    }
    if (ok.isEmpty())
        return Function::invalidId();

    // strobes in exactly this colour, then strobes in white or colourless,
    // then this colour anywhere, then white, then anything
    auto pick = [&ok, this](std::function<bool(TrackFuncInfo *)> test) -> quint32 {
        foreach (TrackFuncInfo *info, ok)
            if (test(info))
                return info->id;
        return Function::invalidId();
    };
    auto onStrobes = [this](TrackFuncInfo *i) {
        foreach (const QString &g, i->groups) if (m_groups.value(g).strobes) return true;
        return false;
    };

    quint32 fid = pick([&](TrackFuncInfo *i) { return onStrobes(i) && i->colour == colour; });
    if (fid != Function::invalidId()) return fid;
    fid = pick([&](TrackFuncInfo *i) { return onStrobes(i) && (i->colour == "white" || i->colour.isEmpty()); });
    if (fid != Function::invalidId()) return fid;
    fid = pick([&](TrackFuncInfo *i) { return i->colour == colour; });
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
        if (slot.startsWith("idle:"))
            stopSlot(slot, false);

    bool isBreak = (state == QStringLiteral("break"));
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
        if (m_colour.isEmpty() == false)
            m_colourCursor++;
        m_colour = m_palette.at(m_colourCursor % m_palette.count());
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
    bool silent = sectionEnergy >= 0.0 && sectionEnergy < 0.12;

    // how many effect groups join the base: a ramp of the energy, with the
    // fraction decided by dice once per section - 55 % and 65 % differ
    auto effectsFor = [&energy, rng](bool drop, bool brk) {
        if (brk)
            return 0;
        qreal want = drop ? 2.0 * qBound(0.0, (energy - 0.15) / 0.65, 1.0)
                          : 1.0 * qBound(0.0, (energy - 0.30) / 0.45, 1.0);
        int whole = int(want);
        qreal frac = want - whole;
        return whole + (rng->bounded(1000) < int(frac * 1000.0) ? 1 : 0);
    };
    if ((sectionChanged || m_lastState.isEmpty()) && hold == false)
    {
        int want = effectsFor(isDrop, isBreak);
        m_effects = qBound(m_effects - 1, want, m_effects + 1);
    }
    m_lastState = state;

    int effects = m_effects;
    if (preDrop)     // no dice here: four beats of joining and leaving would flicker
        effects = qMax(effects, int(qRound(2.0 * qBound(0.0, (energy - 0.15) / 0.65, 1.0))));
    if (isCalm || still)
        effects = 0;
    if (base.isEmpty())
        effects = qMax(effects, 1);                 // no base: something must show

    QStringList pool;
    foreach (const QString &key, eligible)
        if (key != base)
            pool.append(key);

    QSet<QString> castSet;
    if (silent == false)
    {
        if (base.isEmpty() == false)
            castSet.insert(base);
        int n = pool.count();
        for (int i = 0; i < qMin(effects, n); i++)
            castSet.insert(pool.at((m_castCursor + i) % n));
        // strobes carry a drop; swap one in when the energy allows effects
        if ((isDrop || preDrop) && effects > 0 && isCalm == false && hold == false)
        {
            foreach (const QString &key, pool)
                if (m_groups.value(key).strobes && castSet.contains(key) == false && castSet.count() <= 3)
                    castSet.insert(key);
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

    /* ---- positions: sticky, tiered, only changed in the dark for lasers ---- */
    QSet<QString> darkGroups;
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (candidates(ENGINE_ROLE_POSITION, key).isEmpty())
            continue;

        bool inCast = castSet.contains(key);
        // lasers move only in a dark beat at a break; heads move at break or
        // drop starts, and half the time at other section changes - every
        // section would be restless
        bool mayMove = hold == false
                    && ((inCast == false)
                        || (sectionChanged && (g.lasers ? (isBreak && g.hasDimmer)
                                                        : (isBreak || isDrop || rng->bounded(2) == 0))));
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
        if (g.heads && inCast && hold == false && isBreak == false && m_moves.value(key).ownChaser
            && (m_fullAuto || candidates(ENGINE_ROLE_MOTION, key).isEmpty())
            && beatInBar == 0 && bar > 0 && (bar % qMax(1, (isDrop ? 2 : 4) * (m_speed < 0 ? 2 : 1) / (m_speed > 0 ? 2 : 1))) == 0)
        {
            quint32 np = positionFunction(key, m_castCursor + bar, -1);
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
            if (key != base)
                darkGroups.insert(key);
    }

    /* ---- levels: the build climbs like a snare roll, not a straight line ---- */
    qreal tierLevel = isBreak ? 0.35
                    : isBuild ? (0.40 + 0.60 * prog * prog)
                    : isDrop  ? 1.0
                              : 0.70;
    if (preDrop)
        tierLevel = qMax(tierLevel, 0.60);
    if (isCalm)
        tierLevel = qMin(tierLevel, 0.55);
    // the section's LEVEL slider is a brightness trim only; the energy dial
    // decides how many effects join, never how bright the base is
    qreal level = tierLevel * (0.5 + 0.5 * qBound(0.0, energy, 1.0)) * qBound(0.0, levelScale, 1.0);

    bool hard = sectionChanged && isDrop;
    QStringList castSorted = castSet.values();
    castSorted.sort();
    QString accentGroup;
    foreach (const QString &key, castSorted)
        if (key != base) accentGroup = key;      // the last effect group takes the accent

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
                   || (bar > 0 && bar % 16 == 0 && beatInBar == 0 && rng->bounded(2) == 0));
    foreach (const QString &key, castSorted)
    {
        if (redraw == false && m_moves.contains(key))
            continue;
        QList<int> history = m_moveHistory.value(key);
        TrackMove fresh = drawMove(key, tier, isBuild, energy, key == base);
        for (int attempt = 0; attempt < 4 && fresh.pattern != ENGINE_PAT_STATIC && history.contains(fresh.pattern); attempt++)
            fresh = drawMove(key, tier, isBuild, energy, key == base);
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
        // the build tightens its figure past the middle
        bool fresh = redraw || m_sweep.contains(key) == false
                  || (isBuild && prog > 0.5 && beatInBar == 0 && m_sweep.value(key).shape >= 0 && m_sweep.value(key).beats > 4);
        if (fresh)
        {
            QList<int> history = m_sweepHistory.value(key);
            TrackSweep sw = drawSweep(tier, isBuild, prog, energy, g.fixtures.count(), g.lasers);
            for (int attempt = 0; attempt < 4 && sw.shape >= 0 && history.contains(sw.shape); attempt++)
                sw = drawSweep(tier, isBuild, prog, energy, g.fixtures.count(), g.lasers);
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
            // the roll: steps halve as the build climbs, the pulse deepens
            mv.stepBeats = qMax(1, mv.stepBeats >> qBound(0, int(prog * 3.0), 2));
            mv.pulse *= 0.5 + 0.5 * prog;
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
            else if (mv.subSteps < 4 && energy > 0.55)
                mv.subSteps *= 2;
        }
        // a hats-only passage (highs up, no kick) sparkles rather than sits
        if (high > 0.65 && kick >= 0.0 && kick < 0.35 && mv.pattern == ENGINE_PAT_STATIC
            && key != base && isCalm == false && still == false && g.fixtures.count() >= 3)
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
            else if (mv.pattern != ENGINE_PAT_STATIC && mv.pattern != ENGINE_PAT_FILL) mv.subSteps = qMin(4, mv.subSteps * 2);
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
        quint32 cf = splitScene != Function::invalidId() ? splitScene : colourFunction(key, colour);
        // colour scenes swap hard: a soft fade left the old colour adding up
        // with the new one on RGB fixtures for a bar - a blend nobody asked for
        if (cf != Function::invalidId())
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? gl : 1.0, 0, true);
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
            // a soft kick, a soft pulse
            if (kick >= 0.0)
            {
                if (kick < 0.20)
                    pulseBeat = false;
                depth *= 0.5 + 0.5 * qBound(0.0, kick, 1.0);
            }
            if (depth > 0.0 && pulseBeat)
                m_pulseStart.insert(key, m_clock.elapsed());
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
        m_pulseTimer.start();
    else if (anyPulse == false)
        m_pulseTimer.stop();

    /* ---- hits ---- */
    // the minimal guard: more than eight hits in 32 beats is a strobe show,
    // not an accent - then only the drop's own landing may flash
    if (m_hitBeats.isEmpty() == false && m_hitBeats.last() > beat)
        m_hitBeats.clear();
    while (m_hitBeats.isEmpty() == false && m_hitBeats.first() < beat - 32)
        m_hitBeats.removeFirst();
    bool crowded = m_hitBeats.count() >= 8;
    bool hit = isCalm == false && still == false && m_mixing == false
            && ((isBuild && prog > 0.82 && crowded == false)
                || (isDrop && bar == 0 && beatInBar < 2)
                || (moveHit && crowded == false));
    if (m_flash == false)
    {
        if (hit)
        {
            m_hitBeats.append(beat);
            quint32 ff = flashFunction(castSet, m_colour);
            if (ff != Function::invalidId())
                run("flash", ff, 1.0, 0, true);
            else
                genFlash(true);
        }
        else
        {
            stopSlot("flash", true);
            if (m_flashHeld.isEmpty() == false)
                genFlash(false);
        }
    }

    checkConflicts(castSet);

    m_cast = castSet;
    QStringList moveNames;
    foreach (const QString &key, castSorted)
    {
        QString mn = isCalm ? QString() : moveName(m_moves.value(key));
        if (m_active.contains("efx:" + key))
            mn = (mn.isEmpty() ? QString() : mn + " ") + sweepName(m_sweep.value(key));
        moveNames << (mn.isEmpty() ? key : QString("%1 %2").arg(key).arg(mn));
    }
    m_lastMoves = moveNames.join(" + ");
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

TrackMove TrackEngine::drawMove(const QString &group, int tier, bool build, qreal energy, bool isBase) const
{
    // The menu grows with the energy. Low: a static look, nothing else.
    // Middle: colour trades and a soft pulse. High: everything, fast.
    // The base group (the heads) never sparkles or goes dark in halves
    // for long - it is the light the room stands on.
    TrackMove mv;
    QRandomGenerator *rng = QRandomGenerator::global();
    auto pick = [rng](const QList<int> &opts) { return opts.at(int(rng->bounded(opts.count()))); };
    const TrackGroup &g = m_groups.value(group);
    qreal e = qBound(0.0, energy, 1.0);
    mv.phase = int(rng->bounded(8));

    // a pattern device has no intensity to pulse or chase: its own pattern
    // scenes are its movement, and it runs them most of the time
    if (g.patternDevice)
    {
        mv.ownChaser = tier > 0 && rng->bounded(10) < 8;
        return mv;
    }

    if (tier == 0)
    {
        // break: still, but alive - the base breathes over two or four bars;
        // when the room is up it may slowly trade halves instead
        if (isBase && e > 0.45 && rng->bounded(3) == 0)
        {
            mv.pattern = ENGINE_PAT_HALVES;
            mv.stepBeats = 8;
        }
        else if (e > 0.2)
            mv.breatheBars = pick({ 2, 4, 4 });
        mv.texture = 0.15;
        if (g.fixtures.count() < 2)
            mv.pattern = ENGINE_PAT_STATIC;
        return mv;
    }

    if (build)
    {
        // the fill grows with the build; the pulse comes in on the offbeats
        mv.pattern = rng->bounded(3) == 0 ? ENGINE_PAT_STATIC : ENGINE_PAT_FILL;
        mv.pulse = e < 0.35 ? 0.0 : 0.15 + 0.25 * e;
        mv.pulseOn = pick({ 0, 0, 2 });
        mv.ownChaser = rng->bounded(2) == 0;
        if (isBase)
        {
            mv.pattern = ENGINE_PAT_STATIC;
            mv.pulse = qMin(mv.pulse, 0.25);
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
    auto chance = [rng](qreal p) { return rng->bounded(1000) < int(p * 1000.0); };

    if (tier == 1)
    {
        // groove: a static look at the bottom, patterns and a pulse growing in
        qreal live = ramp(e, 0.25, 0.75);            // 0 = still, 1 = full groove
        if (chance(live))
        {
            qreal quick = ramp(e, 0.55, 0.95);       // chases and short steps
            QList<int> menu = { ENGINE_PAT_STATIC, ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES };
            if (chance(quick))
                menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG;
            mv.pattern = pick(menu);
            mv.stepBeats = chance(quick) ? pick({ 2, 4 }) : pick({ 4, 8 });
        }
        else if (isBase)
            mv.breatheBars = 4;                      // still, but alive
        mv.pulse = 0.35 * ramp(e, 0.30, 0.90) * (0.6 + 0.4 * rng->bounded(1000) / 1000.0);
        if (mv.pulse < 0.08)
            mv.pulse = 0.0;
        mv.pulseOn = e < 0.6 ? pick({ 1, 3 }) : pick({ 0, 1 });
        if (chance(0.4 * ramp(e, 0.55, 0.95)))
            mv.colourBars = 4;
    }
    else
    {
        // drop: always moving; how fast, how deep, how wild follows the
        // energy - and the drop's character leans the menu
        qreal wild = ramp(e, 0.20, 0.90);
        QList<int> menu = { ENGINE_PAT_ODDEVEN, ENGINE_PAT_HALVES, ENGINE_PAT_STATIC };
        if (chance(ramp(e, 0.15, 0.60)))
            menu << ENGINE_PAT_CHASE << ENGINE_PAT_PINGPONG;
        if (chance(ramp(e, 0.55, 0.95)))
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
        mv.flashBar = chance((m_dropStyle == 1 ? 0.7 : 0.35) * ramp(e, 0.55, 0.95));
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
    if (g.strobes)
        mv.colourBars = 0;
    if (g.fixtures.count() < 2)
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
    qreal dim = g.heads ? 0.35 : (pattern == ENGINE_PAT_CHASE || pattern == ENGINE_PAT_PINGPONG ? 0.0 : 0.15);

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
                mask[i] = i == lit ? 1.0 : (i == tail ? 0.3 : dim);
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

qreal TrackEngine::pulseFactor(const QString &group) const
{
    qreal factor = 1.0;
    qint64 now = m_clock.elapsed();

    // the pulse: full on the beat, down to (1 - depth) a quarter beat later
    // and flat from there - a kick, not a sine
    qreal depth = m_pulseDepth.value(group, 0.0);
    if (depth > 0.0 && m_pulseStart.contains(group))
    {
        qreal tau = qMax(40.0, m_beatMs * 0.25);
        qreal t = qreal(now - m_pulseStart.value(group));
        factor *= (1.0 - depth) + depth * std::exp(-t / tau);
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
    qreal moveP = tier == 0 ? 0.20 + 0.30 * e : (tier == 2 ? 0.85 + 0.15 * e : 0.40 + 0.50 * e);
    if (build)
        moveP = 0.50 + 0.50 * prog;
    if (chance(moveP) == false)
        return sw;

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
    qreal reach = tier == 0 ? 22.0 : (tier == 2 ? 50.0 + 70.0 * e : 28.0 + 45.0 * e);
    if (build)
        reach = 30.0 + 60.0 * prog;
    qreal size = reach * (0.5 + 0.5 * rng->generateDouble());
    // pan has the whole room, tilt has the floor: the heads hang from the
    // ceiling and a figure must not climb the walls
    sw.width = qBound(6, int(size), 127);
    sw.height = qBound(4, int(size * (0.35 + 0.65 * rng->generateDouble())), 28);
    if (sw.shape == int(EFX::Line) || sw.shape == int(EFX::Line2))
        sw.rotation = pick(QList<int>() << 0 << 0 << 90 << 30 << 150 << 60 << 120);
    else
        sw.rotation = chance(0.4) ? int(rng->bounded(360)) : 0;

    // tempo: beats per figure - in time with the music, faster when hot
    if (tier == 0)
        sw.beats = pick(QList<int>() << 16 << 32 << 32);
    else if (tier == 2)
        sw.beats = e > 0.6 ? pick(QList<int>() << 2 << 4 << 4 << 8) : pick(QList<int>() << 4 << 8 << 8 << 16);
    else
        sw.beats = e > 0.5 ? pick(QList<int>() << 4 << 8 << 8 << 16) : pick(QList<int>() << 8 << 16 << 16 << 32);
    if (build)
        sw.beats = prog > 0.5 ? pick(QList<int>() << 2 << 4 << 4) : pick(QList<int>() << 8 << 8 << 16);

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
        sw.beats = qMin(sw.beats, 4);
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
        sw.height = qBound(4, int(sw.height * 0.6), 28);
        sw.beats = qMin(sw.beats, 4);
    }

    // lasers: sideways only, never lifted off the aim the user gave them,
    // small, and never faster than a bar
    if (laser)
    {
        sw.shape = (sw.shape == int(EFX::Lissajous) || sw.shape == int(EFX::Square)) ? int(EFX::Line) : sw.shape;
        sw.height = 0;
        sw.width = qBound(4, sw.width / 2, 36);
        sw.rotation = 0;
        sw.beats = qMax(4, sw.beats);
        if (tier < 2 && chance(0.5))
            sw.shape = -1;
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

    bool running = m_active.contains(slot) && m_active.value(slot) == fid;
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
        if (slot.startsWith("efx:"))
            stopSlot(slot, true);
    m_sweep.clear();
    m_sweepShown.clear();
}

void TrackEngine::checkConflicts(const QSet<QString> &castSet)
{
    // A group the engine holds at zero that still shows light on its master
    // dimmer is being driven by something else - usually a Group Dimmer
    // slider on the Virtual Console. HTP hides that; say it out loud.
    QStringList found;
    QList<Universe *> universes = m_doc->inputOutputMap()->universes();

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        bool fading = false;
        foreach (quint32 part, g.parts)
            if (m_fadeAttr.contains(part))
                fading = true;
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
        if (m_groups.value(key).hasDimmer == false && m_groupOff.contains(key) == false)
            found << tr("%1 has no master dimmer - on/off only").arg(key);

    // a group the engine cannot make colours for (an animation laser) and
    // that lacks a palette colour will sit dark whenever that colour runs
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (g.generatable() || m_groupOff.contains(key))
            continue;
        QStringList missing;
        foreach (const QString &colour, m_palette)
            if (colourFunction(key, colour) == Function::invalidId())
                missing << colour;
        if (missing.isEmpty() == false)
            found << tr("%1 has no scene for %2").arg(key).arg(missing.join(", "));
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
                if (fxi->channel(i) != nullptr && fxi->channel(i)->group() == QLCChannel::Pan)
                    pan = i;
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
    }

    m_warnings = found;
}

QStringList TrackEngine::warnings() const { return m_warnings; }

void TrackEngine::calm(int bars)
{
    // bars <= 0 ends it early
    m_calmUntil = bars <= 0 ? 0 : m_lastBeat + bars * 4;
    emit liveChanged();
}

void TrackEngine::next()
{
    m_forceNext = true;
    emit liveChanged();
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
    m_room = p < 70 ? 0 : (p < 90 ? 1 : (p < 115 ? 2 : 3));
    emit roomChanged(p);
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

bool TrackEngine::hold() const { return m_hold; }

void TrackEngine::setHold(bool on)
{
    if (on == m_hold)
        return;
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
            head << "time,beat,state,cast,colour,level,energy,section_energy,master,moves\n";
        }
    }

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
        << QString(m_lastMoves).replace(',', ';') << '\n';
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
    foreach (const QString &slot, m_active.keys())
        if (slot.startsWith("pos:") == false)
            stopSlot(slot, false);
    m_cast.clear();
    m_lastState.clear();
    m_flash = false;
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
    foreach (const QString &slot, m_active.keys())
    {
        if (slot.startsWith("idle:"))
            continue;
        if (holdBase && (slot == "col:" + base || slot.startsWith("dim:" + base + "#") || slot == "pos:" + base))
            continue;
        stopSlot(slot, false);
    }

    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();

    foreach (TrackFuncInfo *info, list)
        run("idle:" + QString::number(info->id), info->id,
            info->dimmer ? m_master : 1.0, 0, false);

    if (holdBase)
    {
        QString colour = m_colour.isEmpty() ? (m_palette.isEmpty() ? QString() : m_palette.first()) : m_colour;
        quint32 cf = colourFunction(base, colour);
        if (cf != Function::invalidId())
            run("col:" + base, cf, m_funcs.value(cf).dimmer ? 0.35 * m_master : 1.0, 0, false);
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

void TrackEngine::trackLoaded()
{
    // positions are kept: a new track is not a reason to swing the lasers
    m_lastState.clear();
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
    qreal out = m_blackout ? 0.0 : level;        // a blackout: the function runs, at nothing

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_activeAttr.value(slot, -1);
        if (func != nullptr && (func->isRunning() == false || func->stopped()))
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
    }
}

void TrackEngine::tickFades()
{
    if (m_fadeAttr.isEmpty())
        return;

    const qreal step = 0.25;                 // one bar from full to silent
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
    m_moves.clear();
    m_sweep.clear();
    m_sweepShown.clear();
    m_moveHistory.clear();
    m_sweepHistory.clear();
    m_liveMove.clear();
    m_zoom.clear();
    m_dropStyle = 0;
    m_pulseDepth.clear();
    m_breathe.clear();
    m_pulseTimer.stop();
    m_flash = false;
    m_report = tr("(stopped)");
    emit liveChanged();
}
