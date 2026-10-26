#include "codegen.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>    /* LLONG_MAX: decide when an int literal needs a ULL suffix */

/* ========================================================================== */
/* Generator State                                                            */
/* ========================================================================== */

typedef struct {
    FILE *out;
    int indent;
    bool needs_io;      /* io.print actually used -> stdio.h */
    bool needs_os;      /* saw use std.os -> stdlib.h for exit() */
    bool in_function;   /* to know if defer is valid */

    /* Which print helpers the program uses. Set by scan_prints() before
       anything is emitted, so we only write helpers that get called —
       an unused static helper would be a -Wunused-function warning. */
    bool need_str;
    bool need_sint;
    bool need_uint;
    bool need_float;
    bool need_bool;

    /* std.math needs */
    bool needs_math;    /* <math.h>: sqrt, pow, floor, ceil, round, fabs */
    bool needs_llabs;   /* <stdlib.h>: math.abs on signed ints */
    bool need_minmax;   /* our own cb_min/cb_max helpers, evaluated once */

    /*
     * Defer lowering machinery.
     * Before emitting a block, we pre-scan its direct children for defer
     * statements and push their bodies onto this stack (registration order).
     *   - At block end: emit this block's defers in REVERSE (LIFO).
     *   - At `return`:  emit ALL stacked defers in REVERSE, then return.
     * This gives Go/Zig-like cleanup semantics in plain C.
     */
    ASTNode *defer_stack[64];
    int defer_sp;
    bool in_defer_emit; /* guard: prevent infinite recursion if defer has return */
} Gen;

static void pad(Gen *g) {
    for (int i = 0; i < g->indent; i++) fputs("    ", g->out);
}

/* ========================================================================== */
/* Type Mapping: C! types -> C types                                          */
/* ========================================================================== */

static void emit_type(Gen *g, Type *t) {
    if (!t) { fputs("void", g->out); return; }
    switch (t->kind) {
        case TYPE_PRIMITIVE:
            switch (t->data.primitive) {
                case KW_I8: fputs("int8_t", g->out); break;
                case KW_I16: fputs("int16_t", g->out); break;
                case KW_I32: fputs("int32_t", g->out); break;
                case KW_I64: fputs("int64_t", g->out); break;
                case KW_U8: fputs("uint8_t", g->out); break;
                case KW_U16: fputs("uint16_t", g->out); break;
                case KW_U32: fputs("uint32_t", g->out); break;
                case KW_U64: fputs("uint64_t", g->out); break;
                case KW_F32: fputs("float", g->out); break;
                case KW_F64: fputs("double", g->out); break;
                case KW_BOOL: fputs("bool", g->out); break;
                case KW_STR: fputs("cb_str", g->out); break;
                case KW_VOID: fputs("void", g->out); break;
                case KW_UINTPTR: fputs("uintptr_t", g->out); break;
                default: fputs("/*?*/", g->out); break;
            }
            break;
        case TYPE_POINTER:
            emit_type(g, t->data.pointer.pointee);
            fputc('*', g->out);
            break;
        case TYPE_NAMED:
            fprintf(g->out, "%.*s", (int)t->data.named.len, t->data.named.name);
            break;
    }
}

/* ========================================================================== */
/* Expressions                                                                */
/* ========================================================================== */

static void emit_expr(Gen *g, ASTNode *n);

/* Map C! operator token to C spelling */
static const char *op_spelling(TokenType t) {
    switch (t) {
        case OP_ADD: return "+"; case OP_SUB: return "-";
        case OP_MUL: return "*"; case OP_DIV: return "/"; case OP_MOD: return "%";
        case OP_EQ: return "=="; case OP_NE: return "!=";
        case OP_LT: return "<"; case OP_GT: return ">";
        case OP_LE: return "<="; case OP_GE: return ">=";
        case OP_AND: return "&&"; case OP_OR: return "||"; case OP_NOT: return "!";
        case OP_BIT_AND: return "&"; case OP_BIT_OR: return "|";
        case OP_BIT_XOR: return "^"; case OP_BIT_NOT: return "~";
        case OP_SHL: return "<<"; case OP_SHR: return ">>";
        case OP_ASSIGN: return "=";
        default: return "?";
    }
}

static void emit_expr(Gen *g, ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_INT_LIT: {
            /* Above LLONG_MAX a bare decimal constant isn't valid C — the C
               compiler would reject it. The ULL suffix makes it a proper
               unsigned constant that fits u64. */
            unsigned long long v = n->data.int_lit.value;
            if (v > (unsigned long long)LLONG_MAX)
                fprintf(g->out, "%lluULL", v);
            else
                fprintf(g->out, "%llu", v);
            break;
        }
        case AST_FLOAT_LIT:
            fprintf(g->out, "%g", n->data.float_lit.value);
            break;
        case AST_STRING_LIT: {
            /* Lexer token starts AFTER opening quote, includes closing quote.
               So content = start[0 .. len-2], raw text with quotes = need to add opening ".
               Emit: (cb_str){ (const uint8_t*)"CONTENT", sizeof("CONTENT") - 1 } */
            const char *raw = n->data.string_lit.value;
            int content_len = (int)n->data.string_lit.length - 1; /* strip closing quote */
            fputs("(cb_str){ (const uint8_t*)\"", g->out);
            fwrite(raw, 1, content_len, g->out);
            fputs("\", sizeof(\"", g->out);
            fwrite(raw, 1, content_len, g->out);
            fputs("\") - 1 }", g->out);
            break;
        }
        case AST_CHAR_LIT:
            fprintf(g->out, "'\\x%02x'", (unsigned char)n->data.char_lit.value);
            break;
        case AST_BOOL_LIT:
            fputs(n->data.bool_lit.value ? "true" : "false", g->out);
            break;
        case AST_VAR_REF:
            fprintf(g->out, "%.*s", (int)n->data.var_ref.name.length,
                    n->data.var_ref.name.start);
            break;
        case AST_BINARY:
            fputc('(', g->out);
            emit_expr(g, n->data.binary.left);
            fprintf(g->out, " %s ", op_spelling(n->data.binary.op));
            emit_expr(g, n->data.binary.right);
            fputc(')', g->out);
            break;
        case AST_UNARY:
            fputc('(', g->out);
            switch (n->data.unary.op) {
                case OP_SUB: fputc('-', g->out); break;
                case OP_NOT: fputc('!', g->out); break;
                case OP_BIT_NOT: fputc('~', g->out); break;
                case OP_DEREF: fputc('*', g->out); break;
                case OP_ADDR_OF: fputc('&', g->out); break;
                default: break;
            }
            emit_expr(g, n->data.unary.operand);
            fputc(')', g->out);
            break;
        case AST_CALL:
            /* Special-case module calls: io.print(str), os.exit(code) */
            if (n->data.call.callee->type == AST_FIELD) {
                ASTNode *base = n->data.call.callee->data.field.base;
                if (base->type == AST_VAR_REF) {
                    const char *mod = base->data.var_ref.name.start;
                    size_t mlen = base->data.var_ref.name.length;
                    const char *fn = n->data.call.callee->data.field.field_name.start;
                    size_t fnlen = n->data.call.callee->data.field.field_name.length;
                    if (mlen == 2 && memcmp(mod, "io", 2) == 0 &&
                        fnlen == 5 && memcmp(fn, "print", 5) == 0) {
                        /* sema wrote print_kind on the node - codegen has no
                           type information of its own, so trust the note. */
                        const char *helper;
                        switch (n->data.call.print_kind) {
                            case PRINT_UINT:  helper = "cb_print_u64";  break;
                            case PRINT_FLOAT: helper = "cb_print_f64";  break;
                            case PRINT_BOOL:  helper = "cb_print_bool"; break;
                            case PRINT_SINT:  helper = "cb_print_i64";  break;
                            default:          helper = "cb_print";      break;
                        }
                        fputs(helper, g->out);
                        fputc('(', g->out);
                        if (n->data.call.args->count > 0)
                            emit_expr(g, n->data.call.args->items[0]);
                        fputc(')', g->out);
                        break;
                    }
                    if (mlen == 2 && memcmp(mod, "os", 2) == 0 &&
                        fnlen == 4 && memcmp(fn, "exit", 4) == 0) {
                        g->needs_os = true;
                        fputs("exit(", g->out);
                        if (n->data.call.args->count > 0)
                            emit_expr(g, n->data.call.args->items[0]);
                        fputc(')', g->out);
                        break;
                    }
                    if (mlen == 4 && memcmp(mod, "math", 4) == 0) {
                        /* sema chose the exact helper (sema.c: sema_math).
                           min/max go through a function so each argument is
                           evaluated once, never twice as a ternary would. */
                        static const char *names[] = {
                            NULL,                      /* MATH_NONE  */
                            "llabs",                   /* ABS_INT    */
                            NULL,                      /* ABS_UINT   */
                            "fabs",                    /* ABS_FLT    */
                            "sqrt", "pow", "floor", "ceil", "round",
                            "cb_min_i64", "cb_max_i64",
                            "cb_min_u64", "cb_max_u64",
                            "cb_min_f64", "cb_max_f64",
                        };
                        int k = n->data.call.math_kind;
                        const char *name = (k >= 0 && k <= MATH_MAX_FLT)
                                           ? names[k] : NULL;
                        if (!name) {
                            /* ABS_UINT: abs of an unsigned value is itself */
                            if (n->data.call.args->count > 0)
                                emit_expr(g, n->data.call.args->items[0]);
                            break;
                        }
                        fputs(name, g->out);
                        if (k == MATH_ABS_INT) fputs("((long long)", g->out);
                        fputc('(', g->out);
                        for (size_t i = 0; i < n->data.call.args->count; i++) {
                            if (i) fputs(", ", g->out);
                            emit_expr(g, n->data.call.args->items[i]);
                        }
                        fputc(')', g->out);
                        if (k == MATH_ABS_INT) fputc(')', g->out);
                        break;
                    }
                }
            }
            emit_expr(g, n->data.call.callee);
            fputc('(', g->out);
            for (size_t i = 0; i < n->data.call.args->count; i++) {
                if (i) fputs(", ", g->out);
                emit_expr(g, n->data.call.args->items[i]);
            }
            fputc(')', g->out);
            break;
        case AST_INDEX:
            emit_expr(g, n->data.index.base);
            /* str is a {ptr,len} struct, not an array — index its ptr */
            if (n->data.index.base_is_str) fputs(".ptr", g->out);
            fputc('[', g->out);
            emit_expr(g, n->data.index.index);
            fputc(']', g->out);
            break;
        case AST_FIELD:
            /* str.ptr is `const uint8_t*` in C but C! has no const-pointer
               type, so cast away const explicitly (assignment target is *u8). */
            if (n->data.field.on_str &&
                n->data.field.field_name.length == 3 &&
                memcmp(n->data.field.field_name.start, "ptr", 3) == 0)
                fputs("(uint8_t*)", g->out);
            emit_expr(g, n->data.field.base);
            if (n->token.type == OP_ARROW)
                fputs("->", g->out);
            else
                fputc('.', g->out);
            fprintf(g->out, "%.*s", (int)n->data.field.field_name.length,
                    n->data.field.field_name.start);
            break;
        case AST_CAST:
            fputc('(', g->out);
            emit_type(g, n->data.cast.target_type);
            fputc(')', g->out);
            emit_expr(g, n->data.cast.expr);
            break;
        case AST_SIZEOF:
            fputs("sizeof(", g->out);
            emit_type(g, n->data.sizeof_alignof.target_type);
            fputc(')', g->out);
            break;
        case AST_ALIGNOF:
            fputs("_Alignof(", g->out);
            emit_type(g, n->data.sizeof_alignof.target_type);
            fputc(')', g->out);
            break;
        default:
            fputs("/*unhandled expr*/", g->out);
    }
}

/* ========================================================================== */
/* Statements                                                                 */
/* ========================================================================== */

static void emit_stmt(Gen *g, ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL: {
            pad(g);
            emit_type(g, n->data.var_decl.var_type);
            fprintf(g->out, " %.*s",
                    (int)n->data.var_decl.name.length,
                    n->data.var_decl.name.start);
            if (n->data.var_decl.init) {
                fputs(" = ", g->out);
                emit_expr(g, n->data.var_decl.init);
            }
            fputs(";\n", g->out);
            break;
        }
        case AST_CONST_DECL: {
            pad(g);
            fputs("const ", g->out);
            if (n->data.var_decl.var_type)
                emit_type(g, n->data.var_decl.var_type);
            else
                fputs("int64_t", g->out); /* untyped const -> i64 default */
            fprintf(g->out, " %.*s = ",
                    (int)n->data.var_decl.name.length,
                    n->data.var_decl.name.start);
            emit_expr(g, n->data.var_decl.init);
            fputs(";\n", g->out);
            break;
        }
        case AST_BLOCK: {
            pad(g); fputs("{\n", g->out);
            g->indent++;

            /* Pre-scan: register this block's defers (source order) */
            int defer_mark = g->defer_sp;
            ASTNodeList *stmts = n->data.block.stmts;
            for (size_t i = 0; i < stmts->count; i++) {
                ASTNode *s = stmts->items[i];
                if (s->type == AST_DEFER_STMT && g->defer_sp < 64)
                    g->defer_stack[g->defer_sp++] = s->data.defer_stmt.body;
            }

            /* Emit statements, skipping defer nodes (handled below) */
            for (size_t i = 0; i < stmts->count; i++) {
                ASTNode *s = stmts->items[i];
                if (s->type == AST_DEFER_STMT) continue;
                emit_stmt(g, s);
            }

            /* Block exit: run this block's defers in reverse (LIFO) */
            for (int i = g->defer_sp; i > defer_mark; i--) {
                bool saved = g->in_defer_emit;
                g->in_defer_emit = true;
                emit_stmt(g, g->defer_stack[i - 1]);
                g->in_defer_emit = saved;
            }
            g->defer_sp = defer_mark;

            g->indent--;
            pad(g); fputs("}\n", g->out);
            break;
        }
        case AST_IF_STMT:
            pad(g); fputs("if (", g->out);
            emit_expr(g, n->data.if_stmt.cond);
            fputs(")\n", g->out);
            emit_stmt(g, n->data.if_stmt.then_branch);
            if (n->data.if_stmt.else_branch) {
                pad(g); fputs("else\n", g->out);
                emit_stmt(g, n->data.if_stmt.else_branch);
            }
            break;
        case AST_FOR_STMT:
            pad(g); fputs("for (", g->out);
            if (n->data.for_stmt.init) {
                /* emit init without own indent/semicolon handling */
                if (n->data.for_stmt.init->type == AST_VAR_DECL) {
                    ASTNode *v = n->data.for_stmt.init;
                    emit_type(g, v->data.var_decl.var_type);
                    fprintf(g->out, " %.*s",
                            (int)v->data.var_decl.name.length,
                            v->data.var_decl.name.start);
                    if (v->data.var_decl.init) {
                        fputs(" = ", g->out);
                        emit_expr(g, v->data.var_decl.init);
                    }
                } else if (n->data.for_stmt.init->type == AST_EXPR_STMT) {
                    emit_expr(g, n->data.for_stmt.init->data.expr_stmt.expr);
                }
            }
            fputs("; ", g->out);
            if (n->data.for_stmt.cond) emit_expr(g, n->data.for_stmt.cond);
            fputs("; ", g->out);
            if (n->data.for_stmt.step) emit_expr(g, n->data.for_stmt.step);
            fputs(")\n", g->out);
            emit_stmt(g, n->data.for_stmt.body);
            break;
        case AST_WHILE_STMT:
            pad(g); fputs("while (", g->out);
            emit_expr(g, n->data.while_stmt.cond);
            fputs(")\n", g->out);
            emit_stmt(g, n->data.while_stmt.body);
            break;
        case AST_SWITCH_STMT:
            pad(g); fputs("switch (", g->out);
            emit_expr(g, n->data.switch_stmt.expr);
            fputs(") {\n", g->out);
            g->indent++;
            for (size_t i = 0; i < n->data.switch_stmt.cases->count; i++)
                emit_stmt(g, n->data.switch_stmt.cases->items[i]);
            g->indent--;
            pad(g); fputs("}\n", g->out);
            break;
        case AST_CASE_STMT:
            if (n->data.case_stmt.expr) {
                pad(g); fputs("case ", g->out);
                emit_expr(g, n->data.case_stmt.expr);
                fputs(":\n", g->out);
            } else {
                pad(g); fputs("default:\n", g->out);
            }
            /* case body is a block - emit inner statements only */
            if (n->data.case_stmt.body &&
                n->data.case_stmt.body->type == AST_BLOCK) {
                ASTNodeList *stmts = n->data.case_stmt.body->data.block.stmts;
                for (size_t i = 0; i < stmts->count; i++)
                    emit_stmt(g, stmts->items[i]);
            }
            break;
        case AST_RETURN_STMT:
            /* Run ALL pending defers (innermost first, each LIFO) before leaving */
            if (!g->in_defer_emit && g->defer_sp > 0) {
                bool saved = g->in_defer_emit;
                g->in_defer_emit = true;
                for (int i = g->defer_sp; i > 0; i--)
                    emit_stmt(g, g->defer_stack[i - 1]);
                g->in_defer_emit = saved;
            }
            pad(g);
            if (n->data.return_stmt.value) {
                fputs("return ", g->out);
                emit_expr(g, n->data.return_stmt.value);
                fputs(";\n", g->out);
            } else {
                fputs("return;\n", g->out);
            }
            break;
        case AST_BREAK_STMT: case AST_CONTINUE_STMT: pad(g);
            fputs(n->type == AST_BREAK_STMT ? "break;\n" : "continue;\n", g->out);
            break;
        case AST_DEFER_STMT:
            /* Defer nodes are registered by their enclosing block's pre-scan
               and emitted at block exit. If we see one here (shouldn't), skip. */
            break;
        case AST_EXPR_STMT:
            pad(g);
            emit_expr(g, n->data.expr_stmt.expr);
            fputs(";\n", g->out);
            break;
        default:
            pad(g); fputs("/*unhandled stmt*/\n", g->out);
    }
}

/* ========================================================================== */
/* Declarations                                                               */
/* ========================================================================== */

/* A function gets C's `static` (file-local) unless it's pub — except `main`,
   which must stay extern or the linker won't find the entry point. */
static bool fn_is_static(ASTNode *n) {
    /* extern functions are defined in another translation unit or library —
       making them static would make them unresolvable at link time. */
    if (n->data.fn_decl.is_extern) return false;
    if (n->data.fn_decl.is_pub) return false;
    Token *name = &n->data.fn_decl.name;
    if (name->length == 4 && memcmp(name->start, "main", 4) == 0) return false;
    return true;
}

static void emit_fn_decl(Gen *g, ASTNode *n) {
    /* Prototype: [static] ret name(params); */
    if (fn_is_static(n)) fputs("static ", g->out);
    emit_type(g, n->data.fn_decl.ret_type);
    fprintf(g->out, " %.*s(",
            (int)n->data.fn_decl.name.length,
            n->data.fn_decl.name.start);
    ASTNodeList *params = n->data.fn_decl.params;
    if (params->count == 0) fputs("void", g->out);
    for (size_t i = 0; i < params->count; i++) {
        if (i) fputs(", ", g->out);
        ASTNode *p = params->items[i];
        emit_type(g, p->data.var_decl.var_type);
        fprintf(g->out, " %.*s",
                (int)p->data.var_decl.name.length,
                p->data.var_decl.name.start);
    }
    fputs(");\n", g->out);

    /* Definition (skip extern) */
    if (n->data.fn_decl.is_extern || !n->data.fn_decl.body) return;

    if (fn_is_static(n)) fputs("static ", g->out);
    emit_type(g, n->data.fn_decl.ret_type);
    fprintf(g->out, " %.*s(",
            (int)n->data.fn_decl.name.length,
            n->data.fn_decl.name.start);
    for (size_t i = 0; i < params->count; i++) {
        if (i) fputs(", ", g->out);
        ASTNode *p = params->items[i];
        emit_type(g, p->data.var_decl.var_type);
        fprintf(g->out, " %.*s",
                (int)p->data.var_decl.name.length,
                p->data.var_decl.name.start);
    }
    fputs(")\n", g->out);

    g->in_function = true;
    emit_stmt(g, n->data.fn_decl.body);
    g->in_function = false;
}

static void emit_top_level(Gen *g, ASTNode *n) {
    switch (n->type) {
        case AST_FN_DECL:
            emit_fn_decl(g, n);
            fputc('\n', g->out);
            break;
        case AST_STRUCT_DECL: {
            fprintf(g->out, "typedef struct %.*s {\n",
                    (int)n->data.aggregate_decl.name.length,
                    n->data.aggregate_decl.name.start);
            for (size_t i = 0; i < n->data.aggregate_decl.fields->count; i++) {
                ASTNode *f = n->data.aggregate_decl.fields->items[i];
                fputs("    ", g->out);
                emit_type(g, f->data.var_decl.var_type);
                fprintf(g->out, " %.*s;\n",
                        (int)f->data.var_decl.name.length,
                        f->data.var_decl.name.start);
            }
            fprintf(g->out, "} %.*s;\n\n",
                    (int)n->data.aggregate_decl.name.length,
                    n->data.aggregate_decl.name.start);
            break;
        }
        case AST_UNION_DECL: {
            fprintf(g->out, "typedef union %.*s {\n",
                    (int)n->data.aggregate_decl.name.length,
                    n->data.aggregate_decl.name.start);
            for (size_t i = 0; i < n->data.aggregate_decl.fields->count; i++) {
                ASTNode *f = n->data.aggregate_decl.fields->items[i];
                fputs("    ", g->out);
                emit_type(g, f->data.var_decl.var_type);
                fprintf(g->out, " %.*s;\n",
                        (int)f->data.var_decl.name.length,
                        f->data.var_decl.name.start);
            }
            fprintf(g->out, "} %.*s;\n\n",
                    (int)n->data.aggregate_decl.name.length,
                    n->data.aggregate_decl.name.start);
            break;
        }
        case AST_ENUM_DECL: {
            fprintf(g->out, "typedef enum %.*s {\n",
                    (int)n->data.enum_decl.name.length,
                    n->data.enum_decl.name.start);
            for (size_t i = 0; i < n->data.enum_decl.variants->count; i++) {
                ASTNode *v = n->data.enum_decl.variants->items[i];
                fputs("    ", g->out);
                if (v->type == AST_BINARY) {
                    fprintf(g->out, "%.*s = ",
                            (int)v->data.binary.left->data.var_ref.name.length,
                            v->data.binary.left->data.var_ref.name.start);
                    emit_expr(g, v->data.binary.right);
                } else {
                    fprintf(g->out, "%.*s",
                            (int)v->data.var_ref.name.length,
                            v->data.var_ref.name.start);
                }
                fputs(",", g->out);
                if (i + 1 < n->data.enum_decl.variants->count) fputc('\n', g->out);
            }
            fprintf(g->out, "\n} %.*s;\n\n",
                    (int)n->data.enum_decl.name.length,
                    n->data.enum_decl.name.start);
            break;
        }
        case AST_TYPE_ALIAS:
            fputs("typedef ", g->out);
            emit_type(g, n->data.type_alias.aliased_type);
            fprintf(g->out, " %.*s;\n\n",
                    (int)n->data.type_alias.name.length,
                    n->data.type_alias.name.start);
            break;
        case AST_CONST_DECL:
            fputs("static const ", g->out);
            if (n->data.var_decl.var_type)
                emit_type(g, n->data.var_decl.var_type);
            else
                fputs("int64_t", g->out);
            fprintf(g->out, " %.*s = ",
                    (int)n->data.var_decl.name.length,
                    n->data.var_decl.name.start);
            emit_expr(g, n->data.var_decl.init);
            fputs(";\n\n", g->out);
            break;
        case AST_VAR_DECL:
            fputs("static ", g->out);
            emit_type(g, n->data.var_decl.var_type);
            fprintf(g->out, " %.*s",
                    (int)n->data.var_decl.name.length,
                    n->data.var_decl.name.start);
            if (n->data.var_decl.init) {
                fputs(" = ", g->out);
                emit_expr(g, n->data.var_decl.init);
            }
            fputs(";\n\n", g->out);
            break;
        case AST_USE_DECL:
            /* handled via needs_io flag; nothing to emit */
            break;
        default:
            fputs("/*unhandled top-level*/\n\n", g->out);
    }
}

/* ========================================================================== */
/* Program Entry                                                              */
/* ========================================================================== */

/* ========================================================================== */
/* Helper Usage Pre-scan                                                      */
/* ========================================================================== */

/*
 * The runtime helpers are written ABOVE the function bodies that call them,
 * so we must know which ones are needed before emitting anything: a helper
 * that is declared but never called is a -Wunused-function warning, which
 * would break the zero-warning guarantee.
 *
 * sema annotated every io.print with its PRINT_* kind, so this walk only
 * tallies them. It also decides whether <stdio.h> is needed at all --
 * `use std.io` with no io.print no longer drags in dead code.
 */
static void note_print(Gen *g, int kind) {
    bool *slot[] = { NULL, &g->need_str, &g->need_sint, &g->need_uint,
                     &g->need_float, &g->need_bool };
    if (kind > PRINT_NONE && kind <= PRINT_BOOL) {
        *slot[kind] = true;
        g->needs_io = true;         /* any print needs <stdio.h> */
    }
}

/* Which std.math pieces the program touches: headers and helpers are
   emitted above the bodies, so this has to be known first. */
static void note_math(Gen *g, int kind) {
    if (kind <= MATH_NONE || kind > MATH_MAX_FLT) return;
    if (kind >= MATH_MIN_INT) {          /* the 6 min/max helpers */
        g->need_minmax = true;
        return;
    }
    switch (kind) {
        case MATH_ABS_INT:  g->needs_llabs = true; break;
        case MATH_ABS_UINT: break;                  /* identity, no helper */
        default:            g->needs_math  = true; break;
    }
}

/* Reported to main.c so it knows to link -lm. */
static int g_used_libm;
int codegen_used_libm(void) { return g_used_libm; }

static void scan_node(Gen *g, ASTNode *n);
static void scan_list(Gen *g, ASTNodeList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++)
        scan_node(g, l->items[i]);
}

static void scan_node(Gen *g, ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_UNARY:   scan_node(g, n->data.unary.operand); break;
        case AST_BINARY:  scan_node(g, n->data.binary.left);
                          scan_node(g, n->data.binary.right); break;
        case AST_CALL:    note_print(g, n->data.call.print_kind);
                          note_math(g, n->data.call.math_kind);
                          scan_node(g, n->data.call.callee);
                          scan_list(g, n->data.call.args); break;
        case AST_INDEX:   scan_node(g, n->data.index.base);
                          scan_node(g, n->data.index.index); break;
        case AST_FIELD:   scan_node(g, n->data.field.base); break;
        case AST_CAST:    scan_node(g, n->data.cast.expr); break;
        case AST_VAR_DECL: case AST_CONST_DECL:
                          scan_node(g, n->data.var_decl.init); break;
        case AST_FN_DECL: scan_node(g, n->data.fn_decl.body); break;
        case AST_STRUCT_DECL: case AST_UNION_DECL:
                          scan_list(g, n->data.aggregate_decl.fields); break;
        case AST_ENUM_DECL:
                          scan_list(g, n->data.enum_decl.variants); break;
        case AST_EXTERN_DECL:
                          scan_node(g, n->data.extern_decl.decl); break;
        case AST_BLOCK:   scan_list(g, n->data.block.stmts); break;
        case AST_IF_STMT: scan_node(g, n->data.if_stmt.cond);
                          scan_node(g, n->data.if_stmt.then_branch);
                          scan_node(g, n->data.if_stmt.else_branch); break;
        case AST_FOR_STMT: scan_node(g, n->data.for_stmt.init);
                          scan_node(g, n->data.for_stmt.cond);
                          scan_node(g, n->data.for_stmt.step);
                          scan_node(g, n->data.for_stmt.body); break;
        case AST_WHILE_STMT: scan_node(g, n->data.while_stmt.cond);
                          scan_node(g, n->data.while_stmt.body); break;
        case AST_SWITCH_STMT: scan_node(g, n->data.switch_stmt.expr);
                          scan_list(g, n->data.switch_stmt.cases); break;
        case AST_CASE_STMT: scan_node(g, n->data.case_stmt.expr);
                          scan_node(g, n->data.case_stmt.body); break;
        case AST_RETURN_STMT: scan_node(g, n->data.return_stmt.value); break;
        case AST_DEFER_STMT: scan_node(g, n->data.defer_stmt.body); break;
        case AST_EXPR_STMT: scan_node(g, n->data.expr_stmt.expr); break;
        /* leaves: literals, var refs, and the type-only nodes */
        default: break;
    }
}

bool codegen_emit_c(ASTNodeList *program, const char *out_path) {
    FILE *out = fopen(out_path, "w");
    if (!out) { perror(out_path); return false; }

    Gen g = { .out = out, .indent = 0, .needs_io = false, .needs_os = false,
              .in_function = false, .defer_sp = 0, .in_defer_emit = false };

    /* Pass 1: `use std.os` decides <stdlib.h>. io is decided by usage:
       see scan_prints() below -- `use std.io` alone means nothing. */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type == AST_USE_DECL) {
            const char *p = n->data.use_decl.path.start;
            size_t len = n->data.use_decl.path.length;
            if (len == 6 && memcmp(p, "std.os", 6) == 0) g.needs_os = true;
        }
    }

    /* Find every io.print / math.* call before writing anything, so we emit
       exactly the helpers that get called and no more. */
    for (size_t i = 0; i < program->count; i++)
        scan_node(&g, program->items[i]);
    g_used_libm = g.needs_math;     /* tell the driver to link -lm */

    fputs("/* Generated by cbang - C! (C-Bang) compiler */\n", out);
    fputs("#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n", out);
    if (g.needs_io) fputs("#include <stdio.h>\n", out);
    if (g.needs_os || g.needs_llabs) fputs("#include <stdlib.h>\n", out);
    if (g.needs_math) fputs("#include <math.h>\n", out);
    fputs("\n", out);

    /* cb_str runtime: C! str = {ptr,len} slice (spec section 2) */
    fputs("typedef struct { const uint8_t *ptr; uint64_t len; } cb_str;\n\n", out);

    /* Print helpers -- only the ones the program actually uses.
       Each takes the widest form so C promotes the argument for us. */
    if (g.need_str) {
        fputs("static void cb_print(cb_str s) {\n"
              "    fwrite(s.ptr, 1, s.len, stdout);\n"
              "}\n\n", out);
    }
    if (g.need_sint) {
        fputs("static void cb_print_i64(int64_t v) {\n"
              "    fprintf(stdout, \"%lld\", (long long)v);\n"
              "}\n\n", out);
    }
    if (g.need_uint) {
        fputs("static void cb_print_u64(uint64_t v) {\n"
              "    fprintf(stdout, \"%llu\", (unsigned long long)v);\n"
              "}\n\n", out);
    }
    if (g.need_float) {
        fputs("static void cb_print_f64(double v) {\n"
              "    fprintf(stdout, \"%g\", v);\n"
              "}\n\n", out);
    }
    if (g.need_bool) {
        fputs("static void cb_print_bool(bool v) {\n"
              "    fputs(v ? \"true\" : \"false\", stdout);\n"
              "}\n\n", out);
    }

    /* math.min/max as functions, not (a<b?a:b) expressions: a function
       evaluates each argument exactly once, a ternary would evaluate
       them twice if the argument has side effects (math.min(i++, 5)). */
    if (g.need_minmax) {
        fputs("static inline int64_t cb_min_i64(int64_t a, int64_t b) { return a < b ? a : b; }\n"
              "static inline int64_t cb_max_i64(int64_t a, int64_t b) { return a > b ? a : b; }\n"
              "static inline uint64_t cb_min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }\n"
              "static inline uint64_t cb_max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }\n"
              "static inline double cb_min_f64(double a, double b) { return a < b ? a : b; }\n"
              "static inline double cb_max_f64(double a, double b) { return a > b ? a : b; }\n\n", out);
    }

    /* Pass 2: type definitions FIRST (structs/enums/aliases/globals),
       so function prototypes can reference them */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        switch (n->type) {
            case AST_STRUCT_DECL: case AST_UNION_DECL: case AST_ENUM_DECL:
            case AST_TYPE_ALIAS: case AST_CONST_DECL: case AST_VAR_DECL:
                emit_top_level(&g, n);
                break;
            default: break;
        }
    }

    /* Pass 3: forward-declare all functions so call order doesn't matter */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        if (n->type == AST_FN_DECL) {
            if (fn_is_static(n)) fputs("static ", out);
            emit_type(&g, n->data.fn_decl.ret_type);
            fprintf(out, " %.*s(",
                    (int)n->data.fn_decl.name.length,
                    n->data.fn_decl.name.start);
            ASTNodeList *params = n->data.fn_decl.params;
            if (params->count == 0) fputs("void", out);
            for (size_t j = 0; j < params->count; j++) {
                if (j) fputs(", ", out);
                emit_type(&g, params->items[j]->data.var_decl.var_type);
                fputc(' ', out);
                fprintf(out, "%.*s",
                        (int)params->items[j]->data.var_decl.name.length,
                        params->items[j]->data.var_decl.name.start);
            }
            fputs(");\n", out);
        }
    }
    fputs("\n", out);

    /* Pass 4: function bodies + remaining top-level items */
    for (size_t i = 0; i < program->count; i++) {
        ASTNode *n = program->items[i];
        switch (n->type) {
            case AST_STRUCT_DECL: case AST_UNION_DECL: case AST_ENUM_DECL:
            case AST_TYPE_ALIAS: case AST_CONST_DECL: case AST_VAR_DECL:
                break; /* already emitted in pass 2 */
            default:
                emit_top_level(&g, n);
        }
    }

    fclose(out);
    return true;
}