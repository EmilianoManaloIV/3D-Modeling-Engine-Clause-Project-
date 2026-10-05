#pragma once
// Arithmetic for typed-in numbers: "2*3", "(1+2)/4", "-0.5", "2^3", "pi/2",
// "90deg" is not supported, but relative edits are: "+=0.5", "-=1", "*=2",
// "/=4" apply to the field's current value (like Blender / Unity fields).
#include <string>

namespace expr {

// Returns false (and leaves `out` alone) if the text is not a valid expression.
bool evaluate(const std::string& text, double current, double& out);

}  // namespace expr
