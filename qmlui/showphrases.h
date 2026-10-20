// Musical contours only; these helpers never write positions or DMX.
#ifndef TRACKSHOWPHRASES_H
#define TRACKSHOWPHRASES_H
#include <algorithm>
#include <cmath>

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
#endif
