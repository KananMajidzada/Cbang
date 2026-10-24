#ifndef CBANG_SEMA_H
#define CBANG_SEMA_H

#include "ast.h"

/*
 * Semantic analysis (type checking) - runs after parsing, before codegen.
 *
 * What it checks:
 *   1. Unknown identifiers      -> "undeclared variable 'x'"
 *   2. Unknown functions        -> "unknown function 'foo'"
 *   3. Wrong argument counts    -> "foo() expects 2 args, got 3"
 *   4. Unknown types/fields     -> "unknown type 'Pointt'"
 *   5. Type mismatches          -> "cannot assign str to i32"
 *   6. Unknown modules          -> "unknown module 'std.foo'"
 *   7. Duplicate definitions    -> "duplicate definition of 'foo'"
 *   8. return inside defer      -> "return not allowed in defer"
 *
 * What it also DOES (not just checks):
 *   - Auto-deref rewrite: in C! you may write p.x where p is *Point
 *     (spec section 3.3). Sema rewrites the node to OP_ARROW so
 *     codegen emits C's p->x automatically.
 *
 * Returns number of errors (0 = clean).
 */
int sema_check(ASTNodeList *program);

#endif