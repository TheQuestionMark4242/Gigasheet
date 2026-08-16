#include "expr_eval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

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

// Excel-style rendering of a number inside CONCATENATE: fixed notation
// with trailing zeros (and a bare trailing '.') trimmed, so 2.0 -> "2".
std::string FormatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    const auto last = s.find_last_not_of('0');
    if (last != std::string::npos) {
        s.erase(s[last] == '.' ? last : last + 1);
    }
    return s;
}

// "A100" -> {"A", 100}; "A" -> {"A", -1} (no row, e.g. whole-column A:A).
struct CellRef {
    std::string name;
    std::int64_t row = -1;   // 1-based, -1 if absent
};

CellRef SplitCellRef(const std::string& s) {
    std::size_t i = s.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
    CellRef r;
    if (i == 0 || i == s.size()) {
        r.name = s;
    } else {
        r.name = s.substr(0, i);
        r.row = std::stoll(s.substr(i));
    }
    return r;
}

bool IsAggregateName(const std::string& lower) {
    return lower == "sum" || lower == "average" || lower == "avg" ||
           lower == "min" || lower == "max" || lower == "count";
}

struct TypedCompiler {
    const TypedColumnMap& columns;
    const Aggregator& aggregator;
    std::string error;

    using Result = std::optional<TypedFn>;

    Result operator()(const Node& node) {
        return std::visit(*this, node.value);
    }

    // Compile a subexpression that must be numeric; `ctx` names the operator
    // or function for the error message.
    FnNum Num(const Node& node, const std::string& ctx) {
        Result r = (*this)(node);
        if (!r) return {};
        if (auto* fn = std::get_if<FnNum>(&*r)) return *fn;
        error = ctx + " requires a numeric value; use CONCATENATE() to combine strings.";
        return {};
    }

    // Compile a subexpression that must be a string.
    FnStr Str(const Node& node, const std::string& ctx) {
        Result r = (*this)(node);
        if (!r) return {};
        if (auto* fn = std::get_if<FnStr>(&*r)) return *fn;
        error = ctx + " requires a string value (a text column or '...' literal).";
        return {};
    }

    // Compile a subexpression of either type, coercing numbers to text.
    FnStr AsText(const Node& node) {
        Result r = (*this)(node);
        if (!r) return {};
        if (auto* fn = std::get_if<FnStr>(&*r)) return *fn;
        const FnNum fn = std::get<FnNum>(*r);
        return [fn](int i) { return FormatNumber(fn(i)); };
    }

    Result operator()(const NumberLit& n) {
        const double v = n.value;
        return TypedFn{FnNum([v](int) { return v; })};
    }

    Result operator()(const StringLit& s) {
        const std::string v = s.value;
        return TypedFn{FnStr([v](int) { return v; })};
    }

    Result operator()(const ColumnRef& c) {
        auto it = columns.find(c.name);
        if (it == columns.end()) {
            error = "Unknown column: " + c.name;
            return {};
        }
        return it->second;
    }

    Result operator()(const RangeRef& r) {
        error = "A range like " + r.a + ":" + r.b
            + " can only be used inside SUM/AVERAGE/MIN/MAX/COUNT.";
        return {};
    }

    // SUM(A1:A100) and friends: resolved through the aggregator at compile
    // time and folded into a constant.
    Result CompileAggregate(const std::string& name, const RangeRef& range) {
        if (!aggregator) {
            error = name + "() with a range is not available here.";
            return {};
        }
        const CellRef a = SplitCellRef(range.a);
        const CellRef b = SplitCellRef(range.b);
        if (a.name != b.name) {
            error = "Range must stay in one column: " + range.a + ":" + range.b;
            return {};
        }

        AggregateRequest req;
        req.fn = (name == "avg") ? "average" : name;
        req.column = a.name;
        req.first = a.row > 0 ? a.row - 1 : 0;      // 1-based -> 0-based
        req.last = b.row > 0 ? b.row - 1 : -1;      // -1: through the end
        if (a.row > 0 && b.row > 0 && req.first > req.last) {
            std::swap(req.first, req.last);          // A100:A1 == A1:A100
        }

        double out = 0.0;
        if (!aggregator(req, out, error)) {
            if (error.empty()) error = "Failed to compute " + name + "()";
            return {};
        }
        return TypedFn{FnNum([out](int) { return out; })};
    }

    Result operator()(const Unary& u) {
        auto rhs = Num(*u.operand, std::string("Unary '") + u.op + "'");
        if (!rhs) return {};
        if (u.op == '-') {
            return TypedFn{FnNum([rhs](int i) { return -rhs(i); })};
        }
        return TypedFn{rhs};
    }

    Result operator()(const Binary& b) {
        const std::string ctx = std::string("Operator '") + b.op + "'";
        auto lhs = Num(*b.lhs, ctx);
        if (!lhs) return {};
        auto rhs = Num(*b.rhs, ctx);
        if (!rhs) return {};
        switch (b.op) {
            case '+': return TypedFn{FnNum([lhs, rhs](int i) { return lhs(i) + rhs(i); })};
            case '-': return TypedFn{FnNum([lhs, rhs](int i) { return lhs(i) - rhs(i); })};
            case '*': return TypedFn{FnNum([lhs, rhs](int i) { return lhs(i) * rhs(i); })};
            case '/': return TypedFn{FnNum([lhs, rhs](int i) { return lhs(i) / rhs(i); })};
        }
        error = std::string("Unknown operator: ") + b.op;
        return {};
    }

    bool CheckArity(const Call& c, const std::string& name, std::size_t want) {
        if (c.args.size() == want) return true;
        error = name + "() takes exactly " + std::to_string(want)
            + (want == 1 ? " argument, got " : " arguments, got ")
            + std::to_string(c.args.size());
        return false;
    }

    Result operator()(const Call& c) {
        const std::string name = ToLower(c.name);

        // Aggregates take a single range argument; MIN/MAX without a range
        // fall through to the two-argument math functions.
        if (IsAggregateName(name)) {
            if (c.args.size() == 1 &&
                std::holds_alternative<RangeRef>(c.args[0]->value))
            {
                return CompileAggregate(name, std::get<RangeRef>(c.args[0]->value));
            }
            if (name != "min" && name != "max") {
                error = name + "() expects a cell range, e.g. "
                    + c.name + "(A1:A100) or " + c.name + "(A:A)";
                return {};
            }
        }

        if (auto it = UnaryFunctions().find(name); it != UnaryFunctions().end()) {
            if (!CheckArity(c, name, 1)) return {};
            auto arg = Num(*c.args[0], name + "()");
            if (!arg) return {};
            const Fn1 fn = it->second;
            return TypedFn{FnNum([fn, arg](int i) { return fn(arg(i)); })};
        }

        if (auto it = BinaryFunctions().find(name); it != BinaryFunctions().end()) {
            if (!CheckArity(c, name, 2)) return {};
            auto a = Num(*c.args[0], name + "()");
            if (!a) return {};
            auto b = Num(*c.args[1], name + "()");
            if (!b) return {};
            const Fn2 fn = it->second;
            return TypedFn{FnNum([fn, a, b](int i) { return fn(a(i), b(i)); })};
        }

        if (name == "concatenate") {
            if (c.args.empty()) {
                error = "concatenate() takes at least 1 argument, got 0";
                return {};
            }
            std::vector<FnStr> parts;
            parts.reserve(c.args.size());
            for (const NodePtr& arg : c.args) {
                auto part = AsText(*arg);
                if (!part) return {};
                parts.push_back(std::move(part));
            }
            return TypedFn{FnStr([parts](int i) {
                std::string out;
                for (const FnStr& part : parts) out += part(i);
                return out;
            })};
        }

        if (name == "left" || name == "right") {
            if (!CheckArity(c, name, 2)) return {};
            auto s = Str(*c.args[0], name + "()");
            if (!s) return {};
            auto n = Num(*c.args[1], name + "()");
            if (!n) return {};
            const bool left = (name == "left");
            return TypedFn{FnStr([s, n, left](int i) {
                const std::string v = s(i);
                const double raw = n(i);
                const std::size_t count = raw <= 0.0 ? 0
                    : std::min(v.size(), static_cast<std::size_t>(raw));
                return left ? v.substr(0, count) : v.substr(v.size() - count);
            })};
        }

        if (name == "mid") {
            if (!CheckArity(c, name, 3)) return {};
            auto s = Str(*c.args[0], "mid()");
            if (!s) return {};
            auto start = Num(*c.args[1], "mid()");
            if (!start) return {};
            auto len = Num(*c.args[2], "mid()");
            if (!len) return {};
            return TypedFn{FnStr([s, start, len](int i) {
                const std::string v = s(i);
                // Excel MID: 1-based start, clamped; non-positive length -> "".
                const double rawStart = start(i);
                const std::size_t pos = rawStart <= 1.0 ? 0
                    : std::min(v.size(), static_cast<std::size_t>(rawStart) - 1);
                const double rawLen = len(i);
                const std::size_t count = rawLen <= 0.0 ? 0
                    : std::min(v.size() - pos, static_cast<std::size_t>(rawLen));
                return v.substr(pos, count);
            })};
        }

        if (name == "len") {
            if (!CheckArity(c, name, 1)) return {};
            auto s = Str(*c.args[0], "len()");
            if (!s) return {};
            return TypedFn{FnNum([s](int i) {
                return static_cast<double>(s(i).size());
            })};
        }

        error = "Unknown function: " + c.name;
        return {};
    }
};

} // namespace

TypedCompileResult CompileTyped(
    const Node& root, const TypedColumnMap& columns,
    const Aggregator& aggregator)
{
    TypedCompiler compiler{columns, aggregator, {}};
    TypedCompileResult result;
    result.fn = compiler(root);
    if (!result.fn) {
        result.error = compiler.error.empty() ? "compile error" : compiler.error;
    }
    return result;
}

std::vector<std::string> FunctionNames() {
    std::vector<std::string> names;
    for (const auto& [name, fn] : UnaryFunctions()) names.push_back(name);
    for (const auto& [name, fn] : BinaryFunctions()) names.push_back(name);
    // Aggregates and string functions dispatched by name in the Call
    // compiler ("avg" is an alias of AVERAGE, so it is not listed).
    for (const char* s : {"sum", "average", "count",
                          "concatenate", "left", "right", "mid", "len"}) {
        names.emplace_back(s);
    }
    for (std::string& n : names) {
        std::transform(n.begin(), n.end(), n.begin(),
            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

CompileResult CompileNumeric(const Node& root, const ColumnMap& columns) {
    TypedColumnMap typed;
    for (const auto& [name, fn] : columns) {
        typed.emplace(name, TypedFn{fn});
    }

    CompileResult result;
    TypedCompileResult compiled = CompileTyped(root, typed);
    if (!compiled.fn) {
        result.error = compiled.error;
        return result;
    }
    if (auto* fn = std::get_if<FnNum>(&*compiled.fn)) {
        result.fn = *fn;
    } else {
        result.error = "Expression produces a string, but a number is required here.";
    }
    return result;
}

} // namespace exprparse
