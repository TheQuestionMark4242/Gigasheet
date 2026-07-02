#pragma once

#include <functional>
#include <map>
#include <string>

#include "expr_ast.hpp"

namespace exprparse {

// Column name (label or single-letter alias) -> row-indexed value function.
using ColumnMap = std::map<std::string, std::function<double(int)>>;

struct CompileResult {
    std::function<double(int)> fn;   // null on error
    std::string error;               // set on error
};

// Compiles the AST into a numeric row function. Unknown column references
// are a compile error (typos should not silently evaluate to 0).
CompileResult CompileNumeric(const Node& root, const ColumnMap& columns);

} // namespace exprparse
