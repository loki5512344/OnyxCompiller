/*
 * main.c — OnyxCC driver.
 *
 * Usage:
 *   onyxcc [options] input.c [-o output.onx]
 *
 * Pipeline:
 *   1. Preprocess (pp.c) → expanded source buffer
 *   2. Lex (lexer.c) → token stream
 *   3. Parse + codegen (parse.c + gen.c) → g_text, g_rodata, g_data, g_bss
 *   4. Emit .onx (emit.c)
 *
 * Options:
 *   -I <path>     add include path
 *   -D <macro>    define macro (NAME or NAME=value)
 *   -o <file>     output file (default: a.onx)
 *   -e <symbol>   entry symbol (default: _start)
 *   --ring1       emit ONX_FLAGS_RING1 (binary runs in root space)
 *   -v            verbose
 *   --dump-tokens print tokens after lex (debug)
 *   -h, --help    show usage
 */
#include "compat.h"

#include "cc.h"
#include "pp.h"
#include "lexer.h"
#include "types.h"
#include "ast.h"
#include "parse.h"
#include "gen.h"
#include "emit.h"

static void usage(void) {
    fprintf(stderr,
        "OnyxCC — OnyxOS C/C++ → RISC-V64 → .onx compiler\n"
        "\n"
        "Usage: onyxcc [options] <input.c> [-o output.onx]\n"
        "\n"
        "Options:\n"
        "  -I, --include <path>  Add include search path\n"
        "  -D, --define  <name>  Predefine macro (NAME or NAME=value)\n"
        "  -o, --output  <file>  Output file (default: a.onx)\n"
        "  -e, --entry   <sym>   Entry symbol (default: _start)\n"
        "      --ring1            Emit RING1 flag (binary runs in root space)\n"
        "  -v, --verbose         Verbose diagnostics\n"
        "      --dump-tokens      Print token stream after lex\n"
        "  -h, --help            Show this help\n"
        "\n"
        "Environment:\n"
        "  ONYXCC_INCLUDE  Default include path (e.g. /usr/onyxc/include)\n"
        "\n");
}

static struct option long_opts[] = {
    {"include",    required_argument, 0, 'I'},
    {"define",     required_argument, 0, 'D'},
    {"output",     required_argument, 0, 'o'},
    {"entry",      required_argument, 0, 'e'},
    {"ring1",      no_argument,       0, 'R'},
    {"verbose",    no_argument,       0, 'v'},
    {"dump-tokens",no_argument,       0, 'T'},
    {"help",       no_argument,       0, 'h'},
    {0, 0, 0, 0},
};

int main(int argc, char **argv) {
    memset(&g_opts, 0, sizeof(g_opts));
    g_opts.entry_sym = "_start";
    g_opts.output = "a.onx";

    /* Default include path from env. */
    const char *env_inc = getenv("ONYXCC_INCLUDE");
    if (env_inc && g_opts.n_include_paths < 16) {
        g_opts.include_paths[g_opts.n_include_paths++] = env_inc;
    }

    int c;
    int opt_idx = 0;
    while ((c = getopt_long(argc, argv, "I:D:o:e:vh", long_opts, &opt_idx)) != -1) {
        switch (c) {
            case 'I':
                if (g_opts.n_include_paths < 16)
                    g_opts.include_paths[g_opts.n_include_paths++] = optarg;
                break;
            case 'D':
                if (g_opts.n_define_macros < 64)
                    g_opts.define_macros[g_opts.n_define_macros++] = optarg;
                break;
            case 'o': g_opts.output = optarg; break;
            case 'e': g_opts.entry_sym = optarg; break;
            case 'R': g_opts.ring1 = true; break;
            case 'v': g_opts.verbose = true; break;
            case 'T': g_opts.dump_tokens = true; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "onyxcc: no input file\n");
        usage();
        return 1;
    }
    g_opts.input = argv[optind];

    if (g_opts.verbose) {
        fprintf(stderr, "onyxcc: input=%s output=%s entry=%s ring=%d\n",
                g_opts.input, g_opts.output, g_opts.entry_sym, g_opts.ring1 ? 1 : 2);
    }

    /* 1. Preprocess. */
    size_t pp_len = 0;
    char *pp_src = pp_preprocess_file(g_opts.input,
                                      g_opts.include_paths, g_opts.n_include_paths,
                                      g_opts.define_macros, g_opts.n_define_macros,
                                      &pp_len);
    if (!pp_src) {
        return 1;
    }
    if (g_opts.verbose) {
        fprintf(stderr, "onyxcc: preprocessed %zu bytes\n", pp_len);
    }

    /* 2. Init arenas, types, symbol table. */
    cc_arena_init(&g_ast_arena, 4 * 1024 * 1024);   /* 4 MiB for AST */
    cc_arena_init(&g_type_arena, 1 * 1024 * 1024);  /* 1 MiB for types */
    types_init(&g_type_arena);
    tags_init();
    symtab_init();

    /* 3. Lex. */
    lexer_t lx;
    lex_init(&lx, pp_src, pp_len, g_opts.input);

    if (g_opts.dump_tokens) {
        while (lx.cur.kind != T_EOF) {
            printf("%s:%d: %s '%s'\n",
                   lx.cur.pos.file ? lx.cur.pos.file : "?",
                   lx.cur.pos.line,
                   tok_kind_str(lx.cur.kind),
                   lx.cur.text);
            lex_next(&lx);
        }
        free(pp_src);
        return 0;
    }

    /* 4. Init codegen + parse. */
    gen_init();
    parse_translation_unit(&lx);

    /* 5. Finalize (resolve addresses). */
    gen_finalize(g_opts.entry_sym);

    /* 6. Emit .onx. */
    int rc = onx_emit(g_opts.output, &g_text, &g_rodata, &g_data,
                      g_bss_size, g_entry, g_opts.ring1);

    /* Cleanup. */
    free(pp_src);
    cc_arena_free(&g_ast_arena);
    cc_arena_free(&g_type_arena);
    cc_buf_free(&g_text);
    cc_buf_free(&g_rodata);
    cc_buf_free(&g_data);

    if (cc_get_errors() > 0) {
        fprintf(stderr, "onyxcc: %d error(s), %d warning(s)\n",
                cc_get_errors(), cc_get_warnings());
        return 1;
    }
    return rc;
}
