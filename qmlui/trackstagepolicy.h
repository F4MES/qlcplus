#ifndef TRACKSTAGEPOLICY_H
#define TRACKSTAGEPOLICY_H
#include <algorithm>
#include <cmath>

namespace TrackStage {
inline double unit(double v) { return std::max(0.0, std::min(1.0, v)); }
inline double smooth(double v) { v = unit(v); return v * v * (3.0 - 2.0 * v); }
// Emphasis moves through the existing cast. Never raises a dimmer above its
// normal level; the first/last beat joins the unmodified show continuously.
inline double sequenceGain(double beat, int index, int count, bool base)
{
    if (count < 2 || index < 0 || beat < 0 || beat >= count * 4.0) return 1.0;
    const double envelope = smooth(beat) * smooth(count * 4.0 - beat);
    const double distance = std::abs(beat - (index * 4.0 + 2.0));
    const double focus = 1.0 - smooth((distance - 1.0) / 3.0);
    const double floor = base ? 0.75 : 0.40;
    return 1.0 - envelope * (1.0 - floor) * (1.0 - focus);
}
// Saturating exposure memory in seconds. Uses wall time, survives track changes,
// and treats missing ticks as inactivity instead of inventing three minutes of
// peak light while the player was paused/disconnected.
class Exposure {
    double charge_ = 0;
    long long last_ = -1;
    long long rest_ = -90000;
public:
    void sample(long long now, double density)
    {
        if (last_ < 0 || now < last_) { last_ = now; return; }
        const double elapsed = (now - last_) / 1000.0;
        last_ = now;
        const double active = std::min(2.0, elapsed);
        charge_ = std::max(0.0, std::min(240.0, charge_
            + active * (density >= 0.7 ? unit(density) : -1.5)
            - std::max(0.0, elapsed - active) * 1.5));
    }
    bool ready(long long now) const { return charge_ >= 180.0 && now - rest_ >= 90000; }
    void rest(long long now) { rest_ = now; charge_ = std::max(0.0, charge_ - 90.0); }
    double charge() const { return charge_; }
};
}
#endif
