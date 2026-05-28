#ifndef LENO_OPTIMIZE_H
#define LENO_OPTIMIZE_H

#include "leno_ast.h"

void optimize_constant_fold(Ast* ast);
void optimize_dead_code_elimination(Ast* ast);

#endif
