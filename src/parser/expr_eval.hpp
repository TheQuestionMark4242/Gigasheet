#pragma once

#include <cstdint>
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

// Excel-style aggregate over a cell range, e.g. SUM(A1:A100). Rows are
// 0-based inclusive; last == -1 means "through the end of the column"
// (from a whole-column range like A:A). fn is lowercase and normalized
// ("sum", "average", "min", "max", "count").
struct AggregateRequest {
    std::string fn;
    std::string column;
    std::int64_t first = 0;
    std::int64_t last = -1;
};

// Resolves an aggregate to its value (out) or fails with an error message.
// Provided by the table, which owns the data and the chunk-stats index.
using Aggregator = std::function<bool(
    const AggregateRequest&, double& out, std::string& error)>;

// Compiles the AST into a row function whose type (numeric or string) is
// determined bottom-up. Unknown column references, type mismatches (e.g.
// arithmetic on strings), unknown functions and wrong arities are compile
// errors (typos should not silently evaluate to 0).
// SUM/AVERAGE/MIN/MAX/COUNT over a range (=SUM(A1:A100)) are folded into
// constants through the aggregator at compile time; without an aggregator
// they are a compile error.
TypedCompileResult CompileTyped(
    const Node& root, const TypedColumnMap& columns,
    const Aggregator& aggregator = {});

// Numeric-only convenience wrapper: string-typed results are a compile error.
CompileResult CompileNumeric(const Node& root, const ColumnMap& columns);

} // namespace exprparse
