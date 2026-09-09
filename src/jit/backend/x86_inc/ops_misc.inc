/* ============================================================================
 * ops_misc.inc - Misc cases (OP_NULL/TRUE/FALSE, try/catch/finally no-ops, default: unsupported -> bailout)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Simple constant pushes ---- */
            case OP_NULL:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, 0);  /* NULL = 0 */
                TOS_PRODUCE();
                vstack++;
                break;
case OP_TRUE:
                TOS_SPILL();
                /* Leno true value = NaN-boxed bool true (must match VM TRUE_VAL,
                 * otherwise is_falsey/val_is_bool misclassify JIT-sunk bools) */
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)TRUE_VAL);
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_FALSE:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)FALSE_VAL);
                TOS_PRODUCE();
                vstack++;
                break;

            /* ---- Exception handling opcodes (no-op in JIT) ---- */
            /* The JIT skips try/catch/finally setup entirely.
             * In normal execution (no exception), these are effectively
             * no-ops: OP_TRY sets up catch_ip (skipped), OP_JUMP after
             * try body skips the catch body, OP_END_TRY restores state.
             * If an exception occurs from a callout, the JIT bails out
             * and the interpreter re-executes with full try/catch. */
            case OP_TRY:
            case OP_CATCH:
            case OP_FINALLY:
            case OP_END_TRY:
                break;

            default:
                /* Should not reach here if scan passed */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] codegen FAIL: unsupported opcode %d at bc_off=%d\n", op, bc_off);
                return 0;
