// ============================================================================
// 寄存器式 codegen：表达式生成
// 核心接口：gen_expr_to(gen, ast, dst) → 结果写入 R[dst]
//           gen_expr(gen, ast) → 借临时寄存器，返回寄存器号
// ============================================================================

#include "codegen.h"

// 前向声明（语句相关，定义在 codegen_stmt.c）
extern void gen_default_value(CodeGen* gen, Ast* default_expr);

// ============================================================================
// 类型检查 / 安全转换（就地作用于 R[reg]）
//   需要名字的类别（struct/face/enum、struct/face/cstruct）在指令后附 2 字节名字常量
// ============================================================================
static void emit_type_op(CodeGen* gen, OpCode op, int reg, TypeInfo* t, int with_enum, int line) {
    TypeKind kind = t ? t->kind : TYPE_ANY;
    TypeKind elem = (t && t->element_type) ? t->element_type->kind : TYPE_ANY;

    int need_name = 0;
    if (op == OP_TYPE_CHECK) {
        need_name = (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_ENUM);
    } else {
        need_name = (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_CSTRUCT);
    }
    (void)with_enum;

    if (need_name) {
        const char* nm = (t && t->struct_name) ? t->struct_name : "";
        int nc = make_constant(gen, val_obj((Object*)str_copy(nm, (int)strlen(nm))));
        reg_encode_iABC(gen->chunk, op, reg, kind, elem, line);
        chunk_write(gen->chunk, (uint8_t)((nc >> 8) & 0xFF), line);
        chunk_write(gen->chunk, (uint8_t)(nc & 0xFF), line);
    } else {
        reg_encode_iABC(gen->chunk, op, reg, kind, elem, line);
    }
}

void emit_type_check_to(CodeGen* gen, int reg, TypeInfo* t, int line) {
    emit_type_op(gen, OP_TYPE_CHECK, reg, t, 1, line);
}

void emit_as_cast_to(CodeGen* gen, int reg, TypeInfo* t, int line) {
    emit_type_op(gen, OP_AS_CAST, reg, t, 0, line);
}

// ============================================================================
// 表达式入口
// ============================================================================

int gen_expr(CodeGen* gen, Ast* ast) {
    int r = reg_alloc(gen);
    gen_expr_to(gen, ast, r);
    return r;
}

void gen_expr_to(CodeGen* gen, Ast* ast, int dst) {
    if (!ast) {
        emit_loadnil_to(gen, dst, 0);
        return;
    }

    switch (ast->kind) {
        // --- 字面量 ---
        case AST_NUM:
            if (ast->u.num.is_bigint) {
                // BigInt：存常量表
                // TODO: bigint 字面量处理
                ObjBigInt* bi = bigint_from_string(ast->u.num.bigint_str);
                int idx = make_constant(gen, val_obj((Object*)bi));
                emit_loadk_to(gen, dst, idx, ast->line);
            } else if (ast->u.num.is_float) {
                int idx = make_constant(gen, val_float(ast->u.num.value));
                emit_loadk_to(gen, dst, idx, ast->line);
            } else {
                // 整数立即数：小数用 LOADI，大数用 LOADK
                int64_t v = (int64_t)ast->u.num.value;
                if (v >= -32767 && v <= 32767) {
                    emit_loadi_to(gen, dst, (int)v, ast->line);
                } else {
                    int idx = make_constant(gen, val_int(v));
                    emit_loadk_to(gen, dst, idx, ast->line);
                }
            }
            break;

        case AST_STRING:
        {
            ObjString* s = str_copy(ast->u.string.value, ast->u.string.len);
            int idx = make_constant(gen, val_obj((Object*)s));
            emit_loadk_to(gen, dst, idx, ast->line);
            break;
        }

        case AST_BOOL:
            if (ast->u.boolean) {
                emit_loadtrue_to(gen, dst, ast->line);
            } else {
                emit_loadfalse_to(gen, dst, ast->line);
            }
            break;

        case AST_NULL:
            emit_loadnil_to(gen, dst, ast->line);
            break;

        // --- 变量引用 ---
        // 符号解析结果由语义分析阶段写入 AST（u.var.ref），codegen 直接使用。
        // 不再用 scope_resolve(sem->current, name)：codegen 阶段 current 作用域
        // 已不对应声明点，而且语义阶段早有精确结果。
        case AST_VAR:
        {
            SymRef* ref = &ast->u.var.ref;
            if (!ref->name) {
                emit_loadnil_to(gen, dst, ast->line);
                break;
            }
            switch (ref->kind) {
                case SYM_LOCAL:
                case SYM_PARAM:
                    // 局部变量就是寄存器，直接 MOV
                    if (ref->index != dst) {
                        emit_mov(gen, dst, ref->index, ast->line);
                    }
                    break;
                case SYM_GLOBAL:
                    emit_getglobal_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_GLOBAL_FUNC:
                    emit_getglobalfunc_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_UPVALUE:
                    emit_getupval_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_MODULE:
                    // 模块变量：索引是 16 位 Bx（与 VM 的 READ_Bx 一致）
                    reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, ref->index, ast->line);
                    break;
                case SYM_TYPE:
                case SYM_STRUCT:
                case SYM_ENUM:
                {
                    // 类型名作为值使用：加载类型名字符串常量。
                    // 枚举成员访问 `Color.RED` 就是靠它 —— 语义分析把它编译成
                    // obj["RED"]，obj 即这里的类型名字符串，运行期再查全局枚举表。
                    // 此前一律发 nil，于是 `Color.RED` 变成 nil["RED"]，整类枚举
                    // 测试失败。
                    ObjString* tn = str_copy(ref->name, (int)strlen(ref->name));
                    int c = make_constant(gen, val_obj((Object*)tn));
                    emit_loadk_to(gen, dst, c, ast->line);
                    break;
                }
                case SYM_CSTRUCT:
                {
                    // cstruct 类型名 → 运行期 cstruct 定义对象（malloc/size/... 由它分发）
                    // 之前落进 default 发 nil，`TestColor.malloc()` 就变成 nil 上取方法 ✗
                    ObjString* tn = str_copy(ref->name, (int)strlen(ref->name));
                    int c = make_constant(gen, val_obj((Object*)tn));
                    reg_encode_iABx(gen->chunk, OP_GET_CSTRUCT_DEF, dst, c, ast->line);
                    break;
                }
                case SYM_NATIVE:
                case SYM_CLIB:
                case SYM_CFUNC:
                case SYM_FUNC_ALIAS:
                default:
                    // 其余（原生函数引用等）：作为值使用时是 null 占位
                    emit_loadnil_to(gen, dst, ast->line);
                    break;
            }
            break;
        }

        // --- 二元运算 ---
        case AST_BINOP:
            gen_binop(gen, ast, dst);
            break;

        // --- 一元运算 ---
        case AST_UNARY:
            gen_unary(gen, ast, dst);
            break;

        // --- 函数调用 ---
        case AST_CALL:
            gen_call(gen, ast, dst);
            break;

        // --- 匿名函数表达式（闭包工厂返回值等）：创建闭包写到 dst ---
        case AST_FUNC_DEF: {
            ObjFunction* fn = gen_func_proto(gen, ast);
            if (fn) {
                gen_func_closure(gen, ast, fn);
                int cidx = make_constant(gen, val_obj((Object*)fn));
                emit_closure_upvals(gen, dst, cidx, ast);
            } else {
                emit_loadnil_to(gen, dst, ast->line);
            }
            break;
        }

        // --- 索引访问 ---
        case AST_INDEX:
        {
            int obj_reg = gen_expr(gen, ast->u.index.obj);
            int idx_reg = gen_expr(gen, ast->u.index.index);
            // INDEX: R[A] = R[B][R[C]]
            reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, idx_reg, ast->line);
            reg_free(gen, idx_reg);
            reg_free(gen, obj_reg);
            break;
        }

        // --- 字段访问 ---
        // 两条完全不同的运行期语义，必须按**对象的静态类型**分流：
        //   struct/cstruct 实例 → OP_GET_FIELD（按编译期确定的字段索引）
        //   其它（Dict / any / 收窄后的容器）→ obj["字段名"] 通用索引
        // 此前一律发 OP_GET_FIELD，字典字段访问（`d.field`）就会撞上
        // 「字段访问需要结构体对象」而整类收窄/守卫测试失败。
        case AST_FIELD_ACCESS:
        {
            int obj_reg = gen_expr(gen, ast->u.field_access.obj);
            TypeInfo* ot = infer_expr_type(gen->sem, ast->u.field_access.obj);
            int field_idx = ast->u.field_access.field_index;
            int use_field_op = (ot && (ot->kind == TYPE_STRUCT || ot->kind == TYPE_CSTRUCT)
                                && field_idx >= 0);

            if (use_field_op) {
                // GET_FIELD: R[A] = R[B].field(C)
                reg_encode_iABC(gen->chunk, OP_GET_FIELD, dst, obj_reg, field_idx, ast->line);
            } else {
                const char* fname = ast->u.field_access.field_name;
                int ireg = reg_alloc(gen);
                int c = make_constant(gen, val_obj((Object*)str_copy(
                                          fname ? fname : "", fname ? (int)strlen(fname) : 0)));
                emit_loadk_to(gen, ireg, c, ast->line);
                reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, ireg, ast->line);
                reg_free(gen, ireg);
            }
            reg_free(gen, obj_reg);
            break;
        }

        // --- 数组字面量 ---
        // ★ 元素必须落在 R[base+1 .. base+n]（NEWARRAY 按 A+i 读取），
        //   而 base **绝不能直接取 dst** —— dst 常常是**变量槽**（`loopArr = [s]`），
        //   那样元素就会写进**相邻变量**的槽里 ✗（实测 `int acc` 紧邻 `loopArr` 时
        //   被 `loopArr = [s]` 踩成那个 struct）。一律在**临时块**里拼，再 MOV 回 dst。
        case AST_ARRAY:
        {
            int n = ast->u.array.count;
            if (n == 0) {
                reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, 0, ast->line);
                break;
            }
            int base = reg_alloc_block(gen, n + 1);
            for (int i = 0; i < n; i++) {
                gen_expr_to(gen, ast->u.array.items[i], base + 1 + i);
            }
            // NEWARRAY: R[A] = new array(R[A+1..A+C-1]), C = count
            reg_encode_iABC(gen->chunk, OP_NEWARRAY, base, 0, n, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            break;
        }

        // --- 字典字面量 ---
        // 键值对交替落在 R[base+1 .. base+2n]（同 NEWARRAY 的连号约定）；
        // 同样必须用临时块，不能拿 dst 当 base（理由见上）
        case AST_DICT:
        {
            int n = ast->u.dict.count;
            if (n == 0) {
                reg_encode_iABC(gen->chunk, OP_NEWDICT, dst, 0, 0, ast->line);
                break;
            }
            int base = reg_alloc_block(gen, n * 2 + 1);
            for (int i = 0; i < n; i++) {
                gen_expr_to(gen, ast->u.dict.entries[i].key, base + 1 + i * 2);
                gen_expr_to(gen, ast->u.dict.entries[i].value, base + 2 + i * 2);
            }
            reg_encode_iABC(gen->chunk, OP_NEWDICT, base, 0, n, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            break;
        }

        // --- 切片：arr[start:end] ---
        // OP_SLICE 约定：A = 结果，B = 对象，start/end 固定在 R[A+1] / R[A+2]
        case AST_SLICE: {
            int base = reg_alloc_block(gen, 3);
            gen_expr_to(gen, ast->u.slice.obj, base);
            if (ast->u.slice.start) gen_expr_to(gen, ast->u.slice.start, base + 1);
            else emit_loadnil_to(gen, base + 1, ast->line);
            if (ast->u.slice.end) gen_expr_to(gen, ast->u.slice.end, base + 2);
            else emit_loadnil_to(gen, base + 2, ast->line);
            reg_encode_iABC(gen->chunk, OP_SLICE, base, base, 0, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            break;
        }

        // --- 范围 ---
        case AST_RANGE:
        {
            int start_reg = gen_expr(gen, ast->u.range.start);
            int end_reg = gen_expr(gen, ast->u.range.end);
            // RANGE: R[A] = Range(R[B], R[C])
            reg_encode_iABC(gen->chunk, OP_RANGE, dst, start_reg, end_reg, ast->line);
            reg_free(gen, end_reg);
            reg_free(gen, start_reg);
            break;
        }

        // --- 插值字符串 ---
        case AST_INTERP_STRING:
            gen_interp_string(gen, ast, dst);
            break;

        // --- 模块访问 ---
        case AST_MODULE_ACCESS:
            gen_module_access(gen, ast, dst);
            break;

        // --- 模块方法调用 ---
        case AST_MODULE_CALL:
            gen_module_call(gen, ast, dst);
            break;

        // --- if 表达式（三元 / 表达式位置的 if）---
        // 结果必须写在调用方给的 dst 上；gen_if_ex 内部自分配 dst 时，
        // 三元表达式的结果会凭空落在别的寄存器里 ⇒ 变量拿到 null ✗
        case AST_IF:
            gen_if_ex(gen, ast, 1, dst);
            break;

        // --- 类型检查：x is T ---
        case AST_TYPE_CHECK:
        {
            gen_expr_to(gen, ast->u.type_check.expr, dst);
            emit_type_check_to(gen, dst, ast->u.type_check.type, ast->line);
            break;
        }

        // --- 安全类型转换：x as T ---
        case AST_AS_CAST:
        {
            gen_expr_to(gen, ast->u.type_check.expr, dst);
            emit_as_cast_to(gen, dst, ast->u.type_check.type, ast->line);
            break;
        }

        // --- await ---
        case AST_AWAIT:
        {
            int src_reg = gen_expr(gen, ast->u.await.expr);
            // AWAIT: R[A] = await R[B]
            reg_encode_iABC(gen->chunk, OP_AWAIT, dst, src_reg, 0, ast->line);
            reg_free(gen, src_reg);
            break;
        }

        // --- 安全访问 ---
        case AST_SAFE_ACCESS:
            gen_safe_access(gen, ast, dst);
            break;

        // --- 取地址 ---
        case AST_ADDRESS_OF:
        {
            // &obj.field → GET_FIELD_ADDR
            Ast* field_ast = ast->u.address_of.operand;
            int obj_reg = gen_expr(gen, field_ast->u.field_access.obj);
            reg_encode_iABC(gen->chunk, OP_GET_FIELD_ADDR, dst, obj_reg,
                          field_ast->u.field_access.field_index, ast->line);
            reg_free(gen, obj_reg);
            break;
        }

        // --- struct 初始化 ---
        case AST_STRUCT_INIT:
            gen_struct_init(gen, ast, dst);
            break;

        // --- 索引赋值作为表达式（`c["k"] = v` / 复合赋值脱糖）---
        case AST_INDEX_ASSIGN:
            gen_index_assign(gen, ast, dst);
            break;

        // --- 赋值作为表达式（parser 会把赋值语句包成表达式语句）---
        case AST_ASSIGN: {
            gen_assign(gen, ast);
            // 表达式结果 = 被赋的值
            if (ast->u.assign.name_count >= 1 && ast->u.assign.targets) {
                Ast* t = ast->u.assign.targets[0];
                SymRef* ref = (ast->u.assign.refs && ast->u.assign.refs[0].name)
                                  ? &ast->u.assign.refs[0] : &t->u.var.ref;
                if (t->kind == AST_VAR) {
                    if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
                        if (ref->index != dst) emit_mov(gen, dst, ref->index, ast->line);
                    } else if (ref->kind == SYM_GLOBAL) {
                        emit_getglobal_to(gen, dst, ref->index, ast->line);
                    } else if (ref->kind == SYM_UPVALUE) {
                        emit_getupval_to(gen, dst, ref->index, ast->line);
                    } else {
                        emit_loadnil_to(gen, dst, ast->line);
                    }
                } else {
                    emit_loadnil_to(gen, dst, ast->line);
                }
            } else {
                emit_loadnil_to(gen, dst, ast->line);
            }
            break;
        }

        default:
            emit_loadnil_to(gen, dst, ast->line);
            break;
    }
}

// ============================================================================
// 二元运算
// ============================================================================

void gen_binop(CodeGen* gen, Ast* ast, int dst) {
    Ast* lhs = ast->u.binop.l;
    Ast* rhs = ast->u.binop.r;
    LenoTokenType op = ast->u.binop.op;

    // 短路运算
    if (op == TOK_AND) {
        // R[dst] = R[lhs] && R[rhs]
        gen_expr_to(gen, lhs, dst);
        int jmp_false = emit_jmp_if_false(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_false);
        return;
    }
    if (op == TOK_OR) {
        gen_expr_to(gen, lhs, dst);
        int jmp_true = emit_jmp_if_true(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_true);
        return;
    }

    // 空合并 a ?? b：只有 a **是 null** 才求值 b（0 / false / "" 都要保留）
    // ⚠ 不能用 JMP_IF_TRUE（那是真值语义）—— 否则 `0 ?? 99` 会得 99 ✗
    if (op == TOK_NULL_COALESCE) {
        gen_expr_to(gen, lhs, dst);
        int isnull = reg_alloc(gen);
        emit_is_null(gen, isnull, dst, ast->line);
        int jmp = emit_jmp_if_false(gen, isnull, ast->line);  // 非 null → 跳过右侧
        reg_free(gen, isnull);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp);
        return;
    }

    // 普通二元运算：求 lhs → dst，求 rhs → 临时寄存器，运算写回 dst
    gen_expr_to(gen, lhs, dst);
    int r = gen_expr(gen, rhs);

    // ★ 类型特化（与栈式 codegen_expr.c 口径一致）：两侧静态类型都是 int 时
    //   走 int 专用指令。**这不只是性能差异**：int 专用指令把 null 视作 0
    //   （`val_as_int(NULL_VAL) == 0`，TAG_NULL = 0），而通用 ADD 会报
    //   「null 不能参与运算」—— 实测 `merge(o){ return x + o.x }` 里未赋值的
    //   int 字段 x 在栈式得 3、寄存式报错 ✗（test_cross_module_method_args ⑥）。
    TypeKind lt = (lhs && lhs->cached_type) ? lhs->cached_type->kind : TYPE_UNKNOWN;
    TypeKind rt = (rhs && rhs->cached_type) ? rhs->cached_type->kind : TYPE_UNKNOWN;
    int both_int = (lt == TYPE_INT && rt == TYPE_INT);

    switch (op) {
        case TOK_PLUS:
            if (both_int) emit_add_int(gen, dst, dst, r, ast->line);
            else emit_add(gen, dst, dst, r, ast->line);
            break;
        case TOK_MINUS:
            if (both_int) emit_sub_int(gen, dst, dst, r, ast->line);
            else emit_sub(gen, dst, dst, r, ast->line);
            break;
        case TOK_STAR:
            if (both_int) emit_mul_int(gen, dst, dst, r, ast->line);
            else emit_mul(gen, dst, dst, r, ast->line);
            break;
        case TOK_SLASH:    emit_div(gen, dst, dst, r, ast->line); break;
        case TOK_MOD:      emit_mod(gen, dst, dst, r, ast->line); break;
        case TOK_EQEQ:     emit_eq(gen, dst, dst, r, ast->line); break;
        case TOK_NEQ:      emit_neq(gen, dst, dst, r, ast->line); break;
        // 有序比较同样按静态类型特化（与栈式一致）：两侧都是 int 走 int 专用比较。
        //   ⚠ 与算术同理，这不只是性能 —— 泛型约束方法 `compareTo(T other)` 在
        //   T=OrdInt 时实参**是 struct**，通用比较会报「操作数类型不可比较」✗，
        //   而 int 专用比较按栈式口径直接比位模式（test_generic_face_impl）。
        case TOK_LT:
            if (both_int) emit_lt_int(gen, dst, dst, r, ast->line);
            else emit_lt(gen, dst, dst, r, ast->line);
            break;
        case TOK_GT:
            if (both_int) emit_gt_int(gen, dst, dst, r, ast->line);
            else emit_gt(gen, dst, dst, r, ast->line);
            break;
        case TOK_LE:
            if (both_int) emit_le_int(gen, dst, dst, r, ast->line);
            else emit_le(gen, dst, dst, r, ast->line);
            break;
        case TOK_GE:
            if (both_int) emit_ge_int(gen, dst, dst, r, ast->line);
            else emit_ge(gen, dst, dst, r, ast->line);
            break;
        case TOK_BITAND:   emit_bitand(gen, dst, dst, r, ast->line); break;
        case TOK_BITOR:    emit_bitor(gen, dst, dst, r, ast->line); break;
        case TOK_BITXOR:   emit_bitxor(gen, dst, dst, r, ast->line); break;
        case TOK_SHL:      emit_shl(gen, dst, dst, r, ast->line); break;
        case TOK_SHR:      emit_shr(gen, dst, dst, r, ast->line); break;
        case TOK_USHR:     emit_ushr(gen, dst, dst, r, ast->line); break;
        case TOK_IN:       reg_encode_iABC(gen->chunk, OP_IN, dst, dst, r, ast->line); break;
        // not in = in + not（此前完全没处理 ⇒ 结果直接是左操作数本身 ✗）
        case TOK_NOT_IN:
            reg_encode_iABC(gen->chunk, OP_IN, dst, dst, r, ast->line);
            emit_not(gen, dst, dst, ast->line);
            break;
        default:
            // 字符串拼接用 STRCAT
            if (op == TOK_PLUS) {
                emit_strcat(gen, dst, dst, r, ast->line);
            }
            break;
    }
    reg_free(gen, r);
}

// ============================================================================
// 一元运算
// ============================================================================

void gen_unary(CodeGen* gen, Ast* ast, int dst) {
    LenoTokenType op = ast->u.unary.op;
    Ast* operand_ast = ast->u.unary.operand;

    // ++ / -- 必须就地作用于变量的寄存器 —— 否则只改了副本，变量本身不变。
    if (op == TOK_INC || op == TOK_DEC) {
        if (!operand_ast || operand_ast->kind != AST_VAR) {
            // 字段/索引等非变量目标：设计上不支持（且静默不动会让 `pa.x++` 变成空操作 ✗）
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, "++ 和 -- 只能用于变量");
            emit_loadnil_to(gen, dst, ast->line);
            return;
        }
        {
            SymRef* ref = &operand_ast->u.var.ref;
            int is_local = (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM);
            int slot;

            if (is_local) {
                slot = ref->index;
            } else {
                slot = reg_alloc(gen);
                if (ref->kind == SYM_GLOBAL) {
                    emit_getglobal_to(gen, slot, ref->index, ast->line);
                } else if (ref->kind == SYM_UPVALUE) {
                    emit_getupval_to(gen, slot, ref->index, ast->line);
                } else {
                    emit_loadnil_to(gen, slot, ast->line);
                }
            }

            if (ast->u.unary.is_postfix) {
                // 后缀：表达式取旧值，变量随后自增/自减
                if (dst != slot) emit_mov(gen, dst, slot, ast->line);
                if (op == TOK_INC) emit_inc(gen, slot, slot, ast->line);
                else emit_dec(gen, slot, slot, ast->line);
            } else {
                // 前缀：先自增/自减，表达式取新值
                if (op == TOK_INC) emit_inc(gen, slot, slot, ast->line);
                else emit_dec(gen, slot, slot, ast->line);
                if (dst != slot) emit_mov(gen, dst, slot, ast->line);
            }

            if (!is_local) {
                if (ref->kind == SYM_GLOBAL) {
                    emit_setglobal(gen, slot, ref->index, ast->line);
                } else if (ref->kind == SYM_UPVALUE) {
                    emit_setupval(gen, slot, ref->index, ast->line);
                }
                reg_free(gen, slot);
            }
            return;
        }
    }

    int operand = gen_expr(gen, operand_ast);

    switch (op) {
        case TOK_MINUS:    emit_neg(gen, dst, operand, ast->line); break;
        case TOK_NOT:       emit_not(gen, dst, operand, ast->line); break;
        case TOK_BITNOT:    emit_bitnot(gen, dst, operand, ast->line); break;
        default:            emit_mov(gen, dst, operand, ast->line); break;
    }
    reg_free(gen, operand);
}

// ============================================================================
// 函数调用
// ============================================================================

// 调用点补齐默认参数（与栈式实现同一口径）。
//   fdef：被调函数的 AST_FUNC_DEF（可为 NULL）
//   self_offset：params[0] 是否是隐式 self（方法 1，普通函数 0）
//   缺失的默认值依次求值写入 R[base+1+provided ..]；返回实际应传的参数个数。
static int fill_default_args(CodeGen* gen, Ast* fdef, int self_offset,
                             int nargs, int base, int line) {
    if (!fdef || fdef->kind != AST_FUNC_DEF) return nargs;
    int pcnt = fdef->u.func.pcnt;
    int expected = pcnt - self_offset;
    if (expected <= nargs) return nargs;
    for (int i = nargs; i < expected; i++) {
        int pi = i + self_offset;
        Ast* d = (fdef->u.func.param_defaults && pi < pcnt) ? fdef->u.func.param_defaults[pi] : NULL;
        if (d) {
            gen_expr_to(gen, d, base + 1 + i);
        } else {
            emit_loadnil_to(gen, base + 1 + i, line);
        }
    }
    return expected;
}

// clib 方法调用 → OP_CLIB_CALL（成功返回 1）。
//   寄存器布局（VM 直接把 &R[A] 当 Value* 传给 FFI）：
//     R[A]   = clib 库对象
//     R[A+1] = 函数名字符串
//     R[A+2..A+1+nargs] = 实参
//   指令后紧跟：ret_type_kind(1) + arg_types[nargs](1 each)
//   —— 与栈式 op_clib_call.inc 的语义一致（类型用于 str8/str16 自动转换与参数窄化）
#define CLIB_CALL_MAX_ARGS 12   // 与 FFI_MAX_ARGS 口径一致（Win64/AAPCS64 上限）

static int gen_clib_call(CodeGen* gen, Ast* obj_ast, const char* fname,
                         AstList* args, int nargs, int dst, int line) {
    TypeInfo* ot = infer_expr_type(gen->sem, obj_ast);
    if (!ot || ot->kind != TYPE_CLIB || !ot->struct_name) {
        if (ot) type_free(ot);
        return 0;
    }
    char sname[BUFFER_SMALL];
    snprintf(sname, sizeof(sname), "%s", ot->struct_name);
    type_free(ot);

    Symbol* clib_sym = scope_resolve(gen->sem->root_scope, sname);
    if (!clib_sym) clib_sym = scope_resolve(gen->sem->current, sname);
    if (!clib_sym) return 0;

    // 查函数签名（导入的 clib 也已由语义阶段填好签名；查不到才退化为 I32）
    int ret_kind = TYPE_I32;
    int arg_kinds[CLIB_CALL_MAX_ARGS];
    int lim = nargs < CLIB_CALL_MAX_ARGS ? nargs : CLIB_CALL_MAX_ARGS;
    for (int i = 0; i < CLIB_CALL_MAX_ARGS; i++) arg_kinds[i] = TYPE_I32;
    for (int i = 0; i < clib_sym->clib_func_count; i++) {
        if (clib_sym->clib_func_names[i] && strcmp(clib_sym->clib_func_names[i], fname) == 0) {
            TypeInfo* rt = clib_sym->clib_func_return_types
                               ? clib_sym->clib_func_return_types[i] : NULL;
            ret_kind = rt ? (int)rt->kind : (int)TYPE_I32;
            if (clib_sym->clib_func_param_types && clib_sym->clib_func_param_types[i]) {
                for (int j = 0; j < lim; j++) {
                    TypeInfo* pt = clib_sym->clib_func_param_types[i][j];
                    if (pt) arg_kinds[j] = (int)pt->kind;
                }
            }
            break;
        }
    }

    int base = reg_alloc_block(gen, 2 + nargs);
    gen_expr_to(gen, obj_ast, base);                       // 库对象
    int nc = make_constant(gen, val_obj((Object*)str_copy(fname, (int)strlen(fname))));
    emit_loadk_to(gen, base + 1, nc, line);                // 函数名
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, args->items[i], base + 2 + i);
    }

    reg_encode_iABC(gen->chunk, OP_CLIB_CALL, base, 0, nargs, line);
    chunk_write(gen->chunk, (uint8_t)(ret_kind & 0xFF), line);
    for (int i = 0; i < lim; i++) {
        chunk_write(gen->chunk, (uint8_t)(arg_kinds[i] & 0xFF), line);
    }

    if (base != dst) emit_mov(gen, dst, base, line);
    reg_free_block(gen, base);
    return 1;
}

// 生成 obj.name(实参...) 的方法调用：
//   R[base] = R[base].name    （OP_GET_METHOD，产出绑定方法 / native）
//   R[base+1..] = 实参（不足的按默认参数补齐）
//   CALL R[base], nargs, 1
static void gen_method_call(CodeGen* gen, Ast* obj_ast, const char* mname,
                            AstList* args, int nargs, int dst, int line) {
    // clib 库对象的方法调用（`lib.strerror(2)`）走 FFI 专用指令
    if (gen_clib_call(gen, obj_ast, mname, args, nargs, dst, line)) return;

    // 查方法定义（语义分析按 "Struct::method" 注册），用于补齐默认参数
    Ast* mdef = NULL;
    TypeInfo* ot = infer_expr_type(gen->sem, obj_ast);
    if (ot && ot->kind == TYPE_STRUCT && ot->struct_name && mname) {
        char key[BUFFER_SMALL];
        snprintf(key, sizeof(key), "%s::%s", ot->struct_name, mname);
        mdef = func_table_find(&gen->sem->func_table, key);
    }
    if (ot) type_free(ot);

    // ⚠ 方法调用的实参列表**已含隐式 self**（语义分析插入，args[0] = self），
    //   所以这里按 self_offset=0 补齐：缺失的默认值对应 params[i]（i = nargs..pcnt-1）
    int expected = (mdef && mdef->kind == AST_FUNC_DEF && mdef->u.func.pcnt > nargs)
                       ? mdef->u.func.pcnt : nargs;

    int base = reg_alloc_block(gen, expected + 1);
    gen_expr_to(gen, obj_ast, base);

    int mlen = (int)strlen(mname);
    ObjString* nameStr = str_copy(mname, mlen);
    int name_const = make_constant(gen, val_obj((Object*)nameStr));

    // C 为 0 或超 8 位 → 紧随一条 EXTRAARG 携带 24 位常量索引
    if (name_const == 0 || name_const > 255) {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, 0, line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, name_const, line);
    } else {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, name_const, line);
    }

    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, args->items[i], base + 1 + i);
    }
    expected = fill_default_args(gen, mdef, 0, nargs, base, line);

    emit_call(gen, base, expected, 1, line);
    if (base != dst) emit_mov(gen, dst, base, line);
    reg_free_block(gen, base);
}

void gen_call(CodeGen* gen, Ast* ast, int dst) {
    Ast* callee = ast->u.call.callee;
    int nargs = ast->u.call.args.count;

    // --- 方法调用：obj.name(args) ---
    //   语义分析对 `s.len()` 这类会给出 AST_FIELD_ACCESS 或 AST_INDEX("len")，
    //   两种形态都走同一套：GET_METHOD 得到绑定方法后再 CALL。
    // 注：`t.cb()`（func 类型字段）与 `self.on_click()` 也走 gen_method_call ——
    //   运行期 OP_GET_METHOD 找不到同名方法时会退化为"取同名字段"，
    //   字段里就是一等函数值，随后的 CALL 直接调它（见 vm_run.inc 的 OP_GET_METHOD）。
    if (callee && callee->kind == AST_FIELD_ACCESS) {
        gen_method_call(gen, callee->u.field_access.obj, callee->u.field_access.field_name,
                        &ast->u.call.args, nargs, dst, ast->line);
        return;
    }
    if (callee && callee->kind == AST_INDEX && callee->u.index.index &&
        callee->u.index.index->kind == AST_STRING) {
        gen_method_call(gen, callee->u.index.obj, callee->u.index.index->u.string.value,
                        &ast->u.call.args, nargs, dst, ast->line);
        return;
    }

    // 调用约定：R[base] = callee，R[base+1 .. base+nargs] = 实参。
    // 必须整块连续分配，否则实参之间会被临时寄存器隔开（VM 按 A+i 取参）。
    if (callee && callee->kind == AST_VAR) {
        SymRef* ref = &callee->u.var.ref;

        // --- native 直接调用（CALL_NATIVE）：省掉"取函数值 + CALL" ---
        if (ref->kind == SYM_NATIVE) {
            int base = reg_alloc_block(gen, nargs + 1);
            for (int i = 0; i < nargs; i++) {
                gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
            }
            ObjString* nameStr = str_copy(ref->name, (int)strlen(ref->name));
            int name_const = make_constant(gen, val_obj((Object*)nameStr));
            emit_call_native(gen, base, name_const, nargs, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            return;
        }

        // --- 全局函数直接调用（含默认参数补齐）---
        if (ref->kind == SYM_GLOBAL_FUNC) {
            Ast* fdef = ref->name ? func_table_find(&gen->sem->func_table, ref->name) : NULL;
            int expected = (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.pcnt > nargs)
                               ? fdef->u.func.pcnt : nargs;
            int base = reg_alloc_block(gen, expected + 1);
            emit_getglobalfunc_to(gen, base, ref->index, ast->line);
            for (int i = 0; i < nargs; i++) {
                gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
            }
            expected = fill_default_args(gen, fdef, 0, nargs, base, ast->line);
            // ★ async 函数：调用不直接执行函数体，而是建协程并立刻返回 Future
            if (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.is_async) {
                reg_encode_iABC(gen->chunk, OP_ASYNC_CALL, base, expected, 0, ast->line);
            } else {
                emit_call(gen, base, expected, 1, ast->line);
            }
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            return;
        }
    }

    // --- 通用调用（局部函数 / 闭包 / 一等函数值）---
    // 具名局部函数同样要补默认参数：func_table 里局部函数会覆盖同名全局定义
    Ast* fdef = NULL;
    if (callee && callee->kind == AST_VAR && callee->u.var.ref.name) {
        fdef = func_table_find(&gen->sem->func_table, callee->u.var.ref.name);
    }
    int expected = (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.pcnt > nargs)
                       ? fdef->u.func.pcnt : nargs;

    int base = reg_alloc_block(gen, expected + 1);
    gen_expr_to(gen, callee, base);
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
    }
    expected = fill_default_args(gen, fdef, 0, nargs, base, ast->line);
    // async 函数（含局部 async 函数）：走协程创建路径，返回 Future
    if (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.is_async) {
        reg_encode_iABC(gen->chunk, OP_ASYNC_CALL, base, expected, 0, ast->line);
    } else {
        emit_call(gen, base, expected, 1, ast->line);
    }
    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
}

// ============================================================================
// 多返回值调用（解构声明 `var[...](a, b) = f()` 用）
// ----------------------------------------------------------------------------
// 与 gen_call 的唯一区别：CALL 的 nresults = 调用方要的槽位数，结果落在
// R[base .. base+n-1]；返回 base（调用方用 reg_free_block(base) 释放）。
// ⚠ 必须复用同一套「方法调用走 GET_METHOD」「默认参数补齐」逻辑：
//   之前解构路径把 `mrp.two(i)` 的 callee 当普通表达式求值 ⇒ 对 struct 退化成
//   obj["two"] ⇒ 报「struct 不存在字段 'two'」；少参调用也不补默认值 ⇒ 得 null ✗
// ============================================================================
int gen_call_multi(CodeGen* gen, Ast* ast, int nresults, int line) {
    Ast* callee = ast->u.call.callee;
    int nargs = ast->u.call.args.count;

    // --- 方法调用：obj.name(args)（callee 可能是 FIELD_ACCESS 或 INDEX("name")）---
    Ast* obj_ast = NULL;
    const char* mname = NULL;
    if (callee && callee->kind == AST_FIELD_ACCESS) {
        obj_ast = callee->u.field_access.obj;
        mname = callee->u.field_access.field_name;
    } else if (callee && callee->kind == AST_INDEX && callee->u.index.index &&
               callee->u.index.index->kind == AST_STRING) {
        obj_ast = callee->u.index.obj;
        mname = callee->u.index.index->u.string.value;
    }

    if (obj_ast && mname) {
        Ast* mdef = NULL;
        TypeInfo* ot = infer_expr_type(gen->sem, obj_ast);
        if (ot && ot->kind == TYPE_STRUCT && ot->struct_name && mname) {
            char key[BUFFER_SMALL];
            snprintf(key, sizeof(key), "%s::%s", ot->struct_name, mname);
            mdef = func_table_find(&gen->sem->func_table, key);
        }
        if (ot) type_free(ot);

        int expected = (mdef && mdef->kind == AST_FUNC_DEF && mdef->u.func.pcnt > nargs)
                           ? mdef->u.func.pcnt : nargs;
        // ⚠ 块必须同时容纳「callee + 实参」与「nresults 个结果」：
        //   结果写在 R[base .. base+nresults-1]，只按 expected+1 分配会让结果越界
        //   写到块外（8 返回值实测直接把 VM 的 locals 数组写爆 → 退出时堆损坏 ✗）
        int block = expected + 1;
        if (nresults > block) block = nresults;
        int base = reg_alloc_block(gen, block);
        gen_expr_to(gen, obj_ast, base);
        int nc = make_constant(gen, val_obj((Object*)str_copy(mname, (int)strlen(mname))));
        if (nc == 0 || nc > 255) {
            reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, 0, line);
            reg_encode_iAx(gen->chunk, OP_EXTRAARG, nc, line);
        } else {
            reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, nc, line);
        }
        for (int i = 0; i < nargs; i++) {
            gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
        }
        expected = fill_default_args(gen, mdef, 0, nargs, base, line);
        emit_call(gen, base, expected, nresults, line);
        return base;
    }

    // --- 普通函数 / 全局函数 / 局部函数 ---
    Ast* fdef = NULL;
    if (callee && callee->kind == AST_VAR && callee->u.var.ref.name) {
        fdef = func_table_find(&gen->sem->func_table, callee->u.var.ref.name);
    }
    int expected = (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.pcnt > nargs)
                       ? fdef->u.func.pcnt : nargs;
    int block = expected + 1;
    if (nresults > block) block = nresults;   // 同上：结果区必须落在块内
    int base = reg_alloc_block(gen, block);
    gen_expr_to(gen, callee, base);
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
    }
    expected = fill_default_args(gen, fdef, 0, nargs, base, line);
    emit_call(gen, base, expected, nresults, line);
    return base;
}

// ============================================================================
// 插值字符串
// ============================================================================

void gen_interp_string(CodeGen* gen, Ast* ast, int dst) {
    int count = ast->u.interp_string.count;
    // 交替：字符串片段 + 表达式
    // ★ 必须先在**临时寄存器**里拼，最后才 MOV 到 dst：
    //   dst 常常就是被插值表达式读的那个变量槽（`acc = $"{acc}ab"`），
    //   直接往 dst 写第一个片段会先把变量清空 ⇒ 读到的永远是空串 ✗
    int first = 1;
    int cur = reg_alloc(gen);

    for (int i = 0; i < count; i++) {
        // 字符串片段
        if (ast->u.interp_string.parts[i]) {
            ObjString* s = str_copy(ast->u.interp_string.parts[i],
                                    (int)strlen(ast->u.interp_string.parts[i]));
            int idx = make_constant(gen, val_obj((Object*)s));
            if (first) {
                emit_loadk_to(gen, cur, idx, ast->line);
                first = 0;
            } else {
                int tmp = reg_alloc(gen);
                emit_loadk_to(gen, tmp, idx, ast->line);
                emit_strcat(gen, cur, cur, tmp, ast->line);
                reg_free(gen, tmp);
            }
        }
        // 表达式
        if (i < ast->u.interp_string.count && ast->u.interp_string.exprs[i]) {
            if (first) {
                gen_expr_to(gen, ast->u.interp_string.exprs[i], cur);
                first = 0;
            } else {
                int tmp = gen_expr(gen, ast->u.interp_string.exprs[i]);
                emit_strcat(gen, cur, cur, tmp, ast->line);
                reg_free(gen, tmp);
            }
        }
    }

    if (first) {
        // 全空插值串（理论上不该出现）：给一个空串，避免 cur 是未初始化寄存器
        int e = make_constant(gen, val_obj((Object*)str_new("", 0)));
        emit_loadk_to(gen, cur, e, ast->line);
    }
    if (cur != dst) emit_mov(gen, dst, cur, ast->line);
    reg_free(gen, cur);
}

// ============================================================================
// 模块访问 / 模块调用
// ============================================================================

// 取「别名变量持有的模块对象」到 dst（mod.member 的 mod 部分）。
//   语义分析把别名符号写在 ast->u.module_access.ref：
//     SYM_GLOBAL  （主文件里的 import 别名 → 全局变量）
//     SYM_MODULE  （模块内 import 别的模块 → 本模块的全局槽）
//     SYM_LOCAL / SYM_UPVALUE
static void emit_module_object(CodeGen* gen, Ast* ast, int dst) {
    SymRef* ref = &ast->u.module_access.ref;

    if (ref->name) {
        switch (ref->kind) {
            case SYM_LOCAL:
            case SYM_PARAM:
                if (ref->index != dst) emit_mov(gen, dst, ref->index, ast->line);
                return;
            case SYM_GLOBAL:
                emit_getglobal_to(gen, dst, ref->index, ast->line);
                return;
            case SYM_UPVALUE:
                emit_getupval_to(gen, dst, ref->index, ast->line);
                return;
            case SYM_MODULE:
                reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, ref->index, ast->line);
                return;
            default:
                break;
        }
    }

    // 兜底：按别名在全局作用域查
    const char* alias = ast->u.module_access.module_name;
    Symbol* sym = alias ? scope_resolve(gen->sem->root_scope, alias) : NULL;
    if (sym) {
        if (sym->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, dst, sym->index, ast->line);
            return;
        }
        if (sym->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, sym->index, ast->line);
            return;
        }
        if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM) {
            if (sym->index != dst) emit_mov(gen, dst, sym->index, ast->line);
            return;
        }
    }
    emit_loadnil_to(gen, dst, ast->line);
}

// mod.member：取模块对象后按成员名查 exports。
//   ⚠ AST_MODULE_ACCESS 表示的是**成员访问**（不是"模块对象表达式"）——
//     只取模块对象会把整个模块对象当成结果（曾表现为 assert_eq(mod.value, 100)
//     实际得到 [object]）。模块成员（变量/函数/类型）都统一放在 exports 字典里。
void gen_module_access(CodeGen* gen, Ast* ast, int dst) {
    const char* member = ast->u.module_access.member_name;
    if (!member || !member[0]) {
        emit_module_object(gen, ast, dst);
        return;
    }

    int mreg = reg_alloc(gen);
    emit_module_object(gen, ast, mreg);

    int ireg = reg_alloc(gen);
    int cidx = make_constant(gen, val_obj((Object*)str_copy(member, (int)strlen(member))));
    emit_loadk_to(gen, ireg, cidx, ast->line);

    reg_encode_iABC(gen->chunk, OP_INDEX, dst, mreg, ireg, ast->line);

    reg_free(gen, ireg);
    reg_free(gen, mreg);
}

void gen_module_call(CodeGen* gen, Ast* ast, int dst) {
    // 两类模块调用：
    //   原生模块（io / jsons / strings ...）→ OP_MODULE_CALL（按名字查 native 方法表）
    //   .leno 源码模块（别名）→ 取模块对象 → exports[方法名] → 普通 CALL
    // 用「该名字能否被原生模块机制识别」区分；lib_ref 对 .leno 模块没有有效信息。
    extern int native_init_module(const char* name);

    int nargs = ast->u.module_call.args.count;
    const char* modname = ast->u.module_call.module_name ? ast->u.module_call.module_name : "";
    const char* methname = ast->u.module_call.method_name ? ast->u.module_call.method_name : "";

    // ★ clib 库变量的成员调用（`lib.strerror(2)`）：
    //   parser 对 `标识符.方法(...)` 一律产出 AST_MODULE_CALL，语义阶段识别出这是
    //   clib 调用时会打上 cached_type = TYPE_CLIB（struct_name = clib 类型名）并
    //   记录 lib_ref / clib_return_type。必须走 FFI 专用指令，
    //   否则会被当成 "模块对象[方法名]" → OP_INDEX → 「下标访问: 对象不支持索引」✗
    //   判别：语义阶段只在**真的**是 clib 调用时填 lib_ref.name（lib 变量的符号）。
    //   `base.loadCore()` 这类 .leno 模块调用虽然返回 clib，但 lib_ref.name 为空 ⇒ 走普通路径。
    if (ast->cached_type && ast->cached_type->kind == TYPE_CLIB && ast->cached_type->struct_name &&
        ast->u.module_call.lib_ref.name) {
        const char* clib_name = ast->cached_type->struct_name;
        Symbol* clib_sym = scope_resolve(gen->sem->root_scope, clib_name);
        if (!clib_sym) clib_sym = scope_resolve(gen->sem->current, clib_name);

        int ret_kind = TYPE_I32;
        int arg_kinds[CLIB_CALL_MAX_ARGS];
        int lim = nargs < CLIB_CALL_MAX_ARGS ? nargs : CLIB_CALL_MAX_ARGS;
        for (int i = 0; i < CLIB_CALL_MAX_ARGS; i++) arg_kinds[i] = TYPE_I32;
        if (clib_sym) {
            for (int i = 0; i < clib_sym->clib_func_count; i++) {
                if (clib_sym->clib_func_names[i] &&
                    strcmp(clib_sym->clib_func_names[i], methname) == 0) {
                    TypeInfo* rt = clib_sym->clib_func_return_types
                                       ? clib_sym->clib_func_return_types[i] : NULL;
                    ret_kind = rt ? (int)rt->kind : (int)TYPE_I32;
                    if (clib_sym->clib_func_param_types && clib_sym->clib_func_param_types[i]) {
                        for (int j = 0; j < lim; j++) {
                            TypeInfo* pt = clib_sym->clib_func_param_types[i][j];
                            if (pt) arg_kinds[j] = (int)pt->kind;
                        }
                    }
                    break;
                }
            }
        }

        int base = reg_alloc_block(gen, 2 + nargs);
        SymRef* lib = &ast->u.module_call.lib_ref;
        if (lib->name && (lib->kind == SYM_LOCAL || lib->kind == SYM_PARAM)) {
            emit_mov(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, base, lib->index, ast->line);
        } else {
            emit_loadnil_to(gen, base, ast->line);
        }
        int nc = make_constant(gen, val_obj((Object*)str_copy(methname, (int)strlen(methname))));
        emit_loadk_to(gen, base + 1, nc, ast->line);
        for (int i = 0; i < nargs; i++) {
            gen_expr_to(gen, ast->u.module_call.args.items[i], base + 2 + i);
        }

        reg_encode_iABC(gen->chunk, OP_CLIB_CALL, base, 0, nargs, ast->line);
        chunk_write(gen->chunk, (uint8_t)(ret_kind & 0xFF), ast->line);
        for (int i = 0; i < lim; i++) {
            chunk_write(gen->chunk, (uint8_t)(arg_kinds[i] & 0xFF), ast->line);
        }
        if (base != dst) emit_mov(gen, dst, base, ast->line);
        reg_free_block(gen, base);
        return;
    }

    // 先查 import 登记表（编译期 gen_import 记下的"别名 → 真实名 + 是否原生"）；
    // 没有登记（例如模块内 use 来的）再探测原生模块注册表。
    int alias_found = 0;
    const char* real_name = modname;
    int is_native_mod = codegen_module_lookup(gen, modname, &real_name, &alias_found);
    if (!alias_found) {
        is_native_mod = (native_init_module(modname) == 0);
        real_name = modname;
    }


    if (!is_native_mod) {
        // --- .leno 模块成员调用 ---
        int base = reg_alloc_block(gen, nargs + 1);

        SymRef* lib = &ast->u.module_call.lib_ref;
        if (lib->name && (lib->kind == SYM_LOCAL || lib->kind == SYM_PARAM)) {
            emit_mov(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, base, lib->index, ast->line);
        } else {
            // 兜底：按模块名（别名）在全局作用域查
            Symbol* sym = scope_resolve(gen->sem->root_scope, modname);
            if (sym && sym->kind == SYM_GLOBAL) {
                emit_getglobal_to(gen, base, sym->index, ast->line);
            } else if (sym && sym->kind == SYM_MODULE) {
                reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, base, sym->index, ast->line);
            } else if (sym && (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM)) {
                emit_mov(gen, base, sym->index, ast->line);
            } else {
                emit_loadnil_to(gen, base, ast->line);
            }
        }

        // callee = 模块对象[方法名]
        int ireg = reg_alloc(gen);
        int cidx = make_constant(gen, val_obj((Object*)str_copy(methname, (int)strlen(methname))));
        emit_loadk_to(gen, ireg, cidx, ast->line);
        reg_encode_iABC(gen->chunk, OP_INDEX, base, base, ireg, ast->line);
        reg_free(gen, ireg);

        for (int i = 0; i < nargs; i++) {
            gen_expr_to(gen, ast->u.module_call.args.items[i], base + 1 + i);
        }
        emit_call(gen, base, nargs, 1, ast->line);
        if (base != dst) emit_mov(gen, dst, base, ast->line);
        reg_free_block(gen, base);
        return;
    }

    // --- ffi.callback(func, CfuncName)：用 cfunc 声明式签名创建 FFI 回调 ---
    //   第二个实参是**类型名**（cfunc 声明），不是值 ⇒ 不能当普通实参求值。
    if (strcmp(real_name, "ffi") == 0 && strcmp(methname, "callback") == 0 && nargs == 2) {
        Ast* second = ast->u.module_call.args.items[1];
        Symbol* cfunc_sym = NULL;
        if (second && second->kind == AST_VAR && second->u.var.name) {
            cfunc_sym = scope_resolve(gen->sem->current, second->u.var.name);
            if (!cfunc_sym) cfunc_sym = scope_resolve(gen->sem->root_scope, second->u.var.name);
        }
        if (cfunc_sym && cfunc_sym->type && cfunc_sym->type->kind == TYPE_CFUNC) {
            // FFIType 编码（与栈式 op_cfunc_callback.inc / ffi 模块口径一致）
            #define CFUNC_FFI_TYPE(_t) \
                ((_t)->kind == TYPE_NULL ? 0 : \
                 (_t)->kind == TYPE_F32 ? 10 : \
                 ((_t)->kind == TYPE_F64 || (_t)->kind == TYPE_FLOAT) ? 2 : \
                 ((_t)->kind == TYPE_PTR || (_t)->kind == TYPE_PTR_GENERIC || \
                  (_t)->kind == TYPE_STR8 || (_t)->kind == TYPE_STR16) ? 3 : \
                 (_t)->kind == TYPE_BOOL ? 11 : \
                 (_t)->kind == TYPE_I8 ? 5 : (_t)->kind == TYPE_U8 ? 4 : \
                 (_t)->kind == TYPE_I16 ? 7 : (_t)->kind == TYPE_U16 ? 6 : \
                 (_t)->kind == TYPE_I32 ? 9 : (_t)->kind == TYPE_U32 ? 8 : 1)
            int ffi_ret_type = cfunc_sym->cfunc_return_type
                                   ? CFUNC_FFI_TYPE(cfunc_sym->cfunc_return_type) : 0;
            int pcnt = cfunc_sym->cfunc_param_count;
            if (pcnt > 12) pcnt = 12;

            int base = reg_alloc(gen);
            gen_expr_to(gen, ast->u.module_call.args.items[0], base);
            reg_encode_iABC(gen->chunk, OP_CFUNC_CALLBACK, base, 0, 0, ast->line);
            chunk_write(gen->chunk, (uint8_t)ffi_ret_type, ast->line);
            chunk_write(gen->chunk, (uint8_t)pcnt, ast->line);
            for (int i = 0; i < pcnt; i++) {
                TypeInfo* pt = cfunc_sym->cfunc_param_types ? cfunc_sym->cfunc_param_types[i] : NULL;
                chunk_write(gen->chunk, (uint8_t)(pt ? CFUNC_FFI_TYPE(pt) : 1), ast->line);
            }
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free(gen, base);
            #undef CFUNC_FFI_TYPE
            return;
        }
    }

    // --- 原生模块调用 ---
    int base = reg_alloc_block(gen, nargs + 1);

    // ★ 必须先求值实参，再发射 OP_MODULE_CALL —— 指令在执行期直接从
    //   R[base+1..] 取参，若指令先发射、实参后写入，读到的是上一轮的旧值。
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, ast->u.module_call.args.items[i], base + 1 + i);
    }

    // ★ 用**真实模块名**（别名可能被 as 改过，而 native 方法表按注册名查找）
    const char* mod = real_name;
    const char* meth = methname;
    int mlen = (int)strlen(mod);
    int flen = (int)strlen(meth);

    ObjString* combo = str_alloc(mlen + 1 + flen);
    if (combo) {
        memcpy(combo->chars, mod, (size_t)mlen);
        combo->chars[mlen] = '\0';
        memcpy(combo->chars + mlen + 1, meth, (size_t)flen);
        combo->chars[mlen + 1 + flen] = '\0';
        combo->len = mlen + 1 + flen;
        combo->hash = hash_string(combo->chars, combo->len);
    }
    int cidx = make_constant(gen, val_obj((Object*)combo));

    if (cidx == 0 || cidx > 255) {
        reg_encode_iABC(gen->chunk, OP_MODULE_CALL, base, 0, nargs, ast->line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, cidx, ast->line);
    } else {
        reg_encode_iABC(gen->chunk, OP_MODULE_CALL, base, cidx, nargs, ast->line);
    }

    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
}

// ============================================================================
// struct 初始化
// ============================================================================

// TypeInfo → 泛型实参名（与栈式 value_to_generic_type_name 的口径一致）
static const char* typeinfo_to_name(TypeInfo* t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_ANY:    return "any";
        case TYPE_STRUCT: return t->struct_name ? t->struct_name : "struct";
        default:          return "unknown";
    }
}

void gen_struct_init(CodeGen* gen, Ast* ast, int dst) {
    int n = ast->u.struct_init.field_count;
    int base = reg_alloc_block(gen, n + 1);

    // 构造实参 → R[base+1 .. base+n]
    for (int i = 0; i < n; i++) {
        gen_expr_to(gen, ast->u.struct_init.field_values[i], base + 1 + i);
    }

    ObjString* sname = str_copy(ast->u.struct_init.struct_name,
                                (int)strlen(ast->u.struct_init.struct_name));
    int name_const = make_constant(gen, val_obj((Object*)sname));

    reg_encode_iABC(gen->chunk, OP_STRUCT_INIT, base, n, 0, ast->line);

    // --- 紧随数据 ---
    // 名字常量
    chunk_write(gen->chunk, (uint8_t)((name_const >> 8) & 0xFF), ast->line);
    chunk_write(gen->chunk, (uint8_t)(name_const & 0xFF), ast->line);

    // 泛型实参
    int gc = ast->u.struct_init.generic_type_count;
    chunk_write(gen->chunk, (uint8_t)(gc & 0xFF), ast->line);
    for (int i = 0; i < gc; i++) {
        const char* tn = typeinfo_to_name(ast->u.struct_init.generic_type_args
                                              ? ast->u.struct_init.generic_type_args[i] : NULL);
        int tc = make_constant(gen, val_obj((Object*)str_copy(tn, (int)strlen(tn))));
        chunk_write(gen->chunk, (uint8_t)((tc >> 8) & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(tc & 0xFF), ast->line);
    }

    // 每个实参对应字段名（0 = 按位置 i 赋值）
    for (int i = 0; i < n; i++) {
        const char* fname = ast->u.struct_init.field_names ? ast->u.struct_init.field_names[i] : NULL;
        int fc = 0;
        if (fname && fname[0]) {
            fc = make_constant(gen, val_obj((Object*)str_copy(fname, (int)strlen(fname))));
        }
        chunk_write(gen->chunk, (uint8_t)((fc >> 8) & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(fc & 0xFF), ast->line);
    }

    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
}

// ============================================================================
// 安全访问（?.）
// ============================================================================

void gen_safe_access(CodeGen* gen, Ast* ast, int dst) {
    // obj?.field / obj?.method(args)
    //   R[obj]    = 求值对象
    //   R[isnull] = IS_NULL R[obj]
    //   JMP_IF_TRUE R[isnull] → null 路径
    //   非 null 路径：GET_FIELD（或 GET_METHOD + 实参 + CALL）→ R[dst]
    //   JMP → end
    //   null 路径：R[dst] = null
    //   end:
    int line = ast->line;
    int obj_reg = gen_expr(gen, ast->u.safe_access.obj);

    int isnull = reg_alloc(gen);
    emit_is_null(gen, isnull, obj_reg, line);
    int jmp_null = emit_jmp_if_true(gen, isnull, line);
    reg_free(gen, isnull);

    // --- 非 null 路径 ---
    if (!ast->u.safe_access.is_call) {
        int field_idx = ast->u.safe_access.field_index;
        TypeInfo* ot = infer_expr_type(gen->sem, ast->u.safe_access.obj);
        int use_field_op = (ot && (ot->kind == TYPE_STRUCT || ot->kind == TYPE_CSTRUCT)
                            && field_idx >= 0);
        if (use_field_op) {
            reg_encode_iABC(gen->chunk, OP_GET_FIELD, dst, obj_reg, field_idx, line);
        } else {
            const char* fname = ast->u.safe_access.name;
            int ireg = reg_alloc(gen);
            int c = make_constant(gen, val_obj((Object*)str_copy(
                                      fname ? fname : "", fname ? (int)strlen(fname) : 0)));
            emit_loadk_to(gen, ireg, c, line);
            reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, ireg, line);
            reg_free(gen, ireg);
        }
    } else {
        int nargs = ast->u.safe_access.args.count;
        const char* mname = ast->u.safe_access.name ? ast->u.safe_access.name : "";
        // callee + 实参必须连号（VM 按 A+i 取参），整块分配后把对象搬进来
        int base = reg_alloc_block(gen, nargs + 1);
        if (base != obj_reg) emit_mov(gen, base, obj_reg, line);

        int name_const = make_constant(gen, val_obj((Object*)str_copy(mname, (int)strlen(mname))));
        if (name_const == 0 || name_const > 255) {
            reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, 0, line);
            reg_encode_iAx(gen->chunk, OP_EXTRAARG, name_const, line);
        } else {
            reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, name_const, line);
        }
        for (int i = 0; i < nargs; i++) {
            gen_expr_to(gen, ast->u.safe_access.args.items[i], base + 1 + i);
        }
        emit_call(gen, base, nargs, 1, line);
        if (base != dst) emit_mov(gen, dst, base, line);
        reg_free_block(gen, base);
    }

    int jmp_end = emit_jmp(gen, line);

    // --- null 路径 ---
    patch_jmp(gen, jmp_null);
    emit_loadnil_to(gen, dst, line);

    patch_jmp(gen, jmp_end);
    reg_free(gen, obj_reg);
}

// ============================================================================
// 辅助：非 null 跳转（用于空合并）
// ============================================================================

// 临时实现：JMP_IF_TRUE
int emit_jmp_if_true_ex(CodeGen* gen, int a, int line) {
    return emit_jmp_if_true(gen, a, line);
}
