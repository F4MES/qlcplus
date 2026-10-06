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
#include "scenevalue.h"      // B23 H3
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
#define SETTINGS_ENGINE_PUNCH     QStringLiteral("trackengine/punchdrops")   // runde 322: kick/bass of the last 60 DROPS (317/318: "trackengine/punch", per track - not read)
#define SETTINGS_ENGINE_HOLDBARS  QStringLiteral("trackengine/holdbars")
#define SETTINGS_ENGINE_HOLDAUTO  QStringLiteral("trackengine/holdauto")  // runde 189: the fader picks the hold
#define SETTINGS_ENGINE_CLOCKCURVE QStringLiteral("trackengine/clockcurve")   // 28 percents: 20:00, 20:15 ... 02:45 (runde 211; six hourly ones before, still read)
#define ENGINE_CLOCK_POINTS 28
// runde 211 (Tobias): the closing sequence - the last five minutes to closing
// time bring the room down to dark, and the hazer off - can be switched off
#define SETTINGS_ENGINE_CLOSING   QStringLiteral("trackengine/closing")
#define ENGINE_CLOSING_SECS       300
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
    QString partner;          // runde 243: the partner of a two-colour programme
                              // (its name's tag: Fire, Ice, Deep ...), or empty
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
    qreal minLit = 0.0;       // the lowest master dimmer any step leaves any of its lamps
    qreal peakLit = 0.0;      // runde 259: the BRIGHTEST lamp in its darkest step - "at
                              // least one lamp on" (Tobias), 0..1
    qreal beamShare = 1.0;    // runde 338: litShare counted in BEAMS - a laser bar's eight
                              // eyes each count, so "one eye per bar" is 1/8, not 1
    bool fullBank = false;    // runde 338: some step has 3/4 or more of the lamps it touches
                              // at 200+ on their dimmer (3 lamps or more) - a bank at full
                              // at (0..1); 0 = can promise nothing (runde 231)

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
    int width = 1;            // chase / pingpong: how many neighbours are lit together (runde 338)
    qreal drawnE = -1.0;      // the energy this move was drawn at (-1 = m_movesEnergy's) - runde 264
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
    // The draw's FLOOR on the pace, kept apart from `beats` (runde 271).
    // `beats` is the curve's value (sweepPace x dice) and is what the live
    // ratio in applySweep() scales; the floors - FULL AUTO's 16/24 beats on
    // the heads, a drop style's 8 or 16 - are applied after the scaling. They
    // used to be written INTO `beats`, and the ratio then scaled the floor:
    // a base drawn at 100 % in a drop (curve 4, floor 16) pulled to 81 %
    // ran 16 x 1.95 = 31 beats, and the next draw at 81 % gave 16 again - a
    // sawtooth on the heads' pace; pushed from 60 to 79 % it quickened to 12,
    // under its own floor. The closing's slow fall sped the support heads up
    // at every redraw.
    int minBeats = 3;
    // FULL AUTO's role floor (runde 290, Tobias: "alt skal skalere efter
    // energi-slideren"): 0 none, 1 the base, 2 a support group. applySweep()
    // reads the floor off the SLIDER on every beat - the base 16 -> 8 beats a
    // figure over 50-100 %, support 24 -> 12 over 40-100 %. It was a flat 16 /
    // 24 frozen at the draw: a drop's base at 40 % and at 100 % ran the same
    // 16-beat figure, and the support heads ran 24 beats at every fader.
    int paceRole = 0;
    // the SLIDER a laser bar figure was drawn at (runde 293): its height
    // follows the slider live from there (applySweep), -1 = not a bar figure
    qreal drawnF = -1.0;
    bool operator==(const TrackSweep &o) const
    {
        return shape == o.shape && width == o.width && height == o.height && rotation == o.rotation
            && beats == o.beats && spread == o.spread && mirror == o.mirror && fan == o.fan
            && fx == o.fx && fy == o.fy && dx == o.dx && dy == o.dy && minBeats == o.minBeats
            && paceRole == o.paceRole;
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
    /** The ENERGY fader picks how long a colour holds - gliding from 64 bars
     *  at the bottom to 8 at the top, through 64/32/16/8 at the middle of each
     *  quarter (Tobias, 2026-09-23; smooth since runde 291). A holdBars tile
     *  sets a fixed hold instead and turns this off; tapping it again turns
     *  it on. */
    Q_PROPERTY(bool holdAuto READ holdAuto WRITE setHoldAuto NOTIFY tableChanged)
    /** ENERGY by clock: percent at 21, 22, 23, 00, 01 and 02 h (flat to 05, then 0). */
    Q_PROPERTY(QVariantList clockCurve READ clockCurve NOTIFY tableChanged)   // 28 points, 20:00..02:45 (r211)

    Q_PROPERTY(QString colourOverride READ colourOverride WRITE setColourOverride NOTIFY liveChanged)
    /** Runde 304 (Tobias): the colour tiles may be MORE than one - "trykker man
     *  paa blaa og lilla og saa bruger den begge 2 i mix". The chosen colours,
     *  in the order they were tapped; colourOverride is the one leading now. */
    Q_PROPERTY(QStringList colourOverrides READ colourOverrides NOTIFY liveChanged)
    /** R370_COLOUR_MODE (Tobias 10-06): 0 AUTO, 1 FADE, 2 CHASE - how the tiles
     *  in colourOverrides take turns. AUTO with two or more: the engine picks. */
    Q_PROPERTY(int colourMode READ colourMode NOTIFY liveChanged)
    /** the way the tiles take turns right now: 0 none, 1 fade, 2 chase */
    Q_PROPERTY(int colourStyle READ colourStyle NOTIFY liveChanged)
    /** R374_FADE_SHOWN: how far the base's fade is (0-1) and its two colours */
    Q_PROPERTY(qreal colourFadeT READ colourFadeT NOTIFY colourFadeChanged)
    Q_PROPERTY(QString colourFadeFrom READ colourFadeFrom NOTIFY colourFadeChanged)
    Q_PROPERTY(QString colourFadeTo READ colourFadeTo NOTIFY colourFadeChanged)
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
    Q_PROPERTY(bool closingSequence READ closingSequence WRITE setClosingSequence NOTIFY liveChanged)
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
    Q_INVOKABLE void toggleBase(QString key);
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
    QStringList colourOverrides() const;
    /** A tile tapped: in the set, it leaves it; not, it joins (runde 304). */
    Q_INVOKABLE void toggleColourOverride(const QString &colour);
    /** R370_COLOUR_MODE: FADE (1) or CHASE (2) with fewer than two tiles fills
     *  in the colour leading now and its partner; AUTO (0) keeps the tiles and
     *  hands fade/chase back to the engine (the AUTO tile clears them). */
    Q_INVOKABLE void setColourMode(int mode);
    int colourMode() const { return m_colourMode; }
    int colourStyle() const { return m_layerStyle; }
    qreal colourFadeT() const;
    QString colourFadeFrom() const;
    QString colourFadeTo() const;
    /** the tiles' partner for the lead at leadIdx (review 305) */
    QString setPartnerOf(int leadIdx) const;
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
    /** minutes past 20:00 when the house closes: 03:00, 05:00 on New Year's night */
    static int closingMinutes();
    bool closingSequence() const;
    void setClosingSequence(bool on);
    /** called every 200 ms by TrackManager: the closing sequence runs to the
     *  minute with or without beats (the light's dim, the hazer, the slider).
     *  showOn: with SHOW OFF the light is the operator's - nothing is dimmed */
    void closingTick(bool showOn);
    /** 1.0 all night, sliding to 0 over the last five minutes before closing (the
     *  closing sequence, runde 211), 0 after until 06:00 */
    qreal closingCap() const;
    qreal masterOut() const;
    /** The ENERGY percent the clock last handed to TrackManager. */
    Q_INVOKABLE int roomPercent() const;
    /** ENERGY by the clock: 28 quarter-hour points 20:00-02:45 (SETUP >
     *  ADVANCED), a straight line between them; by default still until 22:30,
     *  20 % at 23:00, 45 % at midnight, 70 % at 01:00, 85 % from 02:00. The DJ
     *  pushes the slider when the floor actually opens. */
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
              qreal bass = -1.0, qreal kickAhead = -1.0,
              int rawGap = -1, int rawQuiet = -1);
    // rawGap / rawQuiet (runde 324): from the RAW per-beat curves (BLT 323+),
    // -1 without them. rawGap 1: no kick on this beat nor the one before (a
    // fill, a drop-out); rawQuiet 1: neither kick nor highs on this beat.
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
    /** runde 317: how hard this track's kick and bass are in absolute terms
     *  (BLT's kickRef / lowRef, the numbers its curves were divided by) - the
     *  curves themselves are relative to the track, so a drop reads full in
     *  every track. -1: BLT did not send them. */
    void setTrackPunch(const QString &title, qreal kickRef, qreal lowRef);
    /** Runde 356 (R356_MUSIC_DARK): the track's own sound, beat by beat, as
     *  BLT measured it - the waveform's level (0-255, relative to the
     *  track's own top) and, when BLT sends them, the unsmoothed kick and
     *  highs. The engine reads silence ahead in it. */
    void setTrackAudio(const QVariantList &level, const QVariantList &kickRaw, const QVariantList &highRaw);
    /** Runde 357 (R357_FILLS): rekordbox' fill-ins, [first beat, end beat)
     *  per phrase that has one - the drum fill into the next phrase. */
    void setTrackFills(const QVariantList &fills);
    /** Runde 358 (R358_RHYTHM): per beat the mid band unsmoothed (0-255,
     *  the track's own 90th percentile = 255) and the highs' onset in each
     *  quarter of the beat, four nibbles (quarter q in bits 4q..4q+3, 0-15). */
    void setTrackRhythm(const QVariantList &midRaw, const QVariantList &onsetHigh);
    /** Runde 359 (R359_PUMP): per beat how far the sound falls between two
     *  kicks - 1 - (the quietest stretch after the first quarter of the beat
     *  / the beat's attack), 0-255. */
    void setTrackPump(const QVariantList &pumpRaw);
    /** Runde 358 (R358_BRAKE): the deck's effective tempo from every status
     *  BLT sends - a brake shows as the tempo falling away under the grid's. */
    void setDeckTempo(qreal bpm);
    void setDropForced(bool on) { m_dropForced = on; }   // R361_DROP_FORCED
    /** runde 322: one drop's absolute kick and bass into the history */
    void rememberDropPunch(qreal kick, qreal low);
    /** runde 335: the operator forced a section on the Track page (or let it
     *  go: state empty) - a sig: line in the tracklog, so a night's presses
     *  can be read as corrections of the analysis. Log only. */
    void noteSectionOverride(const QString &state, const QString &analysed);
    /** Nothing is playing but AUTO is on: run the start scene(s). */
    void idle();
    /** AUTO switched off: fade everything out over a bar, then let go. */
    void release();
    void stopAll();

signals:
    void tableChanged();
    void liveChanged();
    /** a colour tile was pressed but the colour cannot be used (not in the
     *  palette any more, banned): the tile blinks (runde 313, B27/Tobias) */
    void colourRejected(const QString &colour);
    void colourFadeChanged();            // runde 374: the FADE tile's progress
    /** ROOM in percent of energy (55 / 80 / 100 / 125): TrackManager puts it
     *  on the ENERGY trim, so ROOM and the ENERGY slider are one dial. */
    void roomChanged(int percent);

protected slots:
    void slotDocChanged();
    void slotDocSettled();
    /** runde 345: the start picture's watchdog (see the definition) */
    void slotFunctionStopped(quint32 fid);
    void slotStartWatch();
    void slotFadeTimer();
    void slotLayerTimer();               // R370_COLOUR_LAYER: the fade between two beats
    void slotPulseTimer();
    void slotChopTimer();                // runde 356: the stabs inside a beat
    void slotSelfTestStep();
    void slotEchoOn();
    void slotEchoOff();
    void slotBeatWatch();     // runde 302: the beat did not come - one-beat events end


protected:
    /* table building */
    void ensureTable();
    QSet<quint32> fixturesOf(Function *func, int depth) const;
    /** B23 H3: Scene::values(), copied once per scene while a build reads */
    QList<SceneValue> valuesOf(const Scene *scene) const;
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
    /** runde 352: the bars' move this section lights more than one eye at a time */
    bool barsWide(const QString &group) const;
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
    /** true when a running laser bar figure was lifted by 2 units or more
     *  (runde 303: the caller darkens the bars for that beat) */
    bool applySweep(const QString &group, const TrackSweep &sweep, qreal bpm, qreal energy);
    /** The floor under a figure's pace: the draw's minBeats and FULL AUTO's
     *  role floor off the slider (runde 290). */
    int sweepFloor(const TrackSweep &sweep) const;
    QString sweepName(const TrackSweep &sweep) const;
    void stopSweeps();
    bool userAllowed(const TrackFuncInfo &info, const QString &group = QString()) const;
    void genFlash(bool on, const QString &colour = QString());
    quint32 dimmerChannel(Fixture *fxi) const;
    /** How much of `touched` a function lights, averaged over its steps: 1.0
     *  for a scene that lights all of them, 1/7 for a chase that walks one
     *  head of seven. Measured once, when the table is built. */
    qreal litShareOf(Function *func, const QSet<quint32> &touched) const;
    qreal minLitOf(Function *func, const QSet<quint32> &touched) const;
    qreal peakLitOf(Function *func, const QSet<quint32> &touched) const;
    qreal beamShareOf(Function *func, const QSet<quint32> &touched) const;
    bool fullBankOf(Function *func, const QSet<quint32> &touched) const;
    bool canOwnDimmers(const TrackFuncInfo &info, bool onBase) const;
    bool baseCovered() const;                // runde 260: another lit lamp group runs beside the base
    /** Does this function write a colour of its own? A chase that only moves
     *  dimmers takes whatever colour the room is in, so it can run under all
     *  of them; one that writes red is a red programme. */
    bool setsColourOf(Function *func) const;
    /** Does this function write a pan or tilt channel in any of its steps? */
    bool aimsOf(Function *func) const;
    bool coversColourOf(Function *func, const QSet<QString> &groups) const;
    int guessStars(const TrackFuncInfo &info) const;
    qreal stepBeats(const TrackFuncInfo &info, qreal bpm) const;
    /** runde 313: the fastest a strobe chase may step, in beats, on the
     *  slider - 2 under 50 %, 1 from 50 %, an eighth from 75 % in a drop */
    qreal strobePaceFloor(int tier) const;
    int divisionFor(const TrackFuncInfo &info, qreal bpm, int division) const;
    void ensureAtmosScenes();
    void applyAtmos(quint32 sceneId, const QList<QPair<quint32, quint32> > &channels, qreal level);

    /* choosing */
    QList<TrackFuncInfo *> candidates(int role, const QString &group) const;
    /** candidates()' table-wide part, kept per (role, group) in m_candIndex
     *  until invalidateCandidates(). "dryp" and "climb" are read off the name
     *  here, once; candidateGate() says per call which of them may pass. */
    struct CandEntry { TrackFuncInfo *info = nullptr; bool dryp = false; bool climb = false; bool climbLong = false; };
    QList<CandEntry> buildCandidates(int role, const QString &group) const;
    /** bit 0: the fader holds "dryp" back; bit 1: a 16-beat climb fits the
     *  build; bit 2: a 32-beat ("Long") one does */
    int candidateGate() const;
    /** Drops every cache over m_funcs (the candidates index, the laser homes,
     *  the shared-look count). On a rebuild, a Doc change, a ban, FULL AUTO. */
    void invalidateCandidates();
    /** homePosition()'s search; homePosition() keeps its answer */
    quint32 findHomePosition(const QString &group) const;
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
    /** A function (a scene, or a chaser's step scenes) writes a non-zero
     *  value on a channel of the fixture's own effect engine (runde 303). */
    bool ownEffectOf(quint32 fid, const QString &group) const;
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
    void startFunction(Function *func, int division, bool glide = false);
    /** A head FIGURE's step (an AUTO chaser of aims, runde 254) on the slider,
     *  in beats: x1.5 at 30 % -> x0.75 at 100 %, never under two (runde 293).
     *  0 for anything else. */
    int figureBeats(quint32 fid) const;
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
    mutable QHash<quint32, QList<SceneValue> > m_valuesCache;   // B23 H3: scene id -> values, during a build
    bool m_valuesCacheOn = false;
    QHash<quint32, TrackFuncInfo> m_funcs;
    /* caches over m_funcs - see invalidateCandidates(). The index holds
     * POINTERS into m_funcs: never keep it across anything that inserts. */
    mutable QHash<QString, QList<CandEntry> > m_candIndex;   // "role|group"
    mutable QHash<QString, quint32> m_homeCache;              // "group|gate" -> homePosition()
    int m_sharedLooks = -1;                                   // checkConflicts(); -1 = not counted
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
    QStringList m_overrideSet;   // runde 304: the tiles lit; m_override leads, the next is the partner
    int m_overrideIdx = 0;       // which of them leads now (turns at every colour change)
    void applyOverride(const QString &colour);   // a tile's side effects (was setColourOverride's body)
    QString m_colour;
    QString m_leftColour;        // runde 236: the room colour before this one - not drawn straight back
    int m_colourBar;          // -1: a fresh track, hold the colour until a break or drop
    int m_colourSince;        // beat of the last colour change
    int m_holdNow;            // bars this colour holds - drawn each change around holdBars
    bool m_holdAuto = true;   // runde 189: the fader picks the hold (64/32/16/8 bars)
    qreal m_holdStretch = 1.0;  // this colour's random stretch of it, drawn at each change
    QString m_accentPick;     // the accent drawn for this section
    QString m_partnerPick;    // runde 243: the ONE partner colour this look may show
    bool m_partnerSolo = false; // runde 292: this look was drawn with no partner - one colour
    QString m_accentGroup;    // the group carrying it - rotates, never the same twice running
    int m_keyBias;            // this track's key: -1 unknown, 0 minor (cold side), 1 major (warm side)
    int m_nextKeyBias;        // the next track's, from BLT "next"
    QString m_nextColour;     // drawn when a mix begins: the colour the incoming track arrives in
    QList<int> m_clockCurve;  // ENGINE_CLOCK_POINTS percents, see SETTINGS_ENGINE_CLOCKCURVE
    QHash<quint32, qint64> m_recentUse;   // programme -> clock ms it last ran (the cooldown)
    qint64 m_cooldownMs;      // the clock reading the cooldown is judged against: frozen per section
    bool m_accentWasWhite;    // the last accent was white: the next one is not
    bool m_hatsOut;           // the strobes sit out: no hi-hats in the music right now
    bool m_strobesPooled = false; // the last bar line let the strobes into the cast pool (fejljagt 2)
    bool m_aniPooled = false;     // ... and the animation lasers, from ENGINE_ANI_ON (runde 344)
    bool m_dropShown = true;      // the last bar line had the fader at the drop line (ENGINE_DROP_SHOW, runde 279)
    bool m_oneLaser = true;       // the last bar line had the fader under the one-laser line (0.85, runde 279)
    bool m_curveBreak = false;   // runde 192: a groove flag with no kick in the next two bars plays as a break
    bool m_curveGroove = false;  // runde 192: a break flag with a solid kick in the next two bars plays as a groove
    int m_curveTurnBeat = -100;  // runde 193: the beat of the last such turn - four bars between turns
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
    int m_mixBeat;                        // the beat a mix began DURING this track (-1: none) - the base's turn to the next colour counts from it
    bool m_mixTurnLatched = false;        // runde 270: the base has turned to m_nextColour in this mix (tick)
    QMap<QString, quint32> m_splitScenes;  // "group|a|b" -> hidden two-colour scene
    /* R370_COLOUR_LAYER (runde 370): the tiles fading or chasing */
    int m_colourMode = 0;                  // 0 AUTO, 1 FADE, 2 CHASE (the tiles)
    int m_layerStyle = 0;                  // what runs: 0 none, 1 fade, 2 chase
    int m_layerPattern = 0;                // chase: 0 the whole group, 1 halves, 2 a walk
    qreal m_layerPos = 0.0;                // colour steps taken (fade: pair + how far into it)
    qreal m_layerRate = 0.0;               // steps per beat, this beat
    int m_layerBeat = -1;
    int m_layerLeadAge = 0;                // beats since the lead (lasers, accent) last turned
    QString m_layerBaseKey;
    QSet<QString> m_layerOwned;            // groups the layer painted this beat
    QHash<QString, qreal> m_layerLevel;    // their colour scenes' level (a scene with a dimmer)
    QMap<QString, quint32> m_chaseScenes;  // R371_CHASE_PAIR: "group|0" / "group|1" -> the two per-lamp scenes
    QHash<QString, QString> m_chaseShown;  // ... and what each holds: "group|n" -> "c0,c1,..."
    QHash<QString, int> m_chaseSide;       // group -> the one showing now
    QHash<QString, QList<quint32> > m_layerFids;   // R371_LAYER_FIDS: group -> its tiles' colour scenes, this beat
    void applyColourLayer(const QString &key, bool frame);
    /* R374_MIX_GLIDE: the base gliding into the next track's colour over a mix */
    bool m_mixGlide = false;
    qreal m_mixGlideP = 0.0;               // 0 the room's colour .. 1 the next track's
    qreal m_mixGlideRate = 0.0;            // per beat
    int m_mixGlideBeat = -1;
    QString m_mixGlideFrom, m_mixGlideTo, m_mixGlideKey;
    void applyMixGlide(bool frame);
    QTimer m_layerTimer;
    void updateColourLayer(int beat, const QString &base, bool isBreak, bool isBuild, bool isDrop,
                           qreal prog, qreal fader, bool frozen, bool jump, qreal kick);
    bool layerGroup(const QString &key) const;
    QStringList layerColours(const QString &key) const;
    void applyColourLayer(const QString &key);
    quint32 chaseColourFunction(const QString &group, const QStringList &perLamp);
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
    // the one extra group a break may show, rolled when the part turns into
    // a break (runde 263): one break in three, whatever came before it.
    // In-class default, out of the constructor's list as m_effectsBeat.
    int m_breakExtra = 0;
    // runde 275: the moves on stage were drawn BY this build (tick). A build
    // is drawn once, where it starts, and climbs live on every beat - an
    // inner flag, the kick wait and the 8-bar roll keep what it drew.
    bool m_buildDrawn = false;
    int m_starCeil;          // hottest star allowed this section (drawn from the energy)
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
    qreal m_movesEnergy = -1.0;            // the energy the moves were last drawn at (the pulse's reference)
    qreal m_movesFader = -1.0;             // ... and the SLIDER then: a jump of a fifth redraws (runde 289)
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
    qreal m_castEnergy = -1.0;             // the SLIDER the cast size was last decided at (a nudge steps it; runde 289)
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
    QSet<QString> m_baseCover;             // runde 260: lamp groups lit beside the base this beat (tick)
    QSet<QString> m_motionDim;             // groups whose MOTION owns the dimmers this beat
    QMap<QString, QVector<qreal> > m_texture;  // per-fixture spread, drifting slowly
    QMap<QString, QList<quint32> > m_zoomScenes; // head group -> 9 levels narrow..wide, then alternating A and B (runde 214)
    QMap<QString, int> m_zoomMode;         // per head group: 0 held, 1 the drop's pulse, 2 alternating heads (runde 214)
    bool m_floorRound = false;             // this build: heads straight down, sharp, one blinking round (runde 214)
    int m_buildStyle = 0;                  // runde 339: this build's shape - 0 fill, 1 odd/even roll, 2 swell, 3 halves
    QMap<QString, int> m_zoom;             // the zoom pick per group, -1 none
    int m_dropStyle;          // this drop's character: 0 none, 1 hard, 2 wide, 3 tight, 4 heavy, 5 nervous
    bool m_landCoin = false;  // runde 287: a plain or tight drop lands on the impact chase (true) or still
    bool m_dropStyleDrawn = false; // m_dropStyle was drawn for the drop on stage - 0 is a style too (runde 282)
    // runde 316: how hard the drop's kick is, heard over its first two bars -
    // the strobes' chase steps on the kick for a hard one, in eighths for a
    // soft one (strobePaceFloor, over 75 %). Once per drop, then held.
    qreal m_dropKickSum = 0.0;
    int m_dropKickN = 0;
    int m_dropKickLast = -1;      // the beat last counted - tick() can run twice on one
    bool m_dropKickLocked = false;
    bool m_strobeOnKick = true;   // true: a beat a step; false: eighths
    // runde 317: the curves are normalised per track (BLT: kick to its 90th,
    // lows to its 75th percentile), so a drop is "full" in EVERY track. What
    // tells a hard kick from a soft one is the number they were divided by -
    // this track's, and the same for the last tracks played (QSettings).
    qreal m_kickRef = -1.0;
    qreal m_lowRef = -1.0;
    QList<qreal> m_punchKick;
    QList<qreal> m_punchLow;
    QStringList m_punchTitle;     // the track each drop was in (runde 322: the history is drops, not tracks)
    QString m_punchNow;           // the track setTrackPunch() last heard
    int m_dropFrom = -1;      // the beat this drop's look was drawn (fejljagt 3: the settle clock)
    bool m_dropCalm = false;  // this drop has settled - held to the next section (fejljagt 3)
    int m_kickGone;           // beats in a row the analysis heard no kick (0 without curves)
    int m_kickBeat;           // the beat it last counted - tick() can run twice on one beat
    bool m_sectionOwed = false;   // R233_SAME_BEAT: a same-beat call's landing, owed to the next beat
    int m_landedBeat = -1;        // R234: the beat the last section landed on - owed only if it was not this one
    qint64 m_whiteLandMs = -1;    // runde 233: the last white drop landing (m_clock ms)
    QTimer m_echoTimer;       // the bars answer a hit half a beat later ...
    QTimer m_echoOffTimer;    // ... and let go a third of a beat after that
    QTimer m_beatWatch;       // runde 302: a beat and a half with no tick - see slotBeatWatch()
    QString m_echoKey;        // the laser group that answers
    quint32 m_echoFid;        // in this colour scene
    int m_echoBeat;           // the beat of the last echo - at most one every four
    QHash<QString, quint32> m_offScenes;              // group -> the scene that forces it to zero
    QHash<QString, quint32> m_darkScenes;             // runde 356: group -> its Intensity channels at zero
    QHash<QString, QList<quint32> > m_strobeScenes;   // group -> a scene per rate, slow to fast
    int m_strobeUntil;        // the beat the burst ends on (-1: not strobing)
    int m_strobeSeen;         // the beat driveStrobe last saw, to catch a scrub
    int m_strobeRate;         // which of the rates is up
    bool m_strobeHeadsOnly = false;   // runde 346: the burst up is the heads' own, in a drop
    int m_strobeWindow;       // first beat of the current 64-beat strobe budget window (-1: none)
    int m_strobeSpent;        // strobe beats used in that window
    QMap<QString, qreal> m_pulseDepth;     // groups pulsing right now, and how deep
    QMap<QString, qint64> m_pulseStart;    // clock reading of their last pulse beat
    QMap<QString, qreal> m_pulseStrength;  // how hard that beat hit, 0.5..1 - the kick's say
    QMap<QString, int> m_subStepSeen;      // the last sub-step the pulse timer masked, per group
    QMap<QString, int> m_breathe;          // groups on a slow sine, and over how many bars
    QMap<QString, qreal> m_breathPhase;    // where that sine stands at the beat, in cycles (runde 273)
    QElapsedTimer m_clock;
    qreal m_beatMs;
    qint64 m_beatStartMs;                  // clock reading of the last beat
    int m_beatIndex;                       // beats since the section started
    QString m_lastMoves;                   // what the report said, for the log
    QTimer m_pulseTimer;                   // 40 ms: the breath between two beats
    // ---- runde 356: the music's own dark (R356_MUSIC_DARK) ----
    // BLACKOUT is the operator's; this is the engine's: the beats the music
    // itself is silent, the last beat before a drop, and the short stabs
    // (Tobias' own BLACKOUT taps) that chop a build and a break at the top.
    // Rides every output the BLACKOUT clamp rides (lightsOut()).
    bool lightsOut() const { return m_blackout || m_autoDark; }
    void setAutoDark(bool on);
    void clearMusicDark();
    bool silentBeat(int beat) const;
    bool silentish(int beat) const;
    int silentRunStart(int beat) const;
    qreal audLevel(int beat) const;
    QVector<quint8> m_audLevel;
    QVector<quint8> m_audKick;
    QVector<quint8> m_audHigh;
    bool m_autoDark = false;
    bool m_hardStart = false;            // runde 356: this tick starts out of the dark
    bool m_silenceDark = false;
    QTimer m_chopTimer;
    QVector<QPair<qint64, qint64> > m_chopPlan;
    QString m_musicDarkEvent;
    // ---- runde 357 ----
    QVector<QPair<int, int> > m_fills;   // R357_FILLS
    bool inFill(int beat) const;
    int m_curBeat = -1;                  // this tick's beat, set before anything is drawn
    int m_landBurstUntil = -1;           // R357_STROBE_LAND: the landing burst, the only hardware strobe on the strobe lamps
    bool landBurstNow() const { return m_landBurstUntil >= 0 && m_curBeat >= 0 && m_curBeat <= m_landBurstUntil; }
    // R362_MINI_LAND: the kick back after a pause inside a drop - the strobe
    // lamps blink the sixteenths like the landing, but UNDER MASTER and the trim
    int m_miniLandUntil = -1;
    int m_miniLandLast = -100;
    qreal m_miniSize = 0.0;
    bool miniLandNow() const { return m_miniLandUntil >= 0 && m_curBeat >= 0 && m_curBeat <= m_miniLandUntil; }
    bool blink16Now() const { return landBurstNow() || miniLandNow(); }
    qreal m_nextDropSize = 1.0;          // R362_BUILD_SIZE: the size of the drop this build leads into (1 without curves)
    int m_nextDropBeat = -1;             // R363_NEXT_DROP: the next drop flag ahead (trackmanager), -1 none
    int m_landBurstFrom = -1;            // R363_LOOP_LAND: the beat the landing burst began
public:
    void setNextDropBeat(int beat) { m_nextDropBeat = beat; }
private:
    bool m_bigDrop = false;              // R357_KICK_BACK: this drop arrived big
    int m_kickBackLast = -100;
    mutable QHash<QString, qreal> m_colourTaste;   // R357_TASTE: learned from the colour tiles
    mutable bool m_tasteLoaded = false;
    qreal colourTaste(const QString &colour) const;
    void learnColour(const QString &picked, const QString &shown);
    // ---- runde 358 ----
    QVector<quint8> m_audMid;            // R358_RHYTHM: the mid band, beat for beat
    QVector<quint16> m_onsetHigh;        // the highs' onsets per quarter beat
    int onsetHigh(int beat, int quarter) const;   // 0-15, or -1 without data
    qreal audMid(int beat) const;        // 0-1, or -1 without data
    // R358_LOOP: the DJ's loop as the beat numbers show it - a step back to
    // the same beat from the same top, again and again
    int m_loopFrom = -1;                 // the loop's top (the last beat before the jump)
    int m_loopTo = -1;                   // where it jumps back to
    int m_loopPasses = 0;                // jumps seen (1 = could still be a cue)
    int m_loopLen = 0;                   // beats, a power of two
    qint64 m_loopJumpMs = -1;
    bool djLoopOn() const { return m_loopPasses >= 2 && m_loopLen > 0; }
    void clearDjLoop();
    void newTrackMemory(const QString &title);
    // R358_REPEAT: the first drop and the first build of a track, as they looked
    struct TrackLookMemory
    {
        QString colour;
        int dropStyle = -1;
        bool landCoin = false;
        QHash<QString, quint32> motion;
        QVector<qreal> sig;              // what it sounded like (level, kick, highs, mids)
        int beat = -1;
    };
    QHash<QString, QVector<TrackLookMemory> > m_lookMemory;   // up to four distinct drops / builds per track
    QString m_lookPending;               // "drop"/"build": recording its first bars
    int m_lookPendingIdx = -1;
    QString m_lookActive;                // a repeat wearing the memory
    int m_lookRepeat = 0;
    QString m_reuseColour;
    int m_reuseStyle = -1;
    bool m_reuseCoin = false;
    QHash<QString, quint32> m_reuseMotion;
    QVector<qreal> soundSig(int from, int beats) const;
    int m_lookFrom = -1;                 // the beat the recorded section began
    bool m_halfTime = false;             // R358_HALFTIME: the kick on every other beat at most
    qreal m_deckSpeed = 1.0;             // R358_BRAKE: deck tempo over its own running tempo
    qreal m_tempoRef = -1.0;
    qint64 m_brakeMs = -1;               // when the tempo fell away: a brake ends in a stop inside two seconds
    qreal brakeDim() const { return m_deckSpeed < 0.85 ? qBound(0.0, (m_deckSpeed - 0.25) / 0.60, 1.0) : 1.0; }
    int m_vocalRun = 0;                  // R358_VOCAL: of the last three beats, those with mids and no kick or bass
    int m_vocalBeat = -1;
    bool m_vocalNow = false;
    int m_grooveSlots = 0;               // R358_GROOVE: quarters (bits 1-3) the drums hit between the beats
    bool m_backbeat = false;             // claps/snare on 2 and 4
    int m_backbeatParity = 1;            // ... on the beats with this (beat - section start) parity
    QHash<QString, int> m_grooveSeen;    // the quarter each group last re-hit on
    // ---- runde 359 ----
    QVector<quint8> m_audPump;           // R359_PUMP: the fall between the kicks, beat for beat
    qreal m_pumpNow = -1.0;              // its mean over the next eight kicked beats, -1 none
    int m_density = 0;                   // R359_DENSITY: +1 busy percussion, -1 sparse, 0 between
    qreal riserAt(int at, int from, int to) const;   // R359_RISER: the build's measured climb, -1 none
    bool m_landSixteenths = true;        // R359_LAND_16: the strobe lamps' landing blinks on the beat's sixteenths
    // ---- runde 360 ----
    bool m_landOwed = false;             // R360_LATE_LAND: a track loaded in a mix may arrive inside its drop
    bool m_dropArrivedNow = false;       // R365_LAND_ONCE: the drop arrives on this beat (tick -> driveStrobe)
    bool m_dropLanded = false;           // R369_FORCED_LAND: this drop has had its landing (or its one chance at it)
    bool m_lateLandNow = false;          // ... and this beat is that drop's landing, a beat or three late
    bool m_dropForced = false;           // R361_DROP_FORCED: the operator pressed DROP (trackmanager)
    qreal m_dropGrowHigh = 0.0;          // R361_GROW_BACK: the highs / level the drop grew to
    qreal m_dropGrowLevel = 0.0;
    qreal m_dropSize = 1.0;              // R360_DROP_SIZE: how big the arriving drop is, 0-1 (1 without curves)
    qreal dropSizeAt(int beat) const;
    bool m_dropGrow = false;             // R360_DROP_GROW: the drop stepped up on an 8-bar line
    int m_dropGrowAt = -1;
    int m_dropLine = -1;                 // R367_GROW_TRACK: the TRACK beat the drop arrived on (never moved by a jump)
    int m_dropGrowLine = -1;             // ... and the track line the growth was last measured on
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
    bool m_closingOn = true;               // the closing sequence (SETTINGS_ENGINE_CLOSING)
    qreal m_closingDim = 1.0;              // closingCap() as last applied to the light (closingTick)
    int m_roomSent;                        // last percent handed to the ENERGY trim
    bool m_fullAuto;
    QSet<QString> m_flashHeld;             // strobe groups the generated flash lit
    bool m_hold;
    // runde 227: the build's own programmes ("... Climb" in the name, from
    // gen_programs.py). m_buildLen is this beat's build length in beats (0
    // outside a build); m_climbGroups the groups that have any.
    int m_buildLen = 0;
    QSet<QString> m_climbGroups;
    // runde 237: the first flag of the build we are in (-1 outside one) - a
    // build split by inner flags climbs from here to the drop, not from each
    int m_buildFrom = -1;
    bool m_startScene;        // the opening picture is held by hand
    qreal m_startLevel;       // a trim on the opening picture
    bool m_startColour;       // the opening picture chose the colour, not the DJ
    bool m_forceNext;
    /** runde 338: a fixture's per-eye channels (4 or more) and its master
     *  dimmer, for beamShareOf/fullBankOf - read once per table build: the
     *  regex over every channel of every fixture of every programme doubled
     *  the rebuild (553 -> 1105 ms headless) */
    mutable QHash<quint32, QSet<quint32> > m_eyeCache;
    mutable QHash<quint32, quint32> m_dimmerCache;
    QList<int> m_hitBeats;                 // beats that carried a hit, last 32 beats

    /* what is running: slot name -> fid, and its attribute override */
    QMap<QString, quint32> m_active;
    bool m_startWatchPending = false;      // runde 345: one restart per burst of stops
    int m_startWatchCount = 0;             // runde 345: restarts in the current 10 s
    qint64 m_startWatchSince = 0;          // runde 345: when that 10 s began (ms)
    QMap<QString, int> m_activeAttr;
    QMap<QString, qreal> m_activeLevel;
    QMap<QString, qreal> m_activeOut;      // what was actually written (pulse, trim, master, blackout applied)
    QMap<quint32, int> m_fadeAttr;
    QMap<quint32, qreal> m_fadeLevel;
};

#endif // TRACKENGINE_H
