/*
  Q Light Controller Plus
  trackengine.h

  The automatic busker behind the Track page.

  A palette and a cast, not five roles running at once:

    * ONE colour at a time, put on every lit group through the scenes that
      share its name (Lasergreen + AniGreen + StrobesGreen ...). It changes
      only when a break or a drop starts, or every 32 bars. Drops may add one
      accent colour on a single group. Never three.

    * A small CAST of fixture groups: one in a break, two in the groove,
      three in a drop. Which groups rotate from section to section. Groups
      outside the cast sit at zero.

    * Each group keeps its character: a static colour in the groove, its
      chases and patterns in a drop, flashes on the hits. Laser positions
      are sticky and only change inside a one-beat dark gap at a break.

    * Intensity lives on the groups' master dimmers - the same channels the
      Group Dimmer sliders move - through one hidden scene per group. Colour
      scenes are never intensity-scaled.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#ifndef TRACKENGINE_H
#define TRACKENGINE_H

#include <QVariantList>
#include <QStringList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QList>
#include <QFile>
#include <QMap>
#include <QSet>

class Function;
class Fixture;
class Scene;
class Doc;

#define ENGINE_ROLE_COLOR     0
#define ENGINE_ROLE_MOTION    1
#define ENGINE_ROLE_POSITION  2
#define ENGINE_ROLE_FLASH     3
#define ENGINE_ROLE_IDLE      4
#define ENGINE_ROLE_COUNT     5

#define SETTINGS_ENGINE_ROLES     QStringLiteral("trackengine/roles")
#define SETTINGS_ENGINE_GROUPOFF  QStringLiteral("trackengine/groupoff")
#define SETTINGS_ENGINE_MASTER    QStringLiteral("trackengine/master")
#define SETTINGS_ENGINE_ACCENT    QStringLiteral("trackengine/accent")
#define SETTINGS_ENGINE_HOLDBARS  QStringLiteral("trackengine/holdbars")
#define SETTINGS_ENGINE_BASE      QStringLiteral("trackengine/base")
#define SETTINGS_ENGINE_LOG       QStringLiteral("trackengine/log")

/** Everything the engine needs to know about one function, derived once. */
struct TrackFuncInfo
{
    quint32 id = 0;
    QString name;
    QString path;
    int type = 0;
    int role = -1;            // assigned role, -1 = not used
    int guess = -1;           // what the classifier would say
    QSet<QString> groups;     // fixture groups it touches
    QString colour;           // canonical colour name, or empty
    bool step = false;        // sits inside a chaser or sequence
    bool junk = false;        // blackout / reset / test / copy ...
    bool dimmer = false;      // sets a master dimmer itself (HTP beats the group dimmer)
    int tier = -1;            // tagged for break (0) / groove (1) / drop (2), or any
    bool sweep = false;       // continuous movement (EFX / chaser of positions)
    uint durationMs = 0;      // a chaser's step duration, for tempo matching
};

/** One fixture group as the engine sees it. */
struct TrackGroup
{
    QString key;              // stable name
    QList<quint32> fixtures;
    quint32 dimmerScene = 0;  // hidden scene holding the master dimmers, or invalid
    bool hasDimmer = false;
    bool strobes = false;     // this is the strobe/blinder group
    bool lasers = false;      // beams that must not move while lit
    bool heads = false;       // pan + tilt and not a laser: a moving head
};

class TrackEngine : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(TrackEngine)

    Q_PROPERTY(int roleCount READ roleCount CONSTANT)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY tableChanged)
    Q_PROPERTY(QVariantList palette READ palette NOTIFY tableChanged)
    Q_PROPERTY(bool showAll READ showAll WRITE setShowAll NOTIFY tableChanged)
    Q_PROPERTY(bool accent READ accent WRITE setAccent NOTIFY tableChanged)
    Q_PROPERTY(int holdBars READ holdBars WRITE setHoldBars NOTIFY tableChanged)

    Q_PROPERTY(QString colourOverride READ colourOverride WRITE setColourOverride NOTIFY liveChanged)
    Q_PROPERTY(QString currentColour READ currentColour NOTIFY liveChanged)
    Q_PROPERTY(QStringList cast READ cast NOTIFY liveChanged)
    Q_PROPERTY(qreal master READ master WRITE setMaster NOTIFY liveChanged)
    Q_PROPERTY(bool flashing READ flashing NOTIFY liveChanged)
    Q_PROPERTY(QString report READ report NOTIFY liveChanged)

    Q_PROPERTY(QStringList warnings READ warnings NOTIFY liveChanged)
    Q_PROPERTY(int calmBarsLeft READ calmBarsLeft NOTIFY liveChanged)
    Q_PROPERTY(bool logEnabled READ logEnabled WRITE setLogEnabled NOTIFY tableChanged)

    Q_PROPERTY(bool hazeAvailable READ hazeAvailable NOTIFY tableChanged)
    Q_PROPERTY(qreal haze READ haze WRITE setHaze NOTIFY liveChanged)
    Q_PROPERTY(qreal fan READ fan WRITE setFan NOTIFY liveChanged)

public:
    TrackEngine(Doc *doc, QObject *parent = nullptr);
    ~TrackEngine();

    /* ---- table ---- */
    int roleCount() const;
    Q_INVOKABLE QString roleName(int role) const;
    Q_INVOKABLE QString roleHint(int role) const;

    /** Rows: { id, name, path, role, guess, group, colour, hidden }. */
    Q_INVOKABLE QVariantList table();
    Q_INVOKABLE void assignRole(quint32 fid, int role);
    Q_INVOKABLE void autoAssign(bool force);
    Q_INVOKABLE void rebuild();

    QVariantList groups();
    Q_INVOKABLE void setGroupEnabled(QString key, bool enable);
    Q_INVOKABLE bool groupEnabled(QString key) const;
    /** ON -> BASE -> OFF -> ON. The base group is always lit; the others
     *  are effects added on top as the evening's energy rises. */
    Q_INVOKABLE void cycleGroup(QString key);
    Q_INVOKABLE QString baseGroup() const;

    QVariantList palette();
    bool showAll() const;
    void setShowAll(bool on);
    bool accent() const;
    void setAccent(bool on);
    int holdBars() const;
    void setHoldBars(int bars);

    /* ---- live ---- */
    QString colourOverride() const;
    void setColourOverride(QString colour);
    QString currentColour() const;
    QStringList cast() const;
    qreal master() const;
    void setMaster(qreal level);
    bool flashing() const;
    Q_INVOKABLE void setFlash(bool pressed);
    QString report() const;

    /** Everything that is not right at the moment: a slider overriding the
     *  engine, a group without a dimmer ... shown on the Track page. */
    QStringList warnings() const;
    /** Panic: base group only, one colour, no motion, for this many bars. */
    Q_INVOKABLE void calm(int bars);
    int calmBarsLeft() const;
    bool logEnabled() const;
    void setLogEnabled(bool on);

    /* ---- atmosphere: the hazer's fan and output, straight from two sliders ---- */
    bool hazeAvailable() const;
    qreal haze() const;
    void setHaze(qreal level);
    qreal fan() const;
    void setFan(qreal level);

    /* ---- driven by TrackManager ---- */
    void tick(const QString &state, int beat, int secStart, int secEnd,
              qreal energy, qreal sectionEnergy, int division, bool sectionChanged,
              const QString &nextState, int beatsToNext, qreal bpm, qreal levelScale);
    void trackLoaded();
    /** Nothing is playing but AUTO is on: run the start scene(s). */
    void idle();
    /** AUTO switched off: fade everything out over a bar, then let go. */
    void release();
    void stopAll();

signals:
    void tableChanged();
    void liveChanged();

protected slots:
    void slotDocChanged();
    void slotFadeTimer();

protected:
    /* table building */
    void ensureTable();
    QSet<quint32> fixturesOf(Function *func, int depth) const;
    QString groupOfFixture(quint32 fid) const;
    QString colourOf(const QString &text) const;
    bool hasWord(const QString &text, const QStringList &words) const;
    int classify(const TrackFuncInfo &info) const;
    void loadRoles();
    void saveRoles();
    void ensureDimmerScenes();
    quint32 dimmerChannel(Fixture *fxi) const;
    void ensureAtmosScenes();
    void applyAtmos(quint32 sceneId, const QList<QPair<quint32, quint32> > &channels, qreal level);

    /* choosing */
    QList<TrackFuncInfo *> candidates(int role, const QString &group) const;
    quint32 colourFunction(const QString &group, const QString &colour) const;
    quint32 motionFunction(const QString &group, const QString &colour,
                           const QSet<QString> &cast, int cursor) const;
    quint32 motionFor(const QString &group, const QString &colour,
                      const QSet<QString> &cast, int cursor, int tier,
                      qreal bpm, int division) const;
    quint32 positionFunction(const QString &group, int cursor, int tier) const;
    quint32 flashFunction(const QSet<QString> &cast, const QString &colour) const;
    int tierOf(const QString &text) const;
    QString accentFor(const QString &colour) const;
    qreal tempoScore(const TrackFuncInfo &info, qreal bpm) const;
    void checkConflicts(const QSet<QString> &cast);
    void logBeat(const QString &state, int beat, qreal level, qreal energy, qreal sectionEnergy);

    /* running */
    void run(const QString &slot, quint32 fid, qreal level, int division, bool hard);
    void startFunction(Function *func, int division);
    void stopSlot(const QString &slot, bool hard);
    void tickFades();
    void setDimmer(const QString &group, qreal level);

private:
    Doc *m_doc;

    bool m_dirty;
    QHash<quint32, TrackFuncInfo> m_funcs;
    QMap<QString, TrackGroup> m_groups;
    QStringList m_groupOrder;
    QSet<QString> m_groupOff;
    QString m_base;           // the always-on group, or empty for automatic

    /* atmosphere */
    QList<QPair<quint32, quint32> > m_hazeChannels;   // fixture, channel
    QList<QPair<quint32, quint32> > m_fanChannels;
    quint32 m_hazeScene;
    quint32 m_fanScene;
    qreal m_haze;
    qreal m_fan;
    QStringList m_palette;
    bool m_showAll;
    bool m_accent;
    int m_holdBars;

    /* live state */
    QString m_override;
    QString m_colour;
    int m_colourCursor;
    int m_colourBar;
    int m_castCursor;
    int m_motionCursor;
    QSet<QString> m_cast;
    QMap<QString, quint32> m_position;   // sticky position pick per group
    qreal m_master;
    bool m_flash;
    QString m_lastState;
    QString m_report;
    QStringList m_warnings;
    int m_effects;            // effect groups this section (locked, hysteresis)
    int m_lastBeat;
    int m_calmUntil;          // beat until which the panic look holds
    QTimer m_fadeTimer;       // keeps fades ticking after a release
    bool m_logEnabled;
    QFile m_log;

    /* what is running: slot name -> fid, and its attribute override */
    QMap<QString, quint32> m_active;
    QMap<QString, int> m_activeAttr;
    QMap<QString, qreal> m_activeLevel;
    QMap<quint32, int> m_fadeAttr;
    QMap<quint32, qreal> m_fadeLevel;
};

#endif // TRACKENGINE_H
