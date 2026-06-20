#pragma once 

#include <map> 
#include <functional>
#include <boost/spirit/home/x3.hpp>
#include <boost/spirit/home/x3/support/ast/variant.hpp>
#include <boost/fusion/include/adapt_struct.hpp>

static std::map<char, std::function<double(int)>> column_map;

namespace client {
    namespace ast {
        struct eval {
            using result_type = std::function<double(int)>;

            result_type operator()(nil) const {
                return [](int){ return 0.0; };
            }

            result_type operator()(unsigned int n) const {
                return [n](int){ return static_cast<double>(n); };
            }

            result_type operator()(ast::column_ref const& c) const {
                auto it = column_map.find(c.name);
                if (it != column_map.end())
                    return it->second;
                return [](int){ return 0.0; };
            }

            result_type operator()(signed_ const& x) const {
                auto rhs = boost::apply_visitor(*this, x.operand_);
                if (x.sign == '-') return [=](int i){ return -rhs(i); };
                return rhs;
            }

            result_type operator()(operation const& op, result_type lhs) const {
                auto rhs = boost::apply_visitor(*this, op.operand_);
                switch (op.operator_) {
                    case '+': return [=](int i){ return lhs(i) + rhs(i); };
                    case '-': return [=](int i){ return lhs(i) - rhs(i); };
                    case '*': return [=](int i){ return lhs(i) * rhs(i); };
                    case '/': return [=](int i){ return lhs(i) / rhs(i); };
                }
                return [](int){ return 0.0; };
            }

            result_type operator()(program const& x) const {
                auto current = boost::apply_visitor(*this, x.first);
                for (auto const& op : x.rest) {
                    current = (*this)(op, current);
                }
                return current;
            }
        };
    }
}

namespace client {
    namespace calculator_grammar {
        using x3::uint_;
        using x3::char_;

        x3::rule<class expression, ast::program> const expression("expression");
        x3::rule<class term, ast::program> const term("term");
        x3::rule<class factor, ast::operand> const factor("factor");
        x3::rule<class colref, ast::column_ref> const colref("colref");

        auto const expression_def = term >> *( (char_('+') >> term) | (char_('-') >> term) );
        auto const term_def = factor >> *( (char_('*') >> factor) | (char_('/') >> factor) );
        auto const colref_def = char_("A-Z");
        auto const factor_def = uint_ | colref | '(' >> expression >> ')' | (char_('-') >> factor) | (char_('+') >> factor);

        BOOST_SPIRIT_DEFINE(expression, term, factor, colref);
        auto calculator = expression;
    }
    using calculator_grammar::calculator;
}