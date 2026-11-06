// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
#pragma once

// What the new-node template (tools/dev/new_node/example_op.cpp) finds as "internal.h" when the
// tests build it: the operators' own internal.h, and the two things a real node gets from its line
// in builtin_operators.inc, under a test's type name (the catalog checks skip "Test" types).

#include "../../../../src/groom/groomlab/core/src/operators/internal.h"

namespace groomlab {
namespace op {
constexpr const char* kExample = "TestExample";
}  // namespace op
void registerExampleOperator(OperatorRegistry& registry);
}  // namespace groomlab
