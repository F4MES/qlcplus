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
#include "barcoverage.h" // C20261020: actual per-bar eye counts
#include "showphrases.h"
#include "programlottery.h"
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
#include <QJsonObject>       // R410_LAB: the lab's store

class Function;
class Fixture;
class Scene;
class Doc;
struct MicTap;                     // R467_MIC_CHECK (track_runtime/miccheck.inc.cpp)
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
#define SETTINGS_ENGINE_CLOSING   QStringLiteral("trackengine/closing-v2")   // R458_CLOSING_OFF: off until switched on (a new key)
#define SETTINGS_ENGINE_STUTTER   QStringLiteral("trackengine/stutterRate")   // R420_STUTTER
#define ENGINE_CLOSING_SECS       300
// R458_LAST_TRACK (track_runtime/lasttrack.inc.cpp)
#define ENGINE_LAST_SHOW_MIN      10        // the button shows this many minutes before closing
#define ENGINE_LAST_WAIT_MIN      60        // the clock's closing waits this long for the last track
#define ENGINE_LAST_OUTRO_BEATS   32        // no OUTRO flag: the last 32 beats are the outro
#define ENGINE_LAST_END_LID       0.25      // the closing lid at the track's last beat
#define ENGINE_HOUSE_LEVEL        1.00      // R479_LAST_WHITE_DEFS (10-17): full - "rammer 100%" (was 0.40, the soft white)
#define ENGINE_HOUSE_IN_MS        8000      // the haze down
#define ENGINE_HOUSE_RISE_MS      2000      // the white the rest of the way up (fader down, deck stopped)
#define ENGINE_LAST_AIR_MS        500       // the deck off air this long in the outro: the end
#define ENGINE_LAST_AIR_MID_MS    4000      // ... before the outro (a cut for the crowd to sing is no end)
#define ENGINE_LAST_AUTO_MS       3000      // R484_LF_DEFS (10-19): every fader down MORE than this, not armed: LAST TRACK ("i mere 3 sekunder")
#define ENGINE_LAST_LOUD          26        // a beat with music: a tenth of the track's own top (m_audLevel)
#define ENGINE_HOUSE_HOLD_MS      300000    // five minutes of white
#define ENGINE_HOUSE_OUT_MS       30000     // then down to nought
// R467_HAZE_NAG (track_runtime/hazenag.inc.cpp)
#define ENGINE_HAZE_NAG_MIN       10        // the haze at 0 this long ...
#define ENGINE_HAZE_NAG_FROM_H    22        // ... from 22:00 ...
#define ENGINE_HAZE_NAG_TO_H      8         // ... to 08:00
#define ENGINE_HAZE_NAG_LATER_MIN 30        // LATER: quiet this long
// R484_HZ_DEFS (Tobias 10-20: "haze +70 procent ikke vaere en knap ... faderen bliver 'elastisk' ... ryger tilbage til 70 naar man slipper faderen";
// 10-20: "lav
// AUTO knappen til haze" / "Det samme princip skal gaelde for haze (husk at haze baade skal huske
// baade fan og haze output)")
#define ENGINE_HAZE_REST          0.70      // default only; hazeLimit() is the saved elastic threshold
#define ENGINE_HAZE_AUTO_STEP_MS  1200      // AUTO moves haze and fan 1 % at most this often (under the log's 1.5 s)
#define SETTINGS_ENGINE_ATMOSROW  QStringLiteral("trackengine/atmos-row")       // HAZE & FAN BY CLOCK: "h:f,h:f,..." (-1 = none)
#define SETTINGS_ENGINE_ATMOSOWN  QStringLiteral("trackengine/atmos-row-own")   // his points: "12,20"
#define SETTINGS_ENGINE_ATMOSVIEW QStringLiteral("trackengine/atmos-view")      // the nights the row learns from
// R469_GROUP_FLASH (track_runtime/padflash.inc.cpp)
#define ENGINE_PAD_FLASH_MS       3000      // a held pad nobody refreshed lets go
// R470_SYNC (track_runtime/sync.inc.cpp)
#define ENGINE_SYNC_MAX_MS        150       // the light moved at most this, either way
#define ENGINE_SYNC_TAPS          16        // taps in a round
#define ENGINE_SYNC_FAR_MS        200       // a tap further off the beat is left out
#define ENGINE_SYNC_BLINK_MS      90        // round 2: the strobes' blink
// R470_CLOCK_HAND (track_runtime/clockhand.inc.cpp)
#define ENGINE_HAND_MIN_SAMPLES   150       // 30 s of his hand in a quarter hour before it counts
// R473_ATMOS (track_runtime/clockhand.inc.cpp): his hand on the hazer, kept - not used
#define ENGINE_ATMOS_CELLS        96        // the night's quarter hours, noon to noon
#define ENGINE_ATMOS_NIGHTS       120       // nights kept (about four months)
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
#define ENGINE_NEXT_SALVO_MS 2000        // R428_NEXT_SALVO: a NEXT this soon after the last is a search, no verdict

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
    // R410_LAB: a LASER LAB look (AUTO Programs/Laser Lab) and what the
    // operator said about it - the ENERGY window it may run in, CALM
    bool lab = false;
    bool labCalm = false;
    qreal labMin = 0.0;
    qreal labMax = 1.0;
    int stars = 0;            // energy 1..3: when this may run (0 = not applicable)
    int starsGuess = 0;       // what the engine would say, from tempo and name
    int fixtureCount = 0;     // how many fixtures it touches - a full look beats a part
    qreal litShare = 1.0;     // how much of what it touches is lit, averaged over its steps
    qreal minLit = 0.0;       // the lowest master dimmer any step leaves any of its lamps
    qreal peakLit = 0.0;      // runde 259: the BRIGHTEST lamp in its darkest step - "at
                              // least one lamp on" (Tobias), 0..1
    TrackBarCoverage barCoverage; // C20261020: measured steps, never a name heuristic
    QByteArray outputKey;    // C20261020_FRESH: exact static output/timing; empty for opaque effects
    QByteArray drawKey;      // R504_GEN_DJ_KEY: a front/back wave draws on its sibling wave's ticket (empty = outputKey)
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
    int rhythm = 0;           // C20261022: 0 straight, 1 tresillo, 2 offbeat, 3 paired steps
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
    int formMask = 0;         // R488_DX_MASK_FIELD: a drop form's own picture (dropforms.inc.cpp), 0 none
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
    // R383_LASER_FAST: how the bars relate at the top of the slider - 0 the wave
    // down the row, 1 the row in unison, 2 from the middle out, 3 the wave back
    int laserForm = 0;
    // R385_BARS_FULL: the bars' slow lift in their alone break - never under
    // the home aim, whatever the slider allows a drop
    bool upOnly = false;
    // R504_MIRROR_FIELD (runde 504, Tobias 10-25: the heads' figures mirrored left/right): the
    // left side of the room (map x < 0.5) runs the pan mirror of its own figure, so the two sides
    // move towards and away from each other (r504_mirror.inc.cpp); false = today's relation
    bool roomMirror = false;
    bool operator==(const TrackSweep &o) const
    {
        return shape == o.shape && width == o.width && height == o.height && rotation == o.rotation
            && roomMirror == o.roomMirror   // R504_MIRROR_EQ
            && beats == o.beats && spread == o.spread && mirror == o.mirror && fan == o.fan
            && fx == o.fx && fy == o.fy && dx == o.dx && dy == o.dy && minBeats == o.minBeats
            && paceRole == o.paceRole && laserForm == o.laserForm && upOnly == o.upOnly;
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
    /** R431_DROP_PICK: the groups of the next drop (dropMode 1) or this one (2) */
    Q_PROPERTY(QStringList dropSet READ dropSet NOTIFY dropSetChanged)
    Q_PROPERTY(int dropMode READ dropMode NOTIFY dropSetChanged)
    /** R432_NEXT_SECTION: the section the plan (dropSet) is for */
    Q_PROPERTY(QString planSection READ planSection NOTIFY dropSetChanged)
    Q_PROPERTY(bool planRemembered READ planRemembered NOTIFY dropSetChanged)   // R439_TRACK_MEMORY
    /** R443_PLAN_LANE: the plan lane on the track (PLAN beside MARKERS) */
    Q_PROPERTY(bool planLane READ planLane WRITE setPlanLane NOTIFY planLaneChanged)
    /** R445_DROP_HIT: 0 off, 1 FLASH, 2 STROBE, 3 DARK -> BANG on the next drop */
    Q_PROPERTY(int hitKind READ hitKind WRITE setHitKind NOTIFY hitChanged)
    Q_PROPERTY(int hitLast READ hitLast NOTIFY hitChanged)
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
    // R420_STUTTER: held = dark on every 1/4 (1), 1/8 (2) or 1/16 (3) of the beat
    Q_PROPERTY(bool stutter READ stutter WRITE setStutter NOTIFY liveChanged)
    Q_PROPERTY(int stutterRate READ stutterRate WRITE setStutterRate NOTIFY liveChanged)
    /** Two decks on air (from BLT): a transition. The colour holds and the
     *  engine stays at groove level until the outgoing track is gone. */
    Q_PROPERTY(bool mixing READ mixing NOTIFY liveChanged)
    Q_PROPERTY(QString report READ report NOTIFY liveChanged)
    /** SETUP > SELF TEST is running: every group's colour scenes, 2 s each. */
    Q_PROPERTY(bool testing READ testing NOTIFY liveChanged)

    /** R410_LAB: SETUP > LASER LAB (runde 410) - the animation laser's looks
     *  one by one: ATLAS (pass 0), VARIANTS (1), APPROVED (2). */
    Q_PROPERTY(bool labAvailable READ labAvailable NOTIFY labChanged)
    Q_PROPERTY(bool labActive READ labActive NOTIFY labChanged)
    Q_PROPERTY(QString labGroup READ labGroup NOTIFY labChanged)
    Q_PROPERTY(int labLampCount READ labLampCount NOTIFY labChanged)
    Q_PROPERTY(int labPass READ labPass NOTIFY labChanged)
    Q_PROPERTY(int labIndex READ labIndex NOTIFY labChanged)
    Q_PROPERTY(int labCount READ labCount NOTIFY labChanged)
    Q_PROPERTY(QString labKey READ labKey NOTIFY labChanged)
    Q_PROPERTY(QString labTitle READ labTitle NOTIFY labChanged)
    Q_PROPERTY(QString labDetail READ labDetail NOTIFY labChanged)
    Q_PROPERTY(QStringList labLocked READ labLocked NOTIFY labChanged)
    Q_PROPERTY(QVariantList labStrip READ labStrip NOTIFY labChanged)
    Q_PROPERTY(int labGood READ labGood NOTIFY labChanged)
    Q_PROPERTY(int labNo READ labNo NOTIFY labChanged)
    Q_PROPERTY(int labSame READ labSame NOTIFY labChanged)
    Q_PROPERTY(int labVerdict READ labVerdict NOTIFY labChanged)
    Q_PROPERTY(QString labTagTitle READ labTagTitle NOTIFY labChanged)
    Q_PROPERTY(QStringList labSections READ labSections NOTIFY labChanged)
    Q_PROPERTY(QStringList labEnergy READ labEnergy NOTIFY labChanged)
    Q_PROPERTY(QStringList labCharacter READ labCharacter NOTIFY labChanged)
    Q_PROPERTY(int labStars READ labStars NOTIFY labChanged)
    Q_PROPERTY(QString labNote READ labNote NOTIFY labChanged)
    Q_PROPERTY(int labAuto READ labAuto NOTIFY labChanged)
    Q_PROPERTY(QString labColour READ labColour NOTIFY labChanged)
    Q_PROPERTY(QStringList labColours READ labColours NOTIFY labChanged)
    Q_PROPERTY(int labLamps READ labLamps NOTIFY labChanged)
    Q_PROPERTY(bool labHold READ labHold NOTIFY labChanged)
    Q_PROPERTY(bool labDark READ labDark NOTIFY labChanged)
    Q_PROPERTY(bool labSlower READ labSlower NOTIFY labChanged)
    Q_PROPERTY(int labApprovedCount READ labApprovedCount NOTIFY labChanged)
    Q_PROPERTY(QVariantList labApprovedList READ labApprovedList NOTIFY labChanged)
    Q_PROPERTY(QString labMessage READ labMessage NOTIFY labChanged)
    /** R436_LEARNED: SETUP > LEARNED - per group { key, base, cells[break, build, normal, drop] } */
    Q_PROPERTY(QVariantList tasteTable READ sectionTasteTable NOTIFY tasteChanged)
    Q_PROPERTY(QString tasteNight READ tasteNight NOTIFY tasteChanged)
    /** R436_SHOW_CHECK: what the build counts on and the show file lacks */
    Q_PROPERTY(QVariantList showMissing READ showMissing NOTIFY showCheckChanged)
    Q_PROPERTY(int showWants READ showWants NOTIFY showCheckChanged)
    Q_PROPERTY(bool showAcked READ showAcked NOTIFY showCheckChanged)
    /** R436_STROBE_LAB: SETUP > LAB > STROBE LAB */
    Q_PROPERTY(bool slabAvailable READ slabAvailable NOTIFY slabChanged)
    Q_PROPERTY(bool slabActive READ slabActive NOTIFY slabChanged)
    Q_PROPERTY(QString slabGroup READ slabGroup NOTIFY slabChanged)
    Q_PROPERTY(int slabLampCount READ slabLampCount NOTIFY slabChanged)
    Q_PROPERTY(int slabTab READ slabTab NOTIFY slabChanged)
    Q_PROPERTY(int slabIndex READ slabIndex NOTIFY slabChanged)
    Q_PROPERTY(int slabCount READ slabCount NOTIFY slabChanged)
    Q_PROPERTY(QString slabTitle READ slabTitle NOTIFY slabChanged)
    Q_PROPERTY(QString slabDetail READ slabDetail NOTIFY slabChanged)
    Q_PROPERTY(QStringList slabChips READ slabChips NOTIFY slabChanged)
    Q_PROPERTY(int slabVerdict READ slabVerdict NOTIFY slabChanged)
    Q_PROPERTY(QVariantList slabStrip READ slabStrip NOTIFY slabChanged)
    Q_PROPERTY(int slabGood READ slabGood NOTIFY slabChanged)
    Q_PROPERTY(int slabNo READ slabNo NOTIFY slabChanged)
    Q_PROPERTY(QVariantList slabTabCounts READ slabTabCounts NOTIFY slabChanged)
    Q_PROPERTY(int slabGoodAll READ slabGoodAll NOTIFY slabChanged)
    Q_PROPERTY(int slabNoAll READ slabNoAll NOTIFY slabChanged)
    Q_PROPERTY(int slabAuto READ slabAuto NOTIFY slabChanged)
    Q_PROPERTY(QString slabColour READ slabColour NOTIFY slabChanged)
    Q_PROPERTY(bool slabOwnColour READ slabOwnColour NOTIFY slabChanged)
    Q_PROPERTY(int slabSpeed READ slabSpeed NOTIFY slabChanged)
    Q_PROPERTY(bool slabCanSpeed READ slabCanSpeed NOTIFY slabChanged)
    Q_PROPERTY(bool slabHold READ slabHold NOTIFY slabChanged)
    Q_PROPERTY(bool slabDark READ slabDark NOTIFY slabChanged)
    Q_PROPERTY(bool slabTest READ slabTest NOTIFY slabChanged)
    Q_PROPERTY(QString slabMessage READ slabMessage NOTIFY slabChanged)
    /** R440_BARS_LAB: 0 STROBE LAB, 1 BARS LAB - one page, one engine half */
    Q_PROPERTY(int slabKind READ slabKind NOTIFY slabChanged)
    Q_PROPERTY(int slabOrder READ slabOrder NOTIFY slabChanged)
    Q_PROPERTY(QStringList slabColours READ slabColours NOTIFY slabChanged)
    Q_PROPERTY(QVariantList slabSummaries READ slabSummaries NOTIFY slabChanged)
    /** R440_COLOUR_LAB: SETUP > LAB > COLOUR LAB */
    Q_PROPERTY(bool clabActive READ clabActive NOTIFY clabChanged)
    Q_PROPERTY(QString clabColour READ clabColour NOTIFY clabChanged)
    Q_PROPERTY(QStringList clabColours READ clabColours NOTIFY clabChanged)
    Q_PROPERTY(QVariantList clabRows READ clabRows NOTIFY clabChanged)
    Q_PROPERTY(QVariantList clabMatrix READ clabMatrix NOTIFY clabChanged)
    Q_PROPERTY(QString clabSolo READ clabSolo NOTIFY clabChanged)
    Q_PROPERTY(bool clabDark READ clabDark NOTIFY clabChanged)
    Q_PROPERTY(qreal clabTest READ clabTest NOTIFY clabChanged)
    Q_PROPERTY(QString clabMessage READ clabMessage NOTIFY clabChanged)
    Q_PROPERTY(int clabChanges READ clabChanges NOTIFY clabChanged)
    /** R440_SEND: SETUP > ADVANCED > SEND TO CLAUDE */
    Q_PROPERTY(int sendState READ sendState NOTIFY sendChanged)
    Q_PROPERTY(QString sendResult READ sendResult NOTIFY sendChanged)

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
    // R458_LAST_TRACK: 0 off, 1 armed, 2 outro, 3 house light, 4 dark
    Q_PROPERTY(int lastTrack READ lastTrack NOTIFY lastTrackChanged)
    Q_PROPERTY(bool lastTrackShown READ lastTrackShown NOTIFY lastTrackChanged)
    Q_PROPERTY(int lastTrackLeft READ lastTrackLeft NOTIFY lastTrackChanged)
    Q_PROPERTY(int lastTrackCloseMin READ lastTrackCloseMin NOTIFY lastTrackChanged)
    Q_PROPERTY(QString lastTrackCloseAt READ lastTrackCloseAt NOTIFY lastTrackChanged)
    Q_PROPERTY(int lastTrackEnd READ lastTrackEndBeat NOTIFY lastTrackChanged)   // R479_LAST_WHITE_PROP: the music's end
    // R467_HAZE_NAG: minutes the haze has stood at 0 (0 = nothing to say)
    Q_PROPERTY(int hazeNag READ hazeNag NOTIFY hazeNagChanged)
    // R484_HZ_PROP: AUTO = haze AND fan from HAZE & FAN BY CLOCK (off at every start-up; a hand on
    // HAZE or FAN SPEED ends it)
    Q_PROPERTY(bool hazeAuto READ hazeAuto WRITE setHazeAuto NOTIFY liveChanged)
    Q_PROPERTY(int hazeLimit READ hazeLimit WRITE setHazeLimit NOTIFY liveChanged)
    // R469_GROUP_FLASH: the groups a flash pad holds at full white now
    Q_PROPERTY(QStringList flashGroups READ flashGroups NOTIFY flashGroupsChanged)
    // R470_SYNC: SETUP > LAB > SYNC - is the light on time with the sound
    Q_PROPERTY(bool syncActive READ syncActive NOTIFY syncChanged)
    Q_PROPERTY(int syncStep READ syncStep NOTIFY syncChanged)
    Q_PROPERTY(int syncTaps READ syncTaps NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncSound READ syncSound NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncLight READ syncLight NOTIFY syncChanged)
    Q_PROPERTY(int syncResult READ syncResult NOTIFY syncChanged)
    Q_PROPERTY(int syncSpread READ syncSpread NOTIFY syncChanged)
    Q_PROPERTY(int syncOffset READ syncOffset NOTIFY syncChanged)
    // R470_CLOCK_HAND: his hand's ENERGY per quarter hour, the last nights
    Q_PROPERTY(QVariantList clockHand READ clockHand NOTIFY clockHandChanged)
    Q_PROPERTY(QStringList clockHandNights READ clockHandNights NOTIFY clockHandChanged)
    // R484_EH_PROP: per clock point, where its value comes from - 0 the clock, 1 learned from his hand, 2 his own
    Q_PROPERTY(QVariantList clockSource READ clockSource NOTIFY tableChanged)
    // R484_EH_VIEW (Tobias 10-20: "energi skal have samme valg som haze"): the nights the row learns
    // from - 0 the last 2 weekends, 1 every Fri + Sat, 2 the other nights (as atmosView)
    Q_PROPERTY(int clockView READ clockView WRITE setClockView NOTIFY clockHandChanged)
    // R473_ATMOS: HAZE & FAN BY HAND - 20:00-03:00, { h, f } per quarter (-1 = nothing yet)
    Q_PROPERTY(QVariantList atmosHand READ atmosHand NOTIFY atmosChanged)
    Q_PROPERTY(QStringList atmosNights READ atmosNights NOTIFY atmosChanged)
    Q_PROPERTY(bool atmosKnown READ atmosKnown NOTIFY atmosChanged)
    Q_PROPERTY(int atmosView READ atmosView WRITE setAtmosView NOTIFY atmosChanged)
    // R484_HZ_ROW_PROP: HAZE & FAN BY CLOCK, 20:00 - 02:45: { h, f, src } per point (-1 = none;
    // src 0 nothing, 1 learned from his hand, 2 his own)
    Q_PROPERTY(QVariantList atmosCurve READ atmosCurve NOTIFY atmosChanged)
    // R467_MIC_CHECK: SETUP > LAB > MIC CHECK - it only listens
    Q_PROPERTY(bool micActive READ micActive NOTIFY micChanged)
    Q_PROPERTY(QString micDevice READ micDevice NOTIFY micChanged)
    Q_PROPERTY(int micDeviceIndex READ micDeviceIndex NOTIFY micChanged)
    Q_PROPERTY(int micDeviceCount READ micDeviceCount NOTIFY micChanged)
    Q_PROPERTY(QString micProblem READ micProblem NOTIFY micChanged)
    Q_PROPERTY(qreal micLevel READ micLevel NOTIFY micChanged)
    Q_PROPERTY(qreal micPeak READ micPeak NOTIFY micChanged)
    Q_PROPERTY(bool micClip READ micClip NOTIFY micChanged)
    Q_PROPERTY(bool micSilent READ micSilent NOTIFY micChanged)
    Q_PROPERTY(QVariantList micBands READ micBands NOTIFY micChanged)
    Q_PROPERTY(QVariantList micRoom READ micRoom NOTIFY micChanged)
    Q_PROPERTY(QVariantList micTrack READ micTrack NOTIFY micChanged)
    Q_PROPERTY(int micBeatTick READ micBeatTick NOTIFY micChanged)
    Q_PROPERTY(int micAgree READ micAgree NOTIFY micChanged)
    Q_PROPERTY(int micRecLeft READ micRecLeft NOTIFY micRecChanged)
    Q_PROPERTY(bool micAuto READ micAuto NOTIFY micRecChanged)
    Q_PROPERTY(QVariantList micRecs READ micRecs NOTIFY micRecChanged)
    /** The evening's opening picture: the IDLE functions (the START scene)
     *  held on their own, with the engine standing still. The first SHOW ON
     *  of the night with ENERGY at 0 puts it up, and ENERGY above 0 takes
     *  that one down again (TrackManager, runde 184); by hand it stays until
     *  the tile is tapped again or the show is switched. */
    Q_PROPERTY(bool startScene READ startScene WRITE setStartScene NOTIFY liveChanged)
    /** R378_POSITIONS: "" AUTO, "start", "column" (Soejle midt), "down" (lige ned) */
    Q_PROPERTY(QString positionMode READ positionMode WRITE setPositionMode NOTIFY liveChanged)

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

    /* ---- R410_LAB: LASER LAB (track_runtime/tracklab.inc.cpp) ---- */
    /** Opens with SHOW OFF only (the tile asks); false, and the reason in
     *  the report line, when it cannot. */
    Q_INVOKABLE bool labOpen();
    Q_INVOKABLE void labClose();
    Q_INVOKABLE void labSetPass(int pass);
    Q_INVOKABLE void labNext();
    Q_INVOKABLE void labBack();
    Q_INVOKABLE void labGoto(int index);
    /** 1 GOOD, -1 NO, 2 SAME AS LAST (the atlas only) */
    Q_INVOKABLE void labDecide(int verdict);
    /** kind "sec" (break groove build drop any), "en" (low mid high top),
     *  "ch" (calm busy hard wide tight) - for the look just kept */
    Q_INVOKABLE void labToggleTag(const QString &kind, const QString &tag);
    Q_INVOKABLE void labSetStars(int stars);
    Q_INVOKABLE void labSetNote(const QString &note);
    /** 0 off, 1 two seconds, 2 four seconds, 3 eight beats */
    Q_INVOKABLE void labSetAuto(int mode);
    Q_INVOKABLE void labSetColour(const QString &colour);
    /** 0 both, 1 the first by address, 2 the last */
    Q_INVOKABLE void labSetLamps(int lamps);
    Q_INVOKABLE void labToggleHold();
    Q_INVOKABLE void labToggleSlower();
    Q_INVOKABLE void labStop();
    /** TOO LOW / AUDIENCE: dark at once, filed as never */
    Q_INVOKABLE void labBan();
    Q_INVOKABLE void labRemove(const QString &key);
    bool labAvailable() const;
    bool labActive() const { return m_labActive; }
    QString labGroup() const { return m_labGroup; }
    int labLampCount() const { return int(m_labFixtures.count()); }
    int labPass() const { return m_labPass; }
    int labIndex() const { return m_labIndex; }
    int labCount() const { return int(m_labList.count()); }
    QString labKey() const { return labCurrentKey(); }
    QString labTitle() const;
    QString labDetail() const;
    QStringList labLocked() const;
    QVariantList labStrip() const;
    int labGood() const { return labCountOf(1); }
    int labNo() const { return labCountOf(-1); }
    int labSame() const { return labCountOf(2); }
    int labVerdict() const { return labVerdictOf(labCurrentKey()); }
    QString labTagTitle() const;
    QStringList labSections() const;
    QStringList labEnergy() const;
    QStringList labCharacter() const;
    int labStars() const;
    QString labNote() const;
    int labAuto() const { return m_labAuto; }
    QString labColour() const { return m_labColour; }
    QStringList labColours() const { return m_labColours; }
    int labLamps() const { return m_labLamps; }
    bool labHold() const { return m_labHold; }
    bool labDark() const { return m_labDark; }
    bool labSlower() const;
    int labApprovedCount() const;
    QVariantList labApprovedList() const;
    QString labMessage() const { return m_labMessage; }
    /* ---- R436_LEARNED ---- */
    QVariantList sectionTasteTable() const;
    QString tasteNight() const;
    /** one box back to neutral; both empty = all of it */
    Q_INVOKABLE void resetSectionTaste(const QString &section, const QString &key);
    /* ---- R436_SHOW_CHECK ---- */
    QVariantList showMissing() const { return m_showMissing; }
    int showWants() const { return m_showWants; }
    bool showAcked() const;
    /** CONTINUE ANYWAY: this show file, as it is, for this run */
    Q_INVOKABLE void showContinue();
    /* ---- R436_STROBE_LAB (track_runtime/strobelab.inc.cpp) ---- */
    Q_INVOKABLE bool slabOpen();
    Q_INVOKABLE void slabClose();
    Q_INVOKABLE void slabSetTab(int tab);
    Q_INVOKABLE void slabNext();
    Q_INVOKABLE void slabBack();
    Q_INVOKABLE void slabGoto(int index);
    /** -1 NO (ban), 0 SKIP, 1 GOOD (up-votes) - the same verdict again takes it back */
    Q_INVOKABLE void slabDecide(int verdict);
    /** 0 off, 1 two seconds, 2 four seconds, 3 eight beats */
    Q_INVOKABLE void slabSetAuto(int mode);
    /** room, red, blue, white - under a colourless programme */
    Q_INVOKABLE void slabSetColour(const QString &colour);
    /** 0 as made, 1 half the pace, 2 double */
    Q_INVOKABLE void slabSetSpeed(int speed);
    Q_INVOKABLE void slabToggleHold();
    /** STOP, and PLAY: the lab opens dark */
    Q_INVOKABLE void slabStop();
    /** ALL LAMPS ON - TEST */
    Q_INVOKABLE void slabToggleTest();
    /** R440_BARS_LAB: open STROBE LAB (0) or BARS LAB (1) */
    Q_INVOKABLE bool slabOpenKind(int kind);
    /** R440_ORDER: 0 ALL (A-Z), 1 MOST SEEN, 2 NOT JUDGED */
    Q_INVOKABLE void slabSetOrder(int order);
    /* ---- R440_COLOUR_LAB (track_runtime/colourlab.inc.cpp) ---- */
    Q_INVOKABLE bool clabOpen();
    Q_INVOKABLE void clabClose();
    Q_INVOKABLE void clabSetColour(const QString &colour);
    /** how bright this group is IN the lab's colour, 0.10-1.00 (1 = as made) */
    Q_INVOKABLE void clabSetLevel(const QString &key, qreal level);
    Q_INVOKABLE void clabResetColour(const QString &key);
    Q_INVOKABLE void clabCopyToAll(const QString &key);
    Q_INVOKABLE void clabSetTest(qreal level);
    /** tap a name: only that group lit - again: all of them */
    Q_INVOKABLE void clabSoloGroup(const QString &key);
    /** 0 ALL SIDE BY SIDE, 1 ONE BY ONE */
    Q_INVOKABLE void clabCompare(int mode);
    /** ALL ON / ALL OFF: the lab opens dark */
    Q_INVOKABLE void clabToggleDark();
    /* ---- R458_LAST_TRACK (track_runtime/lasttrack.inc.cpp) ---- */
    int lastTrack() const;
    bool lastTrackShown() const;
    int lastTrackLeft() const;
    int lastTrackCloseMin() const;
    QString lastTrackCloseAt() const;
    /** a tap (after SURE? on the page): arm, or undo at any stage */
    Q_INVOKABLE void toggleLastTrack();
    /** every 200 ms from closingTick(): the window, the house fade, the haze */
    void lastTrackTick(bool showOn);
    /* ---- R467_HAZE_NAG (track_runtime/hazenag.inc.cpp) ---- */
    int hazeNag() const;
    /** LATER: quiet for 30 minutes */
    Q_INVOKABLE void hazeLater();
    /** every 200 ms from closingTick() */
    void hazeNagTick(bool showOn);
    /* ---- R484_HZ_API: haze AUTO (track_runtime/hazenag.inc.cpp), its row in clockhand.inc.cpp ---- */
    bool hazeAuto() const;
    void setHazeAuto(bool on);
    /** every 200 ms (from hazeNagTick): haze and fan toward the row's point, a step at a time */
    void hazeAutoTick(bool showOn);
    QVariantList atmosCurve() const;
    /** a tap on a point's H (which 0) or F (which 1): +10 - the point is his from then on */
    Q_INVOKABLE void cycleAtmosPoint(int index, int which);
    /** his point given back to the learning (LEARN) */
    Q_INVOKABLE void learnAtmosPoint(int index);
    /* ---- R469_GROUP_FLASH (track_runtime/padflash.inc.cpp) ---- */
    QStringList flashGroups() const;
    /** the group's flash pad: on = held (again every second), off = let go */
    Q_INVOKABLE void flashGroup(const QString &key, bool on);
    /** every 200 ms from closingTick(): a hold nobody refreshed lets go */
    void padFlashTick();
    /* ---- R470_SYNC (track_runtime/sync.inc.cpp) ---- */
    bool syncActive() const;
    int syncStep() const;
    int syncTaps() const;
    QVariantList syncSound() const;
    QVariantList syncLight() const;
    int syncResult() const;
    int syncSpread() const;
    /** what the light is moved by, ms (+ later): TrackManager runs the beat by it */
    int syncOffset() const;
    Q_INVOKABLE void syncOpen();
    Q_INVOKABLE void syncClose();
    Q_INVOKABLE void syncTap();
    Q_INVOKABLE void syncRestart();
    Q_INVOKABLE void syncUse();
    Q_INVOKABLE void syncNudge(int ms);
    Q_INVOKABLE void syncReset();
    /* ---- R470_CLOCK_HAND (track_runtime/clockhand.inc.cpp) ---- */
    QVariantList clockHand() const;
    QStringList clockHandNights() const;
    /* ---- R484_EH_API (Tobias 10-20: "energi skal saette by hand i advanced ... men vi kan 'override'") ---- */
    QVariantList clockSource() const;
    /** a point of his own (a tap on it) given back to the learning */
    Q_INVOKABLE void learnClockPoint(int index);
    int clockView() const;
    void setClockView(int view);
    /** a tile of YOUR HAND (-1: USE YOURS) into ENERGY by clock */
    Q_INVOKABLE void useClockHand(int index);
    /* ---- R473_ATMOS (track_runtime/clockhand.inc.cpp) ---- */
    QVariantList atmosHand() const;
    QStringList atmosNights() const;
    bool atmosKnown() const;
    int atmosView() const;               // 0 the last 2 weekends, 1 every Fri + Sat, 2 the other nights
    Q_INVOKABLE void setAtmosView(int view);
    /* ---- R467_MIC_CHECK (track_runtime/miccheck.inc.cpp) ---- */
    bool micActive() const;
    QString micDevice() const;
    int micDeviceIndex() const;
    int micDeviceCount() const;
    QString micProblem() const;
    qreal micLevel() const;
    qreal micPeak() const;
    bool micClip() const;
    bool micSilent() const;
    QVariantList micBands() const;
    QVariantList micRoom() const;
    QVariantList micTrack() const;
    int micBeatTick() const;
    int micAgree() const;
    int micRecLeft() const;
    bool micAuto() const;
    QVariantList micRecs() const;
    Q_INVOKABLE void micOpen();
    Q_INVOKABLE void micClose();
    Q_INVOKABLE void micNextDevice();
    /** RECORD 30 s - a tap while it runs stops it (and keeps what is there) */
    Q_INVOKABLE void micRecord();
    /** EVERY SECTION TONIGHT - holds for this night only */
    Q_INVOKABLE void micSetAuto(bool on);
    /** every 200 ms from closingTick(): the capture only when something wants it */
    void micTick(bool showOn);
    /** from tick(): the link's beat, and EVERY SECTION TONIGHT */
    void micBeat(const QString &state, bool sectionChanged, qreal bpm);
    void micShutdown();
    /* ---- R445_DROP_HIT (track_runtime/drophit.inc.cpp) ---- */
    int hitKind() const;
    int hitLast() const;
    void setHitKind(int kind);
    /* ---- R443_PLAN_LANE (track_runtime/planlane.inc.cpp) ---- */
    bool planLane() const;
    void setPlanLane(bool on);
    /** sections: [{beat, end, type}] - per section {beat, end, type, mode, next, chips} */
    Q_INVOKABLE QVariantList planLaneOf(const QVariantList &sections, int cur);
    /** the next section's card: {key, in, base, note} per group of the night */
    Q_INVOKABLE QVariantList planRows();
    Q_INVOKABLE void planEngineDecides();
    Q_INVOKABLE void planForgetTrack();
    /** R484_PL_API (Tobias 10-19: "gennem hele tracket fremad"): a LATER section's card -
     *  its rows, a name in / out, ENGINE DECIDES. The next section's boundary goes to the
     *  RIG's plan (planRows / toggleDropGroup / planEngineDecides) as before */
    Q_INVOKABLE QVariantList planRowsAt(int boundary, const QString &section);
    Q_INVOKABLE void togglePlanAt(int boundary, const QString &section, const QString &key);
    Q_INVOKABLE void planEngineDecidesAt(int boundary, const QString &section);
    /* ---- R440_SEND (track_runtime/sendclaude.inc.cpp) ---- */
    Q_INVOKABLE QVariantList sendItems();
    Q_INVOKABLE bool sendMake(const QStringList &ids);
    Q_INVOKABLE void sendReset();
    bool slabAvailable() const;
    bool slabActive() const { return m_slabActive; }
    QString slabGroup() const;
    int slabLampCount() const;
    int slabTab() const { return m_slabTab; }
    int slabIndex() const { return m_slabIndex; }
    int slabCount() const { return int(m_slabList.count()); }
    QString slabTitle() const;
    QString slabDetail() const;
    QStringList slabChips() const;
    int slabVerdict() const;
    QVariantList slabStrip() const;
    int slabGood() const { return slabCountOf(1); }
    int slabNo() const { return slabCountOf(-1); }
    QVariantList slabTabCounts() const;
    int slabGoodAll() const { return slabAllOf(1); }
    int slabNoAll() const { return slabAllOf(-1); }
    int slabAuto() const { return m_slabAuto; }
    QString slabColour() const { return m_slabColour; }
    bool slabOwnColour() const;
    int slabSpeed() const { return m_slabSpeed; }
    bool slabCanSpeed() const;
    bool slabHold() const { return m_slabHold; }
    bool slabDark() const { return m_slabDark; }
    bool slabTest() const { return m_slabTest; }
    QString slabMessage() const { return m_slabMessage; }
    int slabKind() const { return m_slabKind; }               // R440_BARS_LAB
    int slabOrder() const { return m_slabOrder; }
    QStringList slabColours() const;
    QVariantList slabSummaries() const;
    bool clabActive() const { return m_clabActive; }           // R440_COLOUR_LAB
    QString clabColour() const { return m_clabColour; }
    QStringList clabColours() const;
    QVariantList clabRows() const;
    QVariantList clabMatrix() const;
    QString clabSolo() const { return m_clabSolo; }
    bool clabDark() const { return m_clabDark; }
    qreal clabTest() const { return m_clabTest; }
    QString clabMessage() const { return m_clabMessage; }
    int clabChanges() const { clabLoad(); return int(m_colourLevel.count()); }
    int sendState() const { return m_sendState; }              // R440_SEND
    QString sendResult() const { return m_sendResult; }
    /** kill a pending or running bar echo - AUTO off, blackout, a new track */
    void stopEcho();

    QVariantList groups();
    Q_INVOKABLE void setGroupEnabled(QString key, bool enable);
    Q_INVOKABLE bool groupEnabled(QString key) const;
    /** ON -> BASE -> OFF -> ON. The base group is always lit; the others
     *  are effects added on top as the evening's energy rises. */
    Q_INVOKABLE void cycleGroup(QString key);
    Q_INVOKABLE void toggleDropGroup(QString key);   // R431_DROP_PICK: a name in RIG
    QStringList planDropCast(qreal fader) const;     // R431_DROP_PICK: the engine's plan
    QStringList planCast(const QString &section, qreal fader) const;   // R432_NEXT_SECTION
    Q_INVOKABLE void toggleBase(QString key);
    QVariantMap trims() const;
    Q_INVOKABLE qreal groupTrim(QString key) const;
    /** R437_STROBE_START: where a new night starts this group's fader - the
     *  strobes at ENGINE_STROBE_START (R418_STROBE_HALF), the rest at 100 % */
    qreal startTrim(const QString &key) const;
    Q_INVOKABLE void setGroupTrim(QString key, qreal level);
    Q_INVOKABLE QString baseGroup() const;
    /** R430: a small LED wash - not the base, no movement, strobe, laser or
     *  pattern (the Mini 4 Eyes). Never the lead, never a budget place. */
    bool smallWash(const QString &key) const;

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
    /** R450_TEST_TRACK: the Track page's made-up track is playing - nothing
     *  is learned from it and nothing is written to the tracklog */
    // R511_OLD_TEST_TITLE (bug, harness r511 OLD): STOP TEST emptied the page but the engine kept the made-up
    // track's title - the LAST TRACK button stood over the empty page from 02:50, and BLT's "every fader down"
    // fallback (R484_LF_AUTO) could arm it on the stopped test track and put the house light up (haze and fan
    // to 0, haze AUTO off). The test track going off forgets it, as before any track; a real track that takes
    // over is loaded after the stop (R450_TT_BLT), so it is never the one forgotten
    void setTestTrack(bool on) { if (m_testTrack && on == false) m_trackTitle.clear(); m_testTrack = on; }
    bool testTrackOn() const { return m_testTrack; }
    bool stutter() const { return m_stutter; }          // R420_STUTTER
    void setStutter(bool on);
    int stutterRate() const { return m_stutterRate; }
    void setStutterRate(int rate);
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
    /** R379_ZERO_START: keepTiles - the DJ's own tiles stay (red only without) */
    void setStartScene(bool on, bool keepTiles);
    QString positionMode() const { return m_positionMode; }
    void setPositionMode(const QString &mode);
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
    // SAVED_LOOKS_API: complete recipes, independent of temporary function IDs.
    Q_PROPERTY(bool canUndoLook READ canUndoLook NOTIFY savedLooksChanged)
    Q_PROPERTY(QVariantList savedLooks READ savedLooks NOTIFY savedLooksChanged)
    Q_PROPERTY(QVariantList recentSavedLooks READ recentSavedLooks NOTIFY savedLooksChanged)
    Q_PROPERTY(QString savedFeedback READ savedFeedback NOTIFY savedLooksChanged)
    bool canUndoLook() const;
    QVariantList savedLooks() const;
    QVariantList recentSavedLooks() const;
    QString savedFeedback() const { return m_savedFeedback; }
    Q_INVOKABLE void undoLook();
    Q_INVOKABLE void queueSavedLook(const QString &id);
    Q_INVOKABLE void removeSavedLook(const QString &id);
    void savedTrackContext(const QString &title, const QVariantList &markers, int beat);
    Q_INVOKABLE void next();
    void advanceLook();                  // R424_NEXT_RATES: NEXT without its verdict (a thumb down's)
    bool creditVerdict(int verdict);     // R424_NEXT_RATES: the counting rate() always did

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
    QByteArray programmeOutputKey(Function *function) const;
    quint32 pickMotionWeighted(const QList<TrackFuncInfo *> &pool, int cursor) const;
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
    int hazeLimit() const;
    void setHazeLimit(int percent);
    qreal hazeRest() const;
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
    /** R461_ONSETS: the bass's and the mids' onsets per quarter beat (BLT, 4 nibbles) */
    void setTrackOnsets(const QVariantList &onsetLow, const QVariantList &onsetMid);
    /** Runde 359 (R359_PUMP): per beat how far the sound falls between two
     *  kicks - 1 - (the quietest stretch after the first quarter of the beat
     *  / the beat's attack), 0-255. */
    void setTrackPump(const QVariantList &pumpRaw);
    /** Runde 358 (R358_BRAKE): the deck's effective tempo from every status
     *  BLT sends - a brake shows as the tempo falling away under the grid's. */
    void setDeckTempo(qreal bpm);
    /** R479_LAST_WHITE_API: the followed deck's on-air from the DJM (BLT's "air" in its
     *  "pos", only when it has seen a mixer): 1 on air, 0 off air, -1 not known */
    void setDeckAir(int air);
    /** LAST_TRACK_FRESH_MIXER: source time of the actual DJM status, not a repeated pos. */
    void setDeckMixerTime(qint64 sourceMs);
    /** R484_LF_API: how many players' channels the DJM has on air (BLT's "faders" in its
     *  "pos", only when it has seen a mixer): 0 every fader down, -1 not known */
    void setDeckFaders(int onAir);
    void setDropForced(bool on) { m_dropForced = on; }   // R361_DROP_FORCED
    /** runde 322: one drop's absolute kick and bass into the history */
    void rememberDropPunch(qreal kick, qreal low);
    /** runde 335: the operator forced a section on the Track page (or let it
     *  go: state empty) - a sig: line in the tracklog, so a night's presses
     *  can be read as corrections of the analysis. Log only. */
    void noteSectionOverride(const QString &state, const QString &analysed);
    /** runde 429: a SECTION press let go by the analysis' next section
     *  (R429_SECTION_ONCE) - a sig: line, not a press. Log only. */
    void noteSectionRelease(const QString &held, const QString &now);
    /** R479_PRESS_KEEP_DECL: an analysis change right after a SECTION press kept it
     *  (the section he pressed for) - a sig: line, not a press. Log only. */
    void noteSectionKeep(const QString &held, const QString &now);
    void noteUpdate(const QString &script);   // R474_UPDATE: "sig:update:..." in the tracklog
    // R482_LOG_HAND_DECL (night review r478: "Telefon-, SHOW ON/OFF- og ENERGY-handlinger
    // logges ikke"): sig: lines in the tracklog - log only, nothing on stage changes
    void noteShow(bool on);                              // sig:show-on / sig:show-off
    void noteEnergyHand(int from, int to);               // sig:energy:<from>><to>, once it has settled
    void noteRemote(const QString &cmd, const QString &args);   // sig:remote:<cmd>[:<args>], a drag once
    // R486_MI_LOG: sig:midi:<what> - a pad, a fader taking over, LEARN (track_runtime/midimap.inc.cpp)
    void noteMidi(const QString &tag) { logSignal(QStringLiteral("sig:midi:") + tag); }
    // R475_ENGINE_PUBLIC: the section's own start before R236 shifts it (the colour memory), the
    // SECTION press held now (the pick's end - noteSectionOverride() only logs, R335), and who
    // the next setHaze() / setFan() is (the atmos log - "hand" when nobody says)
    void setSectionStart(int beat) { m_realSecStart = beat; }
    void setPressHeld(const QString &state) { m_pressHeld = state; }   // the SECTION press held, from the manager
    void setPressWas(const QString &state) { m_pressWas = state; }     // R479_BUILD_STROBE_2: the analysis' section under it
    void atmosNext(const QString &by) { m_atmosNextBy = by; }
    /** Nothing is playing but AUTO is on: run the start scene(s). */
    void idle();
    /** R463_LINK_IDLE: the link went quiet or BLT left - idle(), but an armed LAST
     *  TRACK is not over (only the deck stopping ends it). */
    void linkIdle();
    /** AUTO switched off: fade everything out over a bar, then let go. */
    void release();
    void stopAll();

signals:
    void settingsImported();             // MIDI_EDIT_IMPORT: consumers reload after successful import
    void tableChanged();
    void lastTrackChanged();             // R458_LAST_TRACK
    void hazeNagChanged();               // R467_HAZE_NAG
    void flashGroupsChanged();           // R469_GROUP_FLASH
    void syncChanged();                  // R470_SYNC
    void clockHandChanged();             // R470_CLOCK_HAND
    void atmosChanged();                 // R473_ATMOS
    void micChanged();                   // R467_MIC_CHECK
    void micRecChanged();
    void dropSetChanged();               // R431_DROP_PICK
    void liveChanged();
    void labChanged();                   // R410_LAB
    void tasteChanged();                 // R436_LEARNED
    void showCheckChanged();             // R436_SHOW_CHECK
    void slabChanged();                  // R436_STROBE_LAB
    void clabChanged();                  // R440_COLOUR_LAB
    void sendChanged();                  // R440_SEND
    void planLaneChanged();
    void savedLooksChanged();              // R443_PLAN_LANE
    void hitChanged();                   // R445_DROP_HIT
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
    QVector<qreal> livePatternMask(const QString &group, const TrackMove &move, qreal beat, qreal prog) const;
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
    // R504_MIRROR_DECL (runde 504): the heads' figures mirror left/right by the room (track_runtime/r504_mirror.inc.cpp)
    static bool r504MirrorOf(int shape, int rotation, int fx, int fy, bool *backward, int *c);
    void r504MirrorDraw(TrackSweep &sw, int heads, bool laser) const;
    bool r504MirrorWanted(const TrackSweep &sw) const;
    QVector<int> r504MirrorPlan(const QString &group, Function *func, const TrackSweep &sw, bool *backward, int *c);
    static void r504MirrorHead(int startOffset, bool backward, int c, bool &runBackward, int &offset);
    QHash<QString, TrackSweep> m_r504MirrorLogged;   // the figure sig:mirror was logged for, per group
    // R507_INF_DECL (runde 507): one marker per feature, each replaced by its owner
    // R507_DECL_WAVE
    // R507_WAVE_DECL (runde 507, WAVE): the kick wave - "each kick hits the lamps by the DJ first and
    // rolls out towards the back wall" (r507_wave.inc.cpp)
    QString r507WaveWash() const;                       // the wash it rolls on ("" none: no wave)
    bool r507WavePhases(const QString &key, QVector<qreal> *phase) const;   // 0 by the DJ .. 1 the back wall
    bool r507WaveReady() const;                         // dropFormDecide: the map knows the wash whole
    bool r507WaveFree(const QString &key) const;        // his picks and the room's other owners: no wave
    qreal r507WaveMaxMs() const;                        // the back wall's delay at this tempo
    bool r507WaveLand(int beat, qreal fader);           // the drop forms' landing: the kick-wave drop
    bool r507WaveHis() const;                           // his hand took the section: the wave ends
    bool r507WaveTakes(const QString &key) const;       // the kick-wave drop draws this wash itself
    void r507WaveMove(const QString &key, TrackMove &mv) const;   // ... every head lit, a pulse a kick
    void r507WaveBeat(int beat, bool isDrop, int beatInBar, qreal fader, bool hold, bool isCalm, bool still,
                      bool forceNext);
    int r507WavePartOf(const QString &slot) const;      // a dimmer part's index from its slot
    qreal r507WaveRatio(const QString &group, int index);   // a head's pulse at its own time / the group's
    void r507WaveStop(const QString &why);
    int m_r507WaveFor = -1;                     // the kick-wave drop's landing beat (-1 none)
    QString m_r507WaveTitle;                    // ... of this track
    QString m_r507WaveKey;                      // the wash it rolls on
    bool m_r507WaveOn = false;                  // it rolls on this beat's hit
    bool m_r507WaveLogged = false;
    QVector<qreal> m_r507WavePhase;             // per part: 0 the row by the DJ .. 1 the back wall
    qint64 m_r507WaveCur = -1;                  // the group's pulse start last seen ...
    qint64 m_r507WavePrev = -1;                 // ... and the one before (a late head shows its tail)
    qreal m_r507WaveCurStrength = 1.0;
    qreal m_r507WavePrevStrength = 1.0;
    // R507_DECL_SUCK
    // R507_SUCK_DECL (runde 507, SUCK): the build sucks the room in - "in the build's last 8 bars the
    // room goes dark from the back wall forwards ... then the drop lands on everything at once" (r507_suck.inc.cpp)
    bool r507SuckPlanOk(int at) const;                  // the drop's plan and his picks allow it
    bool r507SuckFree() const;                          // his hand and the room's other owners: no suck
    bool r507SuckLamps(qreal fader);                    // the wash + strobes, their y, the dark masks
    void r507SuckBeat(int beat, bool isDrop, bool isBuild, const QString &nextState, int beatsToNext,
                      qreal fader, bool hold, bool isCalm, bool still, bool forceNext);
    void r507SuckStart();                               // tick(), the beat drawn: this beat's wall
    bool r507SuckFrame(qint64 now);                     // the pulse timer and reapplyLevels
    void r507SuckApply();                               // the masks for the beat it is in
    void r507SuckStop(const QString &why);              // every mask let go
    int m_r507SuckFor = -1;                     // the drop it runs into (-1 none)
    int m_r507SuckDecidedFor = -1;              // the drop the draw was made for (once a drop)
    QString m_r507SuckTitle;
    bool m_r507SuckPrev = false;                // the last build drawn in this track was sucked
    bool m_r507SuckDrawn = false;               // drawn for m_r507SuckDecidedFor, not started yet
    int m_r507SuckFrom = -1;                    // the window's first beat ...
    int m_r507SuckLast = -1;                    // ... and its last lit beat (the front row alone)
    int m_r507SuckBeatNow = -1;                 // the beat it is in
    qint64 m_r507SuckMs = -1;                   // ... that beat's start, once tick() has drawn it
    bool m_r507SuckPaused = false;              // a hit scene of his show this beat: masks off for it
    QString m_r507SuckWash;
    QString m_r507SuckStrobeKey;                // "" the wash alone
    qreal m_r507SuckFront = 0.20;               // the front row's edge (y)
    QVector<quint32> m_r507SuckFx;              // the lamps: the wash, then the strobes
    QVector<qreal> m_r507SuckY;                 // ... where they hang (0 the DJ .. 1 the back wall)
    QVector<int> m_r507SuckDarkAt;              // ... the beat each goes dark (-1 never: the front row)
    QVector<quint32> m_r507SuckDark;            // ... its dark mask (hidden scene, flashed)
    QVector<bool> m_r507SuckShown;
    QString m_r507SuckCache;                    // the lamps the masks were made for
    // R507_DECL_DUEL
    // R509_INF_DECL (runde 509): one marker per feature, each replaced by its owner
    // R509_DECL_PAUSE
    // R509_PAUSE_DECL (runde 509, PAUSE): the pause hit - the room dark on an abrupt stop inside a drop or
    // a groove, the first beat back a full-room hit, "ikke altid hvid, ogsaa i rummets farve" (r509_pause.inc.cpp)
    bool r509PauseStopAt(int s, int *back, int *before) const;   // a stop starts on s: its comeback beat
    int r509PauseFind(int beat, int *from, int *back) const;     // 1 a stop beat, 2 its comeback, 0 neither
    QString r509PauseWash() const;                      // the wash the hit lands on
    QString r509PauseStrobes() const;                   // the strobe bank ("" none)
    bool r509PauseFree() const;                         // his hand and the room's other owners: no pause
    bool r509PauseOn() const;                           // R509_PAUSE_QUERY: a pause's dark or hit is running
    QString r509PauseAccent(int beat) const;            // the room's accent (a harmony partner)
    bool r509PauseBeat(int beat, const QString &state, const QString &nextState, int beatsToNext, qreal fader,
                       bool hold, bool isCalm, bool still, bool musicDarkOk);   // tick(): this beat dark? (the hit armed)
    void r509PauseArm(int beat, const QString &state, qreal fader);   // the comeback's colour
    void r509PausePlan();                               // tick()'s stabs: the beat before a stop darkens its line
    void r509PauseLamps();                              // the wash + strobes
    bool r509PauseMasks(const QString &washHue, const QString &strobeHue);   // the per-lamp masks
    Scene *r509PauseScene(const QHash<QString, quint32> &named, quint32 id, const QString &name);   // one mask
    void r509PauseStart();                              // tick(), the beat drawn: the hit's first frame
    void r509PauseStrobe(int beat, bool quiet, bool affordable, bool burst);  // driveStrobe: one blink, one burst
    bool r509PauseFrame(qint64 now);                    // the pulse timer and reapplyLevels
    void r509PauseStop();                               // every mask let go
    void r509PauseReset();                              // SHOW OFF, idle, stopAll
    QString m_r509PauseTitle;                        // the track the stops were read for
    int m_r509PauseState = 0;                        // 0 none, 1 the hit armed, 2 the hit lit
    int m_r509PauseFrom = -1;                        // the stop being darkened: its first beat ...
    int m_r509PauseBack = -1;                        // ... its comeback beat
    int m_r509PauseBefore = 0;                       // ... and the level of the 8 beats before it
    bool m_r509PauseInDrop = false;                  // ... which was in a drop
    int m_r509PauseDarkLast = -1;                    // the last beat a pause darkened
    int m_r509PausePreFor = -1;                      // the stop the chop plan darkens into (its first beat)
    int m_r509PauseHitBeat = -1;                     // the comeback beat the hit is for
    qint64 m_r509PauseMs = -1;                       // ... its start, once tick() has drawn it
    qreal m_r509PauseFader = 0.0;
    QString m_r509PauseHue;                          // the hit's colour
    QString m_r509PauseHueKind;                      // accent, white, tile, room
    bool m_r509PauseHitWhite = false;
    int m_r509PauseHits = 0;                         // pause hits this night ...
    int m_r509PauseWhiteAt = -100;                   // ... the last white one's number
    qint64 m_r509PauseWhiteMs = -1;                  // ... and its clock
    bool m_r509PauseWithStrobes = false;             // the strobes blink with this hit
    bool m_r509PauseStrobeAsked = false;
    bool m_r509PauseStrobeMasks = false;             // the strobes have masks in the hit's colour
    QString m_r509PauseWashKey;
    QString m_r509PauseStrobeKey;
    QVector<quint32> m_r509PauseFx;                  // the lamps: the wash, then the strobes
    QVector<bool> m_r509PauseIsStrobe;
    QVector<quint32> m_r509PauseLit;                 // per lamp: its colour mask ...
    QVector<quint32> m_r509PauseDim;                 // ... its dimmer mask (carries the level) ...
    QVector<quint32> m_r509PauseDark;                // ... and a strobe's dark one
    QVector<int> m_r509PauseShown;                   // 0 none, 1 dark, 2 lit
    QString m_r509PauseCache;                        // what the masks were filled for
    // R509_DECL_BARS
    // R509_BARS_DECL (runde 509, BARS): the laser bars count down to the drop and follow the
    // arpeggio - "kun som et program motoren kan traekke" (r509_bars.inc.cpp)
    QString r509BarsKey() const;                        // the laser bars ("" none)
    QList<int> r509BarsOrder(const QString &key) const; // their parts from the back wall to the DJ
    QString r509BarsBusy(const QString &key) const;     // who else has the bars: "his" / "room" / "" (ours may)
    bool r509BarsCountPlanOk(int at) const;                 // the drop's plan allows a countdown
    int r509BarsArpNotes(int beat) const;                   // the quarters of a beat with a note (-1 unknown)
    bool r509BarsArpLine(int from) const;                   // a sixteenth line over the coming 8 beats
    bool r509BarsLive() const;                          // a countdown / an arpeggio holds the bars now
    bool r509BarsWhole(const QString &key) const;       // tick(): no AUTO programme on them
    void r509BarsMove(const QString &key, TrackMove &mv) const;   // tick(): their still picture, our mask
    int r509BarsCountLit(int beat) const;                   // the countdown's bars lit on a beat
    int r509BarsArpStep(qint64 now) const;                  // the arpeggio's step at this moment
    QVector<qreal> r509BarsMask(const QString &group) const;      // patternMask: the parts lit
    bool r509BarsArpScenes();                               // the eye pairs' hidden scenes (from 75 %)
    void r509BarsArpEyes(int pos);                          // the pair flashed (-1 none)
    bool r509BarsFrame(qint64 now);                     // the pulse timer
    void r509BarsStart();                               // tick(), the beat drawn
    void r509BarsStop(const QString &why);
    void r509BarsBeat(QSet<QString> &castSet, const QStringList &eligible, const QString &state, int beat,
                      int secStart, int secEnd, bool isDrop, bool isBuild, const QString &nextState,
                      int beatsToNext, qreal fader, qreal kick, bool hold, bool isCalm, bool still,
                      bool silent, bool forceNext);
    static constexpr int r509BarsMaskCount = 509;           // TrackMove::formMask: the countdown ...
    static constexpr int r509BarsMaskArp = 510;             // ... and the arpeggio (over the drop forms' 1-3)
    int m_r509BarsMode = 0;                         // 0 none, 1 the countdown, 2 the arpeggio
    QString m_r509BarsKey;                          // the bars it holds
    QString m_r509BarsTitle;                        // ... in this track
    int m_r509BarsBeat = -1;                        // the beat it was last asked on
    int m_r509BarsPre = -1;                         // the first beat it holds the bars (dark before the first step)
    int m_r509BarsAt = -1;                          // the countdown: the drop it counts to
    int m_r509BarsFirst = -1;                       // ... its first lit step
    int m_r509BarsSteps = 0;                        // ... its steps
    int m_r509BarsPeriod = 1;                       // ... beats a step
    bool m_r509BarsTwo = false;                     // ... from 75 %: the two back bars to start
    int m_r509BarsCountFor = -1;                    // the drop the countdown was drawn for (once a drop)
    bool m_r509BarsCountPrev = false;               // the last build drawn counted down (never two running)
    int m_r509BarsArpSec = -1;                      // the break the arpeggio was drawn for (once a break)
    bool m_r509BarsCountWon = false;                // the countdown's ticket won, waiting for the room
    bool m_r509BarsArpWon = false;                  // the arpeggio's ticket won, waiting for the room
    QString m_r509BarsWhy;                          // the last reason a won ticket waited (the crumb once)
    int m_r509BarsArpFrom = -1;                     // its first beat
    int m_r509BarsArpBase = 0;                      // the steps before this beat
    int m_r509BarsArpMask = 0;                      // this beat's steps (quarter bits)
    int m_r509BarsArpLastQ = -100;                  // the last step (beat * 4 + quarter)
    int m_r509BarsArpQuiet = 0;                     // beats in a row without a line
    int m_r509BarsKicks = 0;                        // kick beats in a row (the raw kick)
    bool m_r509BarsEyes = false;                    // the arpeggio's eye pairs (from 75 %)
    bool m_r509BarsEyeOk = false;                   // ... their scenes are made
    QList<int> m_r509BarsOrder;                     // the bars' parts from the back wall
    QVector<quint32> m_r509BarsEyeIds;              // the 7 pair scenes
    QString m_r509BarsEyeCache;                     // ... made for this colour scene
    int m_r509BarsEyeShown = -1;                    // the pair flashed (-1 none)
    // R509_DECL_SPLIT
    // R509_SPLIT_DECL (runde 509, SPLIT): the mix in two halves, "men kun nogle gange" (r509_split.inc.cpp)
    bool r509SplitActive() const;                       // the room is split between two decks now
    bool r509SplitOwns(const QString &key) const;       // the split paints this strobe bank's colour
    void r509SplitStop(const QString &why);             // lets go (logged when it ran)
    int r509SplitTarget(const QString &key, int target, int n, int beat, bool frozen);   // the curtain's border
    void r509SplitBeat();                               // every beat, after the curtain's
    quint32 r509SplitColour(const QString &key, const QString &colour);   // the strobes by the border
    bool m_r509SplitOn = false;                 // this mix is a split
    QString m_r509SplitKey;                     // the curtain's group it rides on
    QString m_r509SplitTitle;                   // the outgoing track it was drawn in
    int m_r509SplitMixes = 0;                   // mixes counted in that track (the dice)
    int m_r509SplitStrobesIn = -1;              // strobe heads in the new colour, last logged
    // R510_MASKS_SPLIT_DECL (runde 510, MASKS; Tobias 10-28: "HOLD skal springe tilbage")
    bool r509SplitHeld() const;                         // HOLD on a split: the outgoing colour shown whole
    bool m_r509SplitHeldLog = false;            // ... as last logged (sig:split:hold-back / hold-off)
    // R509_DECL_DRAMA
    // R509_DRAMA_DECL (runde 509, DRAMA): the night's budget of big moments, the last drop is biggest
    // (r509_drama.inc.cpp) - weights on the existing draws, no new effects
public:
    void r509DramaFlags(const QVector<int> &drops);     // trackmanager, every tick: the track's drop starts
protected:
    void r509DramaBeat(int beat, qreal fader);          // tick(): every beat, before the drop's form is decided
    int r509DramaAhead(int pos) const;                  // drops still ahead of pos (-1 unknown)
    qreal r509DramaWeight(int pos) const;               // night x the drops ahead of pos (< 0: night only): suck, duel, rush
    qreal r509DramaForm(int at, bool his, bool *firstDrop);   // dropFormDecide: the big tickets
    bool r509DramaWhiteOk() const;                      // the white landing's gap, longer in the warm-up
    qint64 r509DramaSecs() const;                       // real seconds since the evening's 20:00, -1 outside
    qreal r509DramaClockShare(qint64 secs) const;       // ENERGY by clock then over its top, -1 unknown
    void r509DramaSpend(const QString &what);           // a big moment played: one token
    void r509DramaOut(const QString &title, int ahead); // the DJ left a track: early or after its last drop
    QVector<int> m_r509Drops;                   // the current track's drop starts (trackmanager)
    QString m_r509Night;                        // the night the budget is for (nightKey)
    qreal m_r509Tokens = 2.0;                   // big moments in hand
    qint64 m_r509TokMs = -1;                    // the last refill (m_clock)
    qreal m_r509Peak = 1.0;                     // this beat's peak, 0 warm-up .. 1
    qreal m_r509NightW = 1.0;                   // this beat's night weight (1 = the draws as before)
    int m_r509Quarter = -1;                     // the quarter hour last logged
    int m_r509Tracks = 0;                       // tracks left tonight after a heard drop ...
    int m_r509Early = 0;                        // ... with a drop still ahead (a mix-out before the last drop)
    QString m_r509Counted;                      // the track already counted
    QString m_r509Title;                        // the track the beat belongs to
    bool m_r509Landed = false;                  // ... landed a drop
    bool m_r509Mix = false;                     // a mix was on last beat
    int m_r509AheadNow = -1;                    // drops ahead of the last beat
    int m_r509LandSeen = -1;                    // the moments already spent: a form landing,
    QString m_r509LandTitle;
    int m_r509SuckSeen = -1;                    // ... a suck,
    int m_r509DuelSeen = -1;                    // ... a duel,
    QString m_r509DuelTitle;
    int m_r509RushSeen = -100000;               // ... a phrase rush,
    qint64 m_r509WhiteSeen = -1;                // ... a white landing
    // R509_DECL_COLOUR
    // R509_COLOUR_DECL (runde 509, COLOUR): the white kick on the wash and colour follows the theme
    // (r509_colour.inc.cpp)
    QString r509ColourKickWash() const;
    bool r509ColourKickFree(const QString &key) const;
    quint32 r509ColourKickScene(const QString &key);
    void r509ColourKickStop(const QString &why);
    void r509ColourKickBeat(int beat, bool isDrop, int dropBar, int beatInBar, qreal fader, bool hold,
                            bool isCalm, bool still);
    bool r509ColourKickFrame(qint64 now);
    QString m_r509KickKey;                      // the wash being tinted ("" none)
    quint32 m_r509KickScene = 0xFFFFFFFFu;      // the hidden tint scene (Function::invalidId())
    QString m_r509KickSceneFor;                 // ... made for this wash and these heads
    qint64 m_r509KickMs = -1;                   // the tint now falling (m_clock), -1 none
    int m_r509KickBeat = -100;                  // the last tinted beat
    int m_r509KickCount = 0;                    // tints in this drop
    int m_r509KickTop = 255;                    // the track's loudest beat
    QString m_r509KickTopFor;
    int m_r509KickTopN = -1;
    bool r509ThemeAuto() const;
    bool r509ThemeOk(const QString &colour) const;
    void r509ThemeSeen(const QString &section, int from, const QVector<qreal> &sig);
    QString r509ThemeWant() const;
    bool r509ThemeForce(int beat, bool sectionChanged, bool forceNext, bool isCalm, bool sectionSoon,
                        bool isDrop, int secStart);
    void r509ThemePool(QStringList &pool) const;
    QString r509ThemeDrawn(const QString &drawn, bool forceNext);
    void r509ThemeBeat(int beat, bool changeColour, bool forceNext);
    QString m_r509ThemeTitle;                   // the track the themes' colours belong to
    QHash<QString, QString> m_r509ThemeColours; // "<section>#<n>" -> its colour
    QHash<QString, QList<QVector<qreal> > > m_r509ThemeSigs;   // per section type: each theme's sound
    QHash<QString, QList<QList<int> > > m_r509ThemeFroms;      // ... and the section starts it played at
    QString m_r509ThemeKey, m_r509ThemeCtx;     // the theme arriving now (its motif context)
    QString m_r509ThemeCurKey, m_r509ThemeCurCtx;   // the theme playing (NEXT inside it)
    bool m_r509ThemeBack = false;               // ... returning (heard before in this track)
    bool m_r509ThemePending = false;            // seen this beat, not yet recorded
    // R509_DECL_WANDER
    // R509_WANDER_DECL (runde 509, WANDER): a warm spot wanders round the room in long grooves,
    // "men kun nogle gange" (r509_wander.inc.cpp)
    QString r509WanderWash() const;                     // the wash it runs on ("" none / no map)
    bool r509WanderFree(const QString &key) const;      // his hand and the room's other owners: no wander
    bool r509WanderActive() const;                      // R509_WANDER_QUERY: a spot wanders now
    bool r509WanderScenes(const QString &key);          // one hidden FILTER scene per head
    bool r509WanderStart(const QString &key, int beat); // the path from the map, the scenes on
    void r509WanderBeat(const QString &state, int beat, int secStart, int secEnd, bool hold, bool isCalm,
                        bool still, bool forceNext);     // tick(): the draw, his hand, the section's end
    bool r509WanderFrame(qint64 now);                   // the pulse timer: each head's level now
    void r509WanderStop(const QString &why);            // every scene let go (logged when it ran)
    bool m_r509WanderOn = false;                // a spot wanders now
    bool m_r509WanderWon = false;               // this section's dice said yes
    bool m_r509WanderNoMore = false;            // NEXT ended it: not again in this section
    bool m_r509WanderFirst = false;             // still coming in (2 bars), not back (1 bar)
    QString m_r509WanderTitle;                  // the track the section's draw belongs to
    int m_r509WanderSec = -1;                   // the section drawn (its first beat)
    int m_r509WanderBeat = -1;                  // this tick's beat
    int m_r509WanderFrom = -1;                  // the beat it started on (the spot's clock)
    int m_r509WanderPause = -1;                 // a beat of the music's stop or its comeback
    int m_r509WanderLap = 12;                   // bars a lap
    int m_r509WanderDir = 1;                    // 1 clockwise seen from the DJ, -1 the other way
    int m_r509WanderHot = -1;                   // the head at full, last logged
    qreal m_r509WanderOff = 0.0;                // where it starts (0, .25, .5, .75 of a lap)
    qreal m_r509WanderPresence = 0.0;           // 0 the engine's own picture .. 1 the spot at full depth
    qint64 m_r509WanderMs = -1;                 // the last frame
    qreal m_r509WanderBox[4] = { 0.0, 0.0, 1.0, 1.0 };   // x0, y0, x1, y1 of the heads
    QString m_r509WanderKey;                    // the wash
    QString m_r509WanderAside;                  // what it stands aside for now ("" nothing)
    QList<quint32> m_r509WanderFx;              // the wash's fixtures, in the group's order
    QVector<qreal> m_r509WanderX;               // their place on the map
    QVector<qreal> m_r509WanderY;
    QVector<quint32> m_r509WanderScene;         // the filter scene per head
    QVector<int> m_r509WanderAttr;              // its intensity override
    QVector<qreal> m_r509WanderShown;           // the level it has now
    // R507_DUEL_DECL (runde 507, DUEL): the animation lasers answer each other - "one plays a bar/phrase,
    // the other answers, and on the phrase's climax both" (r507_duel.inc.cpp)
    QString r507DuelGroup() const;                      // the animation laser group of two ("" none)
    bool r507DuelHis() const;                           // his hand took the section: no duel
    bool r507DuelFree(const QString &key) const;        // FLASH, pads, BLACKOUT, his programme: a pause
    void r507DuelSet(int side, int beat);               // one change of side
    void r507DuelBeat(int beat);                        // a duel whose laser left the cast ends
    void r507DuelWake(const QString &key, int beat);    // a laser lit again: dark before move
    void r507DuelMove(const QString &key, TrackMove &mv, const QString &state, int beat, int secStart,
                      int beatsToNext, bool isDrop, int dropBar, int beatInBar, qreal fader, bool hold,
                      bool isCalm, bool still, bool forceNext);
    void r507DuelStatic(TrackMove &mv) const;           // the group's own mask static while it duels
    int r507DuelPartOf(const QString &slot) const;      // a dimmer part's index from its slot
    bool r507DuelDark(const QString &group, int index) const;   // the answering laser: dark
    void r507DuelStop(const QString &why, int softAt);
    bool m_r507DuelOn = false;                  // a duel plays in this section
    QString m_r507DuelKey;                      // the animation laser group
    QString m_r507DuelTitle;                    // ... of this track
    int m_r507DuelSec = -1;                     // the section it plays in
    int m_r507DuelFrom = -1;                    // its first beat
    int m_r507DuelSide = 2;                     // 0 the first laser lit, 1 the second, 2 both (the engine's)
    int m_r507DuelSpan = 2;                     // bars a turn
    int m_r507DuelFirst = 0;                    // the laser that opens the first phrase
    int m_r507DuelEndAt = -1;                   // a soft end: the beat it ended (the dark laser back on a hit)
    int m_r507DuelSwaps = 0;
    int m_r507DuelDrawn = -1;                   // the section last drawn for ...
    QString m_r507DuelDrawnTitle;               // ... in this track
    int m_r507DuelRanSec = -1;                  // the section it last played ...
    QString m_r507DuelRanTitle;                 // ... in this track (never two sections running)
    int m_r507DuelSeen = -1;                    // the last beat the group was in the cast
    bool m_r507DuelHit = false;                 // that beat had the animation laser's strict hit
    int m_r507DuelWake = -1;                    // a laser a change lights again, still dark (-1 none)
    int m_r507DuelWakeLog = -1;
    QString m_r507DuelFids;                     // the group's mot:/col:/pos: on ...
    int m_r507DuelFidsBeat = -1;                // ... this beat
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
    TrackBarCoverage barCoverageOf(Function *func, const QSet<quint32> &touched) const;
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
    quint32 drawAim(const QString &group, const QList<TrackFuncInfo *> &pool) const;   // R405_AIM_DRAW
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
    void qualityBeat();
    void qualitySignal(const QString &tag);
    void qualityFinish(const QString &reason);

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
    // R460: 7 the fall after a groove, 8 a vocal over a drop, 9 the section's timbre
    int m_fallFrom = -1;                 // R460_FALL: the first beat of the fall, -1 none
    int m_fallLen = 0;
    qreal m_fallH0 = 0.0;                // the highs where it began
    qreal m_fallAheadMul = 1.0;          // ... and the groove's own last beats when it starts before the line
    int m_dropVocalBeat = -1;            // R460_DROP_VOCAL
    int m_dropVocalSec = -1;             // the drop its median is of
    qreal m_dropVocalMed = 0.0;
    int m_dropVocalRun = 0;
    bool m_dropVocal = false;
    int m_timbreBias = -1;               // R460_TIMBRE: the last room draw's section - -1 none, 0 dark, 1 bright
    int m_timbreDraw = -1;               // ... read by drawColour() during the room draw only
    qreal m_timbreMed = -1.0;            // the track's own median (highs / mids), -1 not yet
    qreal audHighN(int beat) const;
    int fallLength(int beat, qreal *h0, int *peakAt = nullptr) const;   // R463_FALL_AHEAD: and where the peak was
    int sectionTimbre(int from, int to);
    int m_colourBar;          // -1: a fresh track, hold the colour until a break or drop
    int m_colourSince;        // beat of the last colour change
    bool m_colourMixHeld = false;   // R479_MIX_COLOUR: a break or drop ended the fresh-track hold while a mix blocked it
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
    QHash<QByteArray, qint64> m_recentOutput; // identical outputs share their cooldown
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
    // R479_AIM_MS (night review 10-17): the same stamp on the wall clock
    // (m_clock ms). Never cleared: a deck handover resets the beat numbers.
    QHash<QString, qint64> m_aimMs;
    QHash<QString, int> m_darkUntil;     // group -> last beat of a planned dark stretch
    // R503_SAFE_AIM_MS (runde 503, SAFE): when a laser bar group's mirror last moved (m_clock ms) and the
    // aim it moved to (its pos: fid; none for a figure's stop or lift). tick() keeps the group dark 150 ms
    // after it, and until the MasterTimer has written that aim twice (at most 1 s). Never cleared: a stamp
    // older than a second does nothing.
    QHash<QString, qint64> m_r503AimMs;
    QHash<QString, quint32> m_r503AimFid;
    // R507_SAFE_DECL (runde 507, SAFE; Tobias: "never the laser bars and an animation laser lit together",
    // "never move a lit laser - the light goes off BEFORE a mirror moves"). A laser group held dark on the
    // wall clock, as R503_SAFE_DARK holds the bars after a re-aim: 150 ms, and until what had to go is
    // provably off the DMX (its functions stopped, then two MasterTimer frames) and, on an animation laser,
    // its new picture (col:/mot: scene) has been written twice. At most 1 s. Only ever darkens.
    struct R507SafeGate
    {
        qint64 ms = 0;                   // m_clock ms of the last reason
        qint64 clearMs = -1;             // first ms every function in `gone` was seen stopped
        QList<quint32> gone;             // what must be off the DMX first (the other laser type, an old picture)
        bool hit = false;                // the gate kept light off: relight when it opens
    };
    QHash<QString, R507SafeGate> m_r507Safe;
    void r507SafeHold(const QString &group, quint32 gone);       // a reason: dark from NOW
    bool r507SafeClosed(const QString &group);                    // still held?
    bool r507SafeDark(const QString &group);                      // the level hooks: true = write 0
    bool r507SafeFrame();                                         // the pulse timer: open, relight
    void r507SafeOut(const QString &slot, quint32 fid);           // stopSlot(): a laser function goes
    void r507SafeHandover(const QSet<QString> &castSet);          // tick(): the outgoing laser type first
    void r503SafeAim(const QString &group, quint32 aim)
    {
        if (group.isEmpty() || m_groups.contains(group) == false)
            return;
        const TrackGroup &g = m_groups[group];
        if (g.lasers == false || g.patternDevice)
            return;                          // the bars only: the animation laser has no aim to move
        m_r503AimMs.insert(group, m_clock.elapsed());
        m_r503AimFid.insert(group, aim);
    }
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
    bool m_layerFirst = false;             // R419_FIRST_STEP: a FADE/CHASE press not yet answered
    qreal m_layerFirstFrom = -1.0;         // R419_FIRST_STEP: the step it was pressed on
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
    int m_mixGlideEnd = -1;                // R375_GLIDE_END: the beat the mix ended on
    bool m_mixGlideFinish = false;         // R376_GLIDE_FINISH: adopted at the handover, finishing
    QString m_mixGlideFrom, m_mixGlideTo, m_mixGlideKey;
    void applyMixGlide(bool frame);
    /* R378_POSITIONS: a position chosen on the page holds the moving heads */
    QString m_positionMode;
    QHash<QString, quint32> m_holdScenes;  // "group|mode" -> the hold scene
    /* R383_BARS_ALONE: this beat's break has the laser bars alone */
    // C20261022_IDENTITY: a musical motif, not a forced old programme/energy.
    struct TrackMotif {
        int from = -1, rhythm = 0, visits = 0;
        QVector<qreal> sig;
        QHash<QString, int> shapes;
        QHash<QString, QString> families;
    };
    QHash<QString, QVector<TrackMotif>> m_motifs;
    QHash<QString, int> m_motifShapes;
    QHash<QString, QString> m_motifFamilies;
    QString m_motifContext;
    int m_motifPick = -1, m_motifVariation = 0, m_chaseRhythm = 0;
    bool m_motifAllowed = false;
    void motifStart(int from, const QString &section, bool allowed, bool force);
    TrackMove motifMove(const QString &group, TrackMove move) const;
    void motifRecord(int beat, const QString &section);
    bool m_autoFade = false, m_autoFadePrevious = false;
    QString m_autoFadeFrom, m_autoFadeTo;
    QSet<QString> m_autoFadeOwned;
    int m_autoFadeBeat = -1;
    qreal m_autoFadeP = 0.0, m_autoFadeRate = 0.0;
    bool autoFadeGroup(const QString &key) const;
    void autoFadeStop();
    void autoFadeTick(int beat, const QString &previous, bool soft, bool frozen, bool hard);
    void applyAutoFade(const QString &key, bool frame);
    int m_dropFlow = 0; // C20261020: attack/breadth, chase, later climax
    int m_previousFlow = -1;
    int m_previousStyle = -1;
    QString m_previousFlowTrack;
    QString m_dropLastColour;
    QHash<QString, QString> m_dropLastFamilies;
    bool m_quietSpatial = false;
    qreal m_quietSpatialBeat = 0.0;
    bool m_hitReturning = false;
    QString m_strobeFocus; // one automatic burst's principal group
    bool m_barSingleDraw = false; // C20261020: single eyes are a minority of full-energy drops
    bool m_barDetailDraw = false; // one class draw per section / energy redraw
    bool m_barStrobeDraw = false; // C20261020_MUSICAL: rare, only a fully lit bank
    int m_softRedrawBeat = -1;    // a small figure change follows colour by a bar
    int m_dialogueStart = -1;
    int m_dialogueBeat = -1;
    bool m_dialoguePrevious = false; // never two neighbouring sections
    QString m_dialogueCall, m_dialogueAnswer;
    qreal dialogueGain(const QString &group) const;
    bool m_barsAloneNow = false;
    /* R384_ALONE_LATCH: the section started at 90 % on the slider */
    bool m_aloneArmed = false;
    /* R385_BARS_FULL: this alone break is the full-light one (every second) */
    bool m_aloneFull = false;
    int m_aloneBreaks = 0;
    // R401_CAST_DRAW / R401_SPECIAL_DRAW / R401_ALONE_KIND: drawn per section
    QStringList m_castOrder;              // the effect groups' order this section
    QMap<QString, int> m_castRest;        // sections each group has been off stage
    bool m_drawAlone = false;             // this break may be the bars alone
    bool m_drawHome = false;              // this section the bars may hold home
    bool m_drawGrooveStrobes = false;     // this groove the strobes may lead
    int m_aloneWait = 0;                  // occasions missed since the last one
    // R405_AIM_DRAW: per head group, how many aims it has taken, when (in that
    // count) each aim was last taken, and the aim it took last
    QHash<QString, int> m_aimSeq;
    QHash<QString, QHash<quint32, int>> m_aimTakenAt;
    QHash<QString, quint32> m_aimLast;
    int m_homeWait = 0;
    int m_grooveWait = 0;
    bool m_aloneFullLast = false;         // the kind of the last alone break
    bool m_dropStrobesOnly = false;       // R401_DROP_LEAD: this drop has shown the strobes alone beside the base
    bool m_lastDropStrobesOnly = false;   // ... and the drop before it did
    bool m_dropLeadYields = false;        // this drop the strobes do not take the first place
    bool varietyDraw(int &wait, QRandomGenerator *rng) const;
    bool positionHeld(const QString &key) const;
    quint32 headHoldFunction(const QString &key);
    bool heldAimBlocks(const QString &key, const TrackFuncInfo &info) const;   // R400_HOLD_AIM
    void applyHeadHold(bool restart);
    /* R378_START_LAYER: the opening picture's fade/chase runs on a clock */
    qint64 m_startLayerMs = 0;
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
    QString m_qualityTrack;
    QMap<QString, int> m_qualityCounts;
    QMap<QString, QByteArray> m_qualityLast;
    QMap<QString, QSet<QByteArray>> m_qualitySeen;
    QByteArray m_qualityLook;
    QSet<QString> m_qualitySingle;
    QMap<QString, qint64> m_qualityGestureAt;
    bool m_qualityWriting = false;
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
    qint64 m_lastNextMs = -1;            // R428_NEXT_SALVO: when NEXT was last pressed (m_clock)
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
    QHash<QString, int> m_sweepDrawn;            // R447_HEADS_ROUND: the beat each group's figure was drawn on
    int m_musicTurnBeat = -1;                         // R447_HEADS_ROUND: the last musical turn (BLT)
    bool headsRoundDone(const QString &key, int beat) const;
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
    QMap<QString, qint64> m_r506HoldStart; // R506_DECL_HOLDMAP: groups holding up through a beat with no hit
    QMap<QString, int> m_r506HoldBeat;     // ... and the last beat they held on
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
    // R420_STUTTER: the operator's stutter - its own stabs beside the chop's
    bool m_stutter = false;
    int m_stutterRate = 3;                 // 1 = 1/4, 2 = 1/8, 3 = 1/16 (R427_STUTTER_16: 1/16 as standard)
    QVector<QPair<qint64, qint64> > m_stutterPlan;
    void planStutter();
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
    // R431_DROP_PICK
    QStringList dropSet() const;
    int dropMode() const { return m_dropMode; }
    QString planSection() const { return m_planSection; }   // R432_NEXT_SECTION
    bool planRemembered() const { return m_planRemembered; }   // R439_TRACK_MEMORY
    Q_INVOKABLE void forgetTrackMemory(const QString &title);  // R439_TRACK_MEMORY: empty = all
    Q_INVOKABLE int trackMemoryCount(const QString &title);    // R439_TRACK_MEMORY: picks kept for a title
    QString m_planSection;               // the next section the plan is for
    bool m_forceOn = false;              // this section runs on his set
    QStringList m_forceSet;
    int m_forceFrom = -1;                // R438_LOOP_ACROSS: the beat his section began
    QString m_forceSection;              // ... and what kind it is
    QString m_pressHeld;                 // R475_HELD_PRESS_MEM: the SECTION press held now (the pick's end)
    QString m_pressWas;                  // R479_BUILD_STROBE_3: the music's own section under that press
    QStringList m_dropSet;               // the drop's groups, in the room's order
    int m_dropMode = 0;                  // 0 none, 1 the next drop (plan), 2 this drop
    bool m_dropTouched = false;          // he tapped: the set is his until the drop ends
    int m_dropPlanFor = -1;              // the drop flag the plan was drawn for
    int m_dropPlanBeat = -1;             // the beat the block last ran on
    qreal m_dropPlanFader = -1.0;        // the slider the plan was sized at
    // R439_TRACK_MEMORY: his picks per track - title -> { "<boundary beat>": "section|fader|a+b", "_t": secs }
    QVariantMap m_trackMem;
    bool m_trackMemLoaded = false;
    bool m_planRemembered = false;       // the plan for the next section is his set from last time
    void loadTrackMemory();
    void rememberPick(int boundary, const QString &section, const QStringList &set, qreal fader);
    QStringList recallPick(int boundary, const QString &section, qreal fader);
    quint32 m_dropPlanSeed = 431u;       // the plan's own dice
    quint32 m_dropPlanCount = 0;
    // R443_PLAN_LANE
    mutable bool m_planLane = false;
    mutable bool m_planLaneLoaded = false;
    qreal m_planFaderNow = 0.5;          // the slider the plan block last read
    QMap<int, QStringList> m_planRan;    // section start -> what it lit (this track)
    void planRecord(int secStart, const QSet<QString> &lit);
    void planRanReset();
    QString planKindOf(const QString &key) const;
    QString planShortOf(const QString &key) const;
    QVariantList planChips(const QStringList &keys) const;
    QStringList planGuess(const QString &section, int boundary);
    // R484_PL_MEMBERS: his picks for later sections of this play, by the section's flag -
    // boundary beat -> "section|a+b" ("section|" all out, "section|*" ENGINE DECIDES). Not
    // saved: the section's crossing makes it the RIG's touched plan, and what lights is
    // remembered (R439) and learned (R433) there, as a tap in RIG. A new track clears it
    QMap<int, QString> m_planAhead;
    int planAheadKey(int boundary, const QString &section) const;
    int planAheadAt(int boundary, const QString &section, QStringList *set) const;   // 0 none, 1 his, 2 engine
    QStringList planAheadShown(int boundary, const QString &section);
    bool planIsRig(int boundary) const;
    bool planAheadOpen(int boundary) const;
    void planAheadFollow();
    void planAheadLit(int boundary, const QString &section);
    QVariantList planRowsOf(const QStringList &set) const;
    bool m_testTrack = false;            // R450_TEST_TRACK: learns nothing, logs nothing
    // R445_DROP_HIT
    int m_hitKind = 0;
    mutable int m_hitLast = 0;
    bool m_hitDark = false;              // the DARK beats are ours (his BLACKOUT tile is lit for them)
    int m_hitDarkFrom = -1;
    int m_hitFlashBeat = -1;             // the beat the hit's FLASH went on
    bool m_hitStrobeNow = false;         // driveStrobe: this beat is his STROBE landing
    int m_hitStrobeUntil = -1;           // ... and the strobes are on stage to here
    quint32 m_hitDarkSerial = 0;         // R449_DARK_QUIET: which DARK the quiet timer is for
    QStringList m_hitAdded;              // R449_HIT_NOT_DROP: the groups hitCast() put on stage this beat
    void hitStep(int beat, bool isDrop, const QString &nextState, int beatsToNext, qreal bpm);
    void hitCast(int beat, QSet<QString> &castSet);
    void hitUndark();
    void hitReset();
    // R480_HIT_DECL (Tobias 10-17: "saa gaar det helt amok!") - track_runtime/drophit.inc.cpp
    int m_hitStrobeFrom = -1;            // his burst's first beat (DARK: the beat after the bang)
    int m_hitStrobePlayed = 0;           // ... the beats it has played (a DJ loop counts)
    int m_hitBlinks = 0;                 // FLASH: the white beats still to come
    quint32 m_hitBlinkSerial = 0;
    bool m_hitFlashOwn = false;          // the FLASH WHITE is the hit's, not his finger's
    QHash<QString, qint64> m_hitPads;    // the pads the hit lit, with their "held" stamp
    int m_hitSeenBeat = -1;
    int m_hitStep = 0;                   // 1 on a beat's first tick, 0 on a second one
    int m_hitAmokFrom = -1;              // R480_HIT_AMOK: the landing the amok runs from
    int m_hitAmokPlayed = 0;
    int m_hitAmokFull = 32;              // beats at 100 %, then ENGINE_HIT_GLIDE back to his slider
    bool m_hitAmokOwed = false;          // DARK banged before its kick: the amok waits for it
    bool m_hitOwedLand = false;
    int m_hitOwedLeft = 0;
    qreal m_faderReal = 0.0;             // the slider as it reads, under the hit's amok
    bool hitLights(const QString &key) const;
    bool hitBurstNow(int beat) const;
    void hitWhite(bool on);
    void hitBurst(int from, int beat);
    qreal hitAmok(int beat, bool lands, bool isDrop);
    void hitAmokEnd();
    // R446_DROP_VARIETY (track_runtime/dropvariety.inc.cpp)
    QStringList m_prevDropSet;           // the last drop's cast where it landed (sorted)
    QString m_prevDropLead;              // ... the rhythm lead it opened with
    QString m_prevDropTitle;             // ... and its track
    QStringList m_thisDropSet;           // the running drop's, moved over when it ends
    QString m_thisDropLead;
    QString m_thisDropTitle;
    QStringList dropPicks(const QStringList &priority, int effects) const;
    void dropVariety(QStringList &priority, int effects, const QString &base, bool strobeFirst);
    void dropRemember(bool isDrop, const QSet<QString> &castSet);
    // R493_INF_DECL (runde 493): the room features - one declaration block per file, each
    // owner writes only below its own marker line (track_runtime/r493_<name>.inc.cpp)
    QString m_trackGenreTag;             // R493 shared: the DJ's rekordbox genre tag ("" none) - set by the groove file
    qreal m_trackSwing = -1.0;           // R493 shared: the track's swing, 0 straight .. 1 full shuffle, -1 unknown
    // R493_DECL_ROOMMAP: A4 room map table (rig_room.json)
    // R493_A4_DECL_ROOMMAP (track_runtime/r493_roommap.inc.cpp): where each lamp hangs, from
    // rig_room.json (the generator writes the table). x 0 left .. 1 right from the DJ, y 0 at
    // the DJ .. 1 the back; a placeholder in address order until the real map comes
    bool roomOf(quint32 fixtureId, qreal *x, qreal *y) const;   // false: the map does not know it
    QString roomGroupOf(quint32 fixtureId) const;
    QList<quint32> roomDiscoHeads() const;                      // the disco ball's heads (fixture ids)
    QList<quint32> roomOrder(const QString &group) const;       // a group from the DJ to the back
    bool roomPlaceholder() const;
    void roomHangOrder(QList<quint32> &fixtures) const;         // R493_MAP_DECL: room order when the map knows them all
    bool roomBall(qreal *x, qreal *y) const;                    // the ball's position, when known
    int r493RoomRowOf(quint32 fixtureId) const;
    void roomAnnounce();                                        // sig:room:<status>:<known>:<heads>, once
    bool m_r493RoomLogged = false;
    // R493_DECL_GROOVE: A1 swing, stabs, genre tag, finer onsets
    // R493_A1_DECL (runde 493, A1): swing chases and chord stabs - track_runtime/r493_groove.inc.cpp.
    // Sets the shared m_trackGenreTag / m_trackSwing above; everything else is its own.
    void setTrackGroove(const QString &genre, qreal swing, qreal swing16, qreal swing8,
                        const QVariantList &stabMid);   // TrackManager: the track event's r493 fields
    int stabMid(int beat, int quarter) const;          // 0-15, or -1 without data
    void r493GrooveBeat(int beat, int secStart, qreal fader, bool isDrop, bool isBuild, bool isBreak,
                        bool isCalm, bool hold, bool still, bool preDrop, bool landing);
    qreal r493SwingDelay(int subSteps, qreal depth) const;
    qreal r493SwingWithin(const QString &key, const TrackMove &mv, qreal within) const;
    bool r493StabWanted() const;
    bool r493StabFrame(qint64 now);
    void r493StabEnd();
    qreal m_r493Swing16 = -1.0;          // the off sixteenths' swing, 0..1, -1 unknown
    qreal m_r493Swing8 = -1.0;           // the off eighths'
    QVector<quint16> m_r493StabMid;      // the mids' stabs per quarter beat (four nibbles)
    QString m_r493SwingTitle;            // the track sig:swing was logged for
    qreal m_r493SwingDepth16 = 0.0;      // this beat's swing on a sixteenth walk (0 = straight)
    qreal m_r493SwingDepth8 = 0.0;       // ... on an eighth walk
    int m_r493SwingSec = -1;
    qint64 m_r493BeatStartMs = -1;       // the beat the decisions were made on
    int m_r493StabQ = 0;                 // this beat's stab quarters (bits 1-3)
    int m_r493StabFired = 0;
    int m_r493StabBeat = 0;
    int m_r493StabSec = -1;
    int m_r493StabSecNow = -1;
    qreal m_r493StabPeak = 0.0;
    QString m_r493StabKey;
    QString m_r493BlinkKey;              // the blink running: group, its two colour scenes, levels, clock
    quint32 m_r493BlinkCol = 0;
    quint32 m_r493BlinkPartner = 0;
    qreal m_r493BlinkColLevel = 1.0;
    qreal m_r493BlinkPartnerLevel = 1.0;
    qreal m_r493BlinkPeak = 0.0;
    qreal m_r493BlinkLen = 100.0;
    qint64 m_r493BlinkFrom = -1;
    // R493_DECL_GENRE: A2 genre profile
    // R493_A2_GENRE_DECL: the track's genre profile (track_runtime/r493_genre.inc.cpp)
    void r493GenreBeat(qreal bpm);
    qreal r493GenreWeight() const;
    int r493GenreTickets(const QString &colour, int n) const;
    qreal r493GenreStrobe(qreal top) const;
    qreal r493GenrePace() const;
    int r493GenreHold(int holdBars) const;
    qreal r493GenreLaser(qreal pLead) const;
    bool r493GenrePercDraw(QRandomGenerator *rng) const;
    int r493GenreEffects(int effects, bool isDrop) const;
    int r493GenreFadePct(int pct) const;
    qreal r493GenreSwingScale() const;   // for the swing chases: 1 as measured, under 1 straighter
    bool m_r493GenreDraw = false;        // the room's own colour draw is under way (drawColour)
    QString m_r493GenreSeen;             // title|tag|swing the profile was read for
    int m_r493Genre = 0;                 // 0 house (neutral), 1 techno, 2 melodic, 3 afro, 4 minimal
    int m_r493GenreConf = 0;             // 0-100: under 50 the profile is neutral
    QString m_r493GenreSrc;              // analysis, tag or both
    // R493_DECL_MELODY: A2 melody wave
    // R493_A2_MELODY_DECL: the wash on the melody's arc in a kickless break (r493_melody.inc.cpp)
    QVector<qreal> r493MelodyArc(int from, int to) const;
    void r493MelodyBeat(int beat, int secStart, int secEnd, bool isBreak, bool quiet);
    qreal r493MelodyBreath(const QString &group, qreal sine) const;
    QVector<qreal> m_r493MelArc;         // the break's level per beat, 0.60-1.00 (empty: the sine)
    int m_r493MelFrom = -1;              // ... from this beat (the section's start)
    int m_r493MelBeat = -1;
    QString m_r493MelTitle;
    bool m_r493MelLogged = false;
    qreal m_r493MelW = 0.0;              // its weight over the sine at this beat's start
    qreal m_r493MelWTo = 0.0;            // ... and at its end (a quarter a beat)
    // R493_DECL_CAMELOT: A2 Camelot transitions
    // R493_A2_CAMELOT_DECL: glide or hold-and-cut by the two keys (r493_camelot.inc.cpp)
    void r493CamelotNextKey(const QString &key);
    void r493CamelotStash();
    void r493CamelotLanded(const QString &key, bool adopted);
    void r493CamelotBeat(int beat, int beatInBar, bool hold, bool isCalm, bool changeColour);
    bool r493CamelotCutNow(int beat) const;
    QString m_r493KeyNow, m_r493KeyNext; // the keys as rekordbox wrote them
    int m_r493CamState = 0;              // 0 -, 1 match, 2 clash holding, 3 cut owed, 4 cut in the mix
    bool m_r493CamDecided = false;       // this mix has been read
    bool m_r493CamDropOwed = false;      // the incoming drop arrived: cut on the bar line
    int m_r493CamMixEnd = -1;
    int m_r493CamCutBeat = -1;           // the beat the cut went in (a hard one)
    QString m_r493CamCut, m_r493CamIncoming;
    QString m_r493CamNext;               // the next colour a clash keeps out of the mix, for the cut
    // R493_DECL_ZOOMSQUEEZE: A3 zoom squeeze in builds
    // R493_A3_ZS_DECL (runde 493, A3): the build's zoom steps on two-bar lines (track_runtime/r493_zoomsqueeze.inc.cpp)
    bool m_r493ZsOn = true;               // the layer on (no UI in this round)
    int m_r493ZsLogged = -1;              // the build (its first beat) sig:zoom-squeeze was written for
    QString m_r493ZsGroup;                // the head group the squeeze drew the zoom for ...
    int m_r493ZsBeat = -1;                // ... on this beat
    int m_r493ZsStep = -1;                // the step it asked for (0 sharpest .. 8 widest)
    bool r493ZoomSqueeze(const QString &key, int beat, int secStart, int beatsToNext,
                         const QString &nextState, qreal energy, int &want);
    bool r493ZoomSqueezing(const QString &key) const;   // A4: the squeeze drew this group's zoom this beat
    // R493_DECL_EASING: A3 soft movement
    // R493_A3_EASE_DECL (runde 493, A3): soft movement on the wash heads (track_runtime/r493_easing.inc.cpp)
    struct R493EaseHead {
        quint32 scene = 0xFFFFFFFFu;      // its hidden Replace scene, TRACK Ease: <group> <fixture id>
        quint32 pan = 0xFFFFFFFFu, tilt = 0xFFFFFFFFu, dimmer = 0xFFFFFFFFu;
        quint32 chaser = 0xFFFFFFFFu;     // the figure whose steps it shapes
        int step = -1;                    // ... and the step
        qreal a[2] = { 0.0, 0.0 }, b[2] = { 0.0, 0.0 };     // pan, tilt: from, to
        qreal m0[2] = { 0.0, 0.0 }, m1[2] = { 0.0, 0.0 };   // the end slopes (0 = from or to rest)
        int out[2] = { -1, -1 };          // last written
        bool on = false;                  // the layer holds the head
        bool easing = false;              // a step is being shaped
    };
    struct R493EfxRamp { int w0 = 0, h0 = 0, w1 = 0, h1 = 0; qint64 t0 = 0; int ms = 0; };
    struct R493EaseClock { quint32 chaser = 0xFFFFFFFFu; int index = -1; qint64 t0 = 0; qint64 lastPoll = -1; bool known = false; bool layered = false; };
    QMap<QString, R493EaseClock> m_r493EaseClock;     // head group -> its figure step's own clock
    QMap<quint32, R493EaseHead> m_r493Ease;            // wash head (fixture id) -> its layer
    QMap<QString, QList<quint32> > m_r493EaseGroups;   // head group -> its wash heads
    QMap<QString, R493EfxRamp> m_r493EfxRamp;          // head group -> its EFX size easing in
    QSet<quint32> m_r493Yield;            // heads another layer holds (A4's disco ball): no squeeze, no easing
    QTimer m_r493EaseTimer;
    bool m_r493EaseOn = true;             // the layer on (no UI in this round)
    bool m_r493EaseWired = false;
    bool m_r493EaseLogged = false;        // sig:easing:on written for this SHOW ON
    int m_r493EaseSteps = 0;              // figure steps shaped (harness)
    int m_r493EfxGrows = 0;               // EFX figures grown in (harness)
    void r493EnsureEase();
    void r493EaseTick();
    void r493EaseRelease(quint32 fid);
    Scene *r493EaseScene(const R493EaseHead &h) const;
    void r493EaseEfxStart(const QString &group, Function *func, int width, int height, uint ms);
    bool r493EaseEfxSize(const QString &group, Function *func, int width, int height);
    void r493YieldHeads(const QSet<quint32> &fixtureIds);   // A4: heads it holds now (empty: none)
    bool r493EaseHolds(quint32 fixtureId) const;            // A4: the ease layer holds this head's pan/tilt
    // R493_DECL_DISCOBALL: A4 disco ball moment
    // R493_A4_DECL_DISCOBALL (track_runtime/r493_discoball.inc.cpp)
    int m_r493DiscoSec = -1;             // the break (its first beat) the moment was drawn for
    QString m_r493DiscoTitle;            // ... in this track
    int m_r493DiscoWait = 0;             // varietyDraw's wait
    int m_r493DiscoPhase = 0;            // 0 off, 1 travelling dark, 2 lit, 3 home under the dark mask
    int m_r493DiscoFrom = -1;            // the beat the phase began
    int m_r493DiscoKick = 0;             // kick beats in a row
    int m_r493DiscoKickBeat = -1;
    QString m_r493DiscoGroup;            // the heads' group
    QList<quint32> m_r493DiscoFx;        // the heads
    quint32 m_r493DiscoLook = 0xFFFFFFFFu;   // hidden scene: aim, sharp beam, colour (flashed)
    quint32 m_r493DiscoDark = 0xFFFFFFFFu;   // hidden scene: their intensity at nought (flashed)
    int m_r493DiscoAttr = -1;            // the look's intensity override
    qreal m_r493DiscoTarget = 0.0;       // the beam's level (ENERGY)
    qreal m_r493DiscoRamp = 0.0;         // its swell, 0..1
    int m_r493DiscoGen = 0;              // cancels a swell or a dark timer
    bool r493Kickless(int from, int beats, int allowed) const;
    bool r493HisPicks() const;
    bool r493HisTakes() const;           // R494_DECL_HIS_TAKES: his picks that END a ball/roof (not a BLACKOUT/FLASH tap)
    int r493DiscoTravel() const;
    bool r493DiscoHolds(quint32 fixtureId) const;               // for A3: the ball has this head
    qreal r493DiscoGain(const QString &group) const;
    Scene *r493DiscoSceneNamed(const QString &name);
    bool r493DiscoBuild(const QString &group, const QList<quint32> &heads);
    void r493DiscoLevel();
    void r493DiscoSwell(int gen);
    void r493DiscoHome(int beat, const QString &why);
    void r493DiscoReset(bool soft);
    void r493DiscoBeat(const QSet<QString> &castSet, const QString &state, int beat, int secStart, int secEnd,
                       int beatsToNext, qreal fader, qreal kick, bool hold, bool isCalm, bool still, bool silent);
    // R493_DECL_LASERROOF: A4 laser roof
    // R493_A4_DECL_LASERROOF (track_runtime/r493_laserroof.inc.cpp)
    bool m_r493RoofOn = false;           // the laser roof stands
    int m_r493RoofSec = -1;              // the break (its first beat) it was drawn for
    QString m_r493RoofTitle;
    int m_r493RoofWait = 0;              // varietyDraw's wait
    int m_r493RoofFrom = -1;             // its first beat
    int m_r493RoofKick = 0;              // kick beats in a row
    int m_r493RoofKickBeat = -1;
    QString m_r493RoofBars;              // the bars
    QString m_r493RoofA, m_r493RoofB;    // the wave's colours (B empty: one colour, no wave)
    int m_r493RoofPeriod = 16;           // beats from wave to wave
    int m_r493RoofStep = 2;              // beats a bar inside a wave
    quint32 m_r493RoofMask = 0;          // the bars in B now (bit = g.fixtures index)
    QList<quint32> m_r493RoofOrder;      // the bars from the DJ to the back (roomOrder)
    QHash<QString, quint32> m_r493RoofScenes;   // the wave's colour scenes
    bool r493HazeOn() const;
    bool r493RoofStill() const;
    void r493RoofEnd(const QString &why);
    void r493RoofReset();
    qreal r493RoomLevel(const QString &key, qreal groupLevel, bool still) const;
    quint32 r493RoofColour(const QString &key, int beat);
    void r493RoofBeat(QSet<QString> &castSet, const QStringList &eligible, const QString &state, int beat,
                      int secStart, int secEnd, qreal fader, qreal kick, bool hold, bool isCalm, bool still,
                      bool silent);
    // R497_INF_DECL (runde 497): one declaration block per file, each owner writes only
    // below its own marker line (track_runtime/r497_<name>.inc.cpp)
    // R497_DECL_BLOOM: A1 centre-out bloom
    // R497_A1_BLOOM_DECL (runde 497, A1): the wash opens from the centre on a bloom drop's first
    // beat, and collapses into it the beat before - track_runtime/r497_bloom.inc.cpp
    QString r497BloomWash() const;
    bool r497BloomRingsOf(const QString &key, QVector<int> *ring, int *rings) const;
    bool r497BloomReady() const;                // dropFormDecide: the map knows the wash whole
    bool r497BloomFree(const QString &key) const;
    bool r497BloomArm(int mode, int beat);
    bool r497BloomLand(int beat);               // the drop forms' landing: the bloom's beat
    void r497BloomBeat(int beat, bool isDrop, const QString &nextState, int beatsToNext, qreal fader,
                       bool hold, bool isCalm, bool still);   // tick(): the collapse's beat
    bool r497BloomScenes();                     // the rings' masks for this beat's colour
    Scene *r497BloomScene(quint32 id, const QString &name);
    void r497BloomStart();                      // tick(), the beat drawn: the first frame
    bool r497BloomFrame(qint64 now);            // the pulse timer and reapplyLevels: the rings now
    void r497BloomStop();
    int m_r497BloomMode = 0;                    // 0 none, 1 the collapse, 2 the bloom
    int m_r497BloomBeat = -1;                   // the beat it runs in
    qint64 m_r497BloomMs = -1;                  // ... that beat's start, once tick() has drawn it
    QString m_r497BloomKey;                     // the wash
    QVector<int> m_r497BloomRing;               // lamp -> ring, 0 the centre
    int m_r497BloomRings = 0;
    int m_r497CollapseFor = -1;                 // the drop the collapse ran for (once)
    bool m_r497BloomLogged = false;             // its sig: line written
    int m_r497BloomPrepFor = -1;                // the drop its masks were made ahead for
    QVector<quint32> m_r497BloomDark;           // per ring: the dark mask ...
    QVector<quint32> m_r497BloomLit;            // ... and the lit one (hidden scenes, flashed)
    QVector<int> m_r497BloomShown;              // per ring: 0 neither flashed, 1 dark, 2 lit
    QString m_r497BloomCache;                   // the wash and colour scene the masks were made for
    // R497_DECL_ROLL: A1 builds follow the drum roll
    // R497_A1_ROLL_DECL (runde 497, A1): the build's rate never past its measured roll -
    // track_runtime/r497_roll.inc.cpp
    int r497RollMeasure(int barStart) const;    // 4, 8, 16, or -1 no clear roll
    void r497RollBeat(int beat, int beatInBar, bool isBuild, bool isDrop, const QString &nextState,
                      int beatsToNext, bool hold, bool isCalm, bool still);
    int r497RollChop(int chop) const;           // the stabs
    void r497RollMove(TrackMove &mv, const TrackGroup &g) const;   // the walks
    int r497RollStrobe(int want, int rates) const;                 // the strobe riser
    int m_r497RollTier = -1;                    // this bar's roll, -1 none: today's ramp
    int m_r497RollBar = -1;                     // the bar it was read for
    int m_r497RollLogged = -1;                  // the roll last logged in this build
    QString m_r497RollTitle;
    // R497_DECL_CURTAIN: A2 the mix as a curtain
    // R497_A2_CURTAIN_DECL: the mix as a curtain across the room (track_runtime/r497_curtain.inc.cpp)
    bool m_r497CurtainOn = false;        // the glide shows as a curtain now
    int m_r497CurtainK = 0;              // heads in the new track's colour, counted from the right
    int m_r497CurtainMoved = -1;         // the beat the border last moved on
    QString m_r497CurtainKey;            // the group it runs on (the glide's base)
    QList<quint32> m_r497CurtainOrder;   // its heads from left to right (roomHangOrder)
    QHash<QString, quint32> m_r497CurtainScenes;   // "group|a|b|mask" -> hidden colour scene
    QString r497CurtainMayRun(const QString &key, QList<quint32> *order) const;
    void r497CurtainOff(const QString &why);
    void r497CurtainBeat(int beat, int beatInBar, bool frozen);
    quint32 r497CurtainScene(const QString &key, quint32 fa, quint32 fb, const QSet<quint32> &inB);
    bool r497CurtainGlide(const QString &key, quint32 fa, quint32 fb, qreal lvl);
    quint32 r497CurtainColour(const QString &key, const QString &colour);
    // R497_DECL_TAPS: A3 BLACKOUT taps
    // R497_A3_TAPS_DECL: his BLACKOUT taps learned, ghost holes from 80 % (track_runtime/r497_taps.inc.cpp)
    void r497TapLoad();
    void r497TapSave(bool force);
    void r497TapEdge(bool on);           // setBlackout: his tap, learned - the ghost stops at once
    void r497TapBeat(int beat, int bucket, int beatInBar, int dropBar, int beatsToNext, bool dropNext, bool open);
    void r497TapChop(qint64 now, bool &inStab, qint64 &next) const;   // slotChopTimer: the holes
    bool r497TapHush() const;            // his picks and the safeties: no hole
    qreal r497TapRate(int bucket, int ebin) const;   // his taps per 8 bars there
    bool m_r497TapLoaded = false;
    bool m_r497TapDirty = false;
    QVector<qreal> m_r497TapSlot;        // the sixteenth of the bar his taps start on (16)
    QVector<qreal> m_r497TapLen;         // how long, in sixteenths of a beat (16)
    QVector<qreal> m_r497TapTaps;        // taps per section x slider band (4 x 3)
    QVector<qreal> m_r497TapBeats;       // ... and the beats he was at the pad there
    QVector<qreal> m_r497TapBurst;       // his bursts of 1, 2, 3, 4 taps
    QList<QPair<qint64, qint64>> m_r497TapPlan;   // this beat's ghost holes (clock ms)
    qint64 m_r497TapPlanBeat = -1;       // ... planned on this beat start
    qint64 m_r497TapOnMs = -1;           // his tap down, being learned
    int m_r497TapOnSlot = 0;
    int m_r497TapOnBucket = -1;
    int m_r497TapOnEbin = -1;
    qint64 m_r497TapLastMs = -1;         // his last BLACKOUT edge (or STUTTER beat)
    qint64 m_r497TapHisMs = -1;          // his own burst, counted
    int m_r497TapHisN = 0;
    bool m_r497TapHisHigh = false;
    int m_r497TapBucket = -1;            // this beat: 0 drop, 1 break, 2 groove, 3 build, 4 intro/outro
    int m_r497TapBarBeat = 0;
    int m_r497TapBeatNo = -1;
    int m_r497TapCounted = -1;
    int m_r497TapChopBeat = -1;          // the last beat the room was closed to a hole
    int m_r497TapLearned = 0;            // taps learned tonight
    qint64 m_r497TapLogMs = -1;
    qint64 m_r497TapSaveMs = -1;
    qreal m_r497TapCredit = 0.0;         // towards the next burst
    qreal m_r497TapGap = -1.0;
    int m_r497TapLeft = 0;               // holes the burst still owes
    int m_r497TapWin = 0;                // beats left in its two bars
    bool m_r497TapFirst = false;
    qint64 m_r497TapFreeMs = -1;         // no dark before this (half a beat after the last light back)
    int m_r497TapGhosts = 0;
    // R497_DECL_MELODYHEADS: A4 heads follow the melody
    // R498_DECL_RUSH: the room rush (track_runtime/r498_rush.inc.cpp, R498_INF_DECL)
    // R498_R_DECL (runde 498, agent R): the room rush - a fast wave through the room over the wash
    // and the strobes, four directions, its speed on ENERGY (track_runtime/r498_rush.inc.cpp)
    bool r498RushMapped(const QString &key) const;      // a group the map knows whole
    QString r498RushWash() const;                       // the wash it runs on ("" none: no rush)
    QString r498RushStrobes() const;                    // the strobe bank ("" none: the wash alone)
    bool r498RushReady() const;                         // dropFormDecide: the map knows the wash whole
    bool r498RushFree() const;                          // his picks and the room's other owners: no rush
    bool r498RushStrobesFree(qreal fader) const;        // the strobes may join
    bool r498RushLand(int beat);                        // the drop forms' landing: the rush drop's first beat
    void r498RushBeat(int beat, bool isDrop, bool isBuild, const QString &nextState, int beatsToNext,
                      int dropBar, int beatInBar, qreal fader, bool hold, bool isCalm, bool still);
    void r498RushLamps();                               // the lamps: the wash, then the strobe bank
    bool r498RushGo(int beat, int beats, int why, bool toDj, qreal fader, bool isDrop);
    void r498RushPhases();                              // each lamp's place on the direction, 0..1
    bool r498RushMasks(bool isDrop);                    // the lamps' lit / dark masks for this beat's colours
    void r498RushStart();                               // tick(), the beat drawn: the first frame
    void r498RushStrobe(int beat, bool quiet, bool affordable, bool burst);   // driveStrobe: the strobes join, one burst
    bool r498RushFrame(qint64 now);                     // the pulse timer and reapplyLevels: the front now
    void r498RushStop();
    int m_r498RushState = 0;                    // 0 none, 2 a rush running
    int m_r498RushFrom = -1;                    // the beat it started on
    int m_r498RushBeats = 0;                    // its sweep, in beats (4, 2, 1)
    int m_r498RushBeatNow = -1;                 // the beat it is in
    qint64 m_r498RushMs = -1;                   // ... that beat's start, once tick() has drawn it
    int m_r498RushDir = -1;                     // 0 loop, 1 to-dj, 2 from-dj, 3 across
    int m_r498RushLastDir = -1;                 // never the same direction twice running
    bool m_r498RushRight = false;               // across: from the right this time
    int m_r498RushWhy = 0;                      // 0 the drop form, 1 the build, 2 a phrase turn
    qreal m_r498RushFader = 0.0;                // the slider it started at
    bool m_r498RushIsDrop = false;              // it started in a drop (white may be the partner)
    bool m_r498RushWithStrobes = false;         // the strobes joined (one burst spent)
    bool m_r498RushStrobeAsked = false;
    int m_r498RushCount = 0;                    // rushes so far: every other one in the partner colour
    QVector<quint32> m_r498RushFx;              // the lamps: the wash, then the strobes
    QVector<bool> m_r498RushIsStrobe;
    QVector<qreal> m_r498RushPhase;             // -1: not in this rush
    QVector<quint32> m_r498RushLit;             // per lamp: its lit mask - the colour (hidden scene, flashed) ...
    QVector<quint32> m_r498RushDim;             // ... its dimmer, full, carrying the level ...
    QVector<quint32> m_r498RushDark;            // ... and a strobe's dark one
    QVector<int> m_r498RushShown;               // per lamp: 0 neither flashed, 1 dark, 2 lit
    QString m_r498RushCache;                    // the groups and colour scenes the masks were made for
    QString m_r498RushWashKey;
    QString m_r498RushStrobeKey;
    int m_r498RushFormAt = -1;                  // the rush drop's landing beat
    QString m_r498RushTitle;
    int m_r498RushBuildFor = -1;                // the drop a build rush ran for (once)
    int m_r498RushPhraseAt = -100000;           // the last phrase rush (at most one in 32 bars)
    // R497_A4_DECL (runde 497, A4): the Shark heads' figure follows the melody in kickless breaks
    struct R497MhState
    {
        bool on = false;                 // the arc leads this group on this beat
        int sign = 0;                    // the tilt way that lifts off straight down (+1/-1), 0 unread
        qreal k = 1.0, kTo = 1.0;        // the figure's size factor at this beat's start / end
        qreal off = 0.0, offTo = 0.0;    // the centre's tilt offset (EFX y units), the same
        qreal appliedK = 1.0;            // what applySweep() was handed last
        qreal appliedOff = 0.0;
        qreal writtenOff = 0.0;          // what the EFX's centre carries of ours
    };
    QMap<QString, R497MhState> m_r497Mh;   // per wash-head group (r493EnsureEase's)
    int m_r497MhBeat = -1;
    int m_r497MhSec = -1;
    QString m_r497MhTitle;
    bool m_r497MhLogged = false;         // sig:melody-heads:on said in this section
    bool m_r497MhShown = false;          // ... and its off still owed
    int r497MelodyHeadsSign(const QString &group) const;
    void r497MelodyHeadsBeat(int beat, int secStart, bool isBreak, bool hold, bool quiet);
    qreal r497MelodyHeadsShape(const QString &group, Function *func, const TrackSweep &sw, int &width, int &height);
    void r497MelodyHeadsY(const QString &group, Function *func, const TrackSweep &sw);
    // R488_DX_DECL: the drop forms (track_runtime/dropforms.inc.cpp, runde 488)
    int m_formPlan = 0;                  // the form decided for the coming drop flag
    int m_formPlanFor = -1;              // ... that flag
    QString m_formPlanTitle;             // ... in this track
    qreal m_formPlanScore = 0.0;
    bool m_formPlanHit = false;          // ... with his DROP HIT armed
    int m_formNow = 0;                   // the form on stage in this drop (0 = the classic drop)
    int m_formFrom = -1;                 // its landing
    int m_formUntil = -1;                // its last beat
    int m_formLast = 0;                  // the last drop's form: never the same special twice
    int m_formDrops = 0;                 // drops landed in this track
    QString m_formTrack;
    int m_formLandBeat = -1;
    bool m_formWhite = false;            // strob-storm: white this drop
    bool m_formSingle = false;           // strob-storm: whole bars at the stop this drop (R489_EX_MEMBERS)
    bool m_formStormBars = false;        // strob-storm: the show has the storm programmes
    int m_formSideBeats = 2;             // side-mod-side: beats a side
    bool m_formWild = false;             // omvendt: this beat is in the build's wild bars
    int m_formWildFor = -1;
    qreal m_formWildProg = 0.0;          // ... the build's prog where they began
    qreal m_formWildFrac = 0.0;          // ... and how far in
    int m_riseStage = 0;                 // rejsen-op: 0 off, 1 dark, 2 down, 3 rising, 4/5 home in the dark
    int m_riseUnits = 0;
    int m_riseMs = 0;
    int m_riseBeats = 32;
    qint64 m_riseStartMs = -1;
    int m_riseBeat = -1;
    QString m_riseGroup;
    bool dropFormBlocked(int at) const;
    QString dropFormBars() const;
    QString dropFormStrobes() const;
    int dropFormDecide(int at, qreal fader, qreal bpm, int beat);
    void dropFormReset(bool newTrack);
    void dropFormEnd(const QString &why);
    void dropFormBeat(int beat, int secStart, int secEnd, const QString &nextState, bool isDrop,
                      bool isBuild, int dropBar, int beatInBar, qreal fader, bool hold, bool isCalm,
                      bool still, qreal bpm, qreal prog, bool forceNext);
    bool dropFormOwns(const QString &key) const;
    void dropFormMove(const QString &key, TrackMove &mv, int dropBar, qreal fader, const QString &base) const;
    void dropFormPulse(const QString &key, TrackMove &mv, const QString &base) const;
    bool dropFormNative(const QString &key) const;
    quint32 dropFormProgramme(const QString &key, const QString &colour, bool isDrop) const;
    int dropFormDivision(bool single) const;
    QString dropFormColour(const QString &key, const QString &colour) const;
    qreal dropFormLevel(const QString &key, const QString &base, int dropBar, int beatInBar) const;
    void dropFormBreathDark(QSet<QString> &darkGroups, const QSet<QString> &castSet, const QString &base,
                            int dropBar, int beatInBar) const;
    bool dropFormCalm() const;
    bool dropFormCalmLanding(bool isDrop, int secStart) const;
    bool dropFormFullBuild(int beat) const;
    bool dropFormHalfTime() const;
    bool dropFormHoldsColour(bool isDrop, int secStart) const;
    void dropFormCast(QStringList &priority) const;
    void dropFormPlan();
    QString dropFormEvent() const;
    int dropFormSide(const QString &group, int part) const;
    QVector<qreal> dropFormMask(const QString &group, const TrackMove &move, int step) const;
    quint32 dropFormRiseScene(const QString &key, quint32 home, int units);
    void dropFormRiseDark(const QString &key, QSet<QString> &darkGroups);
    qreal dropFormRiseLeft() const;
    bool dropFormRiseAim(const QString &key, QSet<QString> &darkGroups, bool inCast, bool isDrop,
                         bool dropWaiting, int beat, int dropBar, int beatInBar, int secStart,
                         qreal fader, bool isCalm, bool still);
    void dropFormRiseFader(qreal fader);
    static int dropFormRiseAllowed(qreal energy);                                    // R490_FX_RISE_DECL
    bool dropFormRiseSafe(const QString &key, quint32 held, qreal fader) const;
    quint32 dropFormSideColour(const QString &key, const QString &colour, int bar);
    bool dropFormRiseHolds(const QString &key) const;
    bool m_formKickFill = false;         // kick-chase: the fill (from 80 %), else the 3-eye packet
    quint32 m_formKickFid = 0xFFFFFFFFu;  // ... the programme last started on a kick (Function::invalidId())
    int m_formKickBeat = -1;             // ... the beat it last decided on
    int m_formKickLast = -10;            // ... the last beat with a kick (a launch)
    bool m_formKickLaunch = false;       // ... this beat launched
    int m_formKickKind = -1;             // ... 0 fill, 1 packet, 2/3 the same back, 4/5 the step
    bool m_formKickStep = false;         // ... R489_EX_STEP_MEMBERS: one bar a kick (80-90 %)
    int m_formKickStepNo = 0;            // ... the step's kicks so far this way
    bool dropFormKickReady() const;
    quint32 dropFormKick(const QString &key, const QString &colour, qreal kick, int beat, int dropBar);
    bool dropFormKickWait(const QString &key, quint32 mf, const Function *func) const;
    int dropFormKickDivision(quint32 mf) const;
    bool dropFormHomeOnly(const QString &key) const;
    // R483_NEXT_DECL (Tobias 10-20, "3": 57 of 263 NEXT within 4 s of the one before - the
    // staff flipping through). The looks NEXT threw away in this section, last four: not again
    QString m_nextRejState;                          // the section kind they were thrown away in
    QString m_nextRejTitle;                          // ... and the track
    QList<QStringList> m_nextRejCasts;               // their casts (sorted)
    QStringList m_nextRejColours;                    // their colours (the AUTO draw's, not his)
    QHash<QString, QList<int>> m_nextRejPatterns;    // group -> generated patterns thrown away
    QHash<QString, QStringList> m_nextRejFamilies;   // group -> programme families thrown away
    QStringList m_nextSkipped;                       // what the last NEXT's draw stepped round (sig:look-skip)
    bool m_nextFiled = false;                        // R485_A5_NEXT_ONCE_MEM: this look is filed - no beat took the NEXT yet
    void nextRemember();
    void nextForget();
    bool nextVariety(QStringList &priority, int effects, const QString &base, bool strobeFirst);
private:
    bool m_bigDrop = false;              // R357_KICK_BACK: this drop arrived big
    int m_kickBackLast = -100;
    mutable QHash<QString, qreal> m_colourTaste;   // R357_TASTE: learned from the colour tiles
    mutable bool m_tasteLoaded = false;
    // R433_SECTION_TASTE: learned from his picks in RIG, "section/group" -> weight
    mutable QHash<QString, qreal> m_sectionTaste;
    mutable bool m_sectionTasteLoaded = false;
    QStringList m_planOriginal;          // the engine's plan as it stood at his first tap
    // R479_HOLD_PICK_H (night review 10-17): his pick crossed its boundary, not yet on stage
    // (HOLD/CALM) - its boundary and the engine's plan, for the memory and the taste
    int m_pickOwedFor = -1;
    QStringList m_pickOwedPlan;
    qreal sectionTaste(const QString &section, const QString &key) const;
    bool tasteOut(const QString &section, const QString &key) const;
    int tasteWeight(const QString &section, const QString &key, int weight) const;
    void tasteLast(const QString &section, QStringList &order) const;
    void learnSection(const QString &section, const QStringList &plan, const QStringList &his);
    void decaySectionTaste();            // R435_TASTE_DECAY: once a night, toward neutral
    // R435_CRUMBS: the crash trail (Documents/QLC+/crumbs-current.txt)
    QFile m_crumbs;
    QString m_crashNote;                 // the last run's crash file, for the tracklog
    void crumbsOpen();
    void crumb(const QString &what);
    void crumbsEnd();
    // R436_SHOW_CHECK
    QVariantList m_showMissing;
    int m_showWants = 0;
    QString m_showAckKey;
    void updateShowCheck();
    QString showKey() const;
    // R436_STROBE_LAB
    bool m_slabActive = false;
    mutable bool m_slabLoaded = false;
    bool m_slabWired = false;
    bool m_slabDark = true;
    bool m_slabHold = false;
    bool m_slabTest = false;
    int m_slabTab = 0;
    int m_slabIndex = 0;
    int m_slabAuto = 0;
    int m_slabSpeed = 0;
    QString m_slabGroup;
    QString m_slabColour = QStringLiteral("room");
    QString m_slabMessage;
    QList<quint32> m_slabList;
    QHash<int, quint32> m_slabResume;    // where he was, per tab
    mutable QHash<quint32, int> m_slabVerdict;
    QTimer m_slabAutoTimer;
    QString slabPickGroup() const;
    void slabLoad() const;
    void slabSave() const;
    int slabTabOf(const TrackFuncInfo &info) const;
    QList<quint32> slabListOf(const QString &group, int tab, int order = 0) const;
    quint32 slabCurrent() const;
    QString slabColourNow() const;
    int slabDivision(const TrackFuncInfo &info) const;
    void slabDarkParts();
    void slabShow();
    void slabRestartTimer();
    void slabShowCandidate();
    int slabCountOf(int verdict) const;
    int slabAllOf(int verdict) const;
    void slabShutdown();                 // every way LASER LAB closes (labShutdown)
    // R440_BARS_LAB
    int m_slabKind = 0;                  // 0 the strobes, 1 the laser bars
    int m_slabOrder = 0;
    QHash<int, int> m_slabOrderOf;       // the order each lab was last left in
    QHash<int, int> m_slabTabOf;         // ... and the tab
    qint64 m_slabAimMs = 0;              // when the bars were last sent to their aim
    static bool slabKindOf(const TrackGroup &g, int kind);
    QString slabPickGroupOf(int kind) const;
    bool slabAim();                      // R442_BARS_NO_HOME: false = no safe aim known
    QString slabDarkText() const;
    int slabAllOfGroup(int verdict, const QString &group) const;
    // R440_COLOUR_LAB / R440_COLOUR_LEVEL
    bool m_clabActive = false;
    bool m_clabDark = true;
    QString m_clabColour;
    QString m_clabSolo;
    qreal m_clabTest = 0.60;
    QString m_clabMessage;
    mutable QHash<QString, qreal> m_colourLevel;   // "group/colour" -> level, 1.0 not stored
    mutable bool m_colourLevelLoaded = false;
    void clabLoad() const;
    void clabSave() const;
    bool clabGroupCan(const QString &key) const;
    QString colourShown(const QString &group) const;
    qreal colourLevel(const QString &group) const;
    /** the night's group fader x the group's level in the colour it shows */
    qreal trimOf(const QString &group) const;
    /** R442_ANI_MASTER: the animation laser is lit only while MASTER x the
     *  closing is at least ENGINE_ANI_MASTER_MIN - its dimmer is a switch */
    bool aniMasterOn() const;
    QStringList clabGroups() const;
    void clabStopAll();
    void clabShow();
    void clabShutdown();
    void clabSetLevelIn(const QString &key, const QString &colour, qreal level);
    // R440_SEND
    int m_sendState = 0;                 // 0 idle, 1 packing, 2 zip done, 3 folder only
    QString m_sendResult;
    QString m_sendFolder;
    QString m_sendZip;
    QStringList sendFilesOf(const QString &id) const;
    bool sendSnapshot(const QString &path);
    void sendDone(bool ok);
    qreal colourTaste(const QString &colour) const;
    void learnColour(const QString &picked, const QString &shown);
    // ---- runde 358 ----
    QVector<quint8> m_audMid;            // R358_RHYTHM: the mid band, beat for beat
    QVector<quint16> m_onsetHigh;        // the highs' onsets per quarter beat
    int onsetHigh(int beat, int quarter) const;   // 0-15, or -1 without data
    QVector<quint16> m_onsetLow;         // R461_ONSETS: the bass's onsets per quarter beat
    QVector<quint16> m_onsetMid;         // ... and the mids'
    int onsetLow(int beat, int quarter) const;    // 0-15, or -1 without data
    int onsetMid(int beat, int quarter) const;
    // R501_B_HIT_DECL (runde 501, B): does the music hit on this beat? 1 yes, 0 no, -1 not known
    int r501KickAt(int beat, bool real) const;      // the RAW kick (BLT 323+): >= 0.20, or a real one >= 0.45
    int r501OnsetAt(int beat, int quarter) const;   // an onset on this quarter (bass, mids or highs)
    bool r501PulseHit(int beat, int pulseOn, bool grid, bool ani) const;   // a beat pulse may land here
    bool r506LevelHolds(int beat) const;   // R506_DECL_LEVELHOLDS: no quieter than the bars before
    bool m_andBass = false;              // R461_AND_BASS: the bass on the "and" in this groove
    QHash<QString, int> m_andSeen;       // ... the base / Minis already hit on it this beat
    int m_pluckSlots = 0;                // R461_BREAK_PLUCK: the quarters a break's pattern hits (bits 0-3)
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
    // R480_STROBE_PHASE (Tobias 10-17: "jo hoejere Energi og jo stoerre er drop, desto mere strob og jo
    // laengere tid"): how long the drop stays intense - its strobe bursts, and where a long drop settles
    qreal m_dropHard = -1.0;             // the drop's kick + bass against the drops before (runde 322), -1 unknown
    int m_dropPhaseRnd = 0;              // this drop's own -2..+2 bars
    int dropPhaseBeats() const;
    qreal dropSizeAt(int beat) const;
    bool m_dropGrow = false;             // R360_DROP_GROW: the drop stepped up on an 8-bar line
    int m_dropGrowAt = -1;
    int m_dropLine = -1;                 // R367_GROW_TRACK: the TRACK beat the drop arrived on (never moved by a jump)
    int m_dropGrowLine = -1;             // ... and the track line the growth was last measured on
    // ---- R410_LAB: LASER LAB (track_runtime/tracklab.inc.cpp) ----
    void labLoad();
    void labSave();
    void labScan();
    bool labReady() const;
    QHash<QString, int> labValues(const QString &key, int step) const;
    QList<SceneValue> labSceneValues(const QString &key, const QString &colour, int step, int lamps) const;
    QStringList labList(int pass) const;
    QString labCurrentKey() const;
    void labRefresh();
    int labOrdinal(const QString &key) const;
    QString labRange(const QString &key) const;
    QString labTitleOf(const QString &key) const;
    QString labDetailOf(const QString &key) const;
    QString labTagKey() const;
    QStringList labTagList(const QString &kind) const;
    int labVerdictOf(const QString &key) const;
    int labCountOf(int verdict) const;
    void labShow();
    void labShowCandidate();
    void labRestartTimers();
    void labStepTick();
    void labAutoTick();
    void labShutdown(bool rebuildNow);
    Scene *labUpsertScene(QHash<QString, Function *> &mine, const QString &name,
                          const QString &path, const QList<SceneValue> &values);
    void labEnsureShow();
    void labApplyTags();
    bool m_labActive = false;
    bool m_labLoaded = false;
    bool m_labWired = false;
    bool m_labDark = false;              // STOP / TOO LOW: the lab holds the laser dark
    bool m_labHold = false;              // HOLD: no auto-play
    bool m_labTrySlow = false;           // TRY SLOWER on the variant playing
    bool m_labShowDirty = false;         // a kept look changed: rebuild on close
    int m_labPass = 0;
    int m_labIndex = 0;
    int m_labAuto = 0;
    int m_labLamps = 0;
    int m_labBeat = 0;
    int m_labBeatsShown = 0;
    int m_labSide = 0;
    quint32 m_labScene[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };   // the A/B preview scenes
    QString m_labColour = QStringLiteral("white");
    QString m_labGroup;
    QString m_labMessage;
    QString m_labResumeKey;
    QString m_labLastGood;
    QStringList m_labList;
    QStringList m_labColours;
    QList<quint32> m_labFixtures;        // in address order
    QHash<quint32, QHash<QString, quint32> > m_labChan;   // fixture -> role -> channel
    QHash<QString, int> m_labBase;       // role -> the operator's most common value
    QHash<QString, int> m_labColourValue;
    QJsonObject m_labStore;              // key -> { v, sec, en, ch, st, note, slow, ban, t }
    QTimer m_labAutoTimer;
    QTimer m_labStepTimer;
    QSet<quint32> m_labLive;             // this build's kept looks
    QHash<quint32, QString> m_labFuncKey;
    QSet<QString> m_labOnlyGroups;       // R410_LAB_ONLY
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
    bool m_closingOn = false;              // the closing sequence (SETTINGS_ENGINE_CLOSING) - R458_CLOSING_OFF
    qreal m_closingDim = 1.0;              // closingCap() as last applied to the light (closingTick)
    // R458_LAST_TRACK (track_runtime/lasttrack.inc.cpp)
    int m_lastStage = 0;                   // 0 off, 1 armed, 2 outro, 3 house light, 4 dark
    QString m_lastTitle;                   // the track that was made the last
    int m_lastOutro = -1;                  // its OUTRO flag, once played
    int m_lastBeatNow = 0;
    qreal m_lastBpm = 0.0;
    bool m_lastShown = false;              // the button's window
    bool m_lastWindowLatched = false;      // R500_DECL_LATCH: the window opened tonight (until 06:00)
    bool m_showOnNow = false;
    qint64 m_houseMs = 0;                  // when the house light began (m_clock)
    qreal m_houseHaze = 0.0;               // the haze it fades down from
    bool m_houseHazeDone = false;
    qreal m_houseLevel = -1.0;
    QMap<QString, quint32> m_houseZoom;    // group -> its widest-zoom scene
    // R479_LAST_WHITE_MEMBERS (Tobias 10-17): the warm white over the whole last track
    QMap<QString, quint32> m_houseWarm;    // group -> its warm white (RGB only) - never in the table
    QMap<QString, quint32> m_houseFade;    // group -> the same with its master dimmers at full
    QSet<QString> m_houseFadeRoot;         // ... the groups whose fade scene holds a dimmer
    int m_lastArmBeat = 0;                 // where it was armed: the white starts there
    qint64 m_lastBeatAtMs = -1;            // the last beat drawn (m_clock): the white between beats
    qreal m_lastWhiteP = -1.0;             // the white now, 0-1 of the house light (-1: not running)
    qreal m_houseFrom = 0.0;               // where the house light starts from
    qint64 m_mixerSourceMs = -1;          // LAST_TRACK_FRESH_MIXER: -1 is unknown
    qint64 m_airOffSourceMs = -1;
    qint64 m_fadersDownSourceMs = -1;
    qint64 m_airOffMs = -1;                // the deck off air since (m_clock); -1 on air or not known
    qint64 m_airSeenMs = -1;               // the last "pos" that said so
    qint64 m_fadersDownMs = -1;            // R484_LF_MEMBERS: every fader down since (m_clock); -1 up or not known
    qint64 m_fadersSeenMs = -1;            // ... the last "pos" that said so
    QString m_lastUndoTitle;               // ... the track LAST TRACK was undone on by hand: no fallback on it
    static int lastTrackMinutesNow();
    qint64 lastTrackSecsToClose() const;   // R463_LAST_SECS
    bool lastTrackHolds() const;           // R463_LAST_CLOCK
    int m_lastCloseMinSent = -100000;      // R463_HOUSE_SMALL
    bool m_linkIdle = false;               // R463_LINK_IDLE
    // R467_HAZE_NAG (track_runtime/hazenag.inc.cpp)
    int m_hazeNag = 0;
    qint64 m_hazeZeroMs = 0;               // how long the haze has stood at 0, watched
    qint64 m_hazeQuietUntil = -1;          // LATER (m_clock)
    qint64 m_hazeNagLastMs = -1;
    qint64 m_hazeBeatMs = -1;              // the last beat (m_clock): is music playing
    // R484_HZ_MEMBERS: haze AUTO (hazenag.inc.cpp) and HAZE & FAN BY CLOCK (clockhand.inc.cpp)
    int m_hazeLimit = 70;                 // saved per rig, 0..100 in 5% steps
    bool m_hazeAuto = false;               // off at every start-up, as ENERGY's AUTO (R378_ROOM_OFF)
    qint64 m_hazeAutoStepMs = -1;          // its last step (m_clock)
    QString m_hazeAutoNight;               // R487_B6_NIGHT: the night its state belongs to ("" = start-up)
    QString m_hazeAutoWhy;                 // R487_B6_WHY: the next change's reason in the log
    mutable QVector<int> m_atmosRowH;      // the row: haze % per point (-1 = none: AUTO holds)
    mutable QVector<int> m_atmosRowF;      // ... and the fan
    mutable quint32 m_atmosOwn = 0;        // bit i: point i is his - learning leaves it
    mutable bool m_atmosRowRead = false;
    quint32 m_atmosLearned = 0;            // bit i: point i holds his hand's average
    bool m_atmosRowFresh = false;          // the row has learned since start-up
    void atmosRowLoad() const;
    void atmosRowSave() const;
    void atmosIntoRow(int only);
    // R469_GROUP_FLASH (track_runtime/padflash.inc.cpp)
    QSet<QString> m_padFlash;              // the groups a pad holds (also in m_flashHeld)
    QHash<QString, qint64> m_padFlashMs;   // the last "still held" (m_clock)
    QHash<QString, quint32> m_padWasCol;   // R472_PAD_BACK: the colour scene at the press
    QHash<QString, qreal> m_padWasColLevel;
    /** full white under a held FLASH WHITE or a held pad: no MASTER, trim or pulse */
    bool heldFull(const QString &key) const;
    void padFlashClear();
    // R479_PAD_ONE_LASER (padflash.inc.cpp): the other laser type sits a held laser pad out
    QSet<QString> m_padLaserOut;           // what a pad took out of the cast (HOLD gives it back)
    bool padLaserDark(const QString &key, const QSet<QString> &cast) const;
    void padLaserCast(QSet<QString> &castSet, bool heldCast, qreal fader, const QStringList &eligible);
    // R470_COLOUR_MEMORY (track_runtime/colourmem.inc.cpp)
    QVariantMap m_colMem;                  // title -> { "<section start>": "section|set|mode", "_t" }
    bool m_colMemLoaded = false;
    bool m_colMemHand = false;             // he touched the colours in this play of the track
    bool m_colMemAfterStart = false;       // R472_COLMEM_START: the start scene was up
    bool m_colMemRecalled = false;         // the colours now are last time's
    QString m_colMemSection;
    int m_colMemFrom = -1;
    QString m_colMemLast;
    QString colourMemNow() const;
    void colourMemLoad();
    void colourMemWrite(int from, const QString &section, const QString &now);
    QString colourMemRecall(int from, const QString &section);
    void colourMemApply(const QString &entry);
    void colourMemoryBeat(int beat, const QString &state, bool sectionChanged);
    void colourMemoryTrack();
    void colourMemForget(const QString &title);
    // R470_SYNC (track_runtime/sync.inc.cpp)
    bool m_syncOpen = false;
    int m_syncStep = 0;
    QList<int> m_syncSound, m_syncLight;
    int m_syncResult = 0;
    int m_syncSpread = 0;
    mutable int m_syncOffset = 0;
    mutable bool m_syncOffsetRead = false;
    QStringList m_syncBlink;
    QSet<QString> m_syncOwned;             // R472_SYNC_OWN: what the blink itself holds
    int m_realSecStart = -1;               // R475_REALSTART_MEM
    int m_syncBeatNo = -1;                 // R472_SYNC_CLOCK: the link's last beat, its time, its length
    qint64 m_syncBeatAt = -1;
    qreal m_syncBeatLen = 0.0;
    QTimer m_syncBlinkTimer;
    bool m_syncBlinkTimerOn = false;
    void syncSetOffset(int ms);
    void syncBeat(int beat, qreal bpm);
    void syncBlinkOff();
    // R470_CLOCK_HAND (track_runtime/clockhand.inc.cpp)
    mutable QVariantMap m_clockHandStore;
    mutable bool m_clockHandRead = false;
    qint64 m_clockHandSum[ENGINE_CLOCK_POINTS] = {};
    qint64 m_clockHandCnt[ENGINE_CLOCK_POINTS] = {};
    qint64 m_clockHandSavedMs = -1;
    // R484_EH_MEMBERS: YOUR HAND written into ENERGY by clock's row (track_runtime/clockhand.inc.cpp)
    mutable quint32 m_clockOwn = 0;        // bit q: point q is his - a tap set it; learning leaves it
    mutable bool m_clockOwnRead = false;
    quint32 m_clockLearned = 0;            // bit q: point q holds his hand's value
    bool m_clockRowFresh = false;          // the row has learned since the curve was read
    int m_clockView = 0;                   // the nights it learns from (R484_EH_VIEW)
    void clockOwnLoad() const;
    void clockOwnSave() const;
    void clockHandIntoRow(int only);
    void clockHandLoad() const;
    void clockHandSave();
    QStringList clockHandNightsAll() const;
    void clockHandTick(bool showOn);
    void clockHandFlush();
    // R473_ATMOS (track_runtime/clockhand.inc.cpp)
    mutable QVariantMap m_atmosStore;
    mutable bool m_atmosRead = false;
    qint64 m_atmosHazeSum[ENGINE_ATMOS_CELLS] = {};
    qint64 m_atmosFanSum[ENGINE_ATMOS_CELLS] = {};
    qint64 m_atmosCnt[ENGINE_ATMOS_CELLS] = {};
    qint64 m_atmosSavedMs = -1;
    qint64 m_atmosMinuteMs = -1;
    qint64 m_atmosChangedMs = -1;          // a haze / fan change, not logged yet
    QString m_atmosNextBy, m_atmosChangedBy;   // R475_ATMOS_WHO_MEM: who makes it, who made it
    int m_atmosLoggedHaze = -1;
    int m_atmosLoggedFan = -1;
    bool m_atmosShowWas = false;
    // R482_LOG_HAND_MEM: ENERGY by hand not logged yet (from, to, when it last moved) and the
    // phone's drags (when a command last came)
    int m_energyHandFrom = -1;
    int m_energyHandTo = -1;
    qint64 m_energyHandMs = -1;
    QHash<QString, qint64> m_remoteLogMs;
    void energyHandTick();
    int m_atmosView = 0;
    QFile m_atmosFile;
    QString m_atmosFileNight;
    void atmosLoad() const;
    QStringList atmosNightsFor(int view) const;
    QString atmosBy(bool showOn) const;
    void atmosCsv(const QString &why, const QString &by);
    void atmosTick(bool showOn);
    void atmosFlush();
    // R467_MIC_CHECK (track_runtime/miccheck.inc.cpp)
    MicTap *m_mic = nullptr;               // a child QObject: the capture and what it heard
    bool m_micOpen = false;
    QString m_micDeviceName;
    int m_micDeviceIndex = -1;
    int m_micDeviceCount = 0;
    QString m_micProblem;
    QString m_micFileProblem;            // R475_MIC_FILE: the problem is the file, not the capture
    QVariantMap m_micShown;                // the last frame, as the panel reads it
    QTimer m_micUiTimer;                   // 100 ms while the capture runs
    QList<qint64> m_micLinkBeats;          // the link's beats of the last seconds (m_clock)
    QString m_micState;                    // the section playing (tick)
    qreal m_micBpm = 0.0;
    qint64 m_micTriedMs = -1;
    qint64 m_micUnwantedMs = -1;
    mutable bool m_micAutoRead = false;
    mutable QString m_micAutoStored;       // EVERY SECTION TONIGHT: the night it is on for
    QFile m_micFile;
    qint64 m_micRecStartMs = -1;
    qint64 m_micRecUntilMs = -1;           // a recording runs until (m_clock); -1 none
    int m_micRecLeftSent = -1;
    QString m_micRecId, m_micRecWhy, m_micRecSection, m_micRecTitle;
    QVariantList m_micRecs;                // tonight's recordings
    QString m_micRecsNight;
    QSet<QString> m_micAutoDone;           // title|section recorded tonight
    void micDevicesRead();
    bool micStart();
    void micStop();
    void micRead();
    void micWindow();
    void micFrame();
    QVariantList micTrackNow() const;
    bool micRecBegin(const QString &why);
    void micRow(const QVariantMap &v);
    void micRecEnd();
    int lastTrackEndBeat() const;
    int lastTrackOutroBeat() const;
    bool lastTrackHouse() const;
    void setLastStage(int stage);
    void lastTrackBeat(const QString &state, int beat, int secStart, qreal bpm);
    qreal lastTrackCap() const;
    bool lastTrackDark(const TrackGroup &g) const;
    bool houseGroup(const QString &key, bool evenOff = false) const;
    void ensureHouseZoom();
    void houseLook(qreal level);
    void enterHouse();
    // R479_LAST_WHITE_DECLS (lasttrack.inc.cpp)
    void ensureHouseFade();
    quint32 houseColour(const QString &key) const;
    qreal lastWhiteNow() const;
    qreal lastWhiteShow(const QString &group) const;
    void lastWhiteDraw(qreal p);
    void lastWhiteStop();
    qreal lastWhiteDim(const QString &group) const;
    void lastTrackAuto(bool showOn);       // R484_LF_DECLS (lasttrack.inc.cpp)
    int m_roomSent;                        // last percent handed to the ENERGY trim
    bool m_fullAuto;
    QSet<QString> m_laserMoveDark;         // R401_LIFT_TWO_BEATS: bar figures waiting a dark beat to move
    QSet<QString> m_figureEndDark;         // R401_FIGURE_END_DARK: laser figures dark a beat before they stop
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
    // SAVED_LOOKS_MEM: only JSON recipes persist; queued/undo state belongs to this play.
    mutable bool m_savedLoaded = false;
    mutable QVariantList m_savedLooks;
    QString m_savedFeedback;
    QString m_savedContextTitle;
    QVariantList m_savedMarkers;
    int m_savedCur = -1;
    QMap<int, QString> m_savedQueue;
    QVariantMap m_savedPlaying, m_savedVerdict, m_savedUndo, m_savedRestore;
    int m_savedPlayingFrom = -1, m_savedUndoFrom = -1;
    QString m_savedPlayingState, m_savedUndoState;
    int m_savedBeatBoundary = -1;
    mutable QHash<quint32, QString> m_savedProgramCache;
    mutable QHash<QString, quint32> m_savedProgramIndex;
    QVariantMap m_savedUserColours;
    QHash<quint32, int> m_savedUndoDown;
    QMap<QString, int> m_savedUndoAutoDown;
    int m_savedUndoBucket = -1;
    QList<QStringList> m_savedUndoRejectCasts;
    QStringList m_savedUndoRejectColours;
    QHash<QString, QList<int>> m_savedUndoRejectPatterns;
    QHash<QString, QStringList> m_savedUndoRejectFamilies;
    QString m_savedUndoRejectState, m_savedUndoRejectTitle;
    bool m_savedUndoFiled = false;
    void savedLoad() const;
    void savedWrite();
    QVariantMap savedCapture() const;
    QString savedRigKey(const QStringList &groups) const;
    QString savedProgramKey(quint32 id) const;
    quint32 savedProgramFind(const QString &key) const;
    QString savedInvalid(const QVariantMap &look) const;
    QVariantMap savedById(const QString &id) const;
    int savedNext(const QString &type) const;
    int savedPlanKey(int boundary, const QString &section) const;
    bool savedActive() const;
    void savedRemember();
    void savedBeforeNext();
    void savedBeat(const QString &state, int beat, int boundary);
    void savedStop();
    void savedReset();
    void savedCancelAt(int boundary);
    void savedCast(QSet<QString> &cast, bool held, bool calm);
    void savedColours();
    void savedMoves(qreal energy);
    void savedSweep(const QString &group);
    void savedZoom(const QString &group, int &want, int &mode);
    // R501_A_DECL (runde 501): saved looks keep the movement - the drop form, AUTO programmes by figure
    quint32 savedAutoFind(const QString &figure, const QString &group, const QString &colour, int kind = 0) const;
    void savedMotionIn(const QString &key, const QString &colour, quint32 &mf);
    int savedFormAt(int at) const;
    int savedFormLand(int at, int form, bool quiet, qreal fader, int beat) const;
    bool savedLayerOk() const;
    QString savedLookName(const QVariantMap &look) const;
    void savedNames() const;
    mutable QHash<QString, quint32> m_savedAutoCache;   // figure|group|colour|room|partner -> programme
    mutable int m_savedAutoStamp = -1;                   // ... for this many functions in the table
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
