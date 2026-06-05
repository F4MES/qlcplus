/*
  Q Light Controller Plus
  abletonlink.h

  Thin wrapper around the (header-only) Ableton Link library, keeping the
  Link/asio headers out of the widely-included engine headers.

  Licensed under the Apache License, Version 2.0 (the "License");
  Note: the Ableton Link library itself is GPLv2+; a build that links it is
  therefore subject to the GPL. This wrapper only forward-declares it.
*/

#ifndef ABLETONLINK_H
#define ABLETONLINK_H

namespace ableton { class Link; }

/** @addtogroup engine Engine
 * @{
 */

class AbletonLink
{
public:
    AbletonLink(double initialBpm = 120.0);
    ~AbletonLink();

    /** Enable/disable the Link session (network discovery + sync). */
    void setEnabled(bool enable);
    bool isEnabled() const;

    /** Number of other Link peers currently in the session. */
    int numPeers() const;

    /** Capture the current shared timeline.
     *  @param quantum  bar length in beats used for phase (e.g. 4.0)
     *  @param beats    [out] absolute beat position (may be fractional)
     *  @param phase    [out] position within the current bar [0..quantum)
     *  @param tempo    [out] shared tempo in BPM
     *  @return true if Link is enabled and the values were captured. */
    bool capture(double quantum, double &beats, double &phase, double &tempo) const;

    /** Propose a new tempo to the Link session. */
    void setTempo(double bpm);

private:
    ableton::Link *m_link;
    bool m_enabled;
};

/** @} */

#endif
