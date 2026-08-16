#pragma once

#include <string>

#include "expr_ast.hpp"

namespace exprparse {

// Holds the outcome of a parse; shared between the flex scanner and the
// bison parser so no global state is needed (the app is a GUI, parses can
// happen from anywhere).
class Driver {
public:
    NodePtr result;
    std::string error;
};

struct ParseResult {
    NodePtr root;        // null on failure
    std::string error;   // set on failure
};

// Parses an expression like "A + B*2" (without the leading '=').
ParseResult Parse(const std::string& src);

} // namespace exprparse
