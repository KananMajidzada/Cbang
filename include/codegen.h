#ifndef CBANG_CODEGEN_H
#define CBANG_CODEGEN_H

#include "ast.h"
#include <stdio.h>

/*
 * Code generator: walks the C! AST and writes C99 code.
 *
 * How it works:
 *   emit_program() walks top-level declarations twice:
 *     Pass 1: headers, type definitions, function prototypes
 *     Pass 2: function bodies
 *
 *   Every emit_* function prints C code for one AST node.
 *   emit_expr() takes a "parent precedence" so it knows when to
 *   add parentheses:  a + (b * c)  not  a + b * c... wait, that's
 *   the same... but (a + b) * c NEEDS parens. Precedence decides.
 */

/* Emit full C program. Returns false on failure. */
bool codegen_emit_c(ASTNodeList *program, const char *out_path);

#endif