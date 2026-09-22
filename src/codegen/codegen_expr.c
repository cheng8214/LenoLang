// ============================================================================
// 寄存器式 codegen：表达式生成
// 核心接口：gen_expr_to(gen, ast, dst) → 结果写入 R[dst]
//           gen_expr(gen, ast) → 借临时寄存器，返回寄存器号
// ============================================================================

#include "codegen.h"

// 前向声明（语句相关，定义在 codegen_stmt.c）
extern void gen_default_value(CodeGen* gen, Ast* default_expr);
// 语义侧：按别名查导入模块信息（跨模块调用的默认参数补齐要用，见 gen_module_call）
extern ImportedModuleInfo* find_imported_module(Semantic* s, const char* alias);

// ============================================================================
// 从"默认参数文本"求值到 R[dst]
// ----------------------------------------------------------------------------
// 跨模块调用专用：被调函数的 AST 在**另一个模块**里，本模块只有符号表里记的
// `param_default_texts`（源码文本），所以只能按字面量解析（与栈式的
// gen_default_value_from_text 同口径）：数字 / "字符串"（含转义）/ true / false / null。
// 认不出的（常量表达式、别的模块的标识符…）退回 null —— 跨模块拿不到求值环境，
// 栈式同样只认字面量；至少不会往实参寄存器里塞垃圾。
// ============================================================================
static void gen_default_value_from_text_to(CodeGen* gen, int dst, const char* text, int line) {
    if (!text) { emit_loadnil_to(gen, dst, line); return; }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    if (!*text) { emit_loadnil_to(gen, dst, line); return; }

    if (strcmp(text, "null") == 0)  { emit_loadnil_to(gen, dst, line); return; }
    if (strcmp(text, "true") == 0)  { emit_loadtrue_to(gen, dst, line); return; }
    if (strcmp(text, "false") == 0) { emit_loadfalse_to(gen, dst, line); return; }

    // 字符串字面量（文本自带引号）
    size_t tlen = strlen(text);
    if (text[0] == '"' && tlen >= 2 && text[tlen - 1] == '"') {
        char* buf = (char*)malloc(tlen);   // 反转义只会变短
        if (!buf) { emit_loadnil_to(gen, dst, line); return; }
        int bi = 0;
        for (size_t i = 1; i + 1 < tlen; i++) {
            char ch = text[i];
            if (ch == '\\' && i + 2 < tlen) {
                char nx = text[++i];
                switch (nx) {
                    case 'n':  buf[bi++] = '\n'; break;
                    case 't':  buf[bi++] = '\t'; break;
                    case 'r':  buf[bi++] = '\r'; break;
                    case '"':  buf[bi++] = '"';  break;
                    case '\\': buf[bi++] = '\\'; break;
                    default:   buf[bi++] = nx;   break;
                }
            } else {
                buf[bi++] = ch;
            }
        }
        ObjString* s = str_copy(buf, bi);
        free(buf);
        emit_loadk_to(gen, dst, make_constant(gen, val_obj((Object*)s)), line);
        return;
    }

    // 数字（带符号/小数/科学计数）
    char* endp = NULL;
    double d = strtod(text, &endp);
    if (endp && endp != text) {
        int is_float = (strchr(text, '.') != NULL || strchr(text, 'e') != NULL ||
                        strchr(text, 'E') != NULL);
        Value v = is_float ? val_float(d) : val_int((int64_t)d);
        emit_loadk_to(gen, dst, make_constant(gen, v), line);
        return;
    }
    emit_loadnil_to(gen, dst, line);
}

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

// ============================================================================
// 「比较 + 条件跳转」融合（T10-①）
// ----------------------------------------------------------------------------
// `if x <= 1 {` / `while i < n {` 原先要两条指令：`*_INT[_IMM]` 比较 + `JMP_IF_FALSE`。
// 这里在**条件位置**直接把比较合进跳转（与栈式 codegen 的 try_emit_cmpjmp 对齐）：
// 省掉的是一条 OP_JMP_IF_FALSE 的**派发**（取指 + 行号写回 + 间接跳转），
// 而且不占结果寄存器（原路径要先算进一个临时寄存器）。
// 只处理**有序比较**（< <= > >=）且满足：
//   ① 两侧静态类型都是 int —— 与 `*_INT / *_INT_IMM` 的发射条件完全一致，
//      所以语义等价（int 快路径只排除 null、不做类型检查，见 VM 侧说明）；
//   ② 左操作数是普通局部变量/参数（SYM_LOCAL / SYM_PARAM ⇒ 就是寄存器 R[index]；
//      upvalue / 全局 / 模块需要真正的取值指令，不走这条）；
//   ③ 右操作数是 int 字面量（∈[-128,127] ⇒ int8 立即数形式）或另一个局部变量/参数。
// 成功返回跳转偏移的回填位置（供 patch_jmp 用），失败返回 -1（调用方走原路径）。
int try_emit_cmpjmp(CodeGen* gen, Ast* cond, int line) {
    // 诊断 / 基准开关：`LENO_NO_CMPJMP=1` 关掉融合 —— **同一份二进制**里就能做 A/B，
    // 不必重新构建两次（跨构建比较会被机器频率漂移污染，实测被误导过一次）。
    // 与栈式的 `LENO_NO_CMPJMP` 同名同义（见其 codegen_stmt.c 的 try_emit_cmpjmp）。
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("LENO_NO_CMPJMP") ? 1 : 0;
    if (disabled) return -1;

    if (!cond || cond->kind != AST_BINOP) return -1;
    Ast* l = cond->u.binop.l;
    Ast* r = cond->u.binop.r;
    if (!l || !r) return -1;
    if (!l->cached_type || !r->cached_type) return -1;
    if (l->cached_type->kind != TYPE_INT || r->cached_type->kind != TYPE_INT) return -1;

    OpCode op;
    switch (cond->u.binop.op) {
        case TOK_LT: op = OP_CMPJMP_LT; break;
        case TOK_LE: op = OP_CMPJMP_LE; break;
        case TOK_GT: op = OP_CMPJMP_GT; break;
        case TOK_GE: op = OP_CMPJMP_GE; break;
        default: return -1;
    }

    // 左操作数：普通局部变量 / 参数
    if (l->kind != AST_VAR) return -1;
    SymRef* lref = &l->u.var.ref;
    if ((lref->kind != SYM_LOCAL && lref->kind != SYM_PARAM) || lref->index < 0) return -1;
    int a = lref->index;

    // 右操作数：int 字面量（立即数形式）
    if (r->kind == AST_NUM && !r->u.num.is_float && !r->u.num.is_bigint) {
        double dv = r->u.num.value;
        if (dv >= -128.0 && dv <= 127.0) {
            return emit_cmpjmp(gen, op, a, (int)dv, 1, line);
        }
        return -1;      // 立即数超出 int8 ⇒ 交给原路径
    }

    // 右操作数：另一个局部变量 / 参数（寄存器形式）
    if (r->kind == AST_VAR) {
        SymRef* rref = &r->u.var.ref;
        if ((rref->kind == SYM_LOCAL || rref->kind == SYM_PARAM) && rref->index >= 0) {
            return emit_cmpjmp(gen, op, a, rref->index, 0, line);
        }
    }
    return -1;
}

// ============================================================================
// 语句位置的 `arr.add(x)`（T10-②）
// ----------------------------------------------------------------------------
// 作为**独立语句**时结果没人要：通用路径要 `MOV 接收者 + GET_METHOD + CALL`，
// 表达式位置的融合还要多一次"把新长度搬回 dst"的 MOV。这里直接发
// `OP_ARRAY_APPEND(..., need_result=0)` —— 既不写回长度也不搬寄存器（VM 侧按 C 位跳过
// 写回），等价于栈式的 `OP_ARRAY_APPEND_NOPUSH`，而且不必新增 opcode。
// 只处理本引擎 `a.add(x)` 的两种 AST 形态（与 gen_method_call 的两个调用点同源）：
//   AST_CALL + callee = AST_FIELD_ACCESS（字段名 "add"）
//   AST_CALL + callee = AST_INDEX（字符串下标 "add"）
// 且接收者静态类型必须是数组、实参恰 1 个；否则返回 0，交回原路径（语义不变）。
int try_emit_stmt_array_add(CodeGen* gen, Ast* e) {
    if (!e || e->kind != AST_CALL) return 0;
    Ast* callee = e->u.call.callee;
    if (!callee || e->u.call.args.count != 1) return 0;
    Ast* obj = NULL;
    if (callee->kind == AST_FIELD_ACCESS) {
        const char* nm = callee->u.field_access.field_name;
        if (!nm || strcmp(nm, "add") != 0) return 0;
        obj = callee->u.field_access.obj;
    } else if (callee->kind == AST_INDEX && callee->u.index.index &&
               callee->u.index.index->kind == AST_STRING) {
        const char* nm = callee->u.index.index->u.string.value;
        if (!nm || strcmp(nm, "add") != 0) return 0;
        obj = callee->u.index.obj;
    } else {
        return 0;
    }
    if (!obj) return 0;
    TypeInfo* rt = infer_expr_type(gen->sem, obj);
    int is_arr = (rt && rt->kind == TYPE_ARRAY);
    if (rt) type_free(rt);
    if (!is_arr) return 0;
    gen_array_add(gen, obj, e->u.call.args.items[0], 0, e->line);
    return 1;
}

int gen_expr(CodeGen* gen, Ast* ast) {
    int r = reg_alloc(gen);
    gen_expr_to(gen, ast, r);
    return r;
}

// 大数组字面量的兜底生成：R[dst] = 空数组，再逐个 APPEND 元素。
// ----------------------------------------------------------------------------
// 什么时候需要它：连号块（R[base+1 .. base+n] + NEWARRAY count）有两个硬上限 ——
//   ① count 只在 iABC 的 C 字段（8 位）：n ≥ 256 被截断成 n & 0xFF。
//      实测 `Array[int] SBOX = [256 项]` 得到一个 **空数组**，AES 用例里
//      `SBOX[t0]` 直接报「数组索引越界: 索引 207, 长度 0」；
//   ② 寄存器号是 8 位（MAX_REG = 256）：base + n 越过 255 时，后面的元素会被
//      截断写到**别的槽位**（静默踩变量，比报错更难查）。
// 增量路径只需 1 个临时寄存器、指令数与元素数同阶，任意长度都成立；
// 由于数组始终在 R[dst]（GC 可见的寄存器）里，也不必担心 GC 半成品。
static void gen_array_literal_append(CodeGen* gen, Ast* ast, int n, int dst) {
    reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, 0, ast->line);
    for (int i = 0; i < n; i++) {
        int r = gen_expr(gen, ast->u.array.items[i]);
        // ARRAY_APPEND: 把 R[A] 追加到 R[B]；R[A] 会被写成长度（用完即弃，随即释放）
        reg_encode_iABC(gen->chunk, OP_ARRAY_APPEND, r, dst, 0, ast->line);
        reg_free(gen, r);
    }
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
            // ★ 对象/下标是**普通局部变量/参数**时不必搬进临时寄存器（与字段访问、右值直取同一手法：
            //   实测光线追踪 Phase A 的 `spheres[si]` 每轮白搬一次下标）。安全性：OP_INDEX 是
            //   "先读 R[B]、R[C]，再写 R[A]"，即使 dst 恰好是对象或下标自己（`x = a[x]`）也安全。
            Ast* iobj = ast->u.index.obj;
            Ast* iidx = ast->u.index.index;
            int obj_reg, idx_reg;
            int obj_is_temp = 1, idx_is_temp = 1;
            if (iobj && iobj->kind == AST_VAR) {
                SymRef* r0 = &iobj->u.var.ref;
                if ((r0->kind == SYM_LOCAL || r0->kind == SYM_PARAM) && r0->index >= 0) {
                    obj_reg = r0->index; obj_is_temp = 0;
                } else { obj_reg = gen_expr(gen, iobj); }
            } else { obj_reg = gen_expr(gen, iobj); }
            // INDEX: R[A] = R[B][R[C]]
            // ★ 静态类型特化：R[B] 是 Array[int]/Array[float] 且 R[C] 是 int48 时，
            //   直接发 OP_INDEX_ARRAY_INT/FLOAT（信任静态类型，跳过运行期 obj/下标判型）。
            //   数组被 ROTASET 改结构、或静态类型推断不到 → 回落通用 OP_INDEX 兜底。
            TypeInfo* ot = infer_expr_type(gen->sem, iobj);
            TypeInfo* it = infer_expr_type(gen->sem, iidx);
            int arr_spec = (ot && ot->kind == TYPE_ARRAY && ot->element_type &&
                            (ot->element_type->kind == TYPE_INT ||
                             ot->element_type->kind == TYPE_FLOAT) &&
                            it && it->kind == TYPE_INT);
            // ★ 立即数下标：数组已特化 + 下标是 [-128,127] 整数字面量 ⇒ **不必求值下标**，
            //   也不必为它分配/装载一个寄存器（Lua 的 GETI 就是把这个小下标编在指令里）。
            //   动机：`arr[0]` 原是「LOADI tmp,0 + INDEX_ARRAY_INT」两条派发，现为一条。
            //   触发条件与 OP_INDEX_ARRAY_IMM 的语义严格对齐（见 leno_vm.h 该 opcode 的说明）。
            int idx_imm = 0, idx_imm_val = 0;
            if (arr_spec && iidx && iidx->kind == AST_NUM && !iidx->u.num.is_float &&
                !iidx->u.num.is_bigint) {
                double dv = iidx->u.num.value;
                if (dv >= -128.0 && dv <= 127.0 && dv == (double)(int)dv) {
                    idx_imm = 1;
                    idx_imm_val = (int)dv;
                }
            }
            if (idx_imm) {
                // 没求值 ⇒ 没分配 ⇒ 下面**不能** reg_free（沿用 gen_binary 立即数路径的约定：
                // 跳过求值就必须跳过释放，否则会去 free 一个不属于本表达式的寄存器）
                idx_reg = 0;
                idx_is_temp = 0;
            } else if (iidx && iidx->kind == AST_VAR) {
                SymRef* r1 = &iidx->u.var.ref;
                if ((r1->kind == SYM_LOCAL || r1->kind == SYM_PARAM) && r1->index >= 0) {
                    idx_reg = r1->index; idx_is_temp = 0;
                } else { idx_reg = gen_expr(gen, iidx); }
            } else { idx_reg = gen_expr(gen, iidx); }
            if (arr_spec) {
                if (idx_imm) {
                    reg_encode_iABC(gen->chunk, OP_INDEX_ARRAY_IMM, dst, obj_reg,
                                    (int)((uint8_t)(int8_t)idx_imm_val), ast->line);
                } else {
                    int op = (ot->element_type->kind == TYPE_INT) ? OP_INDEX_ARRAY_INT : OP_INDEX_ARRAY_FLOAT;
                    reg_encode_iABC(gen->chunk, op, dst, obj_reg, idx_reg, ast->line);
                }
                if (idx_is_temp) reg_free(gen, idx_reg);
                if (obj_is_temp) reg_free(gen, obj_reg);
                break;
            }
            reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, idx_reg, ast->line);
            if (idx_is_temp) reg_free(gen, idx_reg);
            if (obj_is_temp) reg_free(gen, obj_reg);
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
            // ★ 对象是**普通局部变量/参数**时不必搬进临时寄存器：原先每个字段访问都先来一条
            //   `OP_MOV`（实测光线追踪 Phase A 的 `s.cx + s.cy + s.cz + s.r` 每轮白搬 4 次，
            //   是我们比栈式慢 1.31x 的主因之一）。与 gen_binop 的 rl_direct / 右值直取同一手法：
            //   对象表达式只读、无副作用，且该寄存器在 next_reg 之下（分配器不会当临时寄存器复用）。
            Ast* obj_ast = ast->u.field_access.obj;
            int obj_reg;
            int obj_is_temp = 1;
            if (obj_ast && obj_ast->kind == AST_VAR) {
                SymRef* oref = &obj_ast->u.var.ref;
                if ((oref->kind == SYM_LOCAL || oref->kind == SYM_PARAM) && oref->index >= 0) {
                    obj_reg = oref->index;
                    obj_is_temp = 0;
                } else {
                    obj_reg = gen_expr(gen, obj_ast);
                }
            } else {
                obj_reg = gen_expr(gen, obj_ast);
            }
            TypeInfo* ot = infer_expr_type(gen->sem, ast->u.field_access.obj);
            int field_idx = ast->u.field_access.field_index;
            int use_field_op = (ot && (ot->kind == TYPE_STRUCT || ot->kind == TYPE_CSTRUCT)
                                && field_idx >= 0);

            if (use_field_op) {
                if (ot->kind == TYPE_STRUCT) {
                    // GET_FIELD_FAST: R[A] = R[B].field(C)
                    // ★ 静态类型已是 struct（**非** cstruct）⇒ 走特化版：省掉 cstruct 分流
                    //   与 struct_get_field() 的跨 TU 调用（其内部还重复一次越界检查）。
                    //   ⚠ 只在静态类型确定是 struct 时发；cstruct 必须走通用版（数组/嵌套字段
                    //   要返回视图），静态类型存疑（any/face/收窄后）也一律走通用版。
                    reg_encode_iABC(gen->chunk, OP_GET_FIELD_FAST, dst, obj_reg, field_idx, ast->line);
                } else {
                    // GET_FIELD: R[A] = R[B].field(C)
                    reg_encode_iABC(gen->chunk, OP_GET_FIELD, dst, obj_reg, field_idx, ast->line);
                }
            } else {
                const char* fname = ast->u.field_access.field_name;
                int ireg = reg_alloc(gen);
                int c = make_constant(gen, val_obj((Object*)str_copy(
                                          fname ? fname : "", fname ? (int)strlen(fname) : 0)));
                emit_loadk_to(gen, ireg, c, ast->line);
                reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, ireg, ast->line);
                reg_free(gen, ireg);
            }
            if (obj_is_temp) reg_free(gen, obj_reg);
            break;
        }

        // --- 数组字面量 ---
        // ★ 元素必须落在 R[base+1 .. base+n]（NEWARRAY 按 A+i 读取），
        //   而 base **绝不能直接取 dst** —— dst 常常是**变量槽**（`loopArr = [s]`），
        //   那样元素就会写进**相邻变量**的槽里 ✗（实测 `int acc` 紧邻 `loopArr` 时
        //   被 `loopArr = [s]` 踩成那个 struct）。一律在**临时块**里拼，再 MOV 回 dst。
        //
        // ★★ 连号块有两个硬上限，越界必须换路子（见 gen_array_literal_append）：
        //   ① NEWARRAY 的 count 编在 iABC 的 C 字段（**8 位**）⇒ n ≥ 256 时被截断，
        //      实测 256 元素的 `Array[int] SBOX = [..256 项..]` 直接变成**空数组**
        //      （test_gc_safepoint 的 AES 表全废 ⇒ `SBOX[t0]` 报"索引越界: 索引 207, 长度 0"）；
        //   ② 元素寄存器号是 8 位（MAX_REG=256）⇒ base+n ≥ 256 时后面的元素写到了别的槽位。
        case AST_ARRAY:
        {
            int n = ast->u.array.count;
            if (n == 0) {
                reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, 0, ast->line);
                break;
            }
            if (n > 255 || gen->next_reg + n + 1 > MAX_REG) {
                gen_array_literal_append(gen, ast, n, dst);
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
            // 同数组：count 只有 8 位、连号块越不过 256 号寄存器 ⇒ 超限走逐条 DICT_SET
            if (n > 255 || gen->next_reg + n * 2 + 1 > MAX_REG) {
                reg_encode_iABC(gen->chunk, OP_NEWDICT, dst, 0, 0, ast->line);
                for (int i = 0; i < n; i++) {
                    int kreg = gen_expr(gen, ast->u.dict.entries[i].key);
                    int vreg = gen_expr(gen, ast->u.dict.entries[i].value);
                    // DICT_SET: R[B][R[C]] = R[A]
                    reg_encode_iABC(gen->chunk, OP_DICT_SET, vreg, dst, kreg, ast->line);
                    reg_free(gen, vreg);
                    reg_free(gen, kreg);
                }
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

// dst 是否可能被 rhs 的求值读到（"别名风险"探测）
// ----------------------------------------------------------------------------
// 背景：gen_binop 的惯用写法是"**先把 lhs 写进 dst**，再求 rhs，最后把结果写回 dst"，
//   而 dst 常常就是**赋值目标的变量槽位**（gen_assign → gen_expr_to(value, 目标槽位)）。
//   于是 rhs 里若读到同一个变量，读到的就是刚写进去的 lhs：
//     `b = a % b` → 5 % 5 = 0（应为 5）
//     `k = j + k` → 5 + 5 = 10（应为 31）
//     `n = m - n` → 10 - 10 = 0（应为 7）
//   全是**静默错值**（栈式按压栈求值天然没有这个问题，所以差分对照才暴露出来）。
//
// 探测口径：表达式只会读**变量槽位**（自己的临时寄存器由 reg_alloc 保证不与人冲突），
//   所以只需看 rhs 子树里有没有"索引等于 slot 的局部/参数引用"。
//   认得的节点精确递归；认不得的节点一律返回 1（保守 → 走安全路径，只多占一个临时寄存器）。
static int ast_may_read_slot(Ast* ast, int slot) {
    if (!ast) return 0;
    switch (ast->kind) {
        case AST_NUM: case AST_STRING: case AST_BOOL: case AST_NULL:
            return 0;                       // 常量叶子：不读变量
        case AST_VAR: {
            SymRef* r = &ast->u.var.ref;
            return (r->kind == SYM_LOCAL || r->kind == SYM_PARAM) && r->index == slot;
        }
        case AST_BINOP:
            return ast_may_read_slot(ast->u.binop.l, slot) ||
                   ast_may_read_slot(ast->u.binop.r, slot);
        case AST_UNARY:
            return ast_may_read_slot(ast->u.unary.operand, slot);
        case AST_INDEX:
            return ast_may_read_slot(ast->u.index.obj, slot) ||
                   ast_may_read_slot(ast->u.index.index, slot);
        case AST_SLICE:
            return ast_may_read_slot(ast->u.slice.obj, slot) ||
                   ast_may_read_slot(ast->u.slice.start, slot) ||
                   ast_may_read_slot(ast->u.slice.end, slot);
        case AST_FIELD_ACCESS:
            return ast_may_read_slot(ast->u.field_access.obj, slot);
        case AST_TYPE_CHECK:
            return ast_may_read_slot(ast->u.type_check.expr, slot);
        case AST_ADDRESS_OF:
            return ast_may_read_slot(ast->u.address_of.operand, slot);
        case AST_EXPR_STMT:
            return ast_may_read_slot(ast->u.expr_stmt.expr, slot);
        case AST_CALL: {
            if (ast_may_read_slot(ast->u.call.callee, slot)) return 1;
            for (int i = 0; i < ast->u.call.args.count; i++) {
                if (ast_may_read_slot(ast->u.call.args.items[i], slot)) return 1;
            }
            return 0;
        }
        case AST_MODULE_CALL: {
            for (int i = 0; i < ast->u.module_call.args.count; i++) {
                if (ast_may_read_slot(ast->u.module_call.args.items[i], slot)) return 1;
            }
            return 0;
        }
        case AST_ARRAY: {
            for (int i = 0; i < ast->u.array.count; i++) {
                if (ast_may_read_slot(ast->u.array.items[i], slot)) return 1;
            }
            return 0;
        }
        case AST_DICT: {
            for (int i = 0; i < ast->u.dict.count; i++) {
                if (ast_may_read_slot(ast->u.dict.entries[i].key, slot)) return 1;
                if (ast_may_read_slot(ast->u.dict.entries[i].value, slot)) return 1;
            }
            return 0;
        }
        case AST_INTERP_STRING: {
            for (int i = 0; i < ast->u.interp_string.count; i++) {
                if (ast_may_read_slot(ast->u.interp_string.exprs[i], slot)) return 1;
            }
            return 0;
        }
        case AST_STRUCT_INIT: {
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                if (ast_may_read_slot(ast->u.struct_init.field_values[i], slot)) return 1;
            }
            return 0;
        }
        default:
            return 1;                       // 保守：认不出就当作"会读"
    }
}

// ============================================================================
// 多字段累加融合（对齐栈式 OP_ACC_FIELDS）
// ----------------------------------------------------------------------------
// `s.cx + s.cy + s.cz + s.r` 原路径是 GET_FIELD×4 + ADD_F×3 = 7 条派发
// （实测 examples/性能测试/光线追踪对象版.leno 的 Phase A 内层循环一共只有 12 条指令，
//  其中 7 条都是它），栈式一条 OP_ACC_FIELDS 就够。
//
// 判据（全部编译期可判定，任一不满足就走原路径）：
//   ① 整棵子树是一串 `+`，叶子**全部**是字段访问（不含 `sum` 这类其它项）；
//   ② 所有字段访问的对象是**同一个**局部变量/参数槽位；
//   ③ 每个字段的静态类型都是 float ⇒ 原路径必然是 ADD_F 连加 ⇒ 融合版用
//      「double 左到右累加 + val_float」与之**逐位等价**（见 VM 的 OP_ACC_FIELDS）；
//   ④ 字段数 2..MAX_ACC_FIELDS、字段索引 0..255。
//   ⚠ int 字段/混合类型**绝不能**融合：原路径走 ADD_INT（结果是 int），
//     而融合版结果恒为 float ⇒ 会改变语义。
// ============================================================================
#define MAX_ACC_FIELDS 32

// 把 `+` 链摊平成叶子列表（左叶子在前＝保持原有的左结合求值顺序）；
// 遇到非 `+` 节点即当作叶子。返回 0 表示超出容量（放弃融合）。
static int flatten_plus_chain(Ast* ast, Ast** leaves, int* n, int max) {
    if (!ast) return 0;
    if (ast->kind == AST_BINOP && ast->u.binop.op == TOK_PLUS) {
        if (!flatten_plus_chain(ast->u.binop.l, leaves, n, max)) return 0;
        return flatten_plus_chain(ast->u.binop.r, leaves, n, max);
    }
    if (*n >= max) return 0;
    leaves[(*n)++] = ast;
    return 1;
}

// 成功（已发射 OP_ACC_FIELDS）返回 1；不满足判据返回 0，调用方继续走原路径。
int try_emit_acc_fields(CodeGen* gen, Ast* ast, int dst) {
    if (!ast || ast->kind != AST_BINOP || ast->u.binop.op != TOK_PLUS) return 0;

    Ast* leaves[MAX_ACC_FIELDS];
    int n = 0;
    if (!flatten_plus_chain(ast, leaves, &n, MAX_ACC_FIELDS)) return 0;
    if (n < 2) return 0;            // 单字段融合反而更贵（原路径就是一条 GET_FIELD）

    int obj_index = -1, obj_kind = -1;
    uint8_t idx[MAX_ACC_FIELDS];
    for (int i = 0; i < n; i++) {
        Ast* f = leaves[i];
        if (!f || f->kind != AST_FIELD_ACCESS) return 0;
        int fi = f->u.field_access.field_index;
        if (fi < 0 || fi > 255) return 0;
        Ast* o = f->u.field_access.obj;
        if (!o || o->kind != AST_VAR) return 0;    // 只处理"对象是变量"（与栈式同口径）
        SymRef* r = &o->u.var.ref;
        if (!((r->kind == SYM_LOCAL || r->kind == SYM_PARAM) && r->index >= 0)) return 0;
        if (i == 0) {
            obj_index = r->index;
            obj_kind = (int)r->kind;
        } else if (r->index != obj_index || (int)r->kind != obj_kind) {
            return 0;
        }
        TypeInfo* ft = infer_expr_type(gen->sem, f);
        if (!ft || ft->kind != TYPE_FLOAT) return 0;
        idx[i] = (uint8_t)fi;
    }

    // `dst` 与对象槽位相同也安全：ACC_FIELDS 是"先把字段全读出来、再写 dst"。
    reg_encode_iABC(gen->chunk, OP_ACC_FIELDS, dst, obj_index, n, ast->line);
    for (int i = 0; i < n; i++) chunk_write(gen->chunk, idx[i], ast->line);
    return 1;
}

void gen_binop(CodeGen* gen, Ast* ast, int dst) {
    Ast* lhs = ast->u.binop.l;
    Ast* rhs = ast->u.binop.r;
    LenoTokenType op = ast->u.binop.op;

    // 短路运算
    if (op == TOK_AND) {
        // R[dst] = R[lhs] && R[rhs]
        if (ast_may_read_slot(rhs, dst)) {
            // 别名风险：lhs 先落临时寄存器 —— 否则"先写 dst"会把 rhs 要读的原值覆盖掉
            // （`x = y && x` 会变成 y && y）
            int rl = gen_expr(gen, lhs);
            int jmp_false = emit_jmp_if_false(gen, rl, ast->line);
            gen_expr_to(gen, rhs, dst);
            int jmp_end = emit_jmp(gen, ast->line);
            patch_jmp(gen, jmp_false);
            emit_mov(gen, dst, rl, ast->line);
            patch_jmp(gen, jmp_end);
            reg_free(gen, rl);
            return;
        }
        gen_expr_to(gen, lhs, dst);
        int jmp_false = emit_jmp_if_false(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_false);
        return;
    }
    if (op == TOK_OR) {
        if (ast_may_read_slot(rhs, dst)) {
            int rl = gen_expr(gen, lhs);
            int jmp_true = emit_jmp_if_true(gen, rl, ast->line);
            gen_expr_to(gen, rhs, dst);
            int jmp_end = emit_jmp(gen, ast->line);
            patch_jmp(gen, jmp_true);
            emit_mov(gen, dst, rl, ast->line);
            patch_jmp(gen, jmp_end);
            reg_free(gen, rl);
            return;
        }
        gen_expr_to(gen, lhs, dst);
        int jmp_true = emit_jmp_if_true(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_true);
        return;
    }

    // 空合并 a ?? b：只有 a **是 null** 才求值 b（0 / false / "" 都要保留）
    // ⚠ 不能用 JMP_IF_TRUE（那是真值语义）—— 否则 `0 ?? 99` 会得 99 ✗
    if (op == TOK_NULL_COALESCE) {
        if (ast_may_read_slot(rhs, dst)) {
            int rl = gen_expr(gen, lhs);       // 同上：先落临时寄存器避开别名
            int isnull = reg_alloc(gen);
            emit_is_null(gen, isnull, rl, ast->line);
            int jmp = emit_jmp_if_false(gen, isnull, ast->line);  // 非 null → 跳过右侧
            reg_free(gen, isnull);
            gen_expr_to(gen, rhs, dst);
            int jmp_end = emit_jmp(gen, ast->line);
            patch_jmp(gen, jmp);
            emit_mov(gen, dst, rl, ast->line);
            patch_jmp(gen, jmp_end);
            reg_free(gen, rl);
            return;
        }
        gen_expr_to(gen, lhs, dst);
        int isnull = reg_alloc(gen);
        emit_is_null(gen, isnull, dst, ast->line);
        int jmp = emit_jmp_if_false(gen, isnull, ast->line);  // 非 null → 跳过右侧
        reg_free(gen, isnull);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp);
        return;
    }

    // ★ 多字段累加融合：整棵 `+` 子树都是「同一对象的 float 字段访问」时合成一条
    //   OP_ACC_FIELDS（对齐栈式；实测光线追踪 Phase A 内层 7 条派发 → 1 条）。
    if (op == TOK_PLUS && try_emit_acc_fields(gen, ast, dst)) return;

    // 普通二元运算：求 lhs → dst（有别名风险时 → 临时寄存器），求 rhs → 临时寄存器，
    //   运算把这两个寄存器合到 dst。求值顺序保持"先左后右"不变。
    // ★ 立即数快速路径（与栈式的 SUB_INT_IMM / ADD_INT_IMM 同思路）：
    //   右操作数是 -128..127 的**整数字面量**时，不必先把它求值到寄存器，
    //   直接编成一条立即数指令 —— 省掉「求右值 → LOADI → *_INT」两条指令。
    //   实测 examples/性能测试/经典斐波那契数列测试_int.leno：`n-2` / `n-1` 各 3 条指令，
    //   栈式只要 1 条，这一段是递归基准上指令数差距的一部分。
    int rhs_imm = 0;
    int rhs_imm_val = 0;
    // ⚠ 触发条件必须与"真的会用立即数指令"完全一致，否则会**跳过右值求值却走通用路径**：
    //   ① 只有下面这些运算符有立即数形式（乘除/位运算等仍要按老路把右值求进寄存器 ——
    //      否则 `n <= 1` 会被编成 `OP_LE_INT A=1 B=1 C=0`＝`n <= n` 恒真，fib 直接返回 n）；
    //   ② 左操作数也必须是 int（`big1 + 100` 里左是 bigint ⇒ 只有通用 ADD 能算，
    //      若此时跳过右值求值，通用 ADD 会拿到一个空寄存器 ⇒ 实测 `+100` 静默失效）。
    // ★ TOK_STAR 也走立即数路径（T10-③）：`x * 2` 这类下标/偏移计算很常见，
    //   有 OP_MUL_INT_IMM 后能省掉把字面量装进寄存器的那条 LOADI。
    //   ⚠ 前提仍是"左操作数是 int"（与其它立即数运算同一条件，见上面 ② 的说明：
    //   `big1 * 100` 这类左值是 bigint 的必须走通用 MUL，否则通用路径会读到空寄存器）。
    int imm_ok_op = (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR ||
                     op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE);
    if (imm_ok_op && lhs && lhs->cached_type &&
        lhs->cached_type->kind == TYPE_INT && rhs && rhs->kind == AST_NUM &&
        !rhs->u.num.is_float && !rhs->u.num.is_bigint) {
        double dv = rhs->u.num.value;
        if (dv >= -128.0 && dv <= 127.0) {
            rhs_imm_val = (int)dv;
            rhs_imm = 1;
        }
    }

    // ★ 左操作数是**普通局部变量/参数**时不必把它搬进 dst：运算指令本来就能
    //   "读 R[B]、写 R[A]"（下面所有 *_INT / *_INT_IMM / 立即数比较都是这个形状）。
    //   原先一律 `gen_expr_to(lhs, dst)` 生成一条多余的 OP_MOV ——
    //   实测 fib 每次递归有 3 次这种搬运（比较 `n <= 1`、两个实参表达式 `n-2`/`n-1`）。
    //   安全性：变量无副作用，且它的寄存器在 next_reg 之下（分配器不会当临时寄存器复用）；
    //   SYM_UPVALUE / SYM_GLOBAL / SYM_MODULE 仍走原路（它们需要真正的取值指令）。
    int rl_direct = -1;
    if (lhs && lhs->kind == AST_VAR) {
        SymRef* lref = &lhs->u.var.ref;
        if ((lref->kind == SYM_LOCAL || lref->kind == SYM_PARAM) && lref->index >= 0) {
            rl_direct = lref->index;
        }
    }
    int rl;
    int rl_is_temp = 0;   // 只有"别名安全路径"借来的寄存器才归本函数释放
    if (rl_direct >= 0) {
        rl = rl_direct;               // 变量自己的寄存器：**绝不能** free
    } else if (ast_may_read_slot(rhs, dst)) {
        rl = gen_expr(gen, lhs);
        rl_is_temp = 1;
    } else {
        gen_expr_to(gen, lhs, dst);
        rl = dst;
    }
    // ⚠ 收尾按 `r_is_temp` 决定要不要 free：立即数路径虽然不求右值、不发指令，
    //   也必须补一个占位临时寄存器，否则会去 free 寄存器 0 ⇒ 整个分配器错乱
    //   （表现：单跑测试都过，但 runner 里 351 个用例全挂）。
    // ★ 右操作数是**普通局部变量/参数**时也不必搬进临时寄存器（与左操作数的 rl_direct 同理）：
    //   原先 `a < b` 会多一条 `OP_MOV`（把 b 搬到临时寄存器）。安全性：
    //   ① 变量无副作用，且取值发生在左值求值**之后**（与原来"先左后右"的求值顺序一致）；
    //   ② 运算/比较指令都是"先读两个操作数、再写结果"，即使目标寄存器就是变量自己
    //      （`x = y < x`）也安全；
    //   ③ 该寄存器的生命周期在 next_reg 之下，分配器不会把它当临时寄存器复用
    //      （与 rl_direct 同一论证）。
    int r = 0;
    int r_is_temp = 1;
    if (rhs_imm) {
        r = reg_alloc(gen);          // 占位（收尾按 r_is_temp 释放）
    } else if (rhs && rhs->kind == AST_VAR) {
        SymRef* rref = &rhs->u.var.ref;
        if ((rref->kind == SYM_LOCAL || rref->kind == SYM_PARAM) && rref->index >= 0) {
            r = rref->index;
            r_is_temp = 0;
        } else {
            r = gen_expr(gen, rhs);
        }
    } else {
        r = gen_expr(gen, rhs);
    }

    // ★ 类型特化（与栈式 codegen_expr.c 口径一致）：两侧静态类型都是 int 时
    //   走 int 专用指令。**这不只是性能差异**：int 专用指令把 null 视作 0
    //   （`val_as_int(NULL_VAL) == 0`，TAG_NULL = 0），而通用 ADD 会报
    //   「null 不能参与运算」—— 实测 `merge(o){ return x + o.x }` 里未赋值的
    //   int 字段 x 在栈式得 3、寄存式报错 ✗（test_cross_module_method_args ⑥）。
    TypeKind lt = (lhs && lhs->cached_type) ? lhs->cached_type->kind : TYPE_UNKNOWN;
    TypeKind rt = (rhs && rhs->cached_type) ? rhs->cached_type->kind : TYPE_UNKNOWN;
    int both_int = (lt == TYPE_INT && rt == TYPE_INT);
    // 两侧都是 float ⇒ 有序比较可以走浮点特化（T10-④，省掉 value_compare_stdlib 调用）
    // 诊断/基准开关：`LENO_NO_FLOAT_CMP=1` 关掉它（**同一份二进制**做 A/B，避免重建两次）
    static int float_cmp_disabled = -1;
    if (float_cmp_disabled < 0) float_cmp_disabled = getenv("LENO_NO_FLOAT_CMP") ? 1 : 0;
    int both_float = (!float_cmp_disabled && lt == TYPE_FLOAT && rt == TYPE_FLOAT);

    switch (op) {
        case TOK_PLUS:
            if (both_int && rhs_imm) emit_add_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_add_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_add_f(gen, dst, rl, r, ast->line);
            else emit_add(gen, dst, rl, r, ast->line);
            break;
        case TOK_MINUS:
            if (both_int && rhs_imm) emit_sub_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_sub_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_sub_f(gen, dst, rl, r, ast->line);
            else emit_sub(gen, dst, rl, r, ast->line);
            break;
        case TOK_STAR:
            if (both_int && rhs_imm) emit_mul_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_mul_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_mul_f(gen, dst, rl, r, ast->line);
            else emit_mul(gen, dst, rl, r, ast->line);
            break;
        case TOK_SLASH:
            if (both_float) emit_div_f(gen, dst, rl, r, ast->line);
            else emit_div(gen, dst, rl, r, ast->line);
            break;
        case TOK_MOD:      emit_mod(gen, dst, rl, r, ast->line); break;
        case TOK_EQEQ:     emit_eq(gen, dst, rl, r, ast->line); break;
        case TOK_NEQ:      emit_neq(gen, dst, rl, r, ast->line); break;
        // 有序比较同样按静态类型特化（与栈式一致）：两侧都是 int 走 int 专用比较。
        //   ⚠ 与算术同理，这不只是性能 —— 泛型约束方法 `compareTo(T other)` 在
        //   T=OrdInt 时实参**是 struct**，通用比较会报「操作数类型不可比较」✗，
        //   而 int 专用比较按栈式口径直接比位模式（test_generic_face_impl）。
        case TOK_LT:
            if (both_int && rhs_imm) emit_lt_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_lt_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_lt_f(gen, dst, rl, r, ast->line);
            else emit_lt(gen, dst, rl, r, ast->line);
            break;
        case TOK_GT:
            if (both_int && rhs_imm) emit_gt_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_gt_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_gt_f(gen, dst, rl, r, ast->line);
            else emit_gt(gen, dst, rl, r, ast->line);
            break;
        case TOK_LE:
            if (both_int && rhs_imm) emit_le_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_le_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_le_f(gen, dst, rl, r, ast->line);
            else emit_le(gen, dst, rl, r, ast->line);
            break;
        case TOK_GE:
            if (both_int && rhs_imm) emit_ge_int_imm(gen, dst, rl, rhs_imm_val, ast->line);
            else if (both_int) emit_ge_int(gen, dst, rl, r, ast->line);
            else if (both_float) emit_ge_f(gen, dst, rl, r, ast->line);
            else emit_ge(gen, dst, rl, r, ast->line);
            break;
        case TOK_BITAND:   emit_bitand(gen, dst, rl, r, ast->line); break;
        case TOK_BITOR:    emit_bitor(gen, dst, rl, r, ast->line); break;
        case TOK_BITXOR:   emit_bitxor(gen, dst, rl, r, ast->line); break;
        case TOK_SHL:      emit_shl(gen, dst, rl, r, ast->line); break;
        case TOK_SHR:      emit_shr(gen, dst, rl, r, ast->line); break;
        case TOK_USHR:     emit_ushr(gen, dst, rl, r, ast->line); break;
        case TOK_IN:       reg_encode_iABC(gen->chunk, OP_IN, dst, rl, r, ast->line); break;
        // not in = in + not（此前完全没处理 ⇒ 结果直接是左操作数本身 ✗）
        case TOK_NOT_IN:
            reg_encode_iABC(gen->chunk, OP_IN, dst, rl, r, ast->line);
            emit_not(gen, dst, dst, ast->line);
            break;
        default:
            // 字符串拼接用 STRCAT
            if (op == TOK_PLUS) {
                emit_strcat(gen, dst, rl, r, ast->line);
            }
            break;
    }
    if (r_is_temp) reg_free(gen, r);
    // ⚠ 只释放**借来的**临时寄存器。原先写的是 `if (rl != dst) reg_free(gen, rl)` ——
    //   那在"左值是变量、rl 就是变量自己寄存器"的新路径上会把**活着的变量槽**释放掉，
    //   之后分配器把这个槽当临时寄存器复用 ⇒ 变量被就地覆盖。
    //   实测后果：`args[idx]` 里 args 被搬进 idx 所在的 R0 ⇒ 索引变成数组
    //   （「数组索引必须是数字」），连 assert/run_tests.leno 这个 runner 都跑不起来。
    if (rl_is_temp && rl != dst) reg_free(gen, rl);
}

// ============================================================================
// 一元运算
// ============================================================================

// ============================================================================
// 语句位置的 `i++` / `++i` / `i--`（T10 后续，对齐栈式的 OP_INC_LOCAL_NOPUSH）
// ----------------------------------------------------------------------------
// 作为**独立语句**时结果没人要，而通用路径（gen_unary）会先把**旧值**搬进 dst 再自增：
//     `i++` ⇒ [MOV dst, slot] + [INC slot, slot]
// 那条 MOV 纯属浪费 —— 实测 `examples/性能测试/空调用与自增隔离微基准.leno` 的
// `i++` 循环每轮就是这 2 条指令，而栈式语句位置只发 1 条 OP_INC_LOCAL_NOPUSH。
// 省掉后 `i++` 的指令数与 `i = i + 1` 持平（此前 `i++` 反而更慢 13%）。
// 边界：只处理**变量**操作数、且必须是局部量/参数（全局/upvalue 的通用路径要
// get→自增→set 三条配对，不在本快路径内）；其余形态返回 0，原样交回 gen_expr。
// 语义等价：被省掉的那条 MOV 的目标是临时寄存器，随后立刻被丢弃（语句位置无消费者）。
// ============================================================================
int try_emit_stmt_incdec(CodeGen* gen, Ast* e) {
    if (!e || e->kind != AST_UNARY) return 0;
    LenoTokenType op = e->u.unary.op;
    if (op != TOK_INC && op != TOK_DEC) return 0;
    Ast* operand = e->u.unary.operand;
    if (!operand || operand->kind != AST_VAR) return 0;   // 非变量：交回 gen_unary（会报语义错）
    SymRef* ref = &operand->u.var.ref;
    if (!((ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) && ref->index >= 0)) return 0;
    int slot = ref->index;
    if (op == TOK_INC) emit_inc(gen, slot, slot, e->line);
    else emit_dec(gen, slot, slot, e->line);
    return 1;
}

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

    // ★ 融合：`arr.add(x)` → OP_ARRAY_APPEND（一条指令，省掉方法派发）
    // ----------------------------------------------------------------------------
    // 通用路径要发 4 条指令：MOV(接收者) + MOV(实参) + GET_METHOD("add") + CALL，
    // 走完整的方法查找/绑定/调用；而 add 是数组的**内建方法**，语义与 OP_ARRAY_APPEND
    // 完全一致（都是"追加 + 返回新长度"，已实测两侧都是 3/1 ✓），所以直接发融合指令。
    // 栈式 codegen 早就有这个融合（OP_ARRAY_APPEND_NOPUSH），我们一直没有 ⇒
    // `arr.add(x)` 密集的代码（如"原始快速排序"）因此慢：30 万次 add 我们 4.91ms vs 栈式 3.99ms。
    // 安全性：仅当接收者**静态类型是数组**时才融合（`.add` 也只有数组有）；
    // 实参恰 1 个（数组内建方法不插隐式 self，与 struct 方法不同）。
    if (nargs == 1 && mname && strcmp(mname, "add") == 0) {
        TypeInfo* recv_type = infer_expr_type(gen->sem, obj_ast);
        int is_array_recv = (recv_type && recv_type->kind == TYPE_ARRAY);
        if (recv_type) type_free(recv_type);
        if (is_array_recv) {
            int obj_reg = gen_expr(gen, obj_ast);
            int val_reg = gen_expr(gen, args->items[0]);
            // ARRAY_APPEND: append R[val] to R[obj]；R[val] 被写成长度（need_result=1）
            reg_encode_iABC(gen->chunk, OP_ARRAY_APPEND, val_reg, obj_reg, 1, line);
            if (val_reg != dst) emit_mov(gen, dst, val_reg, line);
            reg_free(gen, val_reg);
            reg_free(gen, obj_reg);
            return;
        }
    }

    // 查方法定义（语义分析按 "Struct::method" 注册），用于补齐默认参数
    Ast* mdef = NULL;
    // 接收者的静态 struct 名（ot 下一行就被释放，先拷出来给后面的融合指令用）
    char recv_struct_name[BUFFER_SMALL];
    recv_struct_name[0] = '\0';
    TypeInfo* ot = infer_expr_type(gen->sem, obj_ast);
    if (ot && ot->kind == TYPE_STRUCT && ot->struct_name && mname) {
        snprintf(recv_struct_name, sizeof(recv_struct_name), "%s", ot->struct_name);
        char key[BUFFER_SMALL];
        snprintf(key, sizeof(key), "%s::%s", ot->struct_name, mname);
        mdef = func_table_find(&gen->sem->func_table, key);
    }
    if (ot) type_free(ot);

    // ⚠ 方法调用的实参列表**已含隐式 self**（语义分析插入，args[0] = self），
    //   所以这里按 self_offset=0 补齐：缺失的默认值对应 params[i]（i = nargs..pcnt-1）
    int expected = (mdef && mdef->kind == AST_FUNC_DEF && mdef->u.func.pcnt > nargs)
                       ? mdef->u.func.pcnt : nargs;

    // 基址寄存器：dst 恰好是"刚分配的临时寄存器"（或已在临时区之上）时直接用它，
    //   这样结果天然落在 dst，省掉收尾的 MOV（与全局函数直呼 OP_CALL_GLOBAL_FUNC 同一手法）。
    int dst_safe = (dst + 1 == gen->next_reg || dst >= gen->next_reg);
    int base = dst_safe ? dst : reg_alloc_block(gen, expected + 1);
    // ★ 抬高寄存器高水位**并**丢掉落在 [base, base+expected] 里的陈旧空闲槽位：
    //   dst_safe 分支不经过 reg_alloc_block（不会清空闲栈），陈旧槽位若落在实参区
    //   就会被求值实参时的 reg_alloc 发出去、把实参盖掉（见 reg_reserve_call_block）。
    reg_reserve_call_block(gen, base, expected + 1);

    // ★ 求值顺序：**先实参（含默认值）后接收者**。
    //   原先"先接收者、后实参"会让实参的取值指令夹在 OP_GET_METHOD 与 OP_CALL 之间，
    //   而 OP_GET_METHOD 里那个"下一条是 CALL 就不分配绑定方法、直接调原生"的窥孔
    //   要求两者**紧邻** —— 隔着一条 LOADK 就不生效，于是每次方法调用都要
    //   bound_method_new() 分配一个绑定方法。实测 `d.has("key1")`（带实参）
    //   因此比栈式慢 4 倍（100 万次 89ms vs 22ms），而 `.len()`（无实参）反而更快。
    //   安全性：接收者只写到 base（不碰 base+1..base+expected），实参区已用
    //   next_reg 预留（上面的 need），所以先后求值互不影响。
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, args->items[i], base + 1 + i);
    }
    expected = fill_default_args(gen, mdef, 0, nargs, base, line);

    gen_expr_to(gen, obj_ast, base);

    int mlen = (int)strlen(mname);
    ObjString* nameStr = str_copy(mname, mlen);
    int name_const = make_constant(gen, val_obj((Object*)nameStr));

    // ★ struct 方法调用融合（对齐栈式 OP_INVOKE_METHOD_TYPED）：
    //   「取方法 + 调用」合成一条 ⇒ 省 1 次指令派发 + 每次调用一次 ObjBoundMethod 分配
    //   + OP_CALL 里"receiver 是否在实参首位"的去重判断。
    //   只在编译期**已确证**「接收者静态类型是 struct，且该 struct 有这个方法」时发：
    //   mdef 来自 func_table 的 "<Struct>::<method>" 键（即脚本方法）、且非 async。
    //   运行期仍按**实际类型**分发 ⇒ 语义与老路径一致；`t.cb()` 这类**函数类型字段**
    //   调用因为没有 mdef，照旧走 OP_GET_METHOD 的"退化为取字段"分支，不受影响。
    int can_fuse = (recv_struct_name[0] != '\0' && mdef && mdef->kind == AST_FUNC_DEF &&
                    !mdef->u.func.is_async && expected >= 0 && expected <= 255);
    if (can_fuse) {
        int type_const = make_constant(gen, val_obj((Object*)str_copy(
                                                  recv_struct_name,
                                                  (int)strlen(recv_struct_name))));
        reg_encode_iABC(gen->chunk, OP_INVOKE_METHOD_TYPED, base, expected, 0, line);
        chunk_write(gen->chunk, (uint8_t)((name_const >> 8) & 0xFF), line);
        chunk_write(gen->chunk, (uint8_t)(name_const & 0xFF), line);
        chunk_write(gen->chunk, (uint8_t)((type_const >> 8) & 0xFF), line);
        chunk_write(gen->chunk, (uint8_t)(type_const & 0xFF), line);
    }
    // C 为 0 或超 8 位 → 紧随一条 EXTRAARG 携带 24 位常量索引
    else if (name_const == 0 || name_const > 255) {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, 0, line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, name_const, line);
    } else {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, name_const, line);
    }

    // ★ async 方法（`async func wait()`）必须和全局/局部 async 函数一样走
    //   **协程创建**路径（OP_ASYNC_CALL），不能直接 CALL：
    //   直接 CALL 会让方法体在**调用者帧**里同步执行、`vm.current_coroutine` 为 NULL，
    //   方法体里第一句 `await asyncs.sleep(...)` 就报「async.sleep 只能在 async 函数中
    //   调用」（examples/async await/struct 方法 + async.leno 整段不执行，
    //   栈式正常输出两行 Timer 完成）。与栈式 codegen 同口径
    //   （D:\CLeno\Leno 的 codegen_expr.c：method_def->u.func.is_async → OP_ASYNC_CALL）。
    if (can_fuse) {
        // 调用已由上面的 OP_INVOKE_METHOD_TYPED 完成 —— **不再发** OP_CALL。
        // （B 操作数用的就是这里的 expected，与实参布局一致：R[base]=receiver、
        //   R[base+1]=self 副本、R[base+2..base+expected]=实参。）
    } else if (mdef && mdef->kind == AST_FUNC_DEF && mdef->u.func.is_async) {
        reg_encode_iABC(gen->chunk, OP_ASYNC_CALL, base, expected, 0, line);
    } else {
        emit_call(gen, base, expected, 1, line);
    }
    // base == dst 时结果已就位、且**不能释放 dst**（那是调用方的目标寄存器，
    // 释放掉会被分配器当临时寄存器复用 —— 与前面全局函数直呼同一处陷阱）
    if (base != dst) {
        emit_mov(gen, dst, base, line);
        reg_free_block(gen, base);
    }
    }

// 被调函数是不是 async？
// ----------------------------------------------------------------------------
// 两个来源取**或**（与栈式 codegen 同口径）：
//   ① func_table 里的函数定义 —— 只是"表里这个名字没被同名局部函数覆盖"时才可靠；
//   ② 语义遍**当场**捕获的 `ast->u.call.callee_is_async`。
// 为什么必须看 ②：func_table 是**按名字**的全局表（无作用域信息），局部函数会覆盖
// 同名全局条目，而 codegen 在整个语义遍**之后**才查表 ⇒ ① 可能拿到被污染的定义。
// 实测（examples/async await/test_edge_async.leno）：closure_capture 里的局部
// `func inner()` 覆盖了顶层 `async func inner()`，于是 outer 里的 `await inner()`
// 被编成**同步 CALL**（那个 inner 不是 async），await 拿到张冠李戴的结果、
// triple 的返回值变成 `triple_inner_result`（期望 `triple_outer_inner_result`）。
// 语义遍是按源码顺序单遍走的：捕获时点早于后续同名局部函数的登记，所以 ② 是对的。
static int is_async_callee(Ast* ast, Ast* fdef) {
    if (fdef && fdef->kind == AST_FUNC_DEF && fdef->u.func.is_async) return 1;
    return ast && ast->u.call.callee_is_async;
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
            // ★ 直呼优化（OP_CALL_GLOBAL_FUNC）：省掉「GETGLOBALFUNC 取函数值 → CALL」
            //   里的取函数那条；且当 dst 正好是刚分配的临时寄存器（或已在临时区之上）时
            //   直接以 dst 为基址放实参 ⇒ 结果天然落在 dst，连结果 MOV 也省掉。
            //   保守条件：槽位/实参个数都能编进 8 位、不是 async（async 走协程路径）、
            //   且 func_table 里能查到定义（拿得到 pcnt，保证默认参数补齐正确）。
            if (fdef && fdef->kind == AST_FUNC_DEF && ref->index >= 0 && ref->index <= 255 &&
                expected <= 255 && !is_async_callee(ast, fdef)) {
                int dst_safe = (dst + 1 == gen->next_reg || dst >= gen->next_reg);
                int base = dst_safe ? dst : reg_alloc_block(gen, expected + 1);
                // 先把实参区（base+1 .. base+expected）抬进临时区，免得求值实参时
                // 分配的临时寄存器把实参槽盖掉。
                // ★ 只抬 next_reg 还不够：空闲栈里的**陈旧槽位**同样会被 reg_alloc 发出，
                //   所以要把落在实参区里的空闲槽位一并丢掉（见 reg_reserve_call_block）。
                reg_reserve_call_block(gen, base, expected + 1);
                // ★ typed 版（T10-⑤）：实参静态类型与形参声明类型**逐个相同**且都限定在
                //   {int, float} —— 这正好是建帧时那套转换**唯一**会动的两种类型 ⇒ 可以
                //   跳过逐参数转换循环（每参数省 4–6 个判断/转换）。
                //   保守条件（缺一不可）：形参类型表齐全、实参个数 == 形参个数（无默认值补齐）、
                //   两侧 kind 相同。其余情况（any / null / int 传进 float 形参等）仍走非 typed 版。
                // 诊断/基准开关：LENO_NO_TYPED_CALL=1 关闭 typed 直呼（同二进制 A/B，避免重建两次）
                static int typed_call_disabled = -1;
                if (typed_call_disabled < 0) typed_call_disabled = getenv("LENO_NO_TYPED_CALL") ? 1 : 0;
                int typed_ok = (!typed_call_disabled &&
                                fdef->u.func.param_types != NULL &&
                                nargs == fdef->u.func.pcnt && nargs > 0);
                for (int i = 0; typed_ok && i < nargs; i++) {
                    TypeInfo* pt = fdef->u.func.param_types[i];
                    TypeInfo* at = ast->u.call.args.items[i]->cached_type;
                    if (!pt || !at || pt->kind != at->kind) { typed_ok = 0; break; }
                    if (pt->kind != TYPE_INT && pt->kind != TYPE_FLOAT) { typed_ok = 0; break; }
                }

                for (int i = 0; i < nargs; i++) {
                    gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
                }
                expected = fill_default_args(gen, fdef, 0, nargs, base, ast->line);
                if (typed_ok) {
                    reg_encode_iABC(gen->chunk, OP_CALL_GLOBAL_FUNC_TYPED, base, expected,
                                    ref->index, ast->line);
                } else {
                    emit_call_global(gen, base, expected, ref->index, ast->line);
                }
                if (base != dst) {
                    emit_mov(gen, dst, base, ast->line);
                    reg_free_block(gen, base);
                }
                return;
            }
            int base = reg_alloc_block(gen, expected + 1);
            emit_getglobalfunc_to(gen, base, ref->index, ast->line);
            for (int i = 0; i < nargs; i++) {
                gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
            }
            expected = fill_default_args(gen, fdef, 0, nargs, base, ast->line);
            // ★ async 函数：调用不直接执行函数体，而是建协程并立刻返回 Future
            if (is_async_callee(ast, fdef)) {
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
    if (is_async_callee(ast, fdef)) {
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
        // ⚠ 上界必须是 count-1：约定是 `count = 表达式个数 + 1`，最后一格（parts[count-1]）
        //   只有尾随文本、没有对应表达式。写 `i < count` 会读 exprs[count-1] ——
        //   解析器收尾那段曾经只扩容 parts、不扩容 exprs，于是 exprs 只有 count-1 个槽，
        //   这一读就是**堆越界**：4 段以上插值（`$"{a}{b}{c}{d}"`）编译期直接段错误
        //   （exit=0xC0000005，--debug 一行都打不出来）。栈式消费端同此边界。
        if (i < count - 1 && ast->u.interp_string.exprs[i]) {
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
// 模块对象 → R[dst]（跨模块 .leno 调用用）
// ----------------------------------------------------------------------------
// 模块别名在不同上下文里是不同的符号种类：局部/参数（函数参数传进来的模块）、
// 全局（主程序 import 的）、upvalue（闭包捕获的）、模块级变量（**模块自己 import 的**，
// 即"模块的模块"）。
// ⚠ 别名解析必须按 **当前作用域 → 根作用域 → 整棵作用域树** 逐级兜底：
//   模块里 import 的别名挂在**模块自己的作用域**，root_scope 里根本查不到 ——
//   LenoSDL3 的 sdl_titlebar.leno 就是个模块，它 `import "sdl_core.leno" as core`，
//   解析不到就发 nil，随后的 `nil["getWindowSize"]` 运行期报
//   「下标访问: 对象不支持索引」（sdl_titlebar.leno:331）。
void emit_module_object_to(CodeGen* gen, Ast* mcall, int dst, int line) {
    SymRef* lib = &mcall->u.module_call.lib_ref;
    if (lib->name && lib->name[0]) {
        switch (lib->kind) {
            case SYM_LOCAL:
            case SYM_PARAM:   emit_mov(gen, dst, lib->index, line); return;
            case SYM_GLOBAL:  emit_getglobal_to(gen, dst, lib->index, line); return;
            case SYM_UPVALUE: emit_getupval_to(gen, dst, lib->index, line); return;
            case SYM_MODULE:  reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, lib->index, line); return;
            default: break;
        }
    }
    const char* alias = mcall->u.module_call.module_name;
    Symbol* sym = alias ? scope_resolve(gen->sem->current, alias) : NULL;
    if (!sym && alias) sym = scope_resolve(gen->sem->root_scope, alias);
    if (!sym && alias) sym = scope_resolve_tree_bfs(gen->sem->root_scope, alias);
    if (sym) {
        if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM) {
            emit_mov(gen, dst, sym->index, line); return;
        }
        if (sym->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, dst, sym->index, line); return;
        }
        if (sym->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, sym->index, line); return;
        }
        if (sym->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, dst, sym->index, line); return;
        }
    }
    emit_loadnil_to(gen, dst, line);
}

// 跨模块 .leno 调用的公共准备（gen_module_call 与解构多返回值**共用**）：
//   ① 模块对象 → R[base]（见 emit_module_object_to）
//   ② callee = R[base] = R[base][方法名]
//   ③ 用户实参 → R[base+1 .. base+nargs]
//   ④ 补齐跨模块**默认参数**（被调函数 AST 在别的模块，只能读符号表里的
//      param_count / param_default_texts，与栈式同一口径）
//   返回 base（块大小 = max(expected+1, nresults)），*out_expected 回填实参个数。
//   调用方随后自己发 emit_call(gen, base, expected, nresults, line) 并 reg_free_block。
// ⚠ 这两条路径此前各写一份，解构那份漏了 ①②④ 的完整口径 ⇒ 见 emit_module_object_to 注释。
// 跨模块被调函数是不是 async？
// ----------------------------------------------------------------------------
// 与本地路径同口径：`export async func work()` / `export struct` 的 async 方法 →
// 扫描器把标记记进模块符号表（ModuleFuncSymbol.is_async / ModuleStructMethod.is_async），
// 这里据此发 OP_ASYNC_CALL（调用即建协程、返回 Future）。
// 注：即便这一步漏了，VM 在 OP_CALL 里也有运行期兜底（按 ObjFunction.is_async），
//   但**静态**发出 ASYNC_CALL 更明确、也少一次运行期判断。
int module_call_is_async(CodeGen* gen, Ast* mcall) {
    if (!gen || !mcall || mcall->kind != AST_MODULE_CALL) return 0;
    const char* methname = mcall->u.module_call.method_name;
    if (!methname || !methname[0]) return 0;
    ImportedModuleInfo* mi = find_imported_module(gen->sem, mcall->u.module_call.module_name);
    if (!mi || !mi->sym_table) return 0;
    ModuleFuncSymbol* mfs = module_symbol_table_find_func(mi->sym_table, methname);
    return (mfs && mfs->is_async) ? 1 : 0;
}

int gen_module_call_prep(CodeGen* gen, Ast* mcall, int nresults, int* out_expected) {
    const char* modname = mcall->u.module_call.module_name;
    const char* methname = mcall->u.module_call.method_name ? mcall->u.module_call.method_name : "";
    int nargs = mcall->u.module_call.args.count;
    int line = mcall->line;

    int expected = nargs;
    ModuleFuncSymbol* mfs = NULL;
    {
        ImportedModuleInfo* mod_info = find_imported_module(gen->sem, modname);
        if (mod_info && mod_info->sym_table && methname[0]) {
            mfs = module_symbol_table_find_func(mod_info->sym_table, methname);
            if (mfs && mfs->param_count > expected) expected = mfs->param_count;
        }
    }

    // 块要同时容纳「模块对象 + 实参」与「nresults 个结果」：结果写在 R[base..]
    int block = expected + 1;
    if (nresults > block) block = nresults;
    int base = reg_alloc_block(gen, block);

    emit_module_object_to(gen, mcall, base, line);

    int ireg = reg_alloc(gen);
    int cidx = make_constant(gen, val_obj((Object*)str_copy(methname, (int)strlen(methname))));
    emit_loadk_to(gen, ireg, cidx, line);
    reg_encode_iABC(gen->chunk, OP_INDEX, base, base, ireg, line);
    reg_free(gen, ireg);

    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, mcall->u.module_call.args.items[i], base + 1 + i);
    }
    // 缺失的默认参数按符号表里的**文本**求值（跨模块只有文本，没有 AST）
    if (expected > nargs && mfs) {
        for (int i = nargs; i < expected; i++) {
            const char* dtext = (mfs->param_default_texts && i < mfs->param_count)
                                    ? mfs->param_default_texts[i] : NULL;
            gen_default_value_from_text_to(gen, base + 1 + i, dtext, line);
        }
    }
    if (out_expected) *out_expected = expected;
    return base;
}

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
        int expected = 0;
        int base = gen_module_call_prep(gen, ast, 1, &expected);
        // ★ 跨模块的 async 函数：同样"调用即建协程、返回 Future"（C2）
        if (module_call_is_async(gen, ast)) {
            reg_encode_iABC(gen->chunk, OP_ASYNC_CALL, base, expected, 0, ast->line);
        } else {
            emit_call(gen, base, expected, 1, ast->line);
        }
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
//   ⚠ 聚合/嵌套类型必须走**唯一的渲染实现** type_to_string：
//     手写 `TYPE_STRUCT → struct_name` 会把 `Box[int]` 打成 `Box`，
//     `new Box[Box[int]]` 的外层实参于是丢掉内层 int（type(outer) 输出 Box[Box]，
//     栈式是 Box[Box[int]] —— 泛型结构体 / 跨模块泛型两个样例都是这条）。
//     Array/Dict/Ptr 同理，此前直接落到 default 变成 "unknown"。
//   注：type_to_string 对**无泛型**的 struct 会渲染成 "struct Point"（带前缀），
//   与实参名口径不一致，所以那种情况仍用 struct_name。
static const char* typeinfo_to_name(TypeInfo* t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_ANY:    return "any";
        case TYPE_STRUCT:
            if (t->generic_count > 0 && t->struct_name) return type_to_string(t);  // Box[int]
            return t->struct_name ? t->struct_name : "struct";
        case TYPE_ARRAY:      // Array[int]
        case TYPE_DICT:       // Dict[string, int]
        case TYPE_PTR_GENERIC: // Ptr[u32]
            return type_to_string(t);
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

    // ---- 模块限定解析操作数（S2/2b-2，追加在字段名数据之后）：mod_space8 + mod_slot16 ----
    // 运行期据它取出**导入模块对象**（不是名字！），再精确取回该模块声明的那份定义。
    // 为什么不能发名字：别名与运行期 ObjModule.name 并不总相等 —— 模块按路径去重，
    //   "首次加载用什么名字就一直是那个名字"（实测：p.leno 先被 `as first` 加载，
    //   入口 `new p2.Point()` 就永远比不中 owner->name）⇒ 只能回退裸名、拿到先注册的那份。
    //   空间：0 = 无；1 = vm.globals[]（入口程序的 import 别名是 SYM_GLOBAL）；
    //        2 = frame->module->globals[]（模块文件里是 SYM_MODULE）。
    // 符号来源与 codegen_import.c"把模块对象存进别名槽位"用的是同一个 ⇒ 槽位必然对得上。
    {
        uint8_t mod_space = 0;
        int mod_slot = 0;
        const char* sn = ast->u.struct_init.struct_name;
        const char* sn_dot = sn ? strchr(sn, '.') : NULL;
        if (sn_dot && sn_dot != sn) {
            size_t alias_len = (size_t)(sn_dot - sn);
            char alias[BUFFER_MEDIUM];
            if (alias_len < sizeof(alias)) {
                memcpy(alias, sn, alias_len);
                alias[alias_len] = '\0';
                Symbol* alias_sym = scope_resolve(gen->sem->root_scope, alias);
                if (!alias_sym) alias_sym = scope_resolve(gen->sem->current, alias);
                if (alias_sym) {
                    if (alias_sym->kind == SYM_GLOBAL) {
                        mod_space = 1;
                        mod_slot = alias_sym->index;
                    } else if (alias_sym->kind == SYM_MODULE) {
                        mod_space = 2;
                        mod_slot = alias_sym->index;
                    }
                }
            }
        }
        chunk_write(gen->chunk, mod_space, ast->line);
        chunk_write(gen->chunk, (uint8_t)((mod_slot >> 8) & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(mod_slot & 0xFF), ast->line);
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
