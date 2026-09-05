/*
  Q Light Controller Plus
  trackengine.cpp

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#include <QRegularExpression>
#include <QSettings>
#include <QDebug>
#include <functional>
#include <algorithm>

#include "trackengine.h"
#include "functionparent.h"
#include "fixturegroup.h"
#include "qlcfixturedef.h"
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

/*********************************************************************
 * Setup
 *********************************************************************/

TrackEngine::TrackEngine(Doc *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_dirty(true)
    , m_showAll(false)
    , m_accent(true)
    , m_holdBars(32)
    , m_colourCursor(0)
    , m_colourBar(-1)
    , m_castCursor(0)
    , m_motionCursor(0)
    , m_master(1.0)
    , m_flash(false)
{
    QSettings settings;
    m_master = settings.value(SETTINGS_ENGINE_MASTER, 1.0).toDouble();
    m_accent = settings.value(SETTINGS_ENGINE_ACCENT, true).toBool();
    m_holdBars = settings.value(SETTINGS_ENGINE_HOLDBARS, 32).toInt();
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

void TrackEngine::slotDocChanged()
{
    m_dirty = true;
    m_position.clear();
    emit tableChanged();
}

int TrackEngine::roleCount() const { return TRACK_ROLE_COUNT; }

QString TrackEngine::roleName(int role) const
{
    switch (role)
    {
    case TRACK_ROLE_COLOR:    return tr("Colour");
    case TRACK_ROLE_MOTION:   return tr("Motion");
    case TRACK_ROLE_POSITION: return tr("Position");
    case TRACK_ROLE_FLASH:    return tr("Flash");
    default:                  return tr("Off");
    }
}

QString TrackEngine::roleHint(int role) const
{
    switch (role)
    {
    case TRACK_ROLE_COLOR:
        return tr("A static colour. The palette puts the same colour on every lit group.");
    case TRACK_ROLE_MOTION:
        return tr("Chases, patterns, EFX. Only in drops and the back half of a build.");
    case TRACK_ROLE_POSITION:
        return tr("Held, never stopped. Changes only inside a dark beat at a break.");
    case TRACK_ROLE_FLASH:
        return tr("Hits: the last bar of a build, the drop, and the FLASH button.");
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
            foreach (SceneValue sv, scene->values())
                out.insert(sv.fxi);
        break;
    }
    case Function::ChaserType:
    case Function::SequenceType:
    {
        Chaser *chaser = qobject_cast<Chaser *>(func);
        if (chaser != nullptr)
            foreach (ChaserStep step, chaser->steps())
                out.unite(fixturesOf(m_doc->function(step.fid), depth + 1));
        break;
    }
    case Function::CollectionType:
    {
        Collection *coll = qobject_cast<Collection *>(func);
        if (coll != nullptr)
            foreach (quint32 child, coll->functions())
                out.unite(fixturesOf(m_doc->function(child), depth + 1));
        break;
    }
    case Function::EFXType:
    {
        EFX *efx = qobject_cast<EFX *>(func);
        if (efx != nullptr)
            foreach (EFXFixture *ef, efx->fixtures())
                if (ef != nullptr)
                    out.insert(ef->head().fxi);
        break;
    }
    case Function::RGBMatrixType:
    {
        RGBMatrix *matrix = qobject_cast<RGBMatrix *>(func);
        if (matrix != nullptr)
        {
            FixtureGroup *grp = m_doc->fixtureGroup(matrix->fixtureGroup());
            if (grp != nullptr)
                foreach (quint32 fid, grp->fixtureList())
                    out.insert(fid);
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

    switch (info.type)
    {
    case Function::CollectionType:
        if (hasWord(n, flashWords))   return TRACK_ROLE_FLASH;
        if (info.colour.isEmpty() == false) return TRACK_ROLE_COLOR;
        return TRACK_ROLE_MOTION;

    case Function::EFXType:
        // a sweep on lasers is a position; on heads it is motion
        return laserOnly ? TRACK_ROLE_POSITION : TRACK_ROLE_MOTION;

    case Function::ChaserType:
    case Function::SequenceType:
    {
        // a chaser whose every step is a position is itself a position
        // (UP/DOWN on the beat) and must obey the same safety rules
        Chaser *chaser = qobject_cast<Chaser *>(m_doc->function(info.id));
        bool allPos = chaser != nullptr && chaser->steps().isEmpty() == false;
        if (chaser != nullptr)
            foreach (ChaserStep step, chaser->steps())
            {
                Function *sf = m_doc->function(step.fid);
                QString sn = sf ? sf->name().toLower() : QString();
                sn.replace("laser", " ").replace("beam", " ");
                if (sf == nullptr || hasWord(sn, positionWords) == false)
                    allPos = false;
            }
        return allPos ? TRACK_ROLE_POSITION : TRACK_ROLE_MOTION;
    }
    case Function::RGBMatrixType:
        return TRACK_ROLE_MOTION;

    default:
        break;
    }

    // scenes
    if (hasWord(n, flashWords))
        return TRACK_ROLE_FLASH;
    if (hasWord(core, positionWords))
        return TRACK_ROLE_POSITION;
    if (hasWord(n, patternWords))
        return TRACK_ROLE_MOTION;
    if (hasWord(n, strobeWords) && info.colour.isEmpty())
        return TRACK_ROLE_FLASH;
    return TRACK_ROLE_COLOR;
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
            foreach (ChaserStep step, chaser->steps())
                steps.insert(step.fid);
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
        if (func->name().startsWith(ENGINE_DIMMER_PREFIX))
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
        info.junk = hasWord(n, junkWords);
        info.colour = colourOf(n);
        if (info.colour.isEmpty())
            info.colour = colourOf(info.name);          // case-sensitive suffix rule

        foreach (quint32 fid, fixturesOf(func, 0))
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
            foreach (SceneValue sv, scene->values())
            {
                Fixture *fxi = m_doc->fixture(sv.fxi);
                if (fxi != nullptr && sv.channel == fxi->masterIntensityChannel() && sv.value > 0)
                    info.dimmer = true;
            }

        info.role = old.contains(info.id) ? old.value(info.id).role : -2;   // -2 = not decided yet
        m_funcs.insert(info.id, info);
    }

    // guesses need the groups to exist first
    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
        it.value().guess = classify(it.value());

    loadRoles();

    for (QHash<quint32, TrackFuncInfo>::iterator it = m_funcs.begin(); it != m_funcs.end(); ++it)
        if (it.value().role == -2)
            it.value().role = it.value().step ? -1 : it.value().guess;

    /* ---- palette: colours that exist on at least two groups ---- */
    QMap<QString, QSet<QString> > coverage;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
    {
        const TrackFuncInfo &info = it.value();
        if (info.role != TRACK_ROLE_COLOR || info.colour.isEmpty())
            continue;
        coverage[info.colour].unite(info.groups);
    }

    m_palette.clear();
    QStringList singles;
    QMapIterator<QString, QSet<QString> > cit(coverage);
    while (cit.hasNext())
    {
        cit.next();
        if (cit.value().count() >= 2)
            m_palette.append(cit.key());
        else
            singles.append(cit.key());
    }
    if (m_palette.count() < 2)
        m_palette.append(singles);

    ensureDimmerScenes();

    qDebug() << "[TrackEngine]" << m_groups.count() << "groups," << m_funcs.count()
             << "functions, palette" << m_palette;
}

void TrackEngine::ensureDimmerScenes()
{
    // one hidden scene per group, holding every master dimmer at full;
    // its intensity attribute is the group's level
    QMap<QString, quint32> existing;
    foreach (Function *func, m_doc->functions())
        if (func != nullptr && func->name().startsWith(ENGINE_DIMMER_PREFIX))
            existing.insert(func->name().mid(ENGINE_DIMMER_PREFIX.length()), func->id());

    for (QMap<QString, TrackGroup>::iterator it = m_groups.begin(); it != m_groups.end(); ++it)
    {
        TrackGroup &g = it.value();

        QList<SceneValue> values;
        foreach (quint32 fid, g.fixtures)
        {
            Fixture *fxi = m_doc->fixture(fid);
            if (fxi == nullptr)
                continue;
            quint32 ch = fxi->masterIntensityChannel();
            if (ch != QLCChannel::invalid())
                values.append(SceneValue(fid, ch, 255));
        }

        g.hasDimmer = values.isEmpty() == false;
        g.dimmerScene = Function::invalidId();
        if (g.hasDimmer == false)
            continue;

        if (existing.contains(g.key))
        {
            Scene *scene = qobject_cast<Scene *>(m_doc->function(existing.value(g.key)));
            if (scene != nullptr)
            {
                foreach (SceneValue sv, values)
                    scene->setValue(sv);
                g.dimmerScene = scene->id();
                continue;
            }
        }

        Scene *scene = new Scene(m_doc);
        scene->setName(ENGINE_DIMMER_PREFIX + g.key);
        scene->setVisible(false);
        foreach (SceneValue sv, values)
            scene->setValue(sv);

        if (m_doc->addFunction(scene))
            g.dimmerScene = scene->id();
        else
            delete scene;
    }
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
}

void TrackEngine::saveRoles()
{
    QStringList entries;
    for (QHash<quint32, TrackFuncInfo>::const_iterator it = m_funcs.constBegin(); it != m_funcs.constEnd(); ++it)
        if (it.value().role != it.value().guess || it.value().step)
            entries << QString("%1:%2").arg(it.key()).arg(it.value().role);
    QSettings().setValue(SETTINGS_ENGINE_ROLES, entries.join(';'));
    QSettings().setValue(SETTINGS_ENGINE_GROUPOFF, QStringList(m_groupOff.values()).join(';'));
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
        QString ga = a.groups.isEmpty() ? QString() : *a.groups.constBegin();
        QString gb = b.groups.isEmpty() ? QString() : *b.groups.constBegin();
        if (ga != gb) return ga < gb;
        return a.name.toLower() < b.name.toLower();
    });

    foreach (const TrackFuncInfo &info, rows)
    {
        bool hidden = info.junk || info.step || info.groups.isEmpty();
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
            if (it.value().role == TRACK_ROLE_COLOR) colours++;
            if (it.value().role == TRACK_ROLE_MOTION) motions++;
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
    QSettings().setValue(SETTINGS_ENGINE_MASTER, level);

    // re-apply to whatever is lit right now
    foreach (const QString &slot, m_active.keys())
        if (slot.startsWith("dim:"))
        {
            Function *func = m_doc->function(m_active.value(slot));
            int attr = m_activeAttr.value(slot, -1);
            if (func != nullptr && attr >= 0)
                func->adjustAttribute(m_activeLevel.value(slot, 1.0) * m_master, attr);
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
        QSet<QString> strobeGroups;
        foreach (const QString &key, m_groupOrder)
            if (m_groups.value(key).strobes)
                strobeGroups.insert(key);
        quint32 fid = flashFunction(strobeGroups, "white");
        if (fid == Function::invalidId())
            fid = flashFunction(QSet<QString>(m_groupOrder.begin(), m_groupOrder.end()), "white");
        if (fid != Function::invalidId())
            run("flash", fid, 1.0, 0, true);
    }
    else
    {
        stopSlot("flash", true);
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
        out.append(const_cast<TrackFuncInfo *>(&info));
    }
    std::sort(out.begin(), out.end(), [](TrackFuncInfo *a, TrackFuncInfo *b) { return a->id < b->id; });
    return out;
}

quint32 TrackEngine::colourFunction(const QString &group, const QString &colour) const
{
    QList<TrackFuncInfo *> list = candidates(TRACK_ROLE_COLOR, group);

    // exactly this group, exactly this colour
    foreach (TrackFuncInfo *info, list)
        if (info->groups.count() == 1 && info->colour == colour)
            return info->id;
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
    QList<TrackFuncInfo *> all = candidates(TRACK_ROLE_MOTION, group);
    QList<TrackFuncInfo *> ok;
    foreach (TrackFuncInfo *info, all)
    {
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
    return ok.at(qAbs(cursor) % ok.count())->id;
}

quint32 TrackEngine::positionFunction(const QString &group, int cursor) const
{
    QList<TrackFuncInfo *> list = candidates(TRACK_ROLE_POSITION, group);
    if (list.isEmpty())
        return Function::invalidId();
    return list.at(qAbs(cursor) % list.count())->id;
}

quint32 TrackEngine::flashFunction(const QSet<QString> &cast, const QString &colour) const
{
    QList<TrackFuncInfo *> list = candidates(TRACK_ROLE_FLASH, QString());
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
                       qreal energy, int division, bool sectionChanged)
{
    if (m_doc == nullptr)
        return;
    ensureTable();
    tickFades();

    bool isBreak = (state == QStringLiteral("break"));
    bool isBuild = (state == QStringLiteral("build"));
    bool isDrop  = (state == QStringLiteral("drop"));

    int len = qMax(1, secEnd - secStart);
    qreal prog = qBound(0.0, qreal(beat - secStart) / qreal(len), 1.0);
    int bar = (beat - secStart) / 4;
    int beatInBar = (beat - secStart) % 4;

    /* ---- palette: one colour, changed rarely ---- */
    int holdBar = beat / qMax(4, m_holdBars * 4);
    bool changeColour = (sectionChanged && (isBreak || isDrop)) || holdBar != m_colourBar;
    if (m_colourBar < 0)
        changeColour = true;
    m_colourBar = holdBar;

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

    QString accentColour;
    if (m_accent && isDrop && m_palette.count() >= 2 && cast.count() >= 2 && m_override.isEmpty())
        accentColour = m_palette.at((m_colourCursor + 1) % m_palette.count());

    /* ---- eligible groups: enabled, and with something to show ---- */
    QStringList eligible;
    foreach (const QString &key, m_groupOrder)
    {
        if (m_groupOff.contains(key))
            continue;
        // no colour look at all means the group cannot take the palette -
        // it waits until it has one (moving heads before their scenes exist)
        if (candidates(TRACK_ROLE_COLOR, key).isEmpty())
            continue;
        eligible.append(key);
    }

    /* ---- cast: how many groups, and which ---- */
    int castSize = isBreak ? 1 : (isDrop ? 3 : 2);
    if (sectionChanged)
    {
        if (m_lastState.isEmpty() == false)
            m_castCursor++;
        m_motionCursor++;
    }
    m_lastState = state;

    QSet<QString> cast;
    if (eligible.isEmpty() == false)
    {
        int n = eligible.count();
        for (int i = 0; i < qMin(castSize, n); i++)
            cast.insert(eligible.at((m_castCursor + i) % n));
        // strobes carry a drop; make sure they are in when there is one
        if (isDrop)
            foreach (const QString &key, eligible)
                if (m_groups.value(key).strobes && cast.count() < 3)
                    cast.insert(key);
    }

    /* ---- positions: sticky, only changed in the dark ---- */
    QSet<QString> darkGroups;
    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        if (candidates(TRACK_ROLE_POSITION, key).isEmpty())
            continue;

        bool inCast = cast.contains(key);
        // heads may move whenever a section changes; a laser only when it is
        // dark - out of the cast, or in the dark beat that opens a break
        bool mayMove = (inCast == false)
                    || (sectionChanged && (g.lasers ? (isBreak && g.hasDimmer) : true));
        quint32 want = m_position.value(key, Function::invalidId());
        if (want == Function::invalidId() || (mayMove && sectionChanged))
        {
            quint32 np = positionFunction(key, m_castCursor);
            if (np != want && (mayMove || want == Function::invalidId()))
            {
                if (inCast && g.lasers)
                    darkGroups.insert(key);
                want = np;
                m_position.insert(key, want);
            }
        }
        if (want != Function::invalidId())
            run("pos:" + key, want, 1.0, 0, true);
    }

    /* ---- per group ---- */
    qreal tierLevel = isBreak ? 0.35
                    : isBuild ? (0.40 + 0.60 * prog)
                    : isDrop  ? 1.0
                              : 0.70;
    qreal level = tierLevel * (0.5 + 0.5 * qBound(0.0, energy, 1.0));

    bool hard = sectionChanged && isDrop;
    QStringList castSorted = cast.values();
    castSorted.sort();
    int accentIndex = castSorted.isEmpty() ? -1 : castSorted.count() - 1;

    foreach (const QString &key, m_groupOrder)
    {
        const TrackGroup &g = m_groups.value(key);
        bool inCast = cast.contains(key);

        if (inCast == false)
        {
            stopSlot("col:" + key, false);
            stopSlot("mot:" + key, false);
            if (g.hasDimmer)
                setDimmer(key, 0.0);
            continue;
        }

        QString colour = m_colour;
        if (accentColour.isEmpty() == false && castSorted.indexOf(key) == accentIndex)
            colour = accentColour;

        // colour: the base look, full attribute - intensity is the dimmer's job
        qreal gl = darkGroups.contains(key) ? 0.0 : level * m_master;
        quint32 cf = colourFunction(key, colour);
        if (cf != Function::invalidId())
            run("col:" + key, cf, m_funcs.value(cf).dimmer ? gl : 1.0, 0, hard);
        else
            stopSlot("col:" + key, false);

        // motion: drops, and the climbing half of a build
        bool wantMotion = isDrop || (isBuild && prog > 0.5);
        if (wantMotion)
        {
            quint32 mf = motionFunction(key, colour, cast, m_motionCursor);
            if (mf != Function::invalidId())
                run("mot:" + key, mf, m_funcs.value(mf).dimmer ? gl : 1.0, division, hard);
            else
                stopSlot("mot:" + key, false);
        }
        else
        {
            stopSlot("mot:" + key, false);
        }

        if (g.hasDimmer)
            setDimmer(key, darkGroups.contains(key) ? 0.0 : level);
    }

    /* ---- hits ---- */
    bool hit = (isBuild && prog > 0.82) || (isDrop && bar == 0 && beatInBar < 2);
    if (m_flash == false)
    {
        if (hit)
        {
            quint32 ff = flashFunction(cast, m_colour);
            if (ff != Function::invalidId())
                run("flash", ff, 1.0, 0, true);
        }
        else
        {
            stopSlot("flash", true);
        }
    }

    m_cast = cast;
    m_report = QString("%1  |  %2%3  |  %4")
        .arg(castSorted.isEmpty() ? tr("(no groups)") : castSorted.join(" + "))
        .arg(m_colour.isEmpty() ? tr("(no colour)") : m_colour)
        .arg(accentColour.isEmpty() ? QString() : QString(" + %1").arg(accentColour))
        .arg(state);
    emit liveChanged();
}

void TrackEngine::trackLoaded()
{
    m_position.clear();
    m_lastState.clear();
    m_colourBar = -1;
    m_castCursor++;
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

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_activeAttr.value(slot, -1);
        if (func != nullptr && attr >= 0 && qFuzzyCompare(level, m_activeLevel.value(slot, -1.0)) == false)
            func->adjustAttribute(level, attr);
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
    else if (func->isRunning() == false)
    {
        if (division > 0)
            func->start(m_doc->masterTimer(), FunctionParent::master(), 0,
                        Function::defaultSpeed(), Function::defaultSpeed(),
                        uint(division), Function::Beats);
        else
            func->start(m_doc->masterTimer(), FunctionParent::master());
    }

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, level);
    m_activeAttr.insert(slot, func->requestAttributeOverride(ENGINE_INTENSITY_ATTR, level));
}

void TrackEngine::stopSlot(const QString &slot, bool hard)
{
    quint32 fid = m_active.value(slot, Function::invalidId());
    if (fid == Function::invalidId())
        return;

    int attr = m_activeAttr.value(slot, -1);
    m_active.remove(slot);
    m_activeAttr.remove(slot);
    qreal level = m_activeLevel.take(slot);

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;

    // the same function may be held by another slot (a collection shared by
    // two groups): then only drop our override
    if (m_active.values().contains(fid))
    {
        if (attr >= 0) func->releaseAttributeOverride(attr);
        return;
    }

    if (hard || attr < 0 || slot.startsWith("dim:"))
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
            func->adjustAttribute(level, attr);
            m_fadeLevel.insert(fid, level);
        }
    }
}

void TrackEngine::setDimmer(const QString &group, qreal level)
{
    const TrackGroup &g = m_groups.value(group);
    if (g.hasDimmer == false || g.dimmerScene == Function::invalidId())
        return;

    QString slot = "dim:" + group;
    quint32 fid = g.dimmerScene;
    qreal applied = qBound(0.0, level, 1.0) * m_master;

    if (m_active.value(slot, Function::invalidId()) == fid)
    {
        Function *func = m_doc->function(fid);
        int attr = m_activeAttr.value(slot, -1);
        if (func != nullptr && attr >= 0)
            func->adjustAttribute(applied, attr);
        m_activeLevel.insert(slot, qBound(0.0, level, 1.0));
        return;
    }

    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return;
    if (func->isRunning() == false)
        func->start(m_doc->masterTimer(), FunctionParent::master());

    m_active.insert(slot, fid);
    m_activeLevel.insert(slot, qBound(0.0, level, 1.0));
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
    m_flash = false;
    m_report = tr("(stopped)");
    emit liveChanged();
}
