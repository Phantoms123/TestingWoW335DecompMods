#include "client_ripple_sprites.h"

#include "log.h"

#include <cstring>

namespace
{
constexpr uint32_t kInfoLoggedTransitions = 2;

bool ReadGate(volatile int32_t* gate, int32_t& value)
{
    value = *gate;
    return true;
}

bool WriteGate(volatile int32_t* gate, int32_t value)
{
    *gate = value;
    return true;
}
}

namespace engine
{
const ClientCodeGuard* ClientRippleGateGuards(size_t& count)
{
    count = 0;
    return nullptr;
}

bool ClientRippleGateCodeMatches(const ClientCodeView&)
{
    return true;
}
}

bool ClientRippleSprites::Bind(volatile int32_t* gate, const ClientCodeView& code)
{
    Restore();
    m_gate = nullptr;
    m_holding = false;
    m_transitions = 0;
    if (!gate || !engine::ClientRippleGateCodeMatches(code))
        return false;
    m_gate = gate;
    return true;
}

void ClientRippleSprites::Update(bool allowed, bool shaded, double seconds)
{
    if (!m_gate)
        return;
    if (!allowed)
    {
        Restore();
        return;
    }
    if (shaded || seconds < m_lastShaded)
        m_lastShaded = seconds;
    if (shaded)
        Hold();
    else if (m_holding && seconds - m_lastShaded > kClientSpriteGraceSeconds)
        Restore();
    else if (m_holding)
        Hold();
}

void ClientRippleSprites::Hold()
{
    int32_t current = 0;
    if (!ReadGate(m_gate, current))
    {
        Detach("read");
        return;
    }
    if (!m_holding)
        LogTransition("hidden while the ripples run", current);
    if (!m_holding || current != 0)
        m_remembered = current;
    m_holding = true;
    if (current != 0 && !WriteGate(m_gate, 0))
        Detach("written");
}

void ClientRippleSprites::Restore()
{
    if (!m_gate || !m_holding)
        return;
    m_holding = false;
    int32_t current = 0;
    if (!ReadGate(m_gate, current))
    {
        Detach("read");
        return;
    }
    if (current == 0 && !WriteGate(m_gate, m_remembered))
    {
        Detach("written");
        return;
    }
    LogTransition("shown again", current != 0 ? current : m_remembered);
}

void ClientRippleSprites::Detach(const char* reason)
{
    VF_LOG_ERROR("water: the client's waterRipples value could not be %s; its splash and wake sprites are left alone",
                 reason);
    m_gate = nullptr;
    m_holding = false;
}

void ClientRippleSprites::LogTransition(const char* text, int32_t value)
{
    const LogLevel level = m_transitions < kInfoLoggedTransitions ? LogLevel::Info : LogLevel::Debug;
    ++m_transitions;
    LogWrite(level, "water: the client's splash and wake sprites are %s (waterRipples %d)", text,
             static_cast<int>(value));
}

ClientRippleSprites& GlobalClientRippleSprites()
{
    static ClientRippleSprites sprites;
    return sprites;
}
