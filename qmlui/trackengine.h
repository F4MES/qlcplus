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
      Group Dimmer sliders move - through one hidden scene per FIXTURE. That
      split is what lets the engine move light without a single chaser:
      chases, ping-pong, odd/even, sparkle and fill run across a group's
      fixtures in whatever colour the palette holds, and a 40 ms timer lets
      the dimmers breathe on the beat.

    * Every section draws a fresh MOVE per group at random - pattern, step
      length, pulse depth and accent rhythm - from a menu that grows with
      the energy: low energy gets a static look, the middle gets colour
      changes and a soft pulse, high energy gets everything. Two sections
      never look quite the same.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#ifndef TRACKENGINE_H
#define TRACKENGINE_H

#include <QElapsedTimer>
#include <QVariantList>
#include <QStringList>
#include <QVector>
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

/* generated dimmer patterns, run across a group's fixtures */
#define ENGINE_PAT_STATIC     0
#define ENGINE_PAT_CHASE      1
#define ENGINE_PAT_PINGPONG   2
#define ENGINE_PAT_ODDEVEN    3
#define ENGINE_PAT_HALVES     4
#define ENGINE_PAT_SPARKLE    5
#define ENGINE_PAT_FILL       6
#define ENGINE_PAT_COUNT      7

#define SETTINGS_ENGINE_ROLES     QStringLiteral("trackengine/roles")
#define SETTINGS_ENGINE_GROUPOFF  QStringLiteral("trackengine/groupoff")
#define SETTINGS_ENGINE_MASTER    QStringLiteral("trackengine/master")
#define SETTINGS_ENGINE_ACCENT    QStringLiteral("trackengine/accent")
#define SETTINGS_ENGINE_HOLDBARS  QStringLiteral("trackengine/holdbars")
#define SETTINGS_ENGINE_BASE      QStringLiteral("trackengine/base")
#define SETTINGS_ENGINE_LOG       QStringLiteral("trackengine/log")
#define SETTINGS_ENGINE_STARS     QStringLiteral("trackengine/stars")
#define SETTINGS_ENGINE_FULLAUTO  QStringLiteral("trackengine/fullauto")

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
    uint durationMs = 0;      // a chaser's step (or an EFX cycle) in ms, 0 = unknown
    qreal beats = 0.0;        // ... or in beats, when the chaser runs in Beats tempo
    bool oneShot = false;     // runs once and stops: retriggered on the beat
    bool generated = false;   // made by the engine (a palette colour the group lacked)
    int stars = 0;            // energy 1..3: when this may run (0 = not applicable)
    int starsGuess = 0;       // what the engine would say, from tempo and name
    int fixtureCount = 0;     // how many fixtures it touches - a full look beats a part
};

/** How one group moves inside a section. Drawn at random when the section
 *  starts, from a menu that depends on the section type and the energy. */
struct TrackMove
{
    int pattern = ENGINE_PAT_STATIC;
    int stepBeats = 4;        // beats per pattern step: 1 = every beat, 4 = every bar
    qreal pulse = 0.0;        // how deep the dimmers breathe on the beat, 0..1
    int pulseOn = 0;          // 0 every beat, 1 beats 1+3, 2 beats 2+4, 3 downbeat only
    int colourBars = 0;       // accent group: swap palette/accent every N bars (0 = hold)
    bool flashBar = false;    // a hit on the downbeat of every second bar
    bool ownChaser = false;   // run one of the user's chases/EFX instead of a pattern
    int breatheBars = 0;      // slow sine on the level over this many bars (breaks)
    int phase = 0;            // random start offset into the pattern
};

/** A figure for a group of moving heads: a hidden EFX run RELATIVE to the
 *  aimed position, so it rides on whatever position scene holds the heads.
 *  Drawn per section from the energy, like the moves. */
struct TrackSweep
{
    int shape = -1;           // EFX::Algorithm, -1 = none: the heads hold their aim
    int width = 0;            // pan reach, 0..127
    int height = 0;           // tilt reach, 0..127
    int rotation = 0;         // degrees
    int beats = 8;            // one figure per this many beats
    int spread = 0;           // 0 in unison, 1 a wave across the heads, 2 one after another
    bool mirror = false;      // every second head runs the figure backwards
    int fan = 0;              // start offset per head, degrees (0 = all at the same point)
    int fx = 2;               // lissajous frequencies
    int fy = 3;
    bool operator==(const TrackSweep &o) const
    {
        return shape == o.shape && width == o.width && height == o.height && rotation == o.rotation
            && beats == o.beats && spread == o.spread && mirror == o.mirror && fan == o.fan
            && fx == o.fx && fy == o.fy;
    }
};

/** One fixture group as the engine sees it. */
struct TrackGroup
{
    QString key;              // stable name
    QList<quint32> fixtures;
    QList<quint32> parts;     // hidden scene per fixture holding its master dimmer, or invalid
    bool hasDimmer = false;
    bool strobes = false;     // this is the strobe/blinder group
    bool lasers = false;      // beams that must not move while lit
    bool heads = false;       // pan + tilt and not a laser: a moving head

    /* what the engine can make from the DMX channels alone */
    bool rgb = false;             // red, green and blue intensity channels
    bool patternDevice = false;   // an animation laser: its patterns live in its own scenes
    /* learned from the user's colour scenes: channels that change with the
     * colour (a macro, or the laser bars' eight per-eye channels) and what
     * each colour sets them to; and channels every colour scene sets alike */
    QMap<quint32, QMap<quint32, QMap<QString, uchar> > > colourValue;  // fixture -> channel -> colour -> value
    QMap<quint32, QMap<quint32, uchar> > baseValue;                     // fixture -> channel -> value
    bool perEye = false;          // several per-eye colour channels: two colours on one lamp
    bool generatable() const { return patternDevice == false && (rgb || colourValue.isEmpty() == false); }
};

class TrackEngine : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(TrackEngine)

    Q_PROPERTY(int roleCount READ roleCount CONSTANT)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY tableChanged)
    Q_PROPERTY(QVariantList palette READ palette NOTIFY tableChanged)
    Q_PROPERTY(bool showAll READ showAll WRITE setShowAll NOTIFY tableChanged)
    /** The engine makes everything itself from the fixtures' channels -
     *  colours, head positions and movement, patterns, the flash - and the
     *  user's scenes step aside. Pattern devices (animation lasers) keep
     *  their scenes, and so do laser positions: a generated tilt is not
     *  a safe tilt. */
    Q_PROPERTY(bool fullAuto READ fullAuto WRITE setFullAuto NOTIFY tableChanged)
    Q_PROPERTY(bool accent READ accent WRITE setAccent NOTIFY tableChanged)
    Q_PROPERTY(int holdBars READ holdBars WRITE setHoldBars NOTIFY tableChanged)

    Q_PROPERTY(QString colourOverride READ colourOverride WRITE setColourOverride NOTIFY liveChanged)
    Q_PROPERTY(QString currentColour READ currentColour NOTIFY liveChanged)
    Q_PROPERTY(QStringList cast READ cast NOTIFY liveChanged)
    /** The DJ's fader per group, 0..1, on top of everything the engine does.
     *  Not remembered between nights. */
    Q_PROPERTY(QVariantMap trims READ trims NOTIFY liveChanged)
    Q_PROPERTY(qreal master READ master WRITE setMaster NOTIFY liveChanged)
    /** The DJ's tempo for everything the engine moves: -1 half speed, 0 as
     *  the music says, +1 double. Patterns, the user's chases, the heads'
     *  walk - all of it. Not remembered between nights. */
    Q_PROPERTY(int speed READ speed WRITE setSpeed NOTIFY liveChanged)
    Q_PROPERTY(bool flashing READ flashing NOTIFY liveChanged)
    /** Everything the engine drives at zero - colours, chases, parts - while
     *  the engine keeps following the track underneath, so releasing it
     *  lands on the right look. A toggle, not a hold. */
    Q_PROPERTY(bool blackout READ blackout WRITE setBlackout NOTIFY liveChanged)
    /** Two decks on air (from BLT): a transition. The colour holds and the
     *  engine stays at groove level until the outgoing track is gone. */
    Q_PROPERTY(bool mixing READ mixing NOTIFY liveChanged)
    Q_PROPERTY(QString report READ report NOTIFY liveChanged)

    Q_PROPERTY(QStringList warnings READ warnings NOTIFY liveChanged)
    Q_PROPERTY(int calmBarsLeft READ calmBarsLeft NOTIFY liveChanged)
    Q_PROPERTY(bool logEnabled READ logEnabled WRITE setLogEnabled NOTIFY tableChanged)

    /** Kept for scripts: 0 empty, 1 warming, 2 full, 3 peak - a preset for
     *  the ENERGY slider. The page itself only has the slider. */
    Q_PROPERTY(int room READ room WRITE setRoom NOTIFY liveChanged)
    /** Let the clock move the ENERGY slider through the night. A hand on
     *  the slider turns it off. */
    Q_PROPERTY(bool roomAuto READ roomAuto WRITE setRoomAuto NOTIFY liveChanged)
    /** Freeze the look: no colour, cast or move changes until released. */
    Q_PROPERTY(bool hold READ hold WRITE setHold NOTIFY liveChanged)

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
    /** Energy 1..3 for a motion: 1 may run anywhere, 3 only in a full-energy drop. */
    Q_INVOKABLE void setStars(quint32 fid, int stars);
    Q_INVOKABLE void autoAssign(bool force);
    Q_INVOKABLE void rebuild();

    QVariantList groups();
    Q_INVOKABLE void setGroupEnabled(QString key, bool enable);
    Q_INVOKABLE bool groupEnabled(QString key) const;
    /** ON -> BASE -> OFF -> ON. The base group is always lit; the others
     *  are effects added on top as the evening's energy rises. */
    Q_INVOKABLE void cycleGroup(QString key);
    QVariantMap trims() const;
    Q_INVOKABLE qreal groupTrim(QString key) const;
    Q_INVOKABLE void setGroupTrim(QString key, qreal level);
    Q_INVOKABLE QString baseGroup() const;

    QVariantList palette();
    bool showAll() const;
    void setShowAll(bool on);
    bool fullAuto() const;
    void setFullAuto(bool on);
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
    int speed() const;
    void setSpeed(int speed);
    bool flashing() const;
    Q_INVOKABLE void setFlash(bool pressed);
    bool blackout() const;
    void setBlackout(bool on);
    bool mixing() const;
    void setMixing(bool on);
    /** Every trackengine/ and trackmanager/ setting to and from
     *  Documents/QLC+/track-settings.json. Returns a message for the page. */
    Q_INVOKABLE QString exportSettings();
    Q_INVOKABLE QString importSettings();
    QString report() const;

    /** Everything that is not right at the moment: a slider overriding the
     *  engine, a group without a dimmer ... shown on the Track page. */
    QStringList warnings() const;
    /** Panic: base group only, one colour, no motion, for this many bars. */
    Q_INVOKABLE void calm(int bars);
    int calmBarsLeft() const;
    /** New colour, new cast, new moves - now. */
    Q_INVOKABLE void next();
    int room() const;
    void setRoom(int room);
    bool roomAuto() const;
    void setRoomAuto(bool on);
    int roomByClock() const;
    /** The ENERGY percent the clock last handed to TrackManager. */
    Q_INVOKABLE int roomPercent() const;
    /** ENERGY by the clock, a restaurant's night: 0 (still) until 22:00,
     *  20 % at 23:00, 45 % at midnight, 70 % at 01:00, 85 % from 02:00, back
     *  to 0 at 05:00 - a slow creep, not steps. The DJ pushes the slider when
     *  the floor actually opens. */
    int clockPercent() const;
    void announceRoom();
    bool hold() const;
    void setHold(bool on);
    bool logEnabled() const;
    void setLogEnabled(bool on);

    /* ---- atmosphere: the hazer's fan and output, straight from two sliders ---- */
    bool hazeAvailable() const;
    qreal haze() const;
    void setHaze(qreal level);
    qreal fan() const;
    void setFan(qreal level);

    /* ---- driven by TrackManager ---- */
    /** kick / high: what the analysis heard on this beat, 0..1, or -1 when
     *  BLT did not send the curves. The pulse follows the kick; a hats-only
     *  passage sparkles. */
    void tick(const QString &state, int beat, int secStart, int secEnd,
              qreal energy, qreal sectionEnergy, int division, bool sectionChanged,
              const QString &nextState, int beatsToNext, qreal bpm, qreal levelScale,
              qreal kick = -1.0, qreal high = -1.0);
    void trackLoaded();
    /** Nothing is playing but AUTO is on: run the start scene(s). */
    void idle();
    /** AUTO switched off: fade everything out over a bar, then let go. */
    void release();
    void stopAll();

signals:
    void tableChanged();
    void liveChanged();
    /** ROOM in percent of energy (55 / 80 / 100 / 125): TrackManager puts it
     *  on the ENERGY trim, so ROOM and the ENERGY slider are one dial. */
    void roomChanged(int percent);

protected slots:
    void slotDocChanged();
    void slotFadeTimer();
    void slotPulseTimer();

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
    void learnGroups();
    void ensureColourScenes();
    void ensurePositionScenes();
    void ensureSweeps();
    TrackSweep drawSweep(int tier, bool build, qreal prog, qreal energy, int heads) const;
    void applySweep(const QString &group, const TrackSweep &sweep, qreal bpm);
    QString sweepName(const TrackSweep &sweep) const;
    void stopSweeps();
    bool userAllowed(const TrackFuncInfo &info) const;
    void genFlash(bool on);
    quint32 dimmerChannel(Fixture *fxi) const;
    int guessStars(const TrackFuncInfo &info) const;
    qreal stepBeats(const TrackFuncInfo &info, qreal bpm) const;
    int divisionFor(const TrackFuncInfo &info, qreal bpm, int division) const;
    void ensureAtmosScenes();
    void applyAtmos(quint32 sceneId, const QList<QPair<quint32, quint32> > &channels, qreal level);

    /* choosing */
    QList<TrackFuncInfo *> candidates(int role, const QString &group) const;
    quint32 colourFunction(const QString &group, const QString &colour) const;
    /** A hidden scene with colour a on the even eyes and b on the odd ones,
     *  for groups whose colour lives on per-eye channels. Made on demand. */
    quint32 splitColourFunction(const QString &group, const QString &a, const QString &b);
    quint32 motionFunction(const QString &group, const QString &colour,
                           const QSet<QString> &cast, int cursor) const;
    quint32 motionFor(const QString &group, const QString &colour,
                      const QSet<QString> &cast, int cursor, int tier,
                      qreal bpm, int division, bool staticOnly, int maxStars) const;
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

    /* generated motion */
    TrackMove drawMove(const QString &group, int tier, bool build, qreal energy, bool isBase) const;
    void applyMove(const QString &group, qreal level, int beat, int secStart, qreal prog,
                   const TrackMove &move, bool patterned);
    void setPart(const QString &group, int index, qreal level);
    QString partSlot(const QString &group, int index) const;
    qreal pulseFactor(const QString &group) const;
    QString moveName(const TrackMove &move) const;

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
    int m_colourBar;          // -1: a fresh track, hold the colour until a break or drop
    int m_colourSince;        // beat of the last colour change
    int m_holdNow;            // bars this colour holds - drawn each change around holdBars
    QString m_accentPick;     // the accent drawn for this section
    int m_castCursor;
    int m_motionCursor;
    QSet<QString> m_cast;
    QMap<QString, quint32> m_position;   // sticky position pick per group
    qreal m_master;
    bool m_blackout;
    bool m_mixing;
    QMap<QString, quint32> m_splitScenes;  // "group|a|b" -> hidden two-colour scene
    int m_speed;                          // -1 half, 0 as the music, +1 double
    QMap<QString, qreal> m_groupTrim;     // the DJ's fader per group, 1.0 when untouched
    bool m_flash;
    QString m_lastState;
    QString m_report;
    QStringList m_warnings;
    QMap<QString, int> m_conflictBeats;   // how long a group has read lit while held dark
    QMap<quint32, int> m_lastPan;         // last pan reading per head, for the Light Rider check
    QMap<QString, int> m_headMoveBeats;   // beats in a row a head group moved without us
    int m_effects;            // effect groups this section (locked, hysteresis)
    int m_starCeil;           // hottest star allowed this section (drawn from the energy)
    int m_lastBeat;
    int m_calmUntil;          // beat until which the panic look holds
    QTimer m_fadeTimer;       // keeps fades ticking after a release
    bool m_logEnabled;
    QFile m_log;

    /* generated motion */
    QMap<QString, TrackMove> m_moves;      // this section's move per group
    QMap<QString, quint32> m_sweepFunc;    // head group -> its hidden relative EFX
    QMap<QString, TrackSweep> m_sweep;     // this section's figure per head group
    QMap<QString, TrackSweep> m_sweepShown; // what the EFX is configured to right now
    QMap<QString, qreal> m_pulseDepth;     // groups pulsing right now, and how deep
    QMap<QString, qint64> m_pulseStart;    // clock reading of their last pulse beat
    QMap<QString, int> m_breathe;          // groups on a slow sine, and over how many bars
    QElapsedTimer m_clock;
    qreal m_beatMs;
    qint64 m_beatStartMs;                  // clock reading of the last beat
    int m_beatIndex;                       // beats since the section started
    QString m_lastMoves;                   // what the report said, for the log
    QTimer m_pulseTimer;                   // 40 ms: the breath between two beats
    int m_room;
    bool m_roomAuto;
    int m_roomSent;                        // last percent handed to the ENERGY trim
    bool m_fullAuto;
    QSet<QString> m_flashHeld;             // strobe groups the generated flash lit
    bool m_hold;
    bool m_forceNext;
    QList<int> m_hitBeats;                 // beats that carried a hit, last 32 beats

    /* what is running: slot name -> fid, and its attribute override */
    QMap<QString, quint32> m_active;
    QMap<QString, int> m_activeAttr;
    QMap<QString, qreal> m_activeLevel;
    QMap<quint32, int> m_fadeAttr;
    QMap<quint32, qreal> m_fadeLevel;
};

#endif // TRACKENGINE_H
