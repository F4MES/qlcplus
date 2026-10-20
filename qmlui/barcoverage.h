// Laser-bar programme policy, Tobias 2026-10-20. No Qt or hardware dependencies.
#ifndef TRACKBARCOVERAGE_H
#define TRACKBARCOVERAGE_H

#include <algorithm>
#include <limits>
#include <vector>

inline int trackBarEyes(int dimmer, bool wheelWritten, int wheel, int eyeCount)
{
    if (dimmer == 0)
        return 0;
    if (wheel > 0 || (!wheelWritten && eyeCount == 0))
        return 8; // external palette under a dimmer-only chase
    return eyeCount; // explicit wheel 0 is per-eye mode, including all-dark
}

struct TrackBarCoverage
{
    bool shutter = false;       // authored hardware bursts stay out of AUTO
    bool compact = true;
    bool moving = false;
    std::vector<unsigned> previousMasks;
    bool known = false;
    bool whole = true;
    int minEyes = 8;             // minimum on any LIT bar; dark bars are allowed
    int peakEyes = 0;            // simultaneous eyes, not the union over the chase
    int minTotalEyes = std::numeric_limits<int>::max();

    void add(const std::vector<int> &lit)
    {
        known = true;
        int total = 0;
        for (int n : lit)
        {
            total += n;
            if (n > 0)
            {
                minEyes = std::min(minEyes, n);
                whole = whole && n == 8;
            }
        }
        peakEyes = std::max(peakEyes, total);
        if (total > 0)
            minTotalEyes = std::min(minTotalEyes, total);
    }

    void addMasks(const std::vector<unsigned> &masks)
    {
        if (!previousMasks.empty() && previousMasks != masks) moving = true;
        previousMasks = masks;
        for (unsigned mask : masks)
        {
            if (mask == 0) continue;
            int count = 0, edges = 0;
            for (int i = 0; i < 8; ++i)
            {
                count += (mask >> i) & 1u;
                edges += ((mask >> i) & 1u) != ((mask >> ((i + 1) % 8)) & 1u);
            }
            compact = compact && count >= 2 && count <= 4 && edges == 2;
        }
    }

    bool denseChase() const
    {
        return known && !shutter && compact && moving && minEyes >= 2 && minTotalEyes >= 10;
    }

    bool allows(double energy, bool drop) const
    {
        if (!known || shutter || peakEyes == 0)
            return false;
        if (whole)
            return true;
        // The physical stop, including DROP HIT's full lift. 99.5 % is NOT 100 %.
        if (minEyes < 2)
            return drop && energy >= 1.0 - 1e-9;
        // Whole bars are the explicit exception (one full bar is eight).
        // An eye chase on just one/two bars must not empty this 48-eye room.
        if (minTotalEyes < 10)
            return drop && energy >= 1.0 - 1e-9;
        return energy >= 0.75 || peakEyes > 12;
    }
};

// A separate draw for the visual class keeps hundreds of narrow programmes
// from drowning the whole-bar menu. Even at 100 %, single eyes are optional.
inline int trackBarDetailChance(double energy)
{
    if (energy < 0.75)
        return 0;
    const double ramp = std::min(1.0, std::max(0.0, (energy - 0.75) / 0.25));
    return 35 + int(50.0 * ramp); // still 15 % whole-bar variety at the stop
}

#endif
