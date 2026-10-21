#include "engine_actors.h"

#include "log.h"

#include "client.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;
constexpr uint32_t kMaxVisibleUnits = 4096;

bool FiniteWithin(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c)
{
    for (float axis : c.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f &&
           FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius) && c.height > 0.0f &&
           FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3])
{
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3])
{
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    return a.guid < b.guid;
}

bool ReadWaterContact(const client::Unit& unit, WaterContact& contact)
{
    if (!(unit.entityLiquidFlags & kEntityLiquidFound) || !(unit.entityLiquidFlags & kEntitySurfaceCrossesBody))
        return false;
    contact.guid = unit.guid;
    contact.onTransport = unit.onTransport;
    std::memcpy(contact.position, contact.onTransport ? unit.entityPosition : unit.position,
                sizeof(contact.position));
    contact.surface = unit.liquidSurface;
    contact.radius = unit.collisionRadius;
    contact.height = unit.collisionHeight;
    contact.swimming = client::UnitIsSwimming(unit.movementFlags);
    return true;
}

struct UnitWalk
{
    const float* centre;
    WaterContactFrame* out;
    uint32_t visited;
};

void VisitUnit(const client::Unit& unit, void* context)
{
    UnitWalk* walk = static_cast<UnitWalk*>(context);
    if (++walk->visited > kMaxVisibleUnits)
    {
        walk->out->truncated = true;
        return;
    }
    WaterContact contact;
    if (ReadWaterContact(unit, contact))
        engine::SelectWaterContact(*walk->out, contact, walk->centre);
}

bool ValidCentre(const float centre[3])
{
    for (int i = 0; i < 3; ++i)
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return true;
}
}

float WaterContactDepth(const WaterContact& contact)
{
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height)
{
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

namespace engine
{
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3])
{
    if (frame.count > kMaxWaterContacts || !ValidCentre(centre) || !PlausibleContact(contact) ||
        SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange)
        return false;
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre))
        ++index;
    if (index == kMaxWaterContacts)
        return false;
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i)
        frame.contacts[i] = frame.contacts[i - 1];
    frame.contacts[index] = contact;
    frame.count = count;
    return true;
}

bool CaptureWaterContactsFrom(uintptr_t, const float centre[3], WaterContactFrame& out)
{
    return CaptureWaterContacts(centre, out);
}

bool WaterContactsSupported()
{
    return true;
}

bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out)
{
    out = {};
    if (!ValidCentre(centre))
        return false;
    WaterContactFrame captured;
    UnitWalk walk = {centre, &captured, 0};
    if (!client::VisitVisibleUnits(&VisitUnit, &walk))
        return false;
    out = captured;
    return true;
}
}
