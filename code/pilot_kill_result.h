#pragma once

namespace pilot_kill_result {

enum class Icon { none, mech, vehicle };
enum class Medal { none, valor, uncommonValor };

// Killed records contain watch IDs. Retired objects cannot supply an icon or
// a class for valor credit; aggregate combat counters are accounted separately.
template<class Object, class ObjectClass>
Icon classify(Object* object, ObjectClass mechClass, ObjectClass vehicleClass)
{
    if (!object)
        return Icon::none;
    const auto objectClass = object->getObjectClass();
    if (objectClass == mechClass)
        return Icon::mech;
    if (objectClass == vehicleClass)
        return Icon::vehicle;
    return Icon::none;
}

constexpr bool countsForValor(Icon icon, bool airborne)
{
    return icon == Icon::mech && !airborne;
}

constexpr Medal medal(long groundMechKills)
{
    return groundMechKills >= 7 ? Medal::uncommonValor
        : groundMechKills >= 3 ? Medal::valor : Medal::none;
}

} // namespace pilot_kill_result
