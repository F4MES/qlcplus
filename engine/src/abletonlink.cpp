/*
  Q Light Controller Plus
  abletonlink.cpp

  Wrapper around the Ableton Link library. The Link/asio headers are only
  included here, so the rest of the engine stays free of them.
*/

#if defined(WIN32) || defined(_WIN32) || defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include <ableton/Link.hpp>

#include "abletonlink.h"

AbletonLink::AbletonLink(double initialBpm)
    : m_link(new ableton::Link(initialBpm))
    , m_enabled(false)
{
}

AbletonLink::~AbletonLink()
{
    if (m_link != nullptr)
    {
        m_link->enable(false);
        delete m_link;
        m_link = nullptr;
    }
}

void AbletonLink::setEnabled(bool enable)
{
    m_enabled = enable;
    if (m_link != nullptr)
        m_link->enable(enable);
}

bool AbletonLink::isEnabled() const
{
    return m_enabled && m_link != nullptr && m_link->isEnabled();
}

int AbletonLink::numPeers() const
{
    if (m_link == nullptr)
        return 0;
    return int(m_link->numPeers());
}

bool AbletonLink::capture(double quantum, double &beats, double &phase, double &tempo) const
{
    if (m_link == nullptr || !m_link->isEnabled())
        return false;

    auto state = m_link->captureAppSessionState();
    const auto now = m_link->clock().micros();
    beats = state.beatAtTime(now, quantum);
    phase = state.phaseAtTime(now, quantum);
    tempo = state.tempo();
    return true;
}

void AbletonLink::setTempo(double bpm)
{
    if (m_link == nullptr)
        return;

    auto state = m_link->captureAppSessionState();
    state.setTempo(bpm, m_link->clock().micros());
    m_link->commitAppSessionState(state);
}
