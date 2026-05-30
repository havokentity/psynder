// SPDX-License-Identifier: MIT
// Psynder — PsyGraph node-type catalog. Lane 15 owns.

#include "NodeTypes.h"

#include <array>

namespace psynder::script::psygraph {

namespace {

// Pin tables. `constexpr` arrays the NodeTypeInfo spans point into.
constexpr PinSpec kExecOut1[] = {{"Then", ValueType::Exec}};
constexpr PinSpec kBranchOut[] = {{"True", ValueType::Exec}, {"False", ValueType::Exec}};
constexpr PinSpec kSeqOut[] = {{"Then0", ValueType::Exec}, {"Then1", ValueType::Exec}};

constexpr PinSpec kTickOut[] = {{"DeltaTime", ValueType::Float}};
constexpr PinSpec kTriggerOut[] = {{"Other", ValueType::Entity}};
constexpr PinSpec kDamagedOut[] = {{"Amount", ValueType::Float}, {"Source", ValueType::Entity}};

constexpr PinSpec kFF_in[] = {{"A", ValueType::Float}, {"B", ValueType::Float}};
constexpr PinSpec kF_in[] = {{"A", ValueType::Float}};
constexpr PinSpec kBB_in[] = {{"A", ValueType::Bool}, {"B", ValueType::Bool}};
constexpr PinSpec kB_in[] = {{"A", ValueType::Bool}};
constexpr PinSpec kFloatOut[] = {{"Result", ValueType::Float}};
constexpr PinSpec kBoolOut[] = {{"Result", ValueType::Bool}};

constexpr PinSpec kCond_in[] = {{"Condition", ValueType::Bool}};

constexpr PinSpec kAnyOut[] = {{"Value", ValueType::Any}};
constexpr PinSpec kAny_in[] = {{"Value", ValueType::Any}};

constexpr PinSpec kLitFloatOut[] = {{"Value", ValueType::Float}};
constexpr PinSpec kLitBoolOut[] = {{"Value", ValueType::Bool}};
constexpr PinSpec kLitIntOut[] = {{"Value", ValueType::Int}};
constexpr PinSpec kLitStrOut[] = {{"Value", ValueType::String}};

constexpr PinSpec kLog_in[] = {{"Message", ValueType::String}};
constexpr PinSpec kSetHealth_in[] = {{"Target", ValueType::Entity}, {"Health", ValueType::Float}};
constexpr PinSpec kApplyDamage_in[] = {{"Target", ValueType::Entity}, {"Amount", ValueType::Float}};
constexpr PinSpec kSpawn_in[] = {{"Prefab", ValueType::String}};
constexpr PinSpec kSpawnOut[] = {{"Spawned", ValueType::Entity}};
constexpr PinSpec kSetActive_in[] = {{"Target", ValueType::Entity}, {"Active", ValueType::Bool}};
constexpr PinSpec kPlaySound_in[] = {{"Sound", ValueType::String}};

// Wave 13 pin tables.
constexpr PinSpec kClamp_in[] = {
    {"A", ValueType::Float}, {"Min", ValueType::Float}, {"Max", ValueType::Float}};
constexpr PinSpec kLerp_in[] = {
    {"A", ValueType::Float}, {"B", ValueType::Float}, {"T", ValueType::Float}};
constexpr PinSpec kRand_in[] = {{"Min", ValueType::Float}, {"Max", ValueType::Float}};
constexpr PinSpec kSetVel_in[] = {{"Target", ValueType::Entity},
                                  {"X", ValueType::Float},
                                  {"Y", ValueType::Float},
                                  {"Z", ValueType::Float}};
constexpr PinSpec kGetHealth_in[] = {{"Target", ValueType::Entity}};
constexpr PinSpec kHealthOut[] = {{"Health", ValueType::Float}};

// Helper to keep the table compact and readable.
#define PSG_NODE(ID, NAME, EV, PURE, EIN, EOUT, DIN, DOUT, PARAMS) \
    NodeTypeInfo {                                                 \
        NodeTypeId::ID, NAME, EV, PURE, EIN, EOUT, DIN, DOUT, PARAMS \
    }

const NodeTypeInfo kCatalog[] = {
    // Events
    PSG_NODE(OnStart, "OnStart", true, false, 0, kExecOut1, {}, {}, 0),
    PSG_NODE(OnTick, "OnTick", true, false, 0, kExecOut1, {}, kTickOut, 0),
    PSG_NODE(OnTrigger, "OnTrigger", true, false, 0, kExecOut1, {}, kTriggerOut, 0),
    PSG_NODE(OnDamaged, "OnDamaged", true, false, 0, kExecOut1, {}, kDamagedOut, 0),

    // Flow
    PSG_NODE(Branch, "Branch", false, false, 1, kBranchOut, kCond_in, {}, 0),
    PSG_NODE(Sequence, "Sequence", false, false, 1, kSeqOut, {}, {}, 0),
    PSG_NODE(Once, "Once", false, false, 1, kExecOut1, {}, {}, 1),  // param0 = var slot

    // Math
    PSG_NODE(Add, "Add", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Sub, "Sub", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Mul, "Mul", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Div, "Div", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Neg, "Neg", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Min, "Min", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Max, "Max", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Abs, "Abs", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Sign, "Sign", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Floor, "Floor", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Ceil, "Ceil", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Sqrt, "Sqrt", false, true, 0, {}, kF_in, kFloatOut, 0),
    PSG_NODE(Mod, "Mod", false, true, 0, {}, kFF_in, kFloatOut, 0),
    PSG_NODE(Clamp, "Clamp", false, true, 0, {}, kClamp_in, kFloatOut, 0),
    PSG_NODE(Lerp, "Lerp", false, true, 0, {}, kLerp_in, kFloatOut, 0),
    PSG_NODE(RandomRange, "RandomRange", false, true, 0, {}, kRand_in, kFloatOut, 1),  // param0 = seed

    // Compare / logic
    PSG_NODE(Equal, "Equal", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(Less, "Less", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(Greater, "Greater", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(And, "And", false, true, 0, {}, kBB_in, kBoolOut, 0),
    PSG_NODE(Or, "Or", false, true, 0, {}, kBB_in, kBoolOut, 0),
    PSG_NODE(Not, "Not", false, true, 0, {}, kB_in, kBoolOut, 0),
    PSG_NODE(NotEqual, "NotEqual", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(LessEqual, "LessEqual", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(GreaterEqual, "GreaterEqual", false, true, 0, {}, kFF_in, kBoolOut, 0),
    PSG_NODE(Xor, "Xor", false, true, 0, {}, kBB_in, kBoolOut, 0),

    // Variables
    PSG_NODE(GetVar, "GetVar", false, true, 0, {}, {}, kAnyOut, 1),
    PSG_NODE(SetVar, "SetVar", false, false, 1, kExecOut1, kAny_in, {}, 1),

    // Literals
    PSG_NODE(LiteralFloat, "LiteralFloat", false, true, 0, {}, {}, kLitFloatOut, 1),
    PSG_NODE(LiteralBool, "LiteralBool", false, true, 0, {}, {}, kLitBoolOut, 1),
    PSG_NODE(LiteralInt, "LiteralInt", false, true, 0, {}, {}, kLitIntOut, 1),
    PSG_NODE(LiteralString, "LiteralString", false, true, 0, {}, {}, kLitStrOut, 1),

    // Actions
    PSG_NODE(Log, "Log", false, false, 1, kExecOut1, kLog_in, {}, 0),
    PSG_NODE(SetHealth, "SetHealth", false, false, 1, kExecOut1, kSetHealth_in, {}, 0),
    PSG_NODE(ApplyDamage, "ApplyDamage", false, false, 1, kExecOut1, kApplyDamage_in, {}, 0),
    PSG_NODE(SpawnEntity, "SpawnEntity", false, false, 1, kExecOut1, kSpawn_in, kSpawnOut, 0),
    PSG_NODE(SetActive, "SetActive", false, false, 1, kExecOut1, kSetActive_in, {}, 0),
    PSG_NODE(PlaySound, "PlaySound", false, false, 1, kExecOut1, kPlaySound_in, {}, 0),
    PSG_NODE(SetVelocity, "SetVelocity", false, false, 1, kExecOut1, kSetVel_in, {}, 0),

    // ECS reads (data node materialized via a host getter, like the event pins)
    PSG_NODE(GetHealth, "GetHealth", false, true, 0, {}, kGetHealth_in, kHealthOut, 0),
};

#undef PSG_NODE

}  // namespace

const NodeTypeInfo* find_node_type(NodeTypeId id) noexcept {
    for (const NodeTypeInfo& info : kCatalog) {
        if (info.id == id)
            return &info;
    }
    return nullptr;
}

std::span<const NodeTypeInfo> all_node_types() noexcept {
    return std::span<const NodeTypeInfo>(kCatalog, sizeof(kCatalog) / sizeof(kCatalog[0]));
}

}  // namespace psynder::script::psygraph
