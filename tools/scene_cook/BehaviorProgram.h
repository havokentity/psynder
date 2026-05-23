// SPDX-License-Identifier: MIT
// Psynder behavior compiler IR shared by PsyScript and PsyGraph frontends.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <string>
#include <vector>

namespace psynder::tools {

enum class BehaviorScalarSource : u8 {
    Constant = 0,
    LinearIndex = 1,
};

struct BehaviorScalarExpr {
    BehaviorScalarSource source = BehaviorScalarSource::Constant;
    f32 base = 0.0f;
    f32 step = 0.0f;

    [[nodiscard]] static constexpr BehaviorScalarExpr constant(f32 value) noexcept {
        return {.source = BehaviorScalarSource::Constant, .base = value, .step = 0.0f};
    }

    [[nodiscard]] static constexpr BehaviorScalarExpr linear_index(f32 base,
                                                                   f32 step) noexcept {
        return {.source = BehaviorScalarSource::LinearIndex, .base = base, .step = step};
    }
};

struct BehaviorSpinOp {
    std::string name;
    std::string target_group;
    math::Vec3 axis{0.0f, 1.0f, 0.0f};
    BehaviorScalarExpr speed{};
    BehaviorScalarExpr phase{};
    bool active = true;
};

struct BehaviorProgram {
    std::vector<BehaviorSpinOp> spin_ops;

    [[nodiscard]] bool empty() const noexcept { return spin_ops.empty(); }
    void clear() noexcept { spin_ops.clear(); }
};

}  // namespace psynder::tools
