#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "expr_ast.hpp"

namespace exprparse {

// Row-indexed value functions. Numeric expressions produce FnNum, string
// expressions (CONCATENATE, LEFT, ...) produce FnStr.
using FnNum = std::function<double(int)>;
using FnStr = std::function<std::string(int)>;
using TypedFn = std::variant<FnNum, FnStr>;

// Column name (label or single-letter alias) -> row-indexed value function.
using ColumnMap = std::map<std::string, FnNum>;
using TypedColumnMap = std::map<std::string, TypedFn>;

struct CompileResult {
    FnNum fn;            // null on error
    std::string error;   // set on error
};

struct TypedCompileResult {
    std::optional<TypedFn> fn;   // empty on error
    std::string error;           // set on error
};

// Compiles the AST into a row function whose type (numeric or string) is
// determined bottom-up. Unknown column references, type mismatches (e.g.
// arithmetic on strings), unknown functions and wrong arities are compile
// errors (typos should not silently evaluate to 0).
TypedCompileResult CompileTyped(const Node& root, const TypedColumnMap& columns);

// Numeric-only convenience wrapper: string-typed results are a compile error.
CompileResult CompileNumeric(const Node& root, const ColumnMap& columns);

} // namespace exprparse
