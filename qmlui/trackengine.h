/*
  Q Light Controller Plus
  trackengine.h

  The automatic busker behind the Track page.

  A palette and a cast, not five roles running at once:

    * ONE colour at a time, put on every lit group through the scenes that
      share its name (Lasergreen + AniGreen + StrobesGreen ...). It changes
      only when a break or a drop starts, or every 32 bars. Drops may add one
      accent colour on a single group. Never three.

    * An ENERGY-budgeted CAST: the base plus up to three effects in a
      groove or four in a drop; breaks normally use the base alone.
      Groups outside the cast sit at zero.

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
#include "trackstagepolicy.h"
#include <QVariantList>
#include <QPoint>
#include <QStringList>
#include <QVector>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QHash>
#include <QList>
#include <QFile>
#include <QByteArray>
#include <QMap>
#include <QSet>

class Function;
class Fixture;
class Scene;
class Doc;
class QSettings;
class QRandomGenerator;

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
#define SETTINGS_ENGINE_HOLDAUTO  QStringLiteral("trackengine/holdauto")  // runde 189: the fader picks the hold
#define SETTINGS_ENGINE_CLOCKCURVE QStringLiteral("trackengine/clockcurve")   // "0,0,20,45,70,85": 21,22,23,00,01,02 h
#define ENGINE_COOLDOWN_MS        (12 * 60 * 1000)   // a programme that ran is drawn again reluctantly for this long
// The ceiling on WHITE on a strobe - 70 % of full (Tobias, 2026-09-22).
// Those lamps at a full white are painful to stand in front of, and it
// applies whether the white comes from a real white channel or is made from
// red, green and blue on the two models here that have no white channel.
#define ENGINE_STROBE_WHITE       178
#define SETTINGS_ENGINE_BASE      QStringLiteral("trackengine/base")
#define SETTINGS_ENGINE_LOG       QStringLiteral("trackengine/log")
#define SETTINGS_ENGINE_STARS     QStringLiteral("trackengine/stars")
#define SETTINGS_ENGINE_FULLAUTO  QStringLiteral("trackengine/fullauto")
#define SETTINGS_ENGINE_RATING    QStringLiteral("trackengine/rating")
#define SETTINGS_ENGINE_BANNED    QStringLiteral("trackengine/banned")
#define SETTINGS_ENGINE_AUTORATING QStringLiteral("trackengine/auto-rating-v1")
#define SETTINGS_ENGINE_RATINGON  QStringLiteral("trackengine/ratingon")
#define SETTINGS_ENGINE_SEEN      QStringLiteral("trackengine/seen")
// runde 184: what one night remembers across a restart (see nightKey())
#define SETTINGS_ENGINE_NIGHT     QStringLiteral("trackengine/night")
#define SETTINGS_ENGINE_ROOMAUTO  QStringLiteral("trackengine/roomauto")
#define SETTINGS_ENGINE_GROUPTRIM QStringLiteral("trackengine/grouptrim")

/** Beats of stage time that count as one "showing". A section is 32-64 beats,
 *  so 64 is roughly "it was up for a section". Exposure is measured in these,
 *  because a verdict has to be read against how much of the night the program
 *  was actually on: three thumbs down on something that ran all night is
 *  nothing, three on something that appeared twice is a verdict. */
#define ENGINE_RATE_EXPOSURE 64.0

/** What one thumb UP is worth against one thumb DOWN.
 *
 *  They are not the same act. A look he likes simply plays on - there is
 *  nothing to do, and no reason to reach for the screen. A look he dislikes
 *  is in his face until he changes it, so a thumb down is the reflex and a
 *  thumb up is a deliberate detour. Counting them one for one would read the
 *  rare deliberate act as the weaker signal, which is backwards. */
#define ENGINE_RATE_UPVOTE 3.0

/** What an AIMED verdict is worth against a spread one.
 *
 *  A tap on a thumb lands on every program on stage and is a guess about
 *  which of them was the point. A long press followed by a tap on a group is
 *  not a guess - he stopped, looked at the list and pointed. It costs him a
 *  second he does not always have, so when he spends it, it should be worth
 *  spending. Three spread taps' worth. */
#define ENGINE_RATE_AIMED 3

/** Which bucket a verdict lands in. A look that is wrong in a break can be
 *  the best thing in the room on a drop, so one number per program would
 *  average the two into "meh". Four is the same split the engine already
 *  picks by, so nothing new has to be decided. */
#define ENGINE_RATE_BREAK   0
#define ENGINE_RATE_BUILD   1
#define ENGINE_RATE_DROP    2
#define ENGINE_RATE_NORMAL  3
#define ENGINE_RATE_BUCKETS 4

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
    bool setsColour = true;   // writes a colour channel of its own (a colourless
                              // dimmer chase does not, and may run under any colour)
    bool aims = false;        // writes a pan or tilt channel in some step: it steers
                              // the heads itself, so the engine's sweep steps aside.
                              // A dimmer walk does not, and the sweep runs under it.
    QString family;           // the figure, not the name: "Row", "Eyes", "Span" ...
    quint32 scatter = 0;      // nameScatter(name): the ORDER candidates() sorts by.
                              // Not the id - see candidates() for why that was
                              // the reason every section looked like the last
    bool coversColour = false; // paints a colour on EVERY fixture of its group, in
                              // every step - so the group's own colour scene under
                              // it is not just redundant, it is HTP-added on top
    bool frozen = false;      // a chaser that can never step: nothing to run
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
    qreal litShare = 1.0;     // how much of what it touches is lit, averaged over its steps

    /* ---- the operator's verdict, per section kind ---- */
    /** Verdict POINTS, not taps. A spread thumb is worth 1, an aimed one
     *  (long press onto a group) ENGINE_RATE_AIMED. The distinction lives
     *  here rather than in a fifth counter, so nothing downstream - not the
     *  weight, not the SETUP row, not rebuild_ratings.py - has to know which
     *  kind a verdict was. The cost is that "5" no longer means five taps,
     *  which is why it says so on this line. */
    int up[ENGINE_RATE_BUCKETS] = { 0, 0, 0, 0 };
    int down[ENGINE_RATE_BUCKETS] = { 0, 0, 0, 0 };
    /** Beats this program has spent on stage, per section kind. The
     *  denominator: a verdict counts for as much as the program is rare. */
    int seen[ENGINE_RATE_BUCKETS] = { 0, 0, 0, 0 };
    /** Never again, whatever the counts say. A hard flag on purpose: it is
     *  the one thing the operator wants to be certain of, and it must not be
     *  at the mercy of a score that can drift back up on one good night. */
    bool banned = false;
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
    int subSteps = 1;         // pattern steps per beat: 1, 2 = eighths, 4 = sixteenths (stepBeats 1 only)
    qreal texture = 0.0;      // per-fixture level spread among the lit ones, 0..0.3 - a flat group looks static
    bool bare = false;        // strobes: blink one at a time with nothing lit behind them
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
    int dx = 0;               // aim jitter: the figure's centre off the aimed position, pan / tilt
    int dy = 0;
    // What the figure was drawn FOR, so applySweep() can rescale it live:
    // width, height and beats above are the size and pace at `drawnE`; the
    // fader is read every beat and the running EFX is stretched or shrunk
    // by the ratio of the tier's reach and pace curves at the two energies.
    // Until 2026-09-18 a figure kept the size it was born with for the whole
    // section, and the fader did nothing visible until the next one.
    int tier = 1;             // 0 break, 1 groove / build, 2 drop
    bool drive = false;       // a groove high in the track's own range: the
                              // curves are read half way towards the drop's
    qreal drawnE = 0.5;       // the energy the size and pace were drawn at
    bool operator==(const TrackSweep &o) const
    {
        return shape == o.shape && width == o.width && height == o.height && rotation == o.rotation
            && beats == o.beats && spread == o.spread && mirror == o.mirror && fan == o.fan
            && fx == o.fx && fy == o.fy && dx == o.dx && dy == o.dy;
    }
};

/** The deterministic half of drawSweep()'s size and pace - the part the
 *  fader decides - kept out here so applySweep() can follow the fader live
 *  with the same curves. The dice multiply on top, once, at draw time.
 *  Reach is pan units (0..127 is the whole travel); pace is beats per figure. */
inline qreal sweepReach(int tier, qreal e, bool drive = false)
{
    // A straight line from bottom to top in every tier, and a long one: at
    // the bottom of the fader a groove figure is a nudge of 10 units, at the
    // top it is 44 - more than four times the travel - and a drop goes from
    // 14 to 60, nearly half the pan range either side of the aim. It used
    // to be 16..30 and 24..42 with a 0.5..1.0 dice on top, so half a fader
    // moved the figure less than the dice did (Tobias, 2026-09-18: "det er
    // svaert at se den store forskel paa energi-slideren").
    e = qBound(0.0, e, 1.0);
    if (tier == 0)
        return 30.0 + 22.0 * e;              // a break: big and very slow, as before
    if (tier == 2)
        return 14.0 + 46.0 * e;
    // A DRIVE sits half way between the groove and the drop - at BOTH ends.
    // Raising the groove floor to 20 (below) without moving this left a drive
    // and a groove identical at the bottom of the fader; 17 -> 52 is the real
    // midpoint of groove 20 -> 44 and drop 14 -> 60.
    if (drive)
        return 17.0 + 35.0 * e;
    // A groove starts at TWENTY units, not ten. Tobias, 2026-09-20: "synes
    // ikke der er nok bevaegelse paa movingheads foer man kommer op i 100 %
    // energi". The straight line is what makes the fader readable, but the
    // bottom of it has to be a figure you can SEE: ten units is a nudge,
    // twenty is a movement. The top is unchanged.
    return 20.0 + 24.0 * e;
}

inline qreal sweepPace(int tier, qreal e, bool drive = false)
{
    // beats per figure. A drop at the top of the fader draws a whole circle
    // in four beats - one bar - which is as quick as a head this size can be
    // asked to travel a figure this big and still read as a figure.
    e = qBound(0.0, e, 1.0);
    if (tier == 0)
        return 128.0 - 96.0 * e;             // a break: a minute down to a quarter of it
    if (tier == 2)
        return 24.0 - 20.0 * e;              // a drop: 24 -> 4
    if (drive)
        return 26.0 - 20.0 * e;              // a drive: 26 -> 6, between 28 -> 8 and 24 -> 4
    // ... and quicker at the bottom too: 28 beats a figure, not 40. Same
    // reason - a 20-unit figure over 40 beats still reads as a still head.
    return 28.0 - 20.0 * e;                  // a groove: 28 -> 8
}

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
    /* where these heads really point, learned from 'TRACK Home: <group>' and
     * 'TRACK Home B: <group>' scenes (lr_import.py writes them from a Light
     * Rider show). home is the aim the operator lives in; the line home->homeB
     * is a direction the operator has already used, so it is safe to move
     * along. Empty: the engine falls back to the middle of the range. */
    QMap<quint32, QPoint> home;    // fixture -> pan/tilt of its home aim
    QMap<quint32, QPoint> homeB;   // fixture -> a second aim of the operator's
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
    /** The ENERGY fader picks how long a colour holds - 64, 32, 16 or 8 bars
     *  over its four quarters (Tobias, 2026-09-23). A holdBars tile sets a
     *  fixed hold instead and turns this off; tapping it again turns it on. */
    Q_PROPERTY(bool holdAuto READ holdAuto WRITE setHoldAuto NOTIFY tableChanged)
    /** ENERGY by clock: percent at 21, 22, 23, 00, 01 and 02 h (flat to 05, then 0). */
    Q_PROPERTY(QVariantList clockCurve READ clockCurve NOTIFY tableChanged)

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
    /** SETUP > SELF TEST is running: every group's colour scenes, 2 s each. */
    Q_PROPERTY(bool testing READ testing NOTIFY liveChanged)

    Q_PROPERTY(QStringList warnings READ warnings NOTIFY liveChanged)
    Q_PROPERTY(int calmBarsLeft READ calmBarsLeft NOTIFY liveChanged)
    Q_PROPERTY(bool logEnabled READ logEnabled WRITE setLogEnabled NOTIFY tableChanged)
    /** Whether the thumbs steer what the engine picks. OFF by default: the
     *  verdicts are still recorded, they just do not count, so a night can
     *  never go wrong because of a score nobody has looked at yet. */
    Q_PROPERTY(bool ratingEnabled READ ratingEnabled WRITE setRatingEnabled NOTIFY tableChanged)

    /** Kept for scripts: 0 empty, 1 warming, 2 full, 3 peak - a preset for
     *  the ENERGY slider. The page itself only has the slider. */
    Q_PROPERTY(int room READ room WRITE setRoom NOTIFY liveChanged)
    /** Let the clock move the ENERGY slider through the night. A hand on
     *  the slider turns it off. */
    Q_PROPERTY(bool roomAuto READ roomAuto WRITE setRoomAuto NOTIFY liveChanged)
    /** The evening's opening picture: the IDLE functions (the START scene)
     *  held on their own, with the engine standing still. The first SHOW ON
     *  of the night with ENERGY at 0 puts it up, and ENERGY above 0 takes
     *  that one down again (TrackManager, runde 184); by hand it stays until
     *  the tile is tapped again or the show is switched. */
    Q_PROPERTY(bool startScene READ startScene WRITE setStartScene NOTIFY liveChanged)

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
    /** Run every enabled group's colour scenes one after the other, two
     *  seconds each, without a track - so "the bars do not light" is seen
     *  on a Monday in SETUP and not on a Saturday at 23:00. A second tap,
     *  or a track starting, stops it. */
    Q_INVOKABLE void selfTest();
    bool testing() const;
    void testDark();
    /** kill a pending or running bar echo - AUTO off, blackout, a new track */
    void stopEcho();

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
    bool holdAuto() const;
    void setHoldAuto(bool on);
    QVariantList clockCurve() const;
    /** one of the six clock points, in steps of ten: 0 -> 10 -> ... -> 90 -> 0 */
    Q_INVOKABLE void cycleClockPoint(int index);
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
    /** The evening's opening picture, held by hand. */
    bool startScene() const;
    void setStartScene(bool on);
    /** Draw the opening picture: the IDLE functions for the aim, every group
     *  lit in one colour, no motion. Follows the colour tiles, MASTER and the
     *  cast faders live. */
    void startLook();
    /** A trim on the opening picture, 1.0 by default: MASTER and the cast
     *  faders are what the operator turns. */
    Q_PROPERTY(qreal startLevel READ startLevel WRITE setStartLevel NOTIFY liveChanged)
    qreal startLevel() const;
    void setStartLevel(qreal level);

    /** Panic: base group only, one colour, no motion, for this many bars. */
    Q_INVOKABLE void calm(int bars);
    int calmBarsLeft() const;
    /** New colour, new cast, new moves - now. */
    Q_INVOKABLE void next();

    /** Called the moment a finger lands on a thumb: freezes what is on stage
     *  so the verdict that follows - however long it takes him to aim it -
     *  lands on what he was actually looking at. */
    Q_INVOKABLE void markVerdictPoint();

    /** The operator's verdict on what is on stage right now: +1 or -1.
     *  Counted once per program on stage (up[]/down[] per section bucket),
     *  written to the tracklog when the log is on, and - only with
     *  ratingEnabled - weighed by pickWeighted(). A -1 also calls next(). */
    Q_INVOKABLE void rate(int verdict);

    /** The same verdict, but on one group's program alone. A long press opens
     *  the list; this is what a tap in it does. */
    Q_INVOKABLE void rateGroup(int verdict, const QString &group);

    /** What is on stage right now, one entry per group that has a look:
     *  { group, name }. For the long-press list - the operator points at a
     *  group, not at a function id. */
    Q_INVOKABLE QVariantList onStage() const;

    /** m_active as it was when the finger landed, or the live one if that was
     *  too long ago. Everything a verdict touches goes through these two. */
    const QMap<QString, quint32> &verdictStage() const;
    int verdictBucket() const;

    /** Never pick this one again, whatever it scores. */
    Q_INVOKABLE void setBanned(quint32 fid, bool on);
    Q_INVOKABLE bool banned(quint32 fid) const;

    /** How often this one should come up, 1..3, from its verdicts in the
     *  section kind we are in. 1 when it is unrated or the switch is off,
     *  so the rotation is exactly what it is today. */
    int rateWeight(const TrackFuncInfo &info) const;
    /** break / build / drop / normal as an index into up[] and down[]. */
    int rateBucket() const;
    QMap<QString, QString> autoLookKeys(const QSet<QString> &cast, qreal energy) const;
    int autoLookWeight(const QMap<QString, QString> &keys, const QString &group) const;
    bool rateAutoLook(int verdict, const QString &group = QString());
    const QMap<QString, QString> &verdictAutoKeys() const;
    /** One from the shortlist, the cursor walking it as it always has - but
     *  the better-rated standing in it more than once. */
    quint32 pickWeighted(const QList<TrackFuncInfo *> &ok, int cursor) const;
    int room() const;
    void setRoom(int room);
    bool roomAuto() const;
    void setRoomAuto(bool on);
    /** minutes past 21:00 when the house closes: 03:00, 05:00 on New Year's night */
    static int closingMinutes();
    /** 1.0 all night, sliding to 0 over the last forty minutes before closing, 0 after */
    qreal closingCap() const;
    /** The ENERGY percent the clock last handed to TrackManager. */
    Q_INVOKABLE int roomPercent() const;
    /** ENERGY by the clock, a restaurant's night: 0 (still) until 22:00,
     *  20 % at 23:00, 45 % at midnight, 70 % at 01:00, 85 % from 02:00, back
     *  to 0 at 05:00 - a slow creep, not steps. The DJ pushes the slider when
     *  the floor actually opens. */
    int clockPercent() const;
    void loadClockCurve(const QSettings &settings);
    static int keyBiasOf(const QString &key);
    static QString dropStyleName(int style);
    void announceRoom();
    /** The laser bars measured against the ENERGY slider (0..1) NOW, with no
     *  beat to wait for: an aim the slider no longer allows goes home in the
     *  dark, and a figure stops under 40 %. TrackManager calls it on a fader
     *  move while no beats arrive (link lost, deck stopped). (runde 190) */
    void laserFaderCheck(qreal slider);
    /** Which night it is, for what is remembered across a restart: the
     *  date, turning at noon - 20:00 is a new evening, 01:00 is still
     *  last night. (runde 184) */
    static QString nightKey();
    /** ENERGY by clock and the group faders, stamped with nightKey(). */
    void saveNight() const;
    bool hold() const;
    void setHold(bool on);
    bool logEnabled() const;
    bool ratingEnabled() const;
    void setRatingEnabled(bool on);
    void setLogEnabled(bool on);

    /* ---- atmosphere: the hazer's fan and output, straight from two sliders ---- */
    bool hazeAvailable();
    qreal haze() const;
    void setHaze(qreal level);
    qreal fan() const;
    void setFan(qreal level);

    /* ---- driven by TrackManager ---- */
    /** kick / high: what the analysis heard on this beat, 0..1, or -1 when
     *  BLT did not send the curves. The pulse follows the kick; a hats-only
     *  passage sparkles.
     *  turn: this beat is a musical turn read in the curves (the kick comes
     *  back after a gap, a crash on the highs, the bass jumps) - colour and
     *  accent changes land on one of these rather than on a timer.
     *  riser: how far the highs have climbed over the last eight bars, 0..1
     *  - with a drop ahead, that is a build whatever the flag says.
     *  hats: the highs over the last two bars, 0..1 or -1 - the strobes are
     *  the hi-hats' lamps and sit out when there are none.
     *  bass: the lows over the last two bars, 0..1 or -1 - a heavy sub makes
     *  the pulse fall deeper between two beats, a thin one lighter. */
    void tick(const QString &state, int beat, int secStart, int secEnd,
              qreal energy, qreal sectionEnergy, int division, bool sectionChanged,
              const QString &nextState, int beatsToNext, qreal bpm, qreal levelScale,
              qreal kick = -1.0, qreal high = -1.0,
              bool turn = false, qreal riser = 0.0, qreal hats = -1.0,
              qreal bass = -1.0, qreal kickAhead = -1.0);
    /** $title is the track TrackManager just loaded. Defaulted so an
     *  un-patched trackmanager.cpp still compiles; the patch passes it. */
    void trackLoaded(const QString &title = QString(), const QString &key = QString());
    /** the key of the track on the other deck (BLT "next"), for the mix */
    /** SHOW ON, once: stop everything the Virtual Console started and have
     *  its Level sliders let go of their channels, so nothing left over from
     *  busking gets in the way of the show. Locks nothing. (Runde 166.) */
    void resetConsole();
    void setNextKey(const QString &key);
    void setIncomingProfile(const QString &title, const QString &state, qreal energy);
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
    void slotDocSettled();
    void slotFadeTimer();
    void slotPulseTimer();
    void slotSelfTestStep();
    void slotEchoOn();
    void slotEchoOff();


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
    void ensureStrobeScenes();
    void ensureOffScenes();
    void applyGroupOff();
    void driveStrobe(const QSet<QString> &cast, int beat, qreal energy, bool isDrop, bool isBuild,
                     qreal prog, int bar, int beatInBar, bool quiet);
    void learnHome();
    void ensurePositionScenes();
    void ensureSweeps();
    void ensureZoomScenes();
    QVector<qreal> patternMask(const QString &group, const TrackMove &move, int step, qreal prog) const;
    /** drive: a groove high in the track's own range - the size and pace curves
     *  are read half way towards the drop's. See tick(), isDrive. */
    TrackSweep drawSweep(int tier, bool build, qreal prog, qreal energy, int heads,
                         bool laser, bool drive = false) const;
    void applySweep(const QString &group, const TrackSweep &sweep, qreal bpm, qreal energy);
    QString sweepName(const TrackSweep &sweep) const;
    void stopSweeps();
    bool userAllowed(const TrackFuncInfo &info, const QString &group = QString()) const;
    void genFlash(bool on, const QString &colour = QString());
    quint32 dimmerChannel(Fixture *fxi) const;
    /** How much of `touched` a function lights, averaged over its steps: 1.0
     *  for a scene that lights all of them, 1/7 for a chase that walks one
     *  head of seven. Measured once, when the table is built. */
    qreal litShareOf(Function *func, const QSet<quint32> &touched) const;
    /** Does this function write a colour of its own? A chase that only moves
     *  dimmers takes whatever colour the room is in, so it can run under all
     *  of them; one that writes red is a red programme. */
    bool setsColourOf(Function *func) const;
    /** Does this function write a pan or tilt channel in any of its steps? */
    bool aimsOf(Function *func) const;
    bool coversColourOf(Function *func, const QSet<QString> &groups) const;
    int guessStars(const TrackFuncInfo &info) const;
    qreal stepBeats(const TrackFuncInfo &info, qreal bpm) const;
    int divisionFor(const TrackFuncInfo &info, qreal bpm, int division) const;
    void ensureAtmosScenes();
    void applyAtmos(quint32 sceneId, const QList<QPair<quint32, quint32> > &channels, qreal level);

    /* choosing */
    QList<TrackFuncInfo *> candidates(int role, const QString &group) const;
    quint32 colourFunction(const QString &group, const QString &colour) const;
    /** true when the group has a scene of its own in exactly this colour */
    bool groupHasColour(const QString &group, const QString &colour) const;
    /** The colour this group can actually show: the room's, or the nearest
     *  one it owns. A colour wheel has seven colours, the palette has more. */
    QString colourForGroup(const QString &group, const QString &colour) const;
    /** A hidden scene with colour a on the even eyes and b on the odd ones,
     *  for groups whose colour lives on per-eye channels. Made on demand. */
    quint32 splitColourFunction(const QString &group, const QString &a, const QString &b);
    quint32 motionFunction(const QString &group, const QString &colour,
                           const QSet<QString> &cast, int cursor) const;
    quint32 motionFor(const QString &group, const QString &colour,
                      const QSet<QString> &cast, int cursor, int tier,
                      qreal bpm, int division, bool staticOnly, int maxStars,
                      qreal litFloor = 0.0) const;
    /** fader: the slider as the operator reads it (tick() recovers it), not the section-scaled energy */
    quint32 positionFunction(const QString &group, int cursor, int tier, qreal fader) const;
    /** True if this scene switches a fixture's own effect/movement macro on. */
    bool macroPosition(quint32 fid) const;
    /** A laser aim (scene or chaser) that never leaves the group's home aim
     *  by more than ENGINE_AIM_REACH, and never writes anything but pan and
     *  tilt. Safe to run without the operator having promised it by name. */
    /** downAllowed: how far BELOW the home aim (a bigger tilt value) a step
     *  may go, in units; -1 = the same reach as upward. */
    bool laserAimSafe(quint32 fid, const QString &group, int downAllowed = -1) const;
    /** How far below the home aim the laser bars may point at this energy:
     *  nothing under 60 %, then a straight line up to ENGINE_AIM_REACH at 100 %. */
    static int laserDownAllowed(qreal energy);
    /** The same question for an EFX: does its tilt travel stay within the
     *  reach upward and within downAllowed downward, on every bar it drives? */
    bool laserSweepSafe(quint32 fid, const QString &group, int downAllowed) const;
    quint32 homePosition(const QString &group) const;
    quint32 flashFunction(const QSet<QString> &cast, const QString &colour) const;
    int tierOf(const QString &text) const;
    /** The FIGURE a programme belongs to - "Row", "Eyes", "Span" ...
        Two names from the same family look the same on the rig. */
    static QString familyOf(const QString &name);
    QString firstRoomColour() const;      // the palette's first entry that is not white
    QString accentFor(const QString &colour, bool allowWhite) const;
    QString drawColour(const QStringList &pool, int keyBias, QRandomGenerator *rng) const;
    qreal tempoScore(const TrackFuncInfo &info, qreal bpm) const;
    void checkConflicts(const QSet<QString> &cast);
    void logBeat(const QString &state, int beat, qreal level, qreal energy, qreal sectionEnergy);
    QByteArray logSettings() const;
    TrackMove composeMove(const QString &group, TrackMove move, int tier) const;
    /** A marker line in the tracklog at the numbers of the last beat: the
     *  operator did something. Same columns as a beat, so one parser reads
     *  both, and the funcs column already says what was on stage. */
    void logSignal(const QString &tag);

    /* running */
    void run(const QString &slot, quint32 fid, qreal level, int division, bool hard);
    void startFunction(Function *func, int division);
    void stopSlot(const QString &slot, bool hard);
    void tickFades();
    void setDimmer(const QString &group, qreal level);

    /* generated motion */
    TrackMove drawMove(const QString &group, int tier, bool build, qreal energy, bool isBase,
                       qreal prog = 0.0) const;
    void applyMove(const QString &group, qreal level, int beat, int secStart, qreal prog,
                   const TrackMove &move, bool patterned);
    void setPart(const QString &group, int index, qreal level);
    QString partSlot(const QString &group, int index) const;
    QString slotGroup(const QString &slot) const;
    bool lightsGroup(quint32 fid, const QString &group) const;
    qreal slotScale(const QString &slot, quint32 fid) const;
    void reapplyLevels();
    qreal pulseFactor(const QString &group) const;
    bool ambientBase(const QString &group) const;
    QString moveName(const TrackMove &move) const;

private:
    Doc *m_doc;

    bool m_dirty;
    bool m_building;          // ensureTable() is mid-rebuild: do not re-enter
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
    bool m_holdAuto = true;   // runde 189: the fader picks the hold (64/32/16/8 bars)
    qreal m_holdStretch = 1.0;  // this colour's random stretch of it, drawn at each change
    QString m_accentPick;     // the accent drawn for this section
    QString m_accentGroup;    // the group carrying it - rotates, never the same twice running
    int m_keyBias;            // this track's key: -1 unknown, 0 minor (cold side), 1 major (warm side)
    int m_nextKeyBias;        // the next track's, from BLT "next"
    QString m_nextColour;     // drawn when a mix begins: the colour the incoming track arrives in
    QList<int> m_clockCurve;  // six percents, see SETTINGS_ENGINE_CLOCKCURVE
    QHash<quint32, qint64> m_recentUse;   // programme -> clock ms it last ran (the cooldown)
    qint64 m_cooldownMs;      // the clock reading the cooldown is judged against: frozen per section
    bool m_accentWasWhite;    // the last accent was white: the next one is not
    bool m_hatsOut;           // the strobes sit out: no hi-hats in the music right now
    bool m_curveBreak = false;   // runde 192: a groove flag with no kick in the next two bars plays as a break
    bool m_curveGroove = false;  // runde 192: a break flag with a solid kick in the next two bars plays as a groove
    bool m_barsLead;          // this section: the laser bars take the first effect place
    int m_castCursor;
    int m_motionCursor;
    QSet<QString> m_cast;
    QMap<QString, quint32> m_position;   // sticky position pick per group
    // the beat each group's aim was last set on. A guard, nothing else:
    // without it a section change could swing a head that had just moved
    // (runde 158). Cleared with the rest in trackLoaded().
    QMap<QString, int> m_aimSince;
    QHash<QString, int> m_darkUntil;     // group -> last beat of a planned dark stretch
    qreal m_master;
    bool m_blackout;
    bool m_mixing;
    int m_mixBeat;                        // the beat a mix began DURING this track (-1: none) - the mix-out fade counts from it
    QMap<QString, quint32> m_splitScenes;  // "group|a|b" -> hidden two-colour scene
    int m_speed;                          // -1 half, 0 as the music, +1 double
    qreal m_faderNow = 0.0;               // this beat's FADER (the slider, before the section scaled it),
                                          // for the hard thresholds and the paths tick() does not hand it to
    QMap<QString, qreal> m_groupTrim;     // the DJ's fader per group, 1.0 when untouched
    bool m_flash;
    QString m_lastState;
    QString m_report;
    QStringList m_warnings;
    QMap<QString, int> m_conflictBeats;   // how long a group has read lit while held dark
    QMap<quint32, int> m_lastPan;         // last pan reading per head, for the Light Rider check
    QMap<QString, int> m_headMoveBeats;   // beats in a row a head group moved without us
    int m_effects;            // effect groups this section (locked, hysteresis)
    int m_effectsBefore;      // what the section before a build had
    // the beat the cast budget last moved. A dwell, so the analysis curve
    // cannot drift a group on and off stage every bar (runde 159). In-class
    // default, deliberately: out of the constructor's list, out of -Wreorder.
    int m_effectsBeat = -1;
    int m_starCeil;           // hottest star allowed this section (drawn from the energy)
    int m_lastBeat;
    int m_calmUntil;          // beat until which the panic look holds
    QTimer m_fadeTimer;       // keeps fades ticking after a release
    QTimer m_docTimer;        // coalesces a burst of document signals into one rebuild
    bool m_logEnabled;
    QFile m_log;
    QByteArray m_logSettingsLast;
    QString m_logSettingsId;
    // The context of the last line written, so a rating can be logged with the
    // same numbers the beat had. Inline-initialised on purpose: added to the
    // constructor's list they would have to sit in declaration order too, and
    // -Wreorder is an error in CI.
    QString m_trackTitle;     // what is playing, for the log's track column
    QHash<QString, int> m_trimLogged;   // group -> beat: one trim line per beat
    bool m_ratingOn = false;            // do the verdicts count? OFF by default
    /** The section the look on stage was CHOSEN for. Not the same as
     *  m_lastState once HOLD is on: the look freezes, the track does not. */
    QString m_lookState;

    /* ---- what was on stage when the finger went down ----
     * A verdict has to land on what he was looking at when he pressed, not on
     * what is there when he lets go. Between the two: a long press is 800 ms,
     * picking a group in the list takes a second or two more, and a thumb
     * down now changes the look immediately - so by the time the verdict is
     * cast, the stage can easily be showing something else entirely. */
    QMap<QString, quint32> m_verdictActive;
    int m_verdictBucket = -1;
    qint64 m_verdictMs = -1;
    int   m_logBeatNo = 0;
    qreal m_logLevel = 0.0;
    qreal m_logEnergy = 0.0;
    qreal m_logSection = 0.0;

    /* generated motion */
    QMap<QString, TrackMove> m_moves;      // this section's move per group
    qreal m_movesEnergy = -1.0;            // the energy the moves were last drawn at (a fader jump redraws)
    QString m_rhythmLead;                 // one leading effect, other groups support it
    QString m_compositionBase;
    TrackStage::Exposure m_exposure;
    int m_restUntil = -1;
    QStringList m_sequenceGroups;
    qint64 m_sequenceStart = -1;
    qreal m_sequenceBeatMs = 500.0;
    qint64 m_sequenceLast = -60000;
    QString m_incomingTitle, m_incomingState;
    qreal m_incomingEnergy = -1.0;
    qint64 m_incomingAt = -1;
    qreal m_mixMotionScale = 1.0;
    int m_compositionTier = 0;
    int m_dropLand = 0;                    // FAKE DROP: the bar of the drop the kick actually arrived on
    qreal m_castEnergy = -1.0;             // the energy the cast size was last decided at (a nudge steps it)
    qreal m_ceilEnergy = -1.0;             // the energy the star ceiling was last drawn at (a nudge redraws it)
    QSet<QString> m_blendSkipped;         // functions left out for building on a mask
    QMap<QString, quint32> m_sweepFunc;    // head group -> its hidden relative EFX
    QMap<QString, TrackSweep> m_sweep;     // this section's figure per head group
    QMap<QString, TrackSweep> m_sweepShown; // what the EFX is configured to right now
    QMap<QString, QList<int> > m_moveHistory;   // the last patterns per group - not again
    QMap<QString, QList<int> > m_sweepHistory;  // the last figures per head group
    int m_fillUntil = -1;
    int m_fillLast = -8;
    QMap<QString, QPair<int, int> > m_autoRatings; // stable recipe -> up/down; never scene IDs
    QMap<QString, QString> m_autoStageKeys;
    QMap<QString, QString> m_verdictAutoKeys;
    QMap<QString, TrackMove> m_liveMove;   // the move as shaped for this beat (build, turnaround)
    QMap<QString, qreal> m_moveLevel;      // the level applyMove last gave a group (sub-beat steps)
    QMap<QString, bool> m_patterned;       // whether that group's pattern is live (sub-beat steps)
    QSet<QString> m_motionDim;             // groups whose MOTION owns the dimmers this beat
    QMap<QString, QVector<qreal> > m_texture;  // per-fixture spread, drifting slowly
    QMap<QString, QList<quint32> > m_zoomScenes; // head group -> narrow, mid, wide
    QMap<QString, int> m_zoom;             // the zoom pick per group, -1 none
    int m_dropStyle;          // this drop's character: 0 none, 1 hard, 2 wide, 3 tight, 4 heavy, 5 nervous
    int m_kickGone;           // beats in a row the analysis heard no kick (0 without curves)
    int m_kickBeat;           // the beat it last counted - tick() can run twice on one beat
    QTimer m_echoTimer;       // the bars answer a hit half a beat later ...
    QTimer m_echoOffTimer;    // ... and let go a third of a beat after that
    QString m_echoKey;        // the laser group that answers
    quint32 m_echoFid;        // in this colour scene
    int m_echoBeat;           // the beat of the last echo - at most one every four
    QHash<QString, quint32> m_offScenes;              // group -> the scene that forces it to zero
    QHash<QString, QList<quint32> > m_strobeScenes;   // group -> a scene per rate, slow to fast
    int m_strobeUntil;        // the beat the burst ends on (-1: not strobing)
    int m_strobeSeen;         // the beat driveStrobe last saw, to catch a scrub
    int m_strobeRate;         // which of the rates is up
    int m_strobeWindow;       // first beat of the current 64-beat strobe budget window (-1: none)
    int m_strobeSpent;        // strobe beats used in that window
    QMap<QString, qreal> m_pulseDepth;     // groups pulsing right now, and how deep
    QMap<QString, qint64> m_pulseStart;    // clock reading of their last pulse beat
    QMap<QString, qreal> m_pulseStrength;  // how hard that beat hit, 0.5..1 - the kick's say
    QMap<QString, int> m_subStepSeen;      // the last sub-step the pulse timer masked, per group
    QMap<QString, int> m_breathe;          // groups on a slow sine, and over how many bars
    QElapsedTimer m_clock;
    qreal m_beatMs;
    qint64 m_beatStartMs;                  // clock reading of the last beat
    int m_beatIndex;                       // beats since the section started
    QString m_lastMoves;                   // what the report said, for the log
    QTimer m_pulseTimer;                   // 40 ms: the breath between two beats
    QTimer m_testTimer;                    // SELF TEST: one colour scene every 2 s
    QList<quint32> m_testSteps;            // the scenes it walks through
    QStringList m_testGroups;              // ... and the group each belongs to
    QStringList m_testLabels;    // "group / colour" for the report
    QStringList m_testSkipped;   // groups with no colour scene to test
    int m_testIndex;
    QHash<QString, quint32> m_sectionMotion; // group -> the motion it holds for this section
    QHash<QString, QString> m_lastFamily;  // group -> the figure it showed last section
    QString m_logAccent;                   // "group=colour" of this beat's accent, for the log
    QString m_logEvent;                    // what moved this beat: turn, colour, section - for the log
    QMap<QString, int> m_turnCursor;       // pattern devices: extra draw offset, bumped on a turn
    QMap<QString, int> m_turnBeat;         // ... and the beat it was last bumped on
    int m_room;
    bool m_roomAuto;
    int m_roomSent;                        // last percent handed to the ENERGY trim
    bool m_fullAuto;
    QSet<QString> m_flashHeld;             // strobe groups the generated flash lit
    bool m_hold;
    bool m_startScene;        // the opening picture is held by hand
    qreal m_startLevel;       // a trim on the opening picture
    bool m_startColour;       // the opening picture chose the colour, not the DJ
    bool m_forceNext;
    QList<int> m_hitBeats;                 // beats that carried a hit, last 32 beats

    /* what is running: slot name -> fid, and its attribute override */
    QMap<QString, quint32> m_active;
    QMap<QString, int> m_activeAttr;
    QMap<QString, qreal> m_activeLevel;
    QMap<QString, qreal> m_activeOut;      // what was actually written (pulse, trim, master, blackout applied)
    QMap<quint32, int> m_fadeAttr;
    QMap<quint32, qreal> m_fadeLevel;
};

#endif // TRACKENGINE_H
