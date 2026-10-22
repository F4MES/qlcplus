// Musical contours only; these helpers never write positions or DMX.
#ifndef TRACKSHOWPHRASES_H
#define TRACKSHOWPHRASES_H
#include <algorithm>
#include <cmath>
#include <utility>

// Generated CHASEs share an eight-bar direction, with a continuous turn at
// the boundary. Saved chasers, ping-pong and laser positions never use this.
struct TrackChaseFrame
{
    int step = 0;
    int previous = 0;
    double trail = 0.0;
};

// Generated chases only. Events are positions, never an instruction to close eyes.
// 1: 3-3-2 over four nominal steps; 2: offbeat; 3: two pairs, then space.
inline TrackChaseFrame trackChaseFrame(double beat, int stepBeats, int subSteps, int phase, int rhythm = 0)
{
    beat = std::isfinite(beat) ? std::max(0.0, beat) : 0.0;
    const double rate = subSteps > 1 ? double(subSteps) : 1.0 / std::max(1, stepBeats);
    const int phrase = int(std::floor(beat / 32.0));
    const double local = (beat - phrase * 32.0) * rate;
    auto at = [rhythm](double x) {
        const int cycle = int(std::floor(x / 4.0));
        const double p = x - cycle * 4.0;
        int tick = int(std::floor(x + 1e-9));
        double last = double(tick);
        if (rhythm == 1) {
            const int inner = (p >= 3.0 - 1e-9) ? 2 : (p >= 1.5 - 1e-9 ? 1 : 0);
            tick = cycle * 3 + inner;
            last = cycle * 4.0 + (inner == 2 ? 3.0 : inner == 1 ? 1.5 : 0.0);
        } else if (rhythm == 2) {
            tick = int(std::floor(x + 0.5 + 1e-9));
            last = tick > 0 ? tick - 0.5 : 0.0;
        } else if (rhythm == 3) {
            const int inner = p >= 2.5 - 1e-9 ? 3 : p >= 2.0 - 1e-9 ? 2 : p >= 0.5 - 1e-9 ? 1 : 0;
            tick = cycle * 4 + inner;
            last = cycle * 4.0 + (inner == 3 ? 2.5 : inner == 2 ? 2.0 : inner == 1 ? 0.5 : 0.0);
        }
        return std::make_pair(tick, last);
    };
    const auto point = at(local);
    const int tick = point.first;
    const int direction = (phrase % 2) == 0 ? 1 : -1;
    const int turn = at(32.0 * rate).first;
    TrackChaseFrame frame;
    frame.step = phase + (direction > 0 ? tick : turn - tick);
    frame.previous = frame.step - direction;
    if (phrase > 0 && tick == 0)
        frame.previous = frame.step + direction;
    const double since = std::max(0.0, local - point.second);
    const double overlap = std::min(rhythm == 3 ? 0.125 : 0.25, 0.25 * rate); // fits even a paired half-step
    if ((phrase > 0 || tick > 0) && since < overlap)
        frame.trail = 0.45 * (1.0 - since / overlap);
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
