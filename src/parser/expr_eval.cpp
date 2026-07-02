#include "expr_eval.hpp"

namespace exprparse {

namespace {

struct Compiler {
    const ColumnMap& columns;
    std::string error;

    std::function<double(int)> operator()(const Node& node) {
        return std::visit(*this, node.value);
    }

    std::function<double(int)> operator()(const NumberLit& n) {
        const double v = n.value;
        return [v](int) { return v; };
    }

    std::function<double(int)> operator()(const StringLit&) {
        error = "String literals are not supported in numeric expressions.";
        return {};
    }

    std::function<double(int)> operator()(const ColumnRef& c) {
        auto it = columns.find(c.name);
        if (it == columns.end()) {
            error = "Unknown column: " + c.name;
            return {};
        }
        return it->second;
    }

    std::function<double(int)> operator()(const Unary& u) {
        auto rhs = (*this)(*u.operand);
        if (!rhs) return {};
        if (u.op == '-') {
            return [rhs](int i) { return -rhs(i); };
        }
        return rhs;
    }

    std::function<double(int)> operator()(const Binary& b) {
        auto lhs = (*this)(*b.lhs);
        if (!lhs) return {};
        auto rhs = (*this)(*b.rhs);
        if (!rhs) return {};
        switch (b.op) {
            case '+': return [lhs, rhs](int i) { return lhs(i) + rhs(i); };
            case '-': return [lhs, rhs](int i) { return lhs(i) - rhs(i); };
            case '*': return [lhs, rhs](int i) { return lhs(i) * rhs(i); };
            case '/': return [lhs, rhs](int i) { return lhs(i) / rhs(i); };
        }
        error = std::string("Unknown operator: ") + b.op;
        return {};
    }

    std::function<double(int)> operator()(const Call& c) {
        error = "Unknown function: " + c.name;
        return {};
    }
};

} // namespace

CompileResult CompileNumeric(const Node& root, const ColumnMap& columns) {
    Compiler compiler{columns, {}};
    CompileResult result;
    result.fn = compiler(root);
    if (!result.fn) {
        result.error = compiler.error.empty() ? "compile error" : compiler.error;
    }
    return result;
}

} // namespace exprparse
