// resource_messages.h -- typed resource messages.
//
// message<IDS_EN_WEIGHT>(maxWeight) returns the catalog text for the id with its arguments substituted, as an owned std::string.
// It replaces cLoadString + sprintf into a fixed buffer: no buffer to overflow, no catalog text used as a format string, and a
// mod override that is malformed or has the wrong argument types falls back to the stock text (see GameOS/gameos/MC2Strings.h).
//
// Each id with arguments declares its signature once below. Arguments are checked at compile time against it:
// Int takes any integer type, Float any floating type, Str a string_view / const char* / std::string.
#pragma once

#include "../resource.h"
#include "gameos_resource.hpp"
#include "utilities.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mc2msg {

using MC2Strings::ArgKind;
using MC2Strings::MessageArg;

// Specialize per id: kinds lists the argument kinds in conversion order.
template <long Id>
struct MessageSig;

template <ArgKind... Kinds>
struct Sig
{
    static constexpr std::array<ArgKind, sizeof...(Kinds)> kinds{Kinds...};
};

template <ArgKind Kind, class T>
inline constexpr bool accepts_v =
    (Kind == ArgKind::Int && std::is_integral_v<std::remove_cvref_t<T>>) ||
    (Kind == ArgKind::Float && std::is_floating_point_v<std::remove_cvref_t<T>>) ||
    (Kind == ArgKind::Str && std::is_convertible_v<T, std::string_view>);

template <ArgKind Kind, class T>
MessageArg makeArg(const T& value)
{
    MessageArg arg;
    arg.kind = Kind;
    if constexpr (Kind == ArgKind::Int)
        arg.i = static_cast<long long>(value);
    else if constexpr (Kind == ArgKind::Float)
        arg.d = static_cast<double>(value);
    else
        arg.s = std::string_view(value);
    return arg;
}

template <long Id, class... Args, std::size_t... Index>
std::string messageImpl(std::index_sequence<Index...>, const Args&... args)
{
    constexpr const auto& kinds = MessageSig<Id>::kinds;
    static_assert((accepts_v<kinds[Index], Args> && ...), "resource message: an argument does not match the declared kind for this id");
    const MessageArg packed[sizeof...(Args) + 1] = {makeArg<kinds[Index]>(args)..., MessageArg()};
    return gos_FormatResourceMessage(gosResourceHandle, static_cast<DWORD>(Id), std::span<const ArgKind>(kinds),
                                     std::span<const MessageArg>(packed, sizeof...(Args)));
}

template <long Id, class... Args>
std::string message(const Args&... args)
{
    static_assert(sizeof...(Args) == MessageSig<Id>::kinds.size(), "resource message: wrong number of arguments for this id");
    return messageImpl<Id>(std::index_sequence_for<Args...>{}, args...);
}

// Signatures, from the catalog templates (data/defs/text/en_us/mc2res_legacy.fit).
template <> struct MessageSig<IDS_EN_HOUSE> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_EN_WEIGHT> : Sig<ArgKind::Float> {};
template <> struct MessageSig<IDS_EN_CLASS> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_EN_SPEED> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_JUMP> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_ARMOR> : Sig<ArgKind::Str, ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_WEIGHT> : Sig<ArgKind::Float> {};
template <> struct MessageSig<IDS_EN_WEAPON_RANGE> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_EN_WEAPON_DAMAGE> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_HEAT> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_AMMO> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_COST> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_ARMOR> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_EN_WEAPON_RATEOFFIRE> : Sig<ArgKind::Float> {};
template <> struct MessageSig<IDS_EN_BUILDING_WEIGHT> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_MP_LM_MAP_LIST_MAP_NAME> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_MP_LM_MAP_LIST_TYPE> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_MP_LM_MAP_LIST_MAX_PLAYERS> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_DIALOG_CBILLS> : Sig<ArgKind::Int> {};
template <> struct MessageSig<IDS_DIALOG_TIME> : Sig<ArgKind::Str, ArgKind::Str> {};
template <> struct MessageSig<IDS_DIALOG_OVERWRITE_PROMPT> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_DIALOG_DELETE_PROMPT> : Sig<ArgKind::Str> {};
template <> struct MessageSig<IDS_PLAYER_LEFT> : Sig<ArgKind::Str> {};

}  // namespace mc2msg
