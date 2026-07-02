/* Expression grammar for derived columns. Reentrant C++ parser paired with
 * the reentrant flex scanner in expr.l (prefix "expr"). */
%skeleton "lalr1.cc"
%require "3.2"
%define api.namespace {exprparse}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.error verbose

%code requires {
    #include "expr_ast.hpp"

    namespace exprparse { class Driver; }

    #ifndef YY_TYPEDEF_YY_SCANNER_T
    #define YY_TYPEDEF_YY_SCANNER_T
    typedef void* yyscan_t;
    #endif
}

%param { exprparse::Driver& drv }
%param { yyscan_t yyscanner }

%code {
    #include "parser_driver.hpp"

    /* flex %option prefix="expr" renames the scanner entry point; make the
     * parser's yylex calls resolve to the same symbol. */
    #define yylex exprlex

    exprparse::Parser::symbol_type yylex(
        exprparse::Driver& drv, yyscan_t yyscanner);
}

%token <double> NUMBER "number"
%token <std::string> IDENT "identifier"
%token <std::string> QIDENT "quoted column name"
%token PLUS "+" MINUS "-" STAR "*" SLASH "/"
%token LPAREN "(" RPAREN ")"
%token COMMA ","

%left "+" "-"
%left "*" "/"
%precedence UMINUS

%type <exprparse::NodePtr> expr
%type <std::vector<exprparse::NodePtr>> arglist

%%

input:
    expr                { drv.result = std::move($1); }
;

expr:
    NUMBER              { $$ = exprparse::MakeNode(exprparse::NumberLit{$1}); }
  | IDENT               { $$ = exprparse::MakeNode(exprparse::ColumnRef{std::move($1)}); }
  | QIDENT              { $$ = exprparse::MakeNode(exprparse::ColumnRef{std::move($1)}); }
  | expr "+" expr       { $$ = exprparse::MakeNode(exprparse::Binary{'+', std::move($1), std::move($3)}); }
  | expr "-" expr       { $$ = exprparse::MakeNode(exprparse::Binary{'-', std::move($1), std::move($3)}); }
  | expr "*" expr       { $$ = exprparse::MakeNode(exprparse::Binary{'*', std::move($1), std::move($3)}); }
  | expr "/" expr       { $$ = exprparse::MakeNode(exprparse::Binary{'/', std::move($1), std::move($3)}); }
  | "-" expr %prec UMINUS { $$ = exprparse::MakeNode(exprparse::Unary{'-', std::move($2)}); }
  | "+" expr %prec UMINUS { $$ = std::move($2); }
  | "(" expr ")"        { $$ = std::move($2); }
  | IDENT "(" ")"       { $$ = exprparse::MakeNode(exprparse::Call{std::move($1), {}}); }
  | IDENT "(" arglist ")" { $$ = exprparse::MakeNode(exprparse::Call{std::move($1), std::move($3)}); }
;

arglist:
    expr                { $$ = std::vector<exprparse::NodePtr>{}; $$.push_back(std::move($1)); }
  | arglist "," expr    { $$ = std::move($1); $$.push_back(std::move($3)); }
;

%%

void exprparse::Parser::error(const std::string& msg) {
    drv.error = msg;
}
