#pragma once

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace exprparse {

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct NumberLit { double value; };
struct StringLit { std::string value; };   // '...' literal (string functions)
struct ColumnRef { std::string name; };    // bare identifier or "quoted name"
struct RangeRef  { std::string a; std::string b; };  // A1:A100 (aggregate arg)
struct Unary     { char op; NodePtr operand; };
struct Binary    { char op; NodePtr lhs; NodePtr rhs; };
struct Call      { std::string name; std::vector<NodePtr> args; };

struct Node {
    std::variant<NumberLit, StringLit, ColumnRef, RangeRef, Unary, Binary, Call> value;
};

template <typename T>
NodePtr MakeNode(T&& v) {
    return std::make_unique<Node>(Node{std::forward<T>(v)});
}

} // namespace exprparse
