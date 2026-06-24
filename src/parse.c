/*
 * parse.c — top-level declaration parser.
 *
 * Drives the translation unit: parses one declaration at a time and
 * invokes gen_decl() to emit code. Functions get their body parsed
 * inside gen.c (gen_func_body) to keep parser/gen interleaving tight.
 */
#include "compat.h"

#include "cc.h"
#include "lexer.h"
#include "types.h"
#include "ast.h"
#include "parse.h"
#include "gen.h"
#include "parser_priv.h"

lexer_t *g_lx = NULL;

void parser_set_lexer(lexer_t *lx) { g_lx = lx; }

void parse_error(const char *msg) {
    cc_error_at(g_lx->cur.pos.file, g_lx->cur.pos.line,
                "parse: %s (got %s '%s')",
                msg, tok_kind_str(g_lx->cur.kind), g_lx->cur.text);
}

void parse_expect(token_kind_t k, const char *what) {
    if (g_lx->cur.kind != k) {
        parse_error(what);
    }
    lex_next(g_lx);
}

bool accept(token_kind_t k) {
    if (g_lx->cur.kind == k) { lex_next(g_lx); return true; }
    return false;
}

/* Skip __attribute__((...)) or asm(...). No-op if current token is not
 * an identifier matching those names. */
void skip_attributes(void) {
    while (g_lx->cur.kind == T_IDENT &&
           (strcmp(g_lx->cur.text, "__attribute__") == 0 ||
            strcmp(g_lx->cur.text, "__attribute") == 0 ||
            strcmp(g_lx->cur.text, "asm") == 0 ||
            strcmp(g_lx->cur.text, "__asm__") == 0)) {
        lex_next(g_lx);  /* consume the identifier */
        /* Expect one or more parenthesized groups. */
        while (accept(T_LPAREN)) {
            int depth = 1;
            while (depth > 0 && g_lx->cur.kind != T_EOF) {
                if (g_lx->cur.kind == T_LPAREN) depth++;
                else if (g_lx->cur.kind == T_RPAREN) depth--;
                lex_next(g_lx);
            }
        }
    }
}

/* Parse a top-level declaration. */
static void parse_top_level(void) {
    type_t *base = NULL;
    bool is_static = false, is_extern = false, is_typedef = false, is_inline = false;
    char tag[CC_MAX_IDENT] = {0};
    if (!parse_decl_spec(g_lx, &base, &is_static, &is_extern, &is_typedef, &is_inline, tag)) {
        if (accept(T_SEMI)) return;
        parse_error("expected declaration");
        lex_next(g_lx);
        return;
    }

    if (g_lx->cur.kind == T_SEMI) {
        lex_next(g_lx);
        return;
    }

    for (;;) {
        type_t *t = base;
        while (accept(T_STAR)) {
            t = type_make_ptr(t);
            while (accept(T_KW_CONST) || accept(T_KW_VOLATILE)) { }
        }
        if (g_lx->cur.kind != T_IDENT) {
            parse_error("expected identifier");
            return;
        }
        char name[CC_MAX_IDENT];
        strncpy(name, g_lx->cur.text, CC_MAX_IDENT - 1);
        name[CC_MAX_IDENT - 1] = 0;
        cc_pos_t pos = g_lx->cur.pos;
        lex_next(g_lx);

        if (g_lx->cur.kind == T_LPAREN) {
            lex_next(g_lx);
            type_t *ftype = type_dup(t);
            ftype->kind = TY_FUNC;
            ftype->ret = t;
            ftype->nparams = 0;
            ftype->is_varargs = false;
            ftype->size = 8;
            ftype->align = 8;
            ftype->is_complete = true;

            if (g_lx->cur.kind != T_RPAREN) {
                for (;;) {
                    if (g_lx->cur.kind == T_ELLIPSIS) {
                        ftype->is_varargs = true;
                        lex_next(g_lx);
                        break;
                    }
                    type_t *pbase = NULL;
                    bool ps, pe, pt, pi;
                    char ptag[CC_MAX_IDENT] = {0};
                    if (!parse_decl_spec(g_lx, &pbase, &ps, &pe, &pt, &pi, ptag)) {
                        parse_error("expected parameter type");
                        break;
                    }
                    type_t *ptyp = pbase;
                    while (accept(T_STAR)) ptyp = type_make_ptr(ptyp);
                    char pname[CC_MAX_IDENT] = {0};
                    /* Parameter name is optional in prototypes. */
                    if (g_lx->cur.kind == T_IDENT) {
                        strncpy(pname, g_lx->cur.text, CC_MAX_IDENT - 1);
                        /* But make sure it's not the start of the next param type. */
                        /* (For MVP we accept any ident as the name.) */
                        lex_next(g_lx);
                    }
                    /* Array param decays to pointer. */
                    if (ptyp->kind == TY_ARRAY) ptyp = type_decay(ptyp);
                    if (ptyp->kind == TY_FUNC)  ptyp = type_make_ptr(ptyp);
                    if (ptyp == &ty_void && pname[0] == 0) {
                        /* (void) — will be detected after the loop. */
                    }
                    if (ftype->nparams < CC_MAX_FUNC_PARAMS) {
                        func_param_t *pp = &ftype->params[ftype->nparams++];
                        strncpy(pp->name, pname, CC_MAX_IDENT - 1);
                        pp->type = ptyp;
                    }
                    if (!accept(T_COMMA)) break;
                }
            }
            parse_expect(T_RPAREN, "expected ')' after parameter list");

            /* (void) means no parameters. */
            if (ftype->nparams == 1 && ftype->params[0].type == &ty_void &&
                ftype->params[0].name[0] == 0) {
                ftype->nparams = 0;
            }

            skip_attributes();

            if (g_lx->cur.kind == T_LBRACE) {
                decl_t *d = ast_new_decl(D_FUNC_DEF, pos);
                d->type = ftype;
                strncpy(d->name, name, CC_MAX_IDENT - 1);
                if (is_static) ftype->is_static = true;
                if (is_inline) ftype->is_inline = true;
                gen_decl(d);
                return;
            } else {
                decl_t *d = ast_new_decl(D_FUNC_DECL, pos);
                d->type = ftype;
                strncpy(d->name, name, CC_MAX_IDENT - 1);
                gen_decl(d);
            }
        } else {
            while (g_lx->cur.kind == T_LBRACKET) {
                lex_next(g_lx);
                uint64_t len = 0;
                if (g_lx->cur.kind == T_INT || g_lx->cur.kind == T_LONG ||
                    g_lx->cur.kind == T_UINT || g_lx->cur.kind == T_ULONG) {
                    len = g_lx->cur.ival;
                    lex_next(g_lx);
                }
                parse_expect(T_RBRACKET, "expected ']'");
                t = type_make_array(t, len);
            }
            expr_t *init = NULL;
            bool has_init = false;
            if (accept(T_ASSIGN)) {
                has_init = true;
                init = gen_parse_global_init(g_lx, t);
            }
            decl_t *d = ast_new_decl(D_VAR, pos);
            d->type = t;
            strncpy(d->name, name, CC_MAX_IDENT - 1);
            d->init = init;
            if (is_static) t->is_static = true;
            if (is_extern)  t->is_extern = true;
            if (is_typedef) {
                d->kind = D_TYPEDEF;
                t->is_typedef = true;
            }
            /* For globals with init, mark is_defined so finalize puts them in .data. */
            if (has_init) {
                /* will be set in gen_decl via init presence */
            }
            gen_decl(d);
        }

        if (accept(T_COMMA)) continue;
        break;
    }

    skip_attributes();
    parse_expect(T_SEMI, "expected ';' after declaration");
}

void parse_translation_unit(lexer_t *lx) {
    parser_set_lexer(lx);
    while (lx->cur.kind != T_EOF) {
        parse_top_level();
    }
}
