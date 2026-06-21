#pragma once
#include <boost/spirit/home/x3.hpp>
#include <boost/spirit/home/x3/support/ast/variant.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <list>

namespace x3 = boost::spirit::x3;
namespace client { namespace ast {
    struct nil {};
    struct signed_;
    struct program;
    struct column_ref;

    struct operand : x3::variant<
            nil
          , unsigned int
          , x3::forward_ast<column_ref>
          , x3::forward_ast<signed_>
          , x3::forward_ast<program>
        >
    {
        using base_type::base_type;
        using base_type::operator=;
    };

    struct column_ref { char name; };
    struct signed_ { char sign; operand operand_; };
    struct operation { char operator_; operand operand_; };
    struct program { operand first; std::list<operation> rest; };
}}

BOOST_FUSION_ADAPT_STRUCT(client::ast::column_ref, name)
BOOST_FUSION_ADAPT_STRUCT(client::ast::signed_, sign, operand_)
BOOST_FUSION_ADAPT_STRUCT(client::ast::operation, operator_, operand_)
BOOST_FUSION_ADAPT_STRUCT(client::ast::program, first, rest)
