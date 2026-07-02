#include "expr_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace exprparse {

namespace {

using Fn1 = double (*)(double);
using Fn2 = double (*)(double, double);

const std::map<std::string, Fn1>& UnaryFunctions() {
    static const std::map<std::string, Fn1> fns = {
        {"log",   [](double x) { return std::log(x); }},
        {"log10", [](double x) { return std::log10(x); }},
        {"log2",  [](double x) { return std::log2(x); }},
        {"exp",   [](double x) { return std::exp(x); }},
        {"sqrt",  [](double x) { return std::sqrt(x); }},
        {"abs",   [](double x) { return std::fabs(x); }},
        {"sin",   [](double x) { return std::sin(x); }},
        {"cos",   [](double x) { return std::cos(x); }},
        {"tan",   [](double x) { return std::tan(x); }},
        {"floor", [](double x) { return std::floor(x); }},
        {"ceil",  [](double x) { return std::ceil(x); }},
        {"round", [](double x) { return std::round(x); }},
    };
    return fns;
}

const std::map<std::string, Fn2>& BinaryFunctions() {
    static const std::map<std::string, Fn2> fns = {
        {"pow", [](double x, double y) { return std::pow(x, y); }},
        {"min", [](double x, double y) { return std::min(x, y); }},
        {"max", [](double x, double y) { return std::max(x, y); }},
    };
    return fns;
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

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
        const std::string name = ToLower(c.name);
        if (auto it = UnaryFunctions().find(name); it != UnaryFunctions().end()) {
            if (c.args.size() != 1) {
                error = name + "() takes exactly 1 argument, got "
                    + std::to_string(c.args.size());
                return {};
            }
            auto arg = (*this)(*c.args[0]);
            if (!arg) return {};
            const Fn1 fn = it->second;
            return [fn, arg](int i) { return fn(arg(i)); };
        }
        if (auto it = BinaryFunctions().find(name); it != BinaryFunctions().end()) {
            if (c.args.size() != 2) {
                error = name + "() takes exactly 2 arguments, got "
                    + std::to_string(c.args.size());
                return {};
            }
            auto a = (*this)(*c.args[0]);
            if (!a) return {};
            auto b = (*this)(*c.args[1]);
            if (!b) return {};
            const Fn2 fn = it->second;
            return [fn, a, b](int i) { return fn(a(i), b(i)); };
        }
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
