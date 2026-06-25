/*
 * gen.c — single-pass C code generator for RISC-V64.
 *
 * Architecture follows tcc's spirit: parse expression → emit code in
 * one pass. Each expression returns a value descriptor describing
 * where its result lives (in a register by convention).
 *
 * Value descriptor (val_t):
 *   kind = VAL_REG    result in reg
 *   kind = VAL_IMM    constant value in .imm (use materialize() to load)
 *   kind = VAL_SYM    refers to a symbol (global/local/static) with offset
 *   kind = VAL_LVAL   address in reg, value is at that address (for
 *                     assignment targets and arrays decaying to pointers)
 *
 * Convention: every expression returns VAL_REG by the time it is
 * consumed, except identifiers used as lvalues which return VAL_SYM
 * (then materialized via load_value()).
 *
 * For brevity, only the C subset needed for typical userspace
 * programs is supported. C++ is layered on top in cpp.c.
 */
#include "compat.h"

#include "cc.h"
#include "lexer.h"
#include "types.h"
#include "ast.h"
#include "gen.h"
#include "riscv64.h"
#include "parser_priv.h"

gen_func_t g_func;

/* ---- Value descriptor ------------------------------------------------ */
typedef enum {
    VAL_REG,    /* value in .reg */
    VAL_IMM,    /* constant in .imm */
    VAL_SYM,    /* symbol: .sym_idx + .offset */
    VAL_LVAL,   /* address in .reg + .offset */
} val_kind_t;

typedef struct {
    val_kind_t kind;
    type_t *type;
    int reg;
    int64_t imm;
    int sym_idx;     /* for VAL_SYM */
    int64_t offset;  /* for VAL_SYM/VAL_LVAL */
} val_t;

/* Forward declarations. */
static val_t parse_assign(lexer_t *lx);
static val_t parse_expr(lexer_t *lx);
static void parse_stmt(lexer_t *lx);
static val_t parse_primary(lexer_t *lx);
static val_t parse_unary(lexer_t *lx);
static void patch_jal(uint32_t off, int32_t delta);
static void patch_sd_imm(uint32_t off, int32_t imm);

/* ---- Label table for goto/label ------------------------------------- */
#define MAX_LABELS 256
#define MAX_PATCHES 256

typedef struct {
    char name[CC_MAX_IDENT];
    uint32_t offset;
    uint32_t patches[MAX_PATCHES];
    int n_patches;
} label_t;

static label_t g_labels[MAX_LABELS];
static int g_n_labels;

static void label_clear(void) {
    g_n_labels = 0;
}

static int label_find_or_add(const char *name) {
    for (int i = 0; i < g_n_labels; i++) {
        if (strcmp(g_labels[i].name, name) == 0) return i;
    }
    if (g_n_labels >= MAX_LABELS) cc_fatal("too many labels");
    int idx = g_n_labels++;
    strncpy(g_labels[idx].name, name, CC_MAX_IDENT - 1);
    g_labels[idx].name[CC_MAX_IDENT - 1] = 0;
    g_labels[idx].offset = 0;
    g_labels[idx].n_patches = 0;
    return idx;
}

static void label_define(const char *name) {
    int idx = label_find_or_add(name);
    label_t *l = &g_labels[idx];
    if (l->offset != 0) {
        cc_error("duplicate label '%s'", name);
        return;
    }
    l->offset = (uint32_t)g_text.size;
    for (int i = 0; i < l->n_patches; i++) {
        int32_t delta = (int32_t)l->offset - (int32_t)l->patches[i];
        patch_jal(l->patches[i], delta);
    }
    l->n_patches = 0;
}

static void label_emit_jump(const char *name) {
    int idx = label_find_or_add(name);
    label_t *l = &g_labels[idx];
    uint32_t patch_off = (uint32_t)g_text.size;
    rv_jal(RV_ZERO, 0);
    if (l->offset != 0) {
        int32_t delta = (int32_t)l->offset - (int32_t)patch_off;
        patch_jal(patch_off, delta);
    } else {
        if (l->n_patches >= MAX_PATCHES) cc_fatal("too many jumps to label");
        l->patches[l->n_patches++] = patch_off;
    }
}

static void label_check_unresolved(void) {
    for (int i = 0; i < g_n_labels; i++) {
        if (g_labels[i].offset == 0 && g_labels[i].n_patches > 0) {
            cc_error("undefined label '%s'", g_labels[i].name);
        }
    }
}

/* ---- Helpers --------------------------------------------------------- */
static int alloc_int_reg(void) {
    /* We use t0/t1/t2 for intermediates, a0 for return values, t3-t6 for
     * spill in complex expressions. For MVP we cap expression complexity
     * at what fits in t0-t6 + a0. */
    return RV_T0;
}

/* Forward decl. */
static void emit_load_global_addr(int reg, int sym_idx, int64_t add_off);

static void materialize(val_t *v, int reg) {
    /* Force v into reg. */
    switch (v->kind) {
        case VAL_IMM:
            rv_addi_imm(reg, RV_ZERO, v->imm);
            v->kind = VAL_REG;
            v->reg = reg;
            break;
        case VAL_SYM: {
            /* Compute address of symbol. */
            bool is_local = (v->sym_idx >= g_n_globals);
            if (is_local) {
                int lidx = v->sym_idx - g_n_globals;
                sym_t *s = &g_locals[lidx];
                rv_addi(reg, RV_FP, (int)s->offset + (int)v->offset);
            } else {
                /* Global: emit lui+addi with fixup. */
                emit_load_global_addr(reg, v->sym_idx, v->offset);
            }
            v->kind = VAL_LVAL;
            v->reg = reg;
            v->offset = 0;
            break;
        }
        case VAL_LVAL:
            /* Address already in v->reg + offset. Move to target reg. */
            if (v->reg != reg) {
                rv_addi(reg, v->reg, (int)v->offset);
                v->reg = reg;
                v->offset = 0;
            }
            break;
        case VAL_REG:
            if (v->reg != reg) {
                rv_mv(reg, v->reg);
                v->reg = reg;
            }
            break;
    }
}

static void load_value(val_t *v, int reg) {
    /* Materialize the actual value (dereference if LVAL). */
    materialize(v, reg);
    if (v->kind == VAL_LVAL) {
        /* Load from [reg + offset]. */
        uint64_t sz = type_sizeof(v->type);
        if (v->type->kind == TY_ARRAY || v->type->kind == TY_FUNC) {
            /* Array decays to pointer: keep address. */
            return;
        }
        switch (sz) {
            case 1: type_is_unsigned(v->type) ? rv_lbu(reg, reg, (int)v->offset) : rv_lb(reg, reg, (int)v->offset); break;
            case 2: type_is_unsigned(v->type) ? rv_lhu(reg, reg, (int)v->offset) : rv_lh(reg, reg, (int)v->offset); break;
            case 4: type_is_unsigned(v->type) ? rv_lwu(reg, reg, (int)v->offset) : rv_lw(reg, reg, (int)v->offset); break;
            case 8: rv_ld(reg, reg, (int)v->offset); break;
            default: rv_ld(reg, reg, (int)v->offset); break;
        }
        v->kind = VAL_REG;
        v->offset = 0;
    }
}

/* ---- Branch/jump patching helpers ----------------------------------- */
static void patch_branch(uint32_t off, int32_t delta) {
    uint8_t *p = g_text.data + off;
    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    insn |= ((uint32_t)(delta & 0x1000) << 19) |
            ((uint32_t)(delta & 0x7E0) << 20) |
            ((uint32_t)(delta & 0x1E) << 7)  |
            ((uint32_t)(delta & 0x800) >> 4);
    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
}

static void patch_jal(uint32_t off, int32_t delta) {
    uint8_t *p = g_text.data + off;
    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    insn &= ~0xFFFFF000;
    insn |= ((uint32_t)(delta & 0x100000) << 11) |
            ((uint32_t)(delta & 0x7FE) << 20) |
            ((uint32_t)(delta & 0x800) << 9)  |
            ((uint32_t)(delta & 0xFF000));
    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
}

static void patch_sd_imm(uint32_t off, int32_t imm) {
    uint8_t *p = g_text.data + off;
    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    insn &= ~0xFE000F80;
    uint32_t imm_lo = (uint32_t)(imm & 0x1F);
    uint32_t imm_hi = (uint32_t)((imm >> 5) & 0x7F);
    insn |= (imm_hi << 25) | (imm_lo << 7);
    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
}

/* ---- Type parsing ---------------------------------------------------- */
bool is_type_start(lexer_t *lx) {
    switch (lx->cur.kind) {
        case T_KW_VOID: case T_KW_CHAR: case T_KW_SHORT: case T_KW_INT:
        case T_KW_LONG: case T_KW_FLOAT: case T_KW_DOUBLE: case T_KW_SIGNED:
        case T_KW_UNSIGNED: case T_KW_CONST: case T_KW_VOLATILE:
        case T_KW_STATIC: case T_KW_EXTERN: case T_KW_TYPEDEF: case T_KW_INLINE:
        case T_KW_REGISTER: case T_KW_RESTRICT: case T_KW_AUTO:
        case T_KW_STRUCT: case T_KW_UNION: case T_KW_ENUM:
        case T_KW_BOOL:
            return true;
        case T_IDENT: {
            /* Could be a typedef name. */
            int idx = symtab_lookup_global(lx->cur.text);
            if (idx >= 0 && g_globals[idx].kind == SYM_TYPEDEF) return true;
            return false;
        }
        default:
            return false;
    }
}

type_t *parse_type_spec(lexer_t *lx) {
    /* Collect base type specifiers. */
    bool is_signed = false, is_unsigned = false;
    bool is_short = false, is_long = false, is_longlong = false;
    bool is_const = false, is_volatile = false;
    type_t *base = NULL;

    for (;;) {
        switch (lx->cur.kind) {
            case T_KW_CONST:    is_const = true;    lex_next(lx); continue;
            case T_KW_VOLATILE: is_volatile = true; lex_next(lx); continue;
            case T_KW_RESTRICT: lex_next(lx); continue;
            case T_KW_REGISTER: lex_next(lx); continue;
            case T_KW_AUTO:     lex_next(lx); continue;
            case T_KW_SIGNED:   is_signed = true;   lex_next(lx); continue;
            case T_KW_UNSIGNED: is_unsigned = true; lex_next(lx); continue;
            case T_KW_SHORT:    is_short = true;    lex_next(lx); continue;
            case T_KW_LONG:
                if (is_long) { is_longlong = true; is_long = false; }
                else is_long = true;
                lex_next(lx);
                continue;
            case T_KW_VOID:     base = &ty_void;    lex_next(lx); break;
            case T_KW_BOOL:     base = &ty_bool;    lex_next(lx); break;
            case T_KW_CHAR:     base = &ty_char;    lex_next(lx); break;
            case T_KW_INT:      base = &ty_int;     lex_next(lx); break;
            case T_KW_FLOAT:    base = &ty_float;   lex_next(lx); break;
            case T_KW_DOUBLE:
                if (is_long) base = &ty_ldouble;
                else base = &ty_double;
                lex_next(lx);
                break;
            case T_KW_STRUCT:
            case T_KW_UNION: {
                bool is_union = (lx->cur.kind == T_KW_UNION);
                lex_next(lx);
                char tag[CC_MAX_IDENT] = {0};
                if (lx->cur.kind == T_IDENT) {
                    strncpy(tag, lx->cur.text, CC_MAX_IDENT - 1);
                    lex_next(lx);
                }
                type_t *t = tag[0] ? tags_lookup(tag) : NULL;
                if (!t) {
                    t = (type_t *)cc_arena_alloc(&g_type_arena, sizeof(type_t), 8);
                    memset(t, 0, sizeof(*t));
                    t->kind = is_union ? TY_UNION : TY_STRUCT;
                    if (tag[0]) {
                        strncpy(t->tag, tag, CC_MAX_IDENT - 1);
                        tags_install(tag, t);
                    }
                    t->is_complete = false;
                }
                /* Definition: { fields } */
                if (lx->cur.kind == T_LBRACE) {
                    lex_next(lx);
                    uint64_t offset = 0;
                    uint64_t max_align = 1;
                    int nfields = 0;
                    while (lx->cur.kind != T_RBRACE) {
                        type_t *fbase = NULL;
                        bool fs, fe, ft, fi;
                        char ftag[CC_MAX_IDENT] = {0};
                        if (!parse_decl_spec(lx, &fbase, &fs, &fe, &ft, &fi, ftag)) {
                            parse_error("expected field type");
                            break;
                        }
                        /* One or more declarators. */
                        if (lx->cur.kind == T_SEMI) {
                            /* Anonymous struct/union member. */
                            lex_next(lx);
                            continue;
                        }
                        do {
                            type_t *ftyp = fbase;
                            while (accept(T_STAR)) ftyp = type_make_ptr(ftyp);
                            char fname[CC_MAX_IDENT] = {0};
                            if (lx->cur.kind == T_IDENT) {
                                strncpy(fname, lx->cur.text, CC_MAX_IDENT - 1);
                                lex_next(lx);
                            }
                            while (lx->cur.kind == T_LBRACKET) {
                                lex_next(lx);
                                uint64_t len = 0;
                                if (lx->cur.kind == T_INT) {
                                    len = lx->cur.ival;
                                    lex_next(lx);
                                }
                                parse_expect(T_RBRACKET, "expected ']'");
                                ftyp = type_make_array(ftyp, len);
                            }
                            /* Allocate field. */
                            uint64_t falign = type_alignof(ftyp);
                            if (falign > max_align) max_align = falign;
                            if (!is_union) {
                                offset = (offset + falign - 1) & ~(falign - 1);
                            } else {
                                offset = 0;
                            }
                            if (nfields < CC_MAX_STRUCT_FIELDS) {
                                struct_field_t *f = &t->fields[nfields++];
                                strncpy(f->name, fname, CC_MAX_IDENT - 1);
                                f->type = ftyp;
                                f->offset = offset;
                                f->is_anon = (fname[0] == 0);
                            }
                            if (!is_union) {
                                offset += type_sizeof(ftyp);
                            }
                        } while (accept(T_COMMA));
                        parse_expect(T_SEMI, "expected ';'");
                    }
                    parse_expect(T_RBRACE, "expected '}'");
                    t->nfields = nfields;
                    if (is_union) {
                        uint64_t max_size = 0;
                        for (int i = 0; i < nfields; i++) {
                            uint64_t s = type_sizeof(t->fields[i].type);
                            if (s > max_size) max_size = s;
                        }
                        t->size = max_size;
                    } else {
                        t->size = (offset + max_align - 1) & ~(max_align - 1);
                    }
                    t->align = max_align;
                    t->is_complete = true;
                }
                base = t;
                break;
            }
            case T_KW_ENUM: {
                lex_next(lx);
                char tag[CC_MAX_IDENT] = {0};
                if (lx->cur.kind == T_IDENT) {
                    strncpy(tag, lx->cur.text, CC_MAX_IDENT - 1);
                    lex_next(lx);
                }
                type_t *t = tag[0] ? tags_lookup(tag) : NULL;
                if (!t) {
                    t = (type_t *)cc_arena_alloc(&g_type_arena, sizeof(type_t), 8);
                    memset(t, 0, sizeof(*t));
                    t->kind = TY_ENUM;
                    t->base = &ty_int;
                    t->size = 4; t->align = 4; t->is_complete = true;
                    if (tag[0]) {
                        strncpy(t->tag, tag, CC_MAX_IDENT - 1);
                        tags_install(tag, t);
                    }
                }
                if (lx->cur.kind == T_LBRACE) {
                    lex_next(lx);
                    int val = 0;
                    while (lx->cur.kind != T_RBRACE) {
                        if (lx->cur.kind != T_IDENT) break;
                        char ename[CC_MAX_IDENT];
                        strncpy(ename, lx->cur.text, CC_MAX_IDENT - 1);
                        ename[CC_MAX_IDENT - 1] = 0;
                        lex_next(lx);
                        if (accept(T_ASSIGN)) {
                            /* Parse constant expr — MVP: just integer literal. */
                            if (lx->cur.kind == T_INT || lx->cur.kind == T_LONG) {
                                val = (int)lx->cur.ival;
                                lex_next(lx);
                            } else if (lx->cur.kind == T_MINUS) {
                                lex_next(lx);
                                if (lx->cur.kind == T_INT) { val = -(int)lx->cur.ival; lex_next(lx); }
                            }
                        }
                        symtab_install_global(ename, SYM_ENUM_CONST, &ty_int);
                        g_globals[g_n_globals - 1].enum_val = val;
                        val++;
                        if (!accept(T_COMMA)) break;
                    }
                    parse_expect(T_RBRACE, "expected '}'");
                }
                base = t;
                break;
            }
            case T_IDENT: {
                int idx = symtab_lookup_global(lx->cur.text);
                if (idx >= 0 && g_globals[idx].kind == SYM_TYPEDEF) {
                    base = g_globals[idx].type;
                    lex_next(lx);
                    break;
                }
                /* Not a typedef — stop here; apply any long/short/etc. */
                goto finish_type_spec;
            }
            default:
                goto finish_type_spec;
        }
        /* After seeing a base type, we keep collecting qualifiers but break
         * on duplicate base types. */
        if (base) {
            /* Allow combining long/short/signed/unsigned with int/char. */
            if (is_short) {
                base = is_unsigned ? &ty_ushort : &ty_short;
            } else if (is_longlong) {
                base = is_unsigned ? &ty_ullong : &ty_llong;
            } else if (is_long) {
                base = is_unsigned ? &ty_ulong : &ty_long;
            } else if (is_unsigned && base == &ty_char) {
                base = &ty_uchar;
            } else if (is_unsigned && base == &ty_int) {
                base = &ty_uint;
            }
            /* Check for trailing modifiers without new base. */
            if (lx->cur.kind == T_KW_INT) { lex_next(lx); }
            break;
        }
    }

finish_type_spec:
    if (!base) {
        /* Apply long/short/signed/unsigned to implicit int. */
        if (is_short)        base = is_unsigned ? &ty_ushort : &ty_short;
        else if (is_longlong) base = is_unsigned ? &ty_ullong : &ty_llong;
        else if (is_long)    base = is_unsigned ? &ty_ulong  : &ty_long;
        else                 base = &ty_int;
    }
    if (is_const)    base->is_const = true;
    if (is_volatile) base->is_volatile = true;
    return base;
}

bool parse_decl_spec(lexer_t *lx, type_t **out_type, bool *out_is_static,
                     bool *out_is_extern, bool *out_is_typedef, bool *out_is_inline,
                     char *tag_out) {
    bool is_static = false, is_extern = false, is_typedef = false, is_inline = false;
    if (tag_out) tag_out[0] = 0;

    /* Loop on storage class + qualifiers. */
    for (;;) {
        switch (lx->cur.kind) {
            case T_KW_STATIC:   is_static = true;  lex_next(lx); continue;
            case T_KW_EXTERN:   is_extern = true;  lex_next(lx); continue;
            case T_KW_TYPEDEF:  is_typedef = true; lex_next(lx); continue;
            case T_KW_INLINE:   is_inline = true;  lex_next(lx); continue;
            default: break;
        }
        break;
    }

    type_t *base = parse_type_spec(lx);
    if (!base && !is_static && !is_extern && !is_typedef && !is_inline) {
        return false;
    }
    if (!base) base = &ty_int;
    *out_type = base;
    *out_is_static = is_static;
    *out_is_extern = is_extern;
    *out_is_typedef = is_typedef;
    *out_is_inline = is_inline;
    return true;
}

/* ---- Symbol address helpers ----------------------------------------- */
/* Global symbol virtual address — assigned by gen_finalize(). */
typedef struct {
    int sym_idx;
    uint32_t patch_lui;    /* offset of lui instruction */
    uint32_t patch_addi;   /* offset of addi instruction (or 0 if not used) */
    int64_t extra_off;     /* extra offset to add to symbol address */
} global_addr_fixup_t;

static global_addr_fixup_t g_fixups[4096];
static int g_n_fixups = 0;

/* Fixups for rodata references (string literals). */
typedef struct {
    uint32_t rodata_off;   /* offset within rodata */
    uint32_t patch_lui;
    uint32_t patch_addi;
} rodata_fixup_t;

static rodata_fixup_t g_rodata_fixups[4096];
static int g_n_rodata_fixups = 0;

/* Fixups for global data pointing to rodata (string initializers). */
typedef struct {
    uint32_t rodata_off;   /* offset within rodata */
    uint32_t data_off;     /* offset within g_data to patch (8 bytes, LE) */
} data_rodata_fixup_t;

static data_rodata_fixup_t g_data_rodata_fixups[4096];
static int g_n_data_rodata_fixups = 0;

/* Emit a global address load into `reg`. Records a fixup for later. */
static void emit_load_global_addr(int reg, int sym_idx, int64_t add_off) {
    uint32_t lui_off = (uint32_t)g_text.size;
    rv_lui(reg, 0);
    uint32_t addi_off = 0;
    if (add_off != 0) {
        addi_off = (uint32_t)g_text.size;
        rv_addi(reg, reg, (int)add_off);
    }
    if (g_n_fixups >= 4096) cc_fatal("too many global refs");
    g_fixups[g_n_fixups].sym_idx = sym_idx;
    g_fixups[g_n_fixups].patch_lui = lui_off;
    g_fixups[g_n_fixups].patch_addi = addi_off;
    g_fixups[g_n_fixups].extra_off = add_off;
    g_n_fixups++;
}

/* ---- Function call codegen ------------------------------------------ */
static val_t gen_call(lexer_t *lx, int fn_sym_idx, type_t *fn_type, val_t *callee) {
    /* Parse args. We evaluate args left-to-right into a0-a7 (max 8).
     * Extra args go on the stack. */
    parse_expect(T_LPAREN, "expected '('");

    int nargs = 0;
    int arg_regs[8] = {RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7};
    val_t args[8];

    if (lx->cur.kind != T_RPAREN) {
        for (;;) {
            val_t a = parse_assign(lx);
            if (nargs < 8) {
                load_value(&a, arg_regs[nargs]);
                args[nargs] = a;
                nargs++;
            }
            if (!accept(T_COMMA)) break;
        }
    }
    parse_expect(T_RPAREN, "expected ')'");

    /* Track max args for stack reservation. */
    if (nargs > g_func.max_call_args) g_func.max_call_args = nargs;

    /* Indirect call vs direct call. */
    if (callee && (callee->kind == VAL_REG || callee->kind == VAL_LVAL || callee->kind == VAL_SYM)) {
        /* Indirect call through function pointer. */
        val_t cv = *callee;
        load_value(&cv, RV_T5);
        rv_jalr(RV_RA, RV_T5, 0);
    } else if (fn_sym_idx >= 0) {
        /* Direct call: auipc + jalr with offset 0.
         * Use t0 as scratch: la t0, sym; jalr ra, t0, 0. */
        emit_load_global_addr(RV_T0, fn_sym_idx, 0);
        rv_jalr(RV_RA, RV_T0, 0);
    } else {
        cc_error("indirect calls not yet supported");
    }

    /* Return value in a0. */
    val_t result;
    memset(&result, 0, sizeof(result));
    result.kind = VAL_REG;
    result.reg = RV_A0;
    result.type = fn_type ? fn_type->ret : &ty_int;
    return result;
}

/* ---- Primary parser ------------------------------------------------- */
static val_t parse_primary(lexer_t *lx) {
    val_t v;
    memset(&v, 0, sizeof(v));
    v.type = &ty_int;

    switch (lx->cur.kind) {
        case T_INT: case T_LONG: case T_UINT: case T_ULONG: {
            v.kind = VAL_IMM;
            v.imm = (int64_t)lx->cur.ival;
            v.type = (lx->cur.kind == T_ULONG) ? &ty_ulong :
                     (lx->cur.kind == T_LONG)  ? &ty_long  :
                     (lx->cur.kind == T_UINT)  ? &ty_uint  : &ty_int;
            lex_next(lx);
            return v;
        }
        case T_STRING: {
            /* Allocate in rodata; load address into T0 and return VAL_REG. */
            uint32_t off = cc_strpool_add(lx->cur.str, lx->cur.str_len);
            lex_next(lx);
            /* Emit lui+addi for rodata address; record fixup. */
            uint32_t lui_off = (uint32_t)g_text.size;
            rv_lui(RV_T0, 0);
            uint32_t addi_off = (uint32_t)g_text.size;
            rv_addi(RV_T0, RV_T0, (int)off);
            /* Record rodata fixup. */
            if (g_n_rodata_fixups >= 4096) cc_fatal("too many rodata refs");
            g_rodata_fixups[g_n_rodata_fixups].rodata_off = off;
            g_rodata_fixups[g_n_rodata_fixups].patch_lui = lui_off;
            g_rodata_fixups[g_n_rodata_fixups].patch_addi = addi_off;
            g_n_rodata_fixups++;
            v.kind = VAL_REG;
            v.reg = RV_T0;
            v.type = type_make_ptr(&ty_char);
            return v;
        }
        case T_CHAR_LIT: {
            v.kind = VAL_IMM;
            v.imm = (int64_t)lx->cur.ival;
            v.type = &ty_int;
            lex_next(lx);
            return v;
        }
        case T_LPAREN: {
            lex_next(lx);
            /* Could be cast: ( type ) expr. */
            if (is_type_start(lx)) {
                type_t *t = parse_type_spec(lx);
                while (accept(T_STAR)) {
                    t = type_make_ptr(t);
                    while (accept(T_KW_CONST) || accept(T_KW_VOLATILE)) { }
                }
                parse_expect(T_RPAREN, "expected ')' after cast");
                /* Could be compound literal { ... } — not supported. */
                val_t e = parse_unary(lx);
                load_value(&e, RV_T0);
                /* Cast: truncate / extend based on target type. */
                if (type_is_integer(t) && type_sizeof(t) < 8) {
                    uint64_t sz = type_sizeof(t);
                    if (sz == 1) type_is_signed(t) ? rv_addi(RV_T0, RV_T0, 0) /* noop; we trust caller */ : 0;
                    /* For MVP we leave full 64-bit; truncation handled at store. */
                }
                e.type = t;
                e.reg = RV_T0;
                return e;
            }
            val_t e = parse_expr(lx);
            parse_expect(T_RPAREN, "expected ')'");
            return e;
        }
        case T_IDENT: {
            char name[CC_MAX_IDENT];
            strncpy(name, lx->cur.text, CC_MAX_IDENT - 1);
            name[CC_MAX_IDENT - 1] = 0;
            cc_pos_t pos = lx->cur.pos;
            lex_next(lx);

            /* Function call? */
            if (lx->cur.kind == T_LPAREN) {
                /* Built-in syscall stubs: __ecall0..3(n[, a[, b[, c]]]) → ecall */
                if (strcmp(name, "__ecall0") == 0 || strcmp(name, "__ecall1") == 0 ||
                    strcmp(name, "__ecall2") == 0 || strcmp(name, "__ecall3") == 0) {
                    int nargs = name[7] - '0';
                    parse_expect(T_LPAREN, "expected '('");
                    long args[4] = {0,0,0,0};
                    int got = 0;
                    if (lx->cur.kind != T_RPAREN) {
                        for (;;) {
                            val_t a = parse_assign(lx);
                            if (got < 4) {
                                int regs[4] = {RV_A7, RV_A0, RV_A1, RV_A2};
                                load_value(&a, regs[got]);
                                args[got] = 1;
                            }
                            got++;
                            if (!accept(T_COMMA)) break;
                        }
                    }
                    parse_expect(T_RPAREN, "expected ')'");
                    (void)args;
                    /* Move first arg (n) into a7, others stay in a0-a2. */
                    if (nargs >= 1) {
                        /* n is currently in A7 (regs[0]); that's where we want it. */
                    }
                    rv_ecall();
                    val_t r;
                    memset(&r, 0, sizeof(r));
                    r.kind = VAL_REG;
                    r.reg = RV_A0;
                    r.type = &ty_long;
                    return r;
                }
                if (strcmp(name, "va_start") == 0 || strcmp(name, "__builtin_va_start") == 0) {
                    /* va_start(ap, last) — `last` is ignored */
                    parse_expect(T_LPAREN, "expected '('");
                    val_t ap = parse_assign(lx);
                    if (strcmp(name, "va_start") == 0) {
                        parse_expect(T_COMMA, "expected ','");
                        parse_assign(lx);
                    }
                    parse_expect(T_RPAREN, "expected ')'");
                    materialize(&ap, RV_T0);
                    if (!g_func.is_variadic) {
                        cc_error("va_start used in non-variadic function");
                    }
                    int gp_off = g_func.nparams * 8;
                    if (gp_off > 64) gp_off = 64;
                    int off = g_func.va_save_off + gp_off;
                    rv_addi(RV_T1, RV_FP, off);
                    rv_sd(RV_T1, RV_T0, 0);
                    val_t vr;
                    memset(&vr, 0, sizeof(vr));
                    return vr;
                }
                if (strcmp(name, "va_arg") == 0) {
                    /* va_arg(ap, type) — type is a C type, not an expression */
                    parse_expect(T_LPAREN, "expected '('");
                    val_t ap = parse_assign(lx);
                    parse_expect(T_COMMA, "expected ','");
                    type_t *arg_type = parse_type_spec(lx);
                    while (accept(T_STAR)) arg_type = type_make_ptr(arg_type);
                    while (accept(T_KW_CONST) || accept(T_KW_VOLATILE)) { }
                    parse_expect(T_RPAREN, "expected ')'");
                    materialize(&ap, RV_T0);
                    rv_ld(RV_T1, RV_T0, 0);
                    uint64_t sz = type_sizeof(arg_type);
                    switch (sz) {
                        case 1: rv_lbu(RV_T2, RV_T1, 0); break;
                        case 2: rv_lhu(RV_T2, RV_T1, 0); break;
                        case 4: rv_lwu(RV_T2, RV_T1, 0); break;
                        case 8: rv_ld(RV_T2, RV_T1, 0); break;
                        default: rv_ld(RV_T2, RV_T1, 0); break;
                    }
                    rv_addi(RV_T1, RV_T1, 8);
                    rv_sd(RV_T1, RV_T0, 0);
                    val_t vr;
                    memset(&vr, 0, sizeof(vr));
                    vr.kind = VAL_REG;
                    vr.reg = RV_T2;
                    vr.type = arg_type;
                    return vr;
                }
                if (strcmp(name, "va_end") == 0 || strcmp(name, "__builtin_va_end") == 0) {
                    parse_expect(T_LPAREN, "expected '('");
                    parse_assign(lx);
                    parse_expect(T_RPAREN, "expected ')'");
                    val_t vr;
                    memset(&vr, 0, sizeof(vr));
                    return vr;
                }
                /* Check local symbols first (e.g. local function pointer). */
                int lidx = symtab_lookup_local(name);
                if (lidx >= 0 && g_locals[lidx].type->kind == TY_PTR &&
                    g_locals[lidx].type->base && g_locals[lidx].type->base->kind == TY_FUNC) {
                    /* Indirect call through local function pointer. */
                    type_t *ft = g_locals[lidx].type->base;
                    val_t callee;
                    memset(&callee, 0, sizeof(callee));
                    callee.kind = VAL_SYM;
                    callee.sym_idx = g_n_globals + lidx;
                    callee.type = g_locals[lidx].type;
                    return gen_call(lx, -1, ft, &callee);
                }

                int idx = symtab_lookup_global(name);
                if (idx >= 0 && g_globals[idx].kind == SYM_GLOBAL_VAR &&
                    g_globals[idx].type->kind == TY_PTR &&
                    g_globals[idx].type->base && g_globals[idx].type->base->kind == TY_FUNC) {
                    /* Indirect call through global function pointer variable. */
                    type_t *ft = g_globals[idx].type->base;
                    val_t callee;
                    memset(&callee, 0, sizeof(callee));
                    callee.kind = VAL_SYM;
                    callee.sym_idx = idx;
                    callee.type = g_globals[idx].type;
                    return gen_call(lx, -1, ft, &callee);
                }

                if (idx < 0) {
                    /* Implicit declaration of function — assume int(). */
                    type_t *ft = (type_t *)cc_arena_alloc(&g_type_arena, sizeof(type_t), 8);
                    memset(ft, 0, sizeof(*ft));
                    ft->kind = TY_FUNC;
                    ft->ret = &ty_int;
                    ft->size = 8; ft->align = 8; ft->is_complete = true;
                    idx = symtab_install_global(name, SYM_FUNCTION, ft);
                    g_globals[idx].is_defined = false;
                }
                type_t *ft = g_globals[idx].type;
                return gen_call(lx, idx, ft, NULL);
            }

            /* Variable / enum const. */
            int lidx = symtab_lookup_local(name);
            if (lidx >= 0) {
                v.kind = VAL_SYM;
                v.sym_idx = g_n_globals + lidx;  /* local encoding */
                v.offset = 0;
                v.type = g_locals[lidx].type;
            } else {
                int gidx = symtab_lookup_global(name);
                if (gidx >= 0) {
                    if (g_globals[gidx].kind == SYM_ENUM_CONST) {
                        v.kind = VAL_IMM;
                        v.imm = g_globals[gidx].enum_val;
                        v.type = &ty_int;
                    } else {
                        v.kind = VAL_SYM;
                        v.sym_idx = gidx;
                        v.offset = 0;
                        v.type = g_globals[gidx].type;
                    }
                } else {
                    cc_error_at(pos.file, pos.line, "undeclared identifier '%s'", name);
                    v.kind = VAL_IMM;
                    v.imm = 0;
                }
            }

            /* Postfix: array index [], member . ->, call (). */
            for (;;) {
                if (lx->cur.kind == T_LBRACKET) {
                    lex_next(lx);
                    val_t idx = parse_expr(lx);
                    parse_expect(T_RBRACKET, "expected ']'");
                    /* Compute address: base + idx * sizeof(*base). */
                    load_value(&v, RV_T0);
                    load_value(&idx, RV_T1);
                    if (v.type->kind == TY_ARRAY) {
                        uint64_t sz = type_sizeof(v.type->base);
                        if (sz != 1) {
                            rv_addi_imm(RV_T2, RV_ZERO, sz);
                            rv_mul(RV_T1, RV_T1, RV_T2);
                        }
                        v.type = type_make_ptr(v.type->base);
                    } else if (v.type->kind == TY_PTR) {
                        uint64_t sz = type_sizeof(v.type->base);
                        if (sz != 1) {
                            rv_addi_imm(RV_T2, RV_ZERO, sz);
                            rv_mul(RV_T1, RV_T1, RV_T2);
                        }
                    }
                    rv_add(RV_T0, RV_T0, RV_T1);
                    v.kind = VAL_LVAL;
                    v.reg = RV_T0;
                    v.offset = 0;
                    continue;
                }
                if (lx->cur.kind == T_DOT || lx->cur.kind == T_ARROW) {
                    int is_arrow = (lx->cur.kind == T_ARROW);
                    lex_next(lx);
                    if (lx->cur.kind != T_IDENT) {
                        parse_error("expected member name");
                        break;
                    }
                    char mname[CC_MAX_IDENT];
                    strncpy(mname, lx->cur.text, CC_MAX_IDENT - 1);
                    lex_next(lx);
                    /* Get struct type. */
                    type_t *st = is_arrow ? v.type->base : v.type;
                    if (!st || (st->kind != TY_STRUCT && st->kind != TY_UNION)) {
                        cc_error("member access on non-struct");
                        break;
                    }
                    int fi = -1;
                    for (int i = 0; i < st->nfields; i++) {
                        if (strcmp(st->fields[i].name, mname) == 0) { fi = i; break; }
                    }
                    if (fi < 0) {
                        cc_error("no member '%s' in struct", mname);
                        break;
                    }
                    if (is_arrow) {
                        /* v is pointer; load address into T0. */
                        load_value(&v, RV_T0);
                        v.kind = VAL_LVAL;
                        v.reg = RV_T0;
                        v.offset = (int64_t)st->fields[fi].offset;
                    } else {
                        /* v is the struct value (must be LVAL/SYM). */
                        if (v.kind == VAL_SYM) {
                            materialize(&v, RV_T0);
                        }
                        v.kind = VAL_LVAL;
                        v.offset += (int64_t)st->fields[fi].offset;
                    }
                    v.type = st->fields[fi].type;
                    continue;
                }
                if (lx->cur.kind == T_LPAREN) {
                    /* Postfix call on a value: e.g. func_ptr(args), arr[i](args). */
                    type_t *ft = NULL;
                    if (v.type->kind == TY_PTR && v.type->base && v.type->base->kind == TY_FUNC) {
                        ft = v.type->base;
                    } else if (v.type->kind == TY_FUNC) {
                        ft = v.type;
                    }
                    if (!ft) {
                        cc_error("called object is not a function or function pointer");
                        break;
                    }
                    return gen_call(lx, -1, ft, &v);
                }
                if (lx->cur.kind == T_INC || lx->cur.kind == T_DEC) {
                    int op = lx->cur.kind;
                    lex_next(lx);
                    if (v.kind != VAL_SYM && v.kind != VAL_LVAL) {
                        cc_error("++/-- requires lvalue");
                        break;
                    }
                    /* Load current value into T0. */
                    val_t cur = v;
                    load_value(&cur, RV_T0);
                    /* Compute new value: +/- 1, scaled by sizeof if pointer. */
                    uint64_t sz = type_is_pointer(v.type) ? type_sizeof(v.type->base) : 1;
                    rv_addi_imm(RV_T1, RV_T0, op == T_INC ? (int64_t)sz : -(int64_t)sz);
                    /* Store back. */
                    int addr_reg = RV_T2;
                    if (v.kind == VAL_SYM) {
                        materialize(&v, addr_reg);
                    } else {
                        rv_addi(addr_reg, v.reg, (int)v.offset);
                    }
                    uint64_t st = type_sizeof(v.type);
                    switch (st) {
                        case 1: rv_sb(RV_T1, addr_reg, 0); break;
                        case 2: rv_sh(RV_T1, addr_reg, 0); break;
                        case 4: rv_sw(RV_T1, addr_reg, 0); break;
                        case 8: rv_sd(RV_T1, addr_reg, 0); break;
                    }
                    /* Post-inc/dec returns the OLD value (already in T0). */
                    v.kind = VAL_REG;
                    v.reg = RV_T0;
                    v.offset = 0;
                    continue;
                }
                break;
            }
            return v;
        }
        case T_KW_SIZEOF: {
            lex_next(lx);
            parse_expect(T_LPAREN, "expected '(' after sizeof");
            if (is_type_start(lx)) {
                type_t *t = parse_type_spec(lx);
                while (accept(T_STAR)) t = type_make_ptr(t);
                parse_expect(T_RPAREN, "expected ')'");
                v.kind = VAL_IMM;
                v.imm = (int64_t)type_sizeof(t);
                v.type = &ty_ulong;
                return v;
            } else {
                val_t e = parse_expr(lx);
                parse_expect(T_RPAREN, "expected ')'");
                v.kind = VAL_IMM;
                v.imm = (int64_t)type_sizeof(e.type);
                v.type = &ty_ulong;
                return v;
            }
        }
        default:
            parse_error("expected expression");
            v.kind = VAL_IMM;
            v.imm = 0;
            return v;
    }
}

/* ---- Unary ---------------------------------------------------------- */
static val_t parse_unary(lexer_t *lx) {
    switch (lx->cur.kind) {
        case T_MINUS: {
            lex_next(lx);
            val_t e = parse_unary(lx);
            load_value(&e, RV_T0);
            rv_neg(RV_T0, RV_T0);
            e.reg = RV_T0;
            return e;
        }
        case T_PLUS: {
            lex_next(lx);
            return parse_unary(lx);
        }
        case T_NOT: {
            lex_next(lx);
            val_t e = parse_unary(lx);
            load_value(&e, RV_T0);
            rv_seqz(RV_T0, RV_T0);
            e.reg = RV_T0;
            e.type = &ty_int;
            return e;
        }
        case T_TILDE: {
            lex_next(lx);
            val_t e = parse_unary(lx);
            load_value(&e, RV_T0);
            rv_not(RV_T0, RV_T0);
            e.reg = RV_T0;
            return e;
        }
        case T_STAR: {
            lex_next(lx);
            val_t e = parse_unary(lx);
            load_value(&e, RV_T0);
            /* e now holds a pointer in T0; result is *T0 as an lvalue. */
            val_t r;
            memset(&r, 0, sizeof(r));
            r.kind = VAL_LVAL;
            r.reg = RV_T0;
            r.offset = 0;
            r.type = e.type->base;
            return r;
        }
        case T_AMP: {
            lex_next(lx);
            val_t e = parse_unary(lx);
            if (e.kind == VAL_SYM) {
                /* &sym — materialize address. */
                materialize(&e, RV_T0);
                e.type = type_make_ptr(e.type);
                return e;
            }
            if (e.kind == VAL_LVAL) {
                /* &*p — same as p. */
                e.type = type_make_ptr(e.type);
                return e;
            }
            cc_error("cannot take address of value");
            return e;
        }
        case T_INC: case T_DEC: {
            int op = lx->cur.kind;
            lex_next(lx);
            val_t e = parse_unary(lx);
            if (e.kind != VAL_SYM && e.kind != VAL_LVAL) {
                cc_error("++/-- requires lvalue");
                return e;
            }
            /* Load current value. */
            val_t cur = e;
            load_value(&cur, RV_T0);
            /* Compute new value: +/- 1, scaled by sizeof if pointer. */
            uint64_t sz = type_is_pointer(e.type) ? type_sizeof(e.type->base) : 1;
            rv_addi_imm(RV_T1, RV_T0, op == T_INC ? (int64_t)sz : -(int64_t)sz);
            /* Store back. */
            int addr_reg = RV_T2;
            if (e.kind == VAL_SYM) {
                materialize(&e, addr_reg);
            } else {
                rv_addi(addr_reg, e.reg, (int)e.offset);
            }
            uint64_t st = type_sizeof(e.type);
            switch (st) {
                case 1: rv_sb(RV_T1, addr_reg, 0); break;
                case 2: rv_sh(RV_T1, addr_reg, 0); break;
                case 4: rv_sw(RV_T1, addr_reg, 0); break;
                case 8: rv_sd(RV_T1, addr_reg, 0); break;
            }
            /* Pre-inc/dec returns the new value. */
            e.kind = VAL_REG;
            e.reg = RV_T0;
            e.offset = 0;
            /* But pre-inc returns NEW value: move T1 to T0. */
            rv_mv(RV_T0, RV_T1);
            return e;
        }
        default:
            return parse_primary(lx);
    }
}

/* Helper: assign a new value to an lvalue. */
static void gen_store(val_t *lhs, val_t *rhs, int op) {
    if (lhs->kind != VAL_SYM && lhs->kind != VAL_LVAL) {
        cc_error("assignment requires lvalue");
        return;
    }
    /* Evaluate RHS into T0. */
    load_value(rhs, RV_T0);

    /* For compound assignments, load current LHS value into T1. */
    int old = RV_T1;
    if (op != T_ASSIGN) {
        val_t cur = *lhs;
        load_value(&cur, old);
    }

    /* Compute new value. */
    switch (op) {
        case T_ASSIGN: break;
        case T_PLUS_ASSIGN:  rv_add(RV_T0, old, RV_T0); break;
        case T_MINUS_ASSIGN: rv_sub(RV_T0, old, RV_T0); break;
        case T_STAR_ASSIGN:  rv_mul(RV_T0, old, RV_T0); break;
        case T_SLASH_ASSIGN: type_is_unsigned(lhs->type) ? rv_divu(RV_T0, old, RV_T0) : rv_div(RV_T0, old, RV_T0); break;
        case T_PERCENT_ASSIGN: type_is_unsigned(lhs->type) ? rv_remu(RV_T0, old, RV_T0) : rv_rem(RV_T0, old, RV_T0); break;
        case T_AND_ASSIGN:   rv_and(RV_T0, old, RV_T0); break;
        case T_OR_ASSIGN:    rv_or(RV_T0, old, RV_T0); break;
        case T_XOR_ASSIGN:   rv_xor(RV_T0, old, RV_T0); break;
        case T_SHL_ASSIGN:   rv_sll(RV_T0, old, RV_T0); break;
        case T_SHR_ASSIGN:   type_is_unsigned(lhs->type) ? rv_srl(RV_T0, old, RV_T0) : rv_sra(RV_T0, old, RV_T0); break;
    }

    /* Compute address of LHS into T2. */
    int addr_reg = RV_T2;
    if (lhs->kind == VAL_SYM) {
        materialize(lhs, addr_reg);
    } else {
        rv_addi(addr_reg, lhs->reg, (int)lhs->offset);
    }
    uint64_t st = type_sizeof(lhs->type);
    switch (st) {
        case 1: rv_sb(RV_T0, addr_reg, 0); break;
        case 2: rv_sh(RV_T0, addr_reg, 0); break;
        case 4: rv_sw(RV_T0, addr_reg, 0); break;
        case 8: rv_sd(RV_T0, addr_reg, 0); break;
        default: rv_sd(RV_T0, addr_reg, 0); break;
    }
}

/* ---- Binary ops ----------------------------------------------------- */
static val_t parse_binop_rhs(lexer_t *lx, int min_prec, val_t lhs);

static val_t parse_binop(lexer_t *lx) {
    val_t lhs = parse_unary(lx);
    return parse_binop_rhs(lx, 0, lhs);
}

static int op_prec(token_kind_t k) {
    switch (k) {
        case T_STAR: case T_SLASH: case T_PERCENT: return 10;
        case T_PLUS: case T_MINUS: return 9;
        case T_SHL: case T_SHR: return 8;
        case T_LT: case T_GT: case T_LE: case T_GE: return 7;
        case T_EQ: case T_NE: return 6;
        case T_AMP: return 5;
        case T_CARET: return 4;
        case T_PIPE: return 3;
        case T_AND: return 2;
        case T_OR: return 1;
        default: return 0;
    }
}

static val_t parse_binop_rhs(lexer_t *lx, int min_prec, val_t lhs) {
    for (;;) {
        int prec = op_prec(lx->cur.kind);
        if (prec < min_prec || prec == 0) break;
        token_kind_t op = lx->cur.kind;
        lex_next(lx);

        /* Short-circuit for && and ||.
         *
         * Emit pattern (for &&):
         *     <eval lhs>          -> T0
         *     beq T0, zero, Lfalse   ; if lhs==0, jump to Lfalse
         *     <eval rhs>          -> T0
         *     j Lend
         *   Lfalse:
         *     li T0, 0
         *   Lend:
         *
         * For || it's the dual:
         *     <eval lhs>          -> T0
         *     bne T0, zero, Ltrue    ; if lhs!=0, jump to Ltrue
         *     <eval rhs>          -> T0
         *     j Lend
         *   Ltrue:
         *     li T0, 1
         *   Lend:
         *
         * Note: we must NOT recurse into parse_binop_rhs here because the
         * surrounding loop already consumes the operator and we now own the
         * lexer. We parse exactly one RHS operand via parse_binop (which
         * honors precedence) and then continue the outer loop so that
         * subsequent operators at our level are handled correctly.
         */
        if (op == T_AND || op == T_OR) {
            load_value(&lhs, RV_T0);
            if (op == T_AND) {
                /* if lhs==0, jump to false label */
                rv_beq(RV_T0, RV_ZERO, 0);
                uint32_t br_false = (uint32_t)g_text.size - 4;
                /* parse RHS: parse_unary then continue with higher-precedence ops */
                val_t rhs = parse_unary(lx);
                int next_prec = op_prec(lx->cur.kind);
                if (next_prec > prec) {
                    rhs = parse_binop_rhs(lx, prec + 1, rhs);
                }
                load_value(&rhs, RV_T0);
                /* normalize: any non-zero becomes 1 */
                rv_snez(RV_T0, RV_T0);
                /* jump to end */
                rv_jal(RV_ZERO, 0);
                uint32_t jmp_end = (uint32_t)g_text.size - 4;
                /* false: */
                uint32_t false_label = (uint32_t)g_text.size;
                rv_addi(RV_T0, RV_ZERO, 0);
                /* end: */
                uint32_t end_label = (uint32_t)g_text.size;
                /* patch br_false to false_label */
                {
                    int32_t delta = (int32_t)false_label - (int32_t)br_false;
                    uint8_t *p = g_text.data + br_false;
                    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    insn |= ((uint32_t)(delta & 0x1000) << 19) |
                            ((uint32_t)(delta & 0x7E0) << 20) |
                            ((uint32_t)(delta & 0x1E) << 7)  |
                            ((uint32_t)(delta & 0x800) >> 4);
                    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
                    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
                }
                /* patch jmp_end to end_label */
                {
                    int32_t delta = (int32_t)end_label - (int32_t)jmp_end;
                    uint8_t *p = g_text.data + jmp_end;
                    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    /* jal is J-type: imm[20|10:1|11|19:12] at bits 31|30:21|20|19:12 */
                    uint32_t imm = ((uint32_t)((delta >> 20) & 1) << 31) |
                                   ((uint32_t)((delta >> 1) & 0x3FF) << 21) |
                                   ((uint32_t)((delta >> 11) & 1) << 20) |
                                   ((uint32_t)((delta >> 12) & 0xFF) << 12);
                    insn = (insn & 0xFFF) | imm;
                    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
                    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
                }
            } else {
                /* if lhs!=0, jump to true label */
                rv_bne(RV_T0, RV_ZERO, 0);
                uint32_t br_true = (uint32_t)g_text.size - 4;
                val_t rhs = parse_unary(lx);
                int next_prec = op_prec(lx->cur.kind);
                if (next_prec > prec) {
                    rhs = parse_binop_rhs(lx, prec + 1, rhs);
                }
                load_value(&rhs, RV_T0);
                rv_snez(RV_T0, RV_T0);
                rv_jal(RV_ZERO, 0);
                uint32_t jmp_end = (uint32_t)g_text.size - 4;
                uint32_t true_label = (uint32_t)g_text.size;
                rv_addi(RV_T0, RV_ZERO, 1);
                uint32_t end_label = (uint32_t)g_text.size;
                /* patch br_true to true_label */
                {
                    int32_t delta = (int32_t)true_label - (int32_t)br_true;
                    uint8_t *p = g_text.data + br_true;
                    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    insn |= ((uint32_t)(delta & 0x1000) << 19) |
                            ((uint32_t)(delta & 0x7E0) << 20) |
                            ((uint32_t)(delta & 0x1E) << 7)  |
                            ((uint32_t)(delta & 0x800) >> 4);
                    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
                    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
                }
                /* patch jmp_end to end_label */
                {
                    int32_t delta = (int32_t)end_label - (int32_t)jmp_end;
                    uint8_t *p = g_text.data + jmp_end;
                    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    uint32_t imm = ((uint32_t)((delta >> 20) & 1) << 31) |
                                   ((uint32_t)((delta >> 1) & 0x3FF) << 21) |
                                   ((uint32_t)((delta >> 11) & 1) << 20) |
                                   ((uint32_t)((delta >> 12) & 0xFF) << 12);
                    insn = (insn & 0xFFF) | imm;
                    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff;
                    p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
                }
            }
            /* Result is in T0; treat as int. */
            lhs.kind = VAL_REG;
            lhs.reg = RV_T0;
            lhs.type = &ty_int;
            continue;
        }

        val_t rhs = parse_unary(lx);
        int next_prec = op_prec(lx->cur.kind);
        if (next_prec > prec) {
            rhs = parse_binop_rhs(lx, prec + 1, rhs);
        }

        load_value(&lhs, RV_T0);
        load_value(&rhs, RV_T1);

        /* Type promotion for arithmetic. */
        type_t *common = type_common(lhs.type, rhs.type);
        if (type_is_pointer(lhs.type) && type_is_integer(rhs.type)) {
            /* pointer + int → pointer arithmetic, scale by sizeof(*lhs). */
            uint64_t sz = type_sizeof(lhs.type->base);
            if (sz != 1) {
                rv_addi_imm(RV_T2, RV_ZERO, sz);
                rv_mul(RV_T1, RV_T1, RV_T2);
            }
            common = lhs.type;
        }

        switch (op) {
            case T_PLUS:    rv_add(RV_T0, RV_T0, RV_T1); break;
            case T_MINUS:
                if (type_is_pointer(lhs.type) && type_is_pointer(rhs.type)) {
                    /* pointer - pointer → divide by sizeof. */
                    rv_sub(RV_T0, RV_T0, RV_T1);
                    uint64_t sz = type_sizeof(lhs.type->base);
                    if (sz != 1) {
                        rv_addi_imm(RV_T1, RV_ZERO, sz);
                        rv_divu(RV_T0, RV_T0, RV_T1);
                    }
                    common = &ty_long;
                } else {
                    rv_sub(RV_T0, RV_T0, RV_T1);
                }
                break;
            case T_STAR:    rv_mul(RV_T0, RV_T0, RV_T1); break;
            case T_SLASH:   type_is_unsigned(common) ? rv_divu(RV_T0, RV_T0, RV_T1) : rv_div(RV_T0, RV_T0, RV_T1); break;
            case T_PERCENT: type_is_unsigned(common) ? rv_remu(RV_T0, RV_T0, RV_T1) : rv_rem(RV_T0, RV_T0, RV_T1); break;
            case T_AMP:     rv_and(RV_T0, RV_T0, RV_T1); break;
            case T_PIPE:    rv_or(RV_T0, RV_T0, RV_T1); break;
            case T_CARET:   rv_xor(RV_T0, RV_T0, RV_T1); break;
            case T_SHL:     rv_sll(RV_T0, RV_T0, RV_T1); break;
            case T_SHR:     type_is_unsigned(lhs.type) ? rv_srl(RV_T0, RV_T0, RV_T1) : rv_sra(RV_T0, RV_T0, RV_T1); break;
            case T_EQ:      rv_sub(RV_T0, RV_T0, RV_T1); rv_seqz(RV_T0, RV_T0); break;
            case T_NE:      rv_sub(RV_T0, RV_T0, RV_T1); rv_sltu(RV_T0, RV_ZERO, RV_T0); break;
            case T_LT:      type_is_unsigned(common) ? rv_sltu(RV_T0, RV_T0, RV_T1) : rv_slt(RV_T0, RV_T0, RV_T1); break;
            case T_GT:      type_is_unsigned(common) ? rv_sltu(RV_T0, RV_T1, RV_T0) : rv_slt(RV_T0, RV_T1, RV_T0); break;
            case T_LE:      type_is_unsigned(common) ? rv_sltu(RV_T0, RV_T1, RV_T0) : rv_slt(RV_T0, RV_T1, RV_T0); rv_seqz(RV_T0, RV_T0); break;
            case T_GE:      type_is_unsigned(common) ? rv_sltu(RV_T0, RV_T0, RV_T1) : rv_slt(RV_T0, RV_T0, RV_T1); rv_seqz(RV_T0, RV_T0); break;
            default: break;
        }

        lhs.kind = VAL_REG;
        lhs.reg = RV_T0;
        lhs.type = common;
    }
    return lhs;
}

/* helper for T_NE: invert — unused, kept for future use */
#if 0
static inline void rv_xori_(int rd, int rs) {
    rv_xor(rd, rd, rd);
    rv_addi(rd, rd, 1);
    rv_sub(rd, rd, rs);
}
#endif

/* Ternary. */
static val_t parse_ternary(lexer_t *lx) {
    val_t cond = parse_binop(lx);
    if (accept(T_QUESTION)) {
        load_value(&cond, RV_T0);
        uint32_t else_label = 0;
        uint32_t end_label = 0;
        /* beq t0, zero, else */
        rv_beq(RV_T0, RV_ZERO, 0);
        uint32_t br1_off = (uint32_t)g_text.size - 4;
        val_t then_v = parse_expr(lx);
        load_value(&then_v, RV_T0);
        /* jal end */
        rv_jal(RV_ZERO, 0);
        uint32_t jmp_off = (uint32_t)g_text.size - 4;
        /* else: */
        else_label = (uint32_t)g_text.size;
        parse_expect(T_COLON, "expected ':' in ternary");
        val_t else_v = parse_ternary(lx);
        load_value(&else_v, RV_T0);
        end_label = (uint32_t)g_text.size;

        /* Patch branches. */
        int32_t d1 = (int32_t)else_label - (int32_t)br1_off;
        uint8_t *p1 = g_text.data + br1_off;
        uint32_t i1 = (uint32_t)p1[0] | ((uint32_t)p1[1] << 8) | ((uint32_t)p1[2] << 16) | ((uint32_t)p1[3] << 24);
        i1 |= ((uint32_t)(d1 & 0x1000) << 19) | ((uint32_t)(d1 & 0x7E0) << 20) |
              ((uint32_t)(d1 & 0x1E) << 7) | ((uint32_t)(d1 & 0x800) >> 4);
        p1[0] = i1 & 0xff; p1[1] = (i1 >> 8) & 0xff; p1[2] = (i1 >> 16) & 0xff; p1[3] = (i1 >> 24) & 0xff;

        int32_t d2 = (int32_t)end_label - (int32_t)jmp_off;
        uint8_t *p2 = g_text.data + jmp_off;
        uint32_t i2 = (uint32_t)p2[0] | ((uint32_t)p2[1] << 8) | ((uint32_t)p2[2] << 16) | ((uint32_t)p2[3] << 24);
        i2 &= ~0xFFFFF000;
        i2 |= ((uint32_t)(d2 & 0x100000) << 11) | ((uint32_t)(d2 & 0x7FE) << 20) |
              ((uint32_t)(d2 & 0x800) << 9) | ((uint32_t)(d2 & 0xFF000));
        p2[0] = i2 & 0xff; p2[1] = (i2 >> 8) & 0xff; p2[2] = (i2 >> 16) & 0xff; p2[3] = (i2 >> 24) & 0xff;

        then_v.kind = VAL_REG;
        then_v.reg = RV_T0;
        return then_v;
    }
    return cond;
}

static val_t parse_assign(lexer_t *lx) {
    val_t lhs = parse_ternary(lx);
    token_kind_t k = lx->cur.kind;
    if (k == T_ASSIGN || k == T_PLUS_ASSIGN || k == T_MINUS_ASSIGN || k == T_STAR_ASSIGN ||
        k == T_SLASH_ASSIGN || k == T_PERCENT_ASSIGN || k == T_AND_ASSIGN || k == T_OR_ASSIGN ||
        k == T_XOR_ASSIGN || k == T_SHL_ASSIGN || k == T_SHR_ASSIGN) {
        lex_next(lx);
        val_t rhs = parse_assign(lx);
        gen_store(&lhs, &rhs, k);
        /* Assignment returns the assigned value. */
        lhs.kind = VAL_REG;
        lhs.reg = RV_T0;
        lhs.offset = 0;
    }
    return lhs;
}

static val_t parse_expr(lexer_t *lx) {
    val_t v = parse_assign(lx);
    while (accept(T_COMMA)) {
        v = parse_assign(lx);
    }
    return v;
}

/* ---- Statements ----------------------------------------------------- */

/* Forward. */
static void parse_compound(lexer_t *lx);

static void parse_stmt(lexer_t *lx) {
    cc_pos_t pos = lx->cur.pos;

    /* Label: identifier : */
    if (lx->cur.kind == T_IDENT) {
        lex_peek(lx);
        if (lx->next.kind == T_COLON) {
            char name[CC_MAX_IDENT];
            strncpy(name, lx->cur.text, CC_MAX_IDENT - 1);
            name[CC_MAX_IDENT - 1] = 0;
            lex_next(lx);
            lex_next(lx);
            label_define(name);
            return;
        }
    }

    /* Declarations inside a function are statements (C99). */
    if (is_type_start(lx)) {
        /* Local declaration: parse type + declarators, allocate stack slots. */
        type_t *base = NULL;
        bool is_static, is_extern, is_typedef, is_inline;
        char tag[CC_MAX_IDENT] = {0};
        if (!parse_decl_spec(lx, &base, &is_static, &is_extern, &is_typedef, &is_inline, tag)) {
            parse_error("expected declaration");
            return;
        }
        if (g_lx->cur.kind == T_SEMI) {
            lex_next(lx);
            return;
        }
        for (;;) {
            type_t *t = base;
            while (accept(T_STAR)) {
                t = type_make_ptr(t);
                while (accept(T_KW_CONST) || accept(T_KW_VOLATILE)) { }
            }
            char name[CC_MAX_IDENT] = {0};
            if (lx->cur.kind == T_IDENT) {
                strncpy(name, lx->cur.text, CC_MAX_IDENT - 1);
                lex_next(lx);
            }
            while (lx->cur.kind == T_LBRACKET) {
                lex_next(lx);
                uint64_t len = 0;
                if (lx->cur.kind == T_INT || lx->cur.kind == T_LONG ||
                    lx->cur.kind == T_UINT || lx->cur.kind == T_ULONG) {
                    len = lx->cur.ival;
                    lex_next(lx);
                }
                parse_expect(T_RBRACKET, "expected ']'");
                t = type_make_array(t, len);
            }
            /* Allocate stack slot. */
            uint64_t sz = type_sizeof(t);
            uint64_t al = type_alignof(t);
            g_func.cur_offset += sz;
            g_func.cur_offset = (g_func.cur_offset + al - 1) & ~(al - 1);
            int64_t off = -(int64_t)g_func.cur_offset;
            int idx = symtab_install_local(name, SYM_LOCAL_VAR, t, g_cur_scope);
            g_locals[idx].offset = off;
            if (g_func.cur_offset > g_func.frame_size) {
                g_func.frame_size = g_func.cur_offset;
            }

            /* Optional initializer. */
            if (accept(T_ASSIGN)) {
                val_t init = parse_assign(lx);
                /* Store into local. */
                val_t lhs;
                memset(&lhs, 0, sizeof(lhs));
                lhs.kind = VAL_SYM;
                lhs.sym_idx = g_n_globals + idx;
                lhs.offset = 0;
                lhs.type = t;
                gen_store(&lhs, &init, T_ASSIGN);
            }

            if (accept(T_COMMA)) continue;
            break;
        }
        parse_expect(T_SEMI, "expected ';'");
        return;
    }

    switch (lx->cur.kind) {
        case T_SEMI: lex_next(lx); return;
        case T_LBRACE: parse_compound(lx); return;
        case T_KW_IF: {
            lex_next(lx);
            parse_expect(T_LPAREN, "expected '(' after if");
            val_t cond = parse_expr(lx);
            parse_expect(T_RPAREN, "expected ')'");
            load_value(&cond, RV_T0);
            rv_beq(RV_T0, RV_ZERO, 0);  /* skip then if false */
            uint32_t br_off = (uint32_t)g_text.size - 4;
            parse_stmt(lx);
            uint32_t end_label;
            if (lx->cur.kind == T_KW_ELSE) {
                lex_next(lx);
                rv_jal(RV_ZERO, 0);
                uint32_t jmp_off = (uint32_t)g_text.size - 4;
                uint32_t else_label = (uint32_t)g_text.size;
                parse_stmt(lx);
                end_label = (uint32_t)g_text.size;
                /* Patch br1 to else_label. */
                int32_t d1 = (int32_t)else_label - (int32_t)br_off;
                uint8_t *p1 = g_text.data + br_off;
                uint32_t i1 = (uint32_t)p1[0] | ((uint32_t)p1[1] << 8) | ((uint32_t)p1[2] << 16) | ((uint32_t)p1[3] << 24);
                i1 |= ((uint32_t)(d1 & 0x1000) << 19) | ((uint32_t)(d1 & 0x7E0) << 20) |
                      ((uint32_t)(d1 & 0x1E) << 7) | ((uint32_t)(d1 & 0x800) >> 4);
                p1[0] = i1 & 0xff; p1[1] = (i1 >> 8) & 0xff; p1[2] = (i1 >> 16) & 0xff; p1[3] = (i1 >> 24) & 0xff;
                int32_t d2 = (int32_t)end_label - (int32_t)jmp_off;
                uint8_t *p2 = g_text.data + jmp_off;
                uint32_t i2 = (uint32_t)p2[0] | ((uint32_t)p2[1] << 8) | ((uint32_t)p2[2] << 16) | ((uint32_t)p2[3] << 24);
                i2 &= ~0xFFFFF000;
                i2 |= ((uint32_t)(d2 & 0x100000) << 11) | ((uint32_t)(d2 & 0x7FE) << 20) |
                      ((uint32_t)(d2 & 0x800) << 9) | ((uint32_t)(d2 & 0xFF000));
                p2[0] = i2 & 0xff; p2[1] = (i2 >> 8) & 0xff; p2[2] = (i2 >> 16) & 0xff; p2[3] = (i2 >> 24) & 0xff;
            } else {
                end_label = (uint32_t)g_text.size;
                int32_t d1 = (int32_t)end_label - (int32_t)br_off;
                uint8_t *p1 = g_text.data + br_off;
                uint32_t i1 = (uint32_t)p1[0] | ((uint32_t)p1[1] << 8) | ((uint32_t)p1[2] << 16) | ((uint32_t)p1[3] << 24);
                i1 |= ((uint32_t)(d1 & 0x1000) << 19) | ((uint32_t)(d1 & 0x7E0) << 20) |
                      ((uint32_t)(d1 & 0x1E) << 7) | ((uint32_t)(d1 & 0x800) >> 4);
                p1[0] = i1 & 0xff; p1[1] = (i1 >> 8) & 0xff; p1[2] = (i1 >> 16) & 0xff; p1[3] = (i1 >> 24) & 0xff;
            }
            return;
        }
        case T_KW_WHILE: {
            lex_next(lx);
            uint32_t cond_label = (uint32_t)g_text.size;
            parse_expect(T_LPAREN, "expected '('");
            val_t cond = parse_expr(lx);
            parse_expect(T_RPAREN, "expected ')'");
            load_value(&cond, RV_T0);
            rv_beq(RV_T0, RV_ZERO, 0);
            uint32_t br_off = (uint32_t)g_text.size - 4;
            int saved_n_break = g_func.n_break_fixups;
            int saved_n_cont = g_func.n_continue_fixups;
            uint32_t saved_break = g_func.break_label;
            uint32_t saved_continue = g_func.continue_label;
            g_func.break_label = 0;
            g_func.continue_label = cond_label;
            g_func.loop_depth++;
            parse_stmt(lx);
            g_func.loop_depth--;
            rv_jal(RV_ZERO, 0);  /* back to cond_label */
            uint32_t jmp_off = (uint32_t)g_text.size - 4;
            uint32_t end_label = (uint32_t)g_text.size;
            /* Patch conditional branch. */
            patch_branch(br_off, (int32_t)end_label - (int32_t)br_off);
            /* Patch loop-back jump. */
            patch_jal(jmp_off, (int32_t)cond_label - (int32_t)jmp_off);
            /* Patch break fixups. */
            for (int i = saved_n_break; i < g_func.n_break_fixups; i++)
                patch_jal(g_func.break_fixups[i], (int32_t)end_label - (int32_t)g_func.break_fixups[i]);
            /* Patch continue fixups (shouldn't be any for while since
             * continue_label is known, but handle for safety). */
            for (int i = saved_n_cont; i < g_func.n_continue_fixups; i++)
                patch_jal(g_func.continue_fixups[i], (int32_t)cond_label - (int32_t)g_func.continue_fixups[i]);
            g_func.n_break_fixups = saved_n_break;
            g_func.n_continue_fixups = saved_n_cont;
            g_func.break_label = saved_break;
            g_func.continue_label = saved_continue;
            return;
        }
        case T_KW_RETURN: {
            lex_next(lx);
            if (lx->cur.kind != T_SEMI) {
                val_t v = parse_expr(lx);
                load_value(&v, RV_A0);
            }
            parse_expect(T_SEMI, "expected ';'");
            /* Epilogue: restore fp, ra; dealloc stack; ret. */
            rv_addi(RV_SP, RV_FP, 0);
            rv_ld(RV_RA, RV_SP, 8);
            rv_ld(RV_FP, RV_SP, 0);
            rv_addi(RV_SP, RV_SP, 16);
            rv_ret();
            return;
        }
        case T_KW_BREAK: {
            lex_next(lx);
            parse_expect(T_SEMI, "expected ';'");
            if (g_func.loop_depth == 0 && g_func.switch_depth == 0) {
                cc_error("'break' outside loop or switch");
                return;
            }
            rv_jal(RV_ZERO, 0);
            uint32_t br_off = (uint32_t)g_text.size - 4;
            g_func.break_fixups[g_func.n_break_fixups++] = br_off;
            return;
        }
        case T_KW_CONTINUE:
            lex_next(lx);
            parse_expect(T_SEMI, "expected ';'");
            if (g_func.loop_depth == 0) {
                cc_error("'continue' outside loop");
                return;
            }
            rv_jal(RV_ZERO, 0);
            {
                uint32_t cont_off = (uint32_t)g_text.size - 4;
                if (g_func.continue_label != 0) {
                    /* Target already known (e.g. while loop) — patch directly. */
                    patch_jal(cont_off, (int32_t)g_func.continue_label - (int32_t)cont_off);
                } else {
                    /* Target not yet known (e.g. for loop post-expr) — add fixup. */
                    g_func.continue_fixups[g_func.n_continue_fixups++] = cont_off;
                }
            }
            return;
        case T_KW_GOTO: {
            lex_next(lx);
            if (lx->cur.kind != T_IDENT) {
                parse_error("expected label name");
                return;
            }
            char name[CC_MAX_IDENT];
            strncpy(name, lx->cur.text, CC_MAX_IDENT - 1);
            name[CC_MAX_IDENT - 1] = 0;
            lex_next(lx);
            parse_expect(T_SEMI, "expected ';'");
            label_emit_jump(name);
            return;
        }
        case T_KW_FOR: {
            /* for(init; cond; post) body — single-pass codegen.
             * Emit: init → cond → [branch if !cond to end] → body → post → jump to cond.
             * The post-expression is skipped during initial lexing (source range
             * saved), then re-lexed and parsed after the body so its code is
             * emitted in the correct position. */
            lex_next(lx);
            parse_expect(T_LPAREN, "expected '(' after for");
            symtab_push_scope();
            /* init */
            if (lx->cur.kind != T_SEMI) {
                parse_stmt(lx);  /* will consume ';' */
            } else {
                lex_next(lx);
            }
            uint32_t cond_label = (uint32_t)g_text.size;
            val_t cond;
            bool has_cond = false;
            if (lx->cur.kind != T_SEMI) {
                cond = parse_expr(lx);
                has_cond = true;
            }
            parse_expect(T_SEMI, "expected ';'");
            if (has_cond) {
                load_value(&cond, RV_T0);
                rv_beq(RV_T0, RV_ZERO, 0);
            }
            uint32_t br_off = has_cond ? (uint32_t)g_text.size - 4 : 0;

            /* Save post-expression source range, then skip it. */
            size_t post_start_off = lx->pos;
            int paren = 0;
            while (lx->cur.kind != T_RPAREN || paren > 0) {
                if (lx->cur.kind == T_LPAREN) paren++;
                else if (lx->cur.kind == T_RPAREN) paren--;
                else if (lx->cur.kind == T_EOF) break;
                lex_next(lx);
            }
            size_t post_end_off = lx->pos;
            parse_expect(T_RPAREN, "expected ')'");

            int saved_n_break = g_func.n_break_fixups;
            int saved_n_cont = g_func.n_continue_fixups;
            uint32_t saved_break = g_func.break_label;
            uint32_t saved_continue = g_func.continue_label;
            g_func.break_label = 0;
            /* continue_label is not known yet (post-expr hasn't been emitted),
             * so set to 0 to signal that continue fixups are needed. */
            g_func.continue_label = 0;
            g_func.loop_depth++;
            parse_stmt(lx);

            /* Continue target: emit post-expression here. */
            uint32_t post_label = (uint32_t)g_text.size;
            if (post_end_off > post_start_off) {
                /* Re-lex and parse the post-expression from the saved source
                 * range using a temporary lexer.  The symbol table is shared
                 * (global state) so variable lookups still work.
                 * We must also swap g_lx because parse_error/parse_expect/
                 * accept use the global lexer pointer. */
                lexer_t post_lx;
                lexer_t *saved_g_lx = g_lx;
                lex_init(&post_lx, lx->src + post_start_off,
                         post_end_off - post_start_off, lx->filename);
                lex_next(&post_lx);
                g_lx = &post_lx;
                val_t pv = parse_expr(&post_lx);
                (void)pv;
                g_lx = saved_g_lx;
            }

            rv_jal(RV_ZERO, 0);
            uint32_t jmp_off = (uint32_t)g_text.size - 4;
            uint32_t end_label = (uint32_t)g_text.size;
            g_func.loop_depth--;

            if (has_cond)
                patch_branch(br_off, (int32_t)end_label - (int32_t)br_off);
            patch_jal(jmp_off, (int32_t)cond_label - (int32_t)jmp_off);
            /* Patch break fixups → end_label. */
            for (int i = saved_n_break; i < g_func.n_break_fixups; i++)
                patch_jal(g_func.break_fixups[i], (int32_t)end_label - (int32_t)g_func.break_fixups[i]);
            /* Patch continue fixups → post_label. */
            for (int i = saved_n_cont; i < g_func.n_continue_fixups; i++)
                patch_jal(g_func.continue_fixups[i], (int32_t)post_label - (int32_t)g_func.continue_fixups[i]);
            g_func.n_break_fixups = saved_n_break;
            g_func.n_continue_fixups = saved_n_cont;
            g_func.break_label = saved_break;
            g_func.continue_label = saved_continue;
            symtab_pop_scope();
            return;
        }
        case T_KW_SWITCH: {
            lex_next(lx);
            parse_expect(T_LPAREN, "expected '(' after switch");
            val_t sv = parse_expr(lx);
            parse_expect(T_RPAREN, "expected ')'");
            load_value(&sv, RV_T0);
            rv_mv(RV_T1, RV_T0);
            parse_expect(T_LBRACE, "expected '{'");
            uint32_t saved_break = g_func.break_label;
            int saved_n_break = g_func.n_break_fixups;
            g_func.break_label = 0;
            g_func.switch_depth++;
            uint32_t prev_branch = 0;
            while (lx->cur.kind != T_RBRACE && lx->cur.kind != T_EOF) {
                if (lx->cur.kind == T_KW_CASE) {
                    if (prev_branch)
                        patch_branch(prev_branch, (int32_t)g_text.size - (int32_t)prev_branch);
                    lex_next(lx);
                    val_t cv = parse_expr(lx);
                    load_value(&cv, RV_T0);
                    parse_expect(T_COLON, "expected ':' after case value");
                    rv_bne(RV_T1, RV_T0, 0);
                    prev_branch = (uint32_t)g_text.size - 4;
                } else if (lx->cur.kind == T_KW_DEFAULT) {
                    if (prev_branch)
                        patch_branch(prev_branch, (int32_t)g_text.size - (int32_t)prev_branch);
                    prev_branch = 0;
                    lex_next(lx);
                    parse_expect(T_COLON, "expected ':' after default");
                } else {
                    parse_stmt(lx);
                }
            }
            if (prev_branch)
                patch_branch(prev_branch, (int32_t)g_text.size - (int32_t)prev_branch);
            uint32_t end = (uint32_t)g_text.size;
            for (int i = saved_n_break; i < g_func.n_break_fixups; i++)
                patch_jal(g_func.break_fixups[i], (int32_t)end - (int32_t)g_func.break_fixups[i]);
            g_func.n_break_fixups = saved_n_break;
            g_func.break_label = saved_break;
            g_func.switch_depth--;
            parse_expect(T_RBRACE, "expected '}' after switch");
            return;
        }
        default: {
            /* Expression statement. */
            val_t v = parse_expr(lx);
            (void)v;
            parse_expect(T_SEMI, "expected ';'");
            return;
        }
    }
}

static void parse_compound(lexer_t *lx) {
    parse_expect(T_LBRACE, "expected '{'");
    symtab_push_scope();
    while (lx->cur.kind != T_RBRACE && lx->cur.kind != T_EOF) {
        parse_stmt(lx);
    }
    symtab_pop_scope();
    parse_expect(T_RBRACE, "expected '}'");
}

/* ---- Global init parser (constant only) ---------------------------- */
expr_t *gen_parse_global_init(lexer_t *lx, type_t *type) {
    /* Support string literal initializers: char *s = "hello"; char s[] = "hello" */
    if (lx->cur.kind == T_STRING) {
        /* Add string to rodata pool. */
        uint32_t off = cc_strpool_add(lx->cur.str, lx->cur.str_len);
        size_t slen = lx->cur.str_len;  /* length without null terminator */
        lex_next(lx);
        /* Create an EX_STR node; ival holds the rodata offset, str_len the byte count. */
        expr_t *e = ast_new_expr(EX_STR, lx->cur.pos);
        e->ival = off;
        e->str_len = slen;
        return e;
    }

    /* For non-string: support only integer/pointer constants. */
    val_t v = parse_assign(lx);
    expr_t *e = ast_new_expr(EX_NUM, lx->cur.pos);
    if (v.kind == VAL_IMM) {
        e->ival = v.imm;
    } else {
        cc_error("non-constant global initializer");
        e->ival = 0;
    }
    return e;
}

/* ---- Function body codegen ----------------------------------------- */
static void gen_func_body(lexer_t *lx, decl_t *d) {
    type_t *ftype = d->type;
    symtab_enter_function(ftype);

    /* Reserve space for ra and fp. */
    g_func.frame_size = 16;  /* ra at 8, fp at 0 */
    g_func.cur_offset = 16;
    g_func.max_call_args = 0;
    g_func.loop_depth = 0;
    g_func.n_break_fixups = 0;
    g_func.n_continue_fixups = 0;
    g_func.in_function = true;
    g_func.nparams = ftype->nparams;
    g_func.is_variadic = ftype->is_varargs;
    label_clear();

    /* Pre-allocate slots for parameters. */
    int arg_regs[8] = {RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7};
    int *param_slots = (int *)calloc(ftype->nparams, sizeof(int));
    for (int i = 0; i < ftype->nparams; i++) {
        type_t *ptyp = ftype->params[i].type;
        uint64_t sz = type_sizeof(ptyp);
        uint64_t al = type_alignof(ptyp);
        g_func.cur_offset += sz;
        g_func.cur_offset = (g_func.cur_offset + al - 1) & ~(al - 1);
        int64_t off = -(int64_t)g_func.cur_offset;
        int idx = symtab_install_local(ftype->params[i].name, SYM_LOCAL_VAR, ptyp, 1);
        g_locals[idx].offset = off;
        param_slots[i] = (int)off;
    }
    if (g_func.cur_offset > g_func.frame_size) g_func.frame_size = g_func.cur_offset;

    /* For variadic functions: reserve 64 bytes for register save area (a0-a7). */
    if (ftype->is_varargs) {
        g_func.cur_offset += 64;
        g_func.cur_offset = (g_func.cur_offset + 7) & ~7;
    }
    if (g_func.cur_offset > g_func.frame_size) g_func.frame_size = g_func.cur_offset;

    /* Parse body — but we need to know frame_size BEFORE prologue.
     * For MVP we use a two-pass approach: parse into AST statements,
     * then emit prologue + body. Simpler: emit prologue with conservative
     * frame size = current cur_offset + space for max_call_args.
     * But we don't know max_call_args yet. So emit prologue AFTER parsing.
     *
     * Approach: parse body into g_text with a placeholder prologue,
     * then patch frame_size. */
    uint32_t prologue_start = (uint32_t)g_text.size;
    /* Emit 16-byte placeholder prologue: */
    /* addi sp, sp, -FRAME  (patched) */
    rv_addi(RV_SP, RV_SP, 0);
    /* sd ra, 8(sp) */
    rv_sd(RV_RA, RV_SP, 8);
    /* sd fp, 0(sp) */
    rv_sd(RV_FP, RV_SP, 0);
    /* addi fp, sp, 0 */
    rv_addi(RV_FP, RV_SP, 0);

    /* Store parameters from a0-a7 to their slots. */
    for (int i = 0; i < ftype->nparams && i < 8; i++) {
        int off = param_slots[i];
        type_t *ptyp = ftype->params[i].type;
        uint64_t sz = type_sizeof(ptyp);
        switch (sz) {
            case 1: rv_sb(arg_regs[i], RV_FP, off); break;
            case 2: rv_sh(arg_regs[i], RV_FP, off); break;
            case 4: rv_sw(arg_regs[i], RV_FP, off); break;
            case 8: rv_sd(arg_regs[i], RV_FP, off); break;
        }
    }

    /* For variadic functions: save all a0-a7 to the register save area. */
    if (ftype->is_varargs) {
        g_func.va_save_off = -(int)g_func.cur_offset;
        for (int i = 0; i < 8; i++) {
            rv_sd(arg_regs[i], RV_FP, g_func.va_save_off + i * 8);
        }
    }

    parse_compound(lx);

    /* Default return. */
    rv_addi(RV_A0, RV_ZERO, 0);
    rv_addi(RV_SP, RV_FP, 0);
    rv_ld(RV_RA, RV_SP, 8);
    rv_ld(RV_FP, RV_SP, 0);
    rv_addi(RV_SP, RV_SP, 16);
    rv_ret();

    /* Patch prologue: replace `addi sp, sp, 0` with `addi sp, sp, -frame`. */
    int frame = (g_func.frame_size + 15) & ~15;
    int32_t f = -frame;
    uint8_t *p = g_text.data + prologue_start;
    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    insn &= ~0xFFF00000;
    insn |= ((uint32_t)(f & 0xFFF)) << 20;
    p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff; p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;

    rv_resolve_fixups();
    label_check_unresolved();
    free(param_slots);

    g_func.in_function = false;
    symtab_leave_function();
}

/* ---- Top-level declaration codegen --------------------------------- */
void gen_init(void) {
    cc_buf_init(&g_text);
    cc_buf_init(&g_rodata);
    cc_buf_init(&g_data);
    g_bss_size = 0;
    g_entry = 0;
}

void gen_decl(decl_t *d) {
    switch (d->kind) {
        case D_TYPEDEF: {
            symtab_install_global(d->name, SYM_TYPEDEF, d->type);
            return;
        }
        case D_FUNC_DECL: {
            int idx = symtab_install_global(d->name, SYM_FUNCTION, d->type);
            g_globals[idx].is_defined = false;
            return;
        }
        case D_FUNC_DEF: {
            int idx = symtab_install_global(d->name, SYM_FUNCTION, d->type);
            g_globals[idx].is_defined = true;
            g_globals[idx].text_off = (uint32_t)g_text.size;
            if (strcmp(d->name, g_opts.entry_sym ? g_opts.entry_sym : "_start") == 0) {
                g_entry = CC_TEXT_VADDR + g_text.size;
            }
            gen_func_body(g_lx, d);
            return;
        }
        case D_VAR: {
            int idx = symtab_install_global(d->name, SYM_GLOBAL_VAR, d->type);
            if (d->init) {
                if (d->init->kind == EX_NUM) {
                    /* Integer/pointer constant initializer. */
                    uint64_t v = d->init->ival;
                    g_globals[idx].is_defined = true;
                    g_globals[idx].text_off = (uint32_t)g_data.size;
                    uint64_t sz = type_sizeof(d->type);
                    for (uint64_t i = 0; i < sz; i++) {
                        cc_buf_push8(&g_data, (uint8_t)(v >> (8 * i)));
                    }
                } else if (d->init->kind == EX_STR) {
                    /* String literal initializer: char *s = "hello" or char s[] = "hello". */
                    uint32_t rodata_off = (uint32_t)d->init->ival;
                    g_globals[idx].is_defined = true;
                    g_globals[idx].text_off = (uint32_t)g_data.size;
                    if (d->type->kind == TY_PTR) {
                        /* char *s = "hello" → store the address of the string.
                         * Write placeholder 0; patched in gen_finalize with rodata address. */
                        uint32_t data_off = (uint32_t)g_data.size;
                        cc_buf_push64(&g_data, 0);
                        /* Record fixup so gen_finalize can patch the address. */
                        if (g_n_data_rodata_fixups >= 4096)
                            cc_fatal("too many data-rodata fixups");
                        g_data_rodata_fixups[g_n_data_rodata_fixups].rodata_off = rodata_off;
                        g_data_rodata_fixups[g_n_data_rodata_fixups].data_off = data_off;
                        g_n_data_rodata_fixups++;
                    } else if (d->type->kind == TY_ARRAY) {
                        /* char s[] = "hello" → copy string bytes (with NUL) into .data. */
                        size_t slen = d->init->str_len + 1; /* include NUL terminator */
                        cc_buf_push(&g_data, g_rodata.data + rodata_off, slen);
                        /* Pad to array size if specified and larger than string. */
                        if (d->type->length > 0 && !d->type->is_incomplete) {
                            uint64_t sz = type_sizeof(d->type);
                            while (slen < sz) {
                                cc_buf_push8(&g_data, 0);
                                slen++;
                            }
                        } else if (d->type->is_incomplete || d->type->length == 0) {
                            /* Incomplete array type (char s[] = "hello"): complete it
                             * from the string length so the symbol size is correct. */
                            d->type->length = slen;
                            d->type->is_incomplete = false;
                            d->type->is_complete = true;
                            d->type->size = slen;
                        }
                    } else {
                        cc_error("string initializer for non-pointer/non-array type");
                    }
                }
            } else {
                /* Uninitialized global → .bss. */
                g_globals[idx].text_off = (uint32_t)g_bss_size;
                g_bss_size += type_sizeof(d->type);
                g_bss_size = (g_bss_size + 7) & ~7;
            }
            return;
        }
        default:
            return;
    }
}

/* ---- Finalize ------------------------------------------------------- */
void gen_finalize(const char *entry_sym) {
    /* Assign virtual addresses to globals. */
    /* .text is at CC_TEXT_VADDR. .data starts after .text + .rodata. */
    uint64_t text_vaddr = CC_TEXT_VADDR;
    uint64_t rodata_vaddr = (text_vaddr + g_text.size + 7) & ~7ULL;
    uint64_t data_vaddr = (rodata_vaddr + g_rodata.size + 7) & ~7ULL;
    uint64_t bss_vaddr = (data_vaddr + g_data.size + 7) & ~7ULL;

    /* Patch all global symbol references. */
    for (int i = 0; i < g_n_globals; i++) {
        sym_t *s = &g_globals[i];
        if (s->kind == SYM_FUNCTION) {
            s->vaddr = text_vaddr + s->text_off;
        } else if (s->kind == SYM_GLOBAL_VAR) {
            /* If it had an initializer, it's in .data; otherwise .bss. */
            /* We need to know which — store a flag in is_defined. */
            if (s->is_defined) {
                s->vaddr = data_vaddr + s->text_off;
            } else {
                s->vaddr = bss_vaddr + s->text_off;
            }
        }
    }

    /* Patch all global address fixups (lui+addi sequences). */
    for (int i = 0; i < g_n_fixups; i++) {
        global_addr_fixup_t *f = &g_fixups[i];
        if (f->sym_idx < 0) continue;
        sym_t *s = &g_globals[f->sym_idx];
        uint64_t addr = s->vaddr + f->extra_off;
        /* Patch lui: imm20 = (addr + 0x800) >> 12. */
        int32_t lui_imm = (int32_t)((int64_t)(addr + 0x800) >> 12);
        int32_t lo = (int32_t)addr - (lui_imm << 12);
        uint8_t *p = g_text.data + f->patch_lui;
        uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        insn &= ~0xFFFFF000;
        insn |= ((uint32_t)lui_imm & 0xFFFFF) << 12;
        p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff; p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
        if (f->patch_addi) {
            uint8_t *q = g_text.data + f->patch_addi;
            uint32_t i2 = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            i2 &= ~0xFFF00000;
            i2 |= ((uint32_t)(lo & 0xFFF)) << 20;
            q[0] = i2 & 0xff; q[1] = (i2 >> 8) & 0xff; q[2] = (i2 >> 16) & 0xff; q[3] = (i2 >> 24) & 0xff;
        }
    }

    /* Patch rodata fixups (string literals). */
    for (int i = 0; i < g_n_rodata_fixups; i++) {
        rodata_fixup_t *f = &g_rodata_fixups[i];
        uint64_t addr = rodata_vaddr + f->rodata_off;
        int32_t lui_imm = (int32_t)((int64_t)(addr + 0x800) >> 12);
        int32_t lo = (int32_t)addr - (lui_imm << 12);
        uint8_t *p = g_text.data + f->patch_lui;
        uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        insn &= ~0xFFFFF000;
        insn |= ((uint32_t)lui_imm & 0xFFFFF) << 12;
        p[0] = insn & 0xff; p[1] = (insn >> 8) & 0xff; p[2] = (insn >> 16) & 0xff; p[3] = (insn >> 24) & 0xff;
        uint8_t *q = g_text.data + f->patch_addi;
        uint32_t i2 = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        i2 &= ~0xFFF00000;
        i2 |= ((uint32_t)(lo & 0xFFF)) << 20;
        q[0] = i2 & 0xff; q[1] = (i2 >> 8) & 0xff; q[2] = (i2 >> 16) & 0xff; q[3] = (i2 >> 24) & 0xff;
    }

    /* Patch data-rodata fixups (global string initializers, e.g. char *s = "hello"). */
    for (int i = 0; i < g_n_data_rodata_fixups; i++) {
        data_rodata_fixup_t *f = &g_data_rodata_fixups[i];
        uint64_t addr = rodata_vaddr + f->rodata_off;
        /* Write 8-byte little-endian address into g_data at data_off. */
        for (int j = 0; j < 8; j++) {
            g_data.data[f->data_off + j] = (uint8_t)(addr >> (8 * j));
        }
    }

    /* Determine entry point. */
    if (g_entry == 0) {
        int idx = symtab_lookup_global(entry_sym ? entry_sym : "_start");
        if (idx >= 0 && g_globals[idx].kind == SYM_FUNCTION) {
            g_entry = g_globals[idx].vaddr;
        } else {
            cc_warn("entry symbol '%s' not found; entry=0", entry_sym);
        }
    }

    if (g_opts.verbose) {
        fprintf(stderr, "gen_finalize: text=%zu rodata=%zu data=%zu bss=%llu entry=0x%llx\n",
                g_text.size, g_rodata.size, g_data.size,
                (unsigned long long)g_bss_size, (unsigned long long)g_entry);
    }
}
