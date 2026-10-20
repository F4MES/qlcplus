// Musical contours only; these helpers never write positions or DMX.
#ifndef TRACKSHOWPHRASES_H
#define TRACKSHOWPHRASES_H
#include <algorithm>
#include <cmath>

// Generated CHASEs share an eight-bar direction, with a continuous turn at
// the boundary. Saved chasers, ping-pong and laser positions never use this.
struct TrackChaseFrame
{
    int step = 0;
    int previous = 0;
    double trail = 0.0;
};

inline TrackChaseFrame trackChaseFrame(double beat, int stepBeats, int subSteps, int phase)
{
    beat = std::max(0.0, beat);
    const double rate = subSteps > 1 ? double(subSteps) : 1.0 / std::max(1, stepBeats);
    const int phrase = int(std::floor(beat / 32.0));
    const double local = (beat - phrase * 32.0) * rate;
    const int tick = int(std::floor(local + 1e-9));
    const int direction = (phrase % 2) == 0 ? 1 : -1;
    const int turn = int(std::lround(32.0 * rate));
    TrackChaseFrame frame;
    frame.step = phase + (direction > 0 ? tick : turn - tick);
    frame.previous = frame.step - direction;
    // At each turn the endpoint is unchanged for one step, so there is no
    // extra jump/flash just because the direction changes.
    if (phrase > 0 && tick == 0)
        frame.previous = frame.step + direction;
    const double fraction = std::max(0.0, local - tick);
    // At most a quarter beat, and at most a quarter of a fast step.
    const double overlap = std::min(0.25, 0.25 * rate);
    if ((phrase > 0 || tick > 0) && fraction < overlap)
        frame.trail = 0.45 * (1.0 - fraction / overlap);
    return frame;
}

// 0: attack then breadth, 1: rhythmic chase, 2: later climax.
inline double trackDropActivity(int flow, int bars)
{
    if (flow == 0)
        return bars < 4 ? 1.0 : 0.70;
    if (flow == 1)
        return 0.80;
    return bars < 4 ? 0.55 : (bars < 12 ? 1.0 : 0.80);
}

// Changes brightness distribution over 16 bars; never lifts any fixture.
inline double trackQuietSpatial(int index, int count, double beat)
{
    if (count < 2)
        return 1.0;
    const double phase = beat / 64.0 - double(index) / double(count);
    return 0.825 + 0.175 * std::cos(6.283185307179586 * phase);
}

// Slow programmes return in musical steps; the energy lift itself remains smooth.
inline int trackHitReturnPace(double energy, double originalEnergy)
{
    if (energy >= 1.0 - 1e-9 || originalEnergy >= 0.95)
        return 1;
    return energy >= 0.75 ? 1 : (energy >= 0.50 ? 2 : 4);
}
// A single question/answer phrase: two bars, then two bars. The other
// group stays present; the base always keeps at least 70 % of its own level.
inline double trackDialogueGain(int playedBeat, bool call, bool base)
{
    if (playedBeat < 0 || playedBeat >= 16) return 1.0;
    const bool leading = (playedBeat < 8) == call;
    return leading ? 1.0 : (base ? 0.70 : 0.45);
}
#endif
