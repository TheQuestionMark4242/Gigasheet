/* Reentrant scanner for derived-column expressions. Paired with the bison
 * C++ parser in expr.y; Parse() lives at the bottom of this file so the
 * flex buffer API stays private to the generated translation unit. */
%option reentrant noyywrap nounistd never-interactive
%option nounput noinput
%option prefix="expr"

%{
#include <cstdlib>
#include <string>

#include "parser_driver.hpp"
#include "expr_parser.hpp"

#define YY_DECL exprparse::Parser::symbol_type yylex( \
    exprparse::Driver& drv, yyscan_t yyscanner)
%}

%%

[ \t\r\n]+                              ;

[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?     {
        return exprparse::Parser::make_NUMBER(std::strtod(yytext, nullptr));
    }

[A-Za-z_][A-Za-z0-9_]*                  {
        return exprparse::Parser::make_IDENT(std::string(yytext, yyleng));
    }

\"[^\"]*\"                              {
        return exprparse::Parser::make_QIDENT(std::string(yytext + 1, yyleng - 2));
    }

'[^']*'                                 {
        return exprparse::Parser::make_STRING(std::string(yytext + 1, yyleng - 2));
    }

"+"                                     { return exprparse::Parser::make_PLUS(); }
"-"                                     { return exprparse::Parser::make_MINUS(); }
"*"                                     { return exprparse::Parser::make_STAR(); }
"/"                                     { return exprparse::Parser::make_SLASH(); }
"("                                     { return exprparse::Parser::make_LPAREN(); }
")"                                     { return exprparse::Parser::make_RPAREN(); }
","                                     { return exprparse::Parser::make_COMMA(); }

.                                       { return exprparse::Parser::make_YYUNDEF(); }

<<EOF>>                                 { return exprparse::Parser::make_YYEOF(); }

%%

namespace exprparse {

ParseResult Parse(const std::string& src) {
    Driver drv;

    yyscan_t scanner = nullptr;
    exprlex_init(&scanner);
    YY_BUFFER_STATE buffer = expr_scan_bytes(src.data(), src.size(), scanner);

    Parser parser(drv, scanner);
    const int rc = parser.parse();

    expr_delete_buffer(buffer, scanner);
    exprlex_destroy(scanner);

    ParseResult result;
    if (rc == 0 && drv.result) {
        result.root = std::move(drv.result);
    } else {
        result.error = drv.error.empty() ? "parse error" : drv.error;
    }
    return result;
}

} // namespace exprparse
