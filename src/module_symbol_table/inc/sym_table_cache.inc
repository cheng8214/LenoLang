// ============================================================================
// 模块符号表缓存（跨运行，.lenosymc 格式）
// 与模块编译缓存（.lenomc）共享 .lenocache/ 目录和相同的失效判定机制
// ============================================================================

#define LENOSYMC_MAGIC   0x4D59534C  // "LSYM" little-endian
#define LENOSYMC_VERSION 0x00000007  // v7: 支持 Type? 可空类型（TypeInfo nullable 标记、ModuleStructField nullable）

// 字符串序列化辅助函数（需在 TypeInfo 序列化之前定义）
static void sym_cache_write_string(FILE* f, const char* s) {
    // 用 0xFFFFFFFF 表示 NULL（与空字符串 "" 区分）
    if (!s) {
        uint32_t null_marker = 0xFFFFFFFF;
        fwrite(&null_marker, 4, 1, f);
        return;
    }
    uint32_t len = (uint32_t)strlen(s);
    fwrite(&len, 4, 1, f);
    if (len > 0) fwrite(s, 1, len, f);
}

static char* sym_cache_read_string(FILE* f) {
    uint32_t len;
    if (fread(&len, 4, 1, f) != 1) return NULL;
    if (len == 0xFFFFFFFF) return NULL;  // NULL 标记
    if (len == 0) return strdup("");
    char* buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    if (fread(buf, 1, len, f) != len) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

// 序列化 TypeInfo（自定义二进制格式，避免 type_to_string/parse_type_from_string 往返问题）
// 格式: kind(u8) + 可选字段
//   TYPE_ARRAY: element_type
//   TYPE_DICT: key_type, value_type
//   TYPE_PTR_GENERIC: element_type
//   TYPE_FUNCTION: return_type, param_count(u16), param_types[]
//   TYPE_STRUCT/TYPE_FACE/TYPE_CSTRUCT/TYPE_CLIB/TYPE_CFUNC: struct_name
//   TYPE_GENERIC_PARAM: type_param_name, constraint_name
//   其他基本类型: 无额外字段
static void sym_cache_write_type_info(FILE* f, TypeInfo* ti) {
    if (!ti) {
        uint8_t none = 0xFF;  // NULL 标记
        fwrite(&none, 1, 1, f);
        return;
    }
    uint8_t kind = (uint8_t)ti->kind;
    fwrite(&kind, 1, 1, f);
    uint8_t nullable = (uint8_t)(ti->nullable ? 1 : 0);
    fwrite(&nullable, 1, 1, f);
    switch (ti->kind) {
        case TYPE_ARRAY:
        case TYPE_PTR_GENERIC:
            sym_cache_write_type_info(f, ti->element_type);
            break;
        case TYPE_DICT:
            sym_cache_write_type_info(f, ti->key_type);
            sym_cache_write_type_info(f, ti->value_type);
            break;
        case TYPE_FUNCTION:
            sym_cache_write_type_info(f, ti->return_type);
            uint16_t pc = (uint16_t)ti->param_count;
            fwrite(&pc, 2, 1, f);
            for (int i = 0; i < ti->param_count; i++) {
                sym_cache_write_type_info(f, ti->param_types[i]);
            }
            break;
        case TYPE_STRUCT:
        case TYPE_FACE:
        case TYPE_CSTRUCT:
        case TYPE_CLIB:
        case TYPE_CFUNC:
            sym_cache_write_string(f, ti->struct_name);
            // 泛型参数
            if (ti->generic_count > 0) {
                uint8_t has_ga = 1;
                fwrite(&has_ga, 1, 1, f);
                uint16_t gc = (uint16_t)ti->generic_count;
                fwrite(&gc, 2, 1, f);
                for (int i = 0; i < ti->generic_count; i++) {
                    sym_cache_write_type_info(f, ti->generic_args[i]);
                }
            } else {
                uint8_t has_ga = 0;
                fwrite(&has_ga, 1, 1, f);
            }
            break;
        case TYPE_GENERIC_PARAM:
            sym_cache_write_string(f, ti->type_param_name);
            sym_cache_write_string(f, ti->constraint_name);
            break;
        default:
            break;  // 基本类型无需额外字段
    }
}

// 反序列化 TypeInfo
static TypeInfo* sym_cache_read_type_info(FILE* f) {
    uint8_t kind;
    if (fread(&kind, 1, 1, f) != 1) return NULL;
    if (kind == 0xFF) return NULL;  // NULL 标记

    uint8_t nullable = 0;
    if (fread(&nullable, 1, 1, f) != 1) return NULL;

    TypeInfo* ti = type_new((TypeKind)kind);
    if (!ti) return NULL;
    ti->nullable = nullable ? 1 : 0;
    switch ((TypeKind)kind) {
        case TYPE_ARRAY:
        case TYPE_PTR_GENERIC: {
            ti->element_type = sym_cache_read_type_info(f);
            break;
        }
        case TYPE_DICT: {
            ti->key_type = sym_cache_read_type_info(f);
            ti->value_type = sym_cache_read_type_info(f);
            break;
        }
        case TYPE_FUNCTION: {
            ti->return_type = sym_cache_read_type_info(f);
            uint16_t pc;
            if (fread(&pc, 2, 1, f) != 1) { type_free(ti); return NULL; }
            ti->param_count = pc;
            if (pc > 0) {
                ti->param_types = (TypeInfo**)calloc(pc, sizeof(TypeInfo*));
                for (int i = 0; i < pc; i++) {
                    ti->param_types[i] = sym_cache_read_type_info(f);
                }
            }
            break;
        }
        case TYPE_STRUCT:
        case TYPE_FACE:
        case TYPE_CSTRUCT:
        case TYPE_CLIB:
        case TYPE_CFUNC: {
            ti->struct_name = sym_cache_read_string(f);
            uint8_t has_ga;
            if (fread(&has_ga, 1, 1, f) != 1) break;
            if (has_ga) {
                uint16_t gc;
                if (fread(&gc, 2, 1, f) != 1) break;
                ti->generic_count = gc;
                if (gc > 0) {
                    ti->generic_args = (TypeInfo**)calloc(gc, sizeof(TypeInfo*));
                    for (int i = 0; i < gc; i++) {
                        ti->generic_args[i] = sym_cache_read_type_info(f);
                    }
                }
            }
            break;
        }
        case TYPE_GENERIC_PARAM: {
            ti->type_param_name = sym_cache_read_string(f);
            ti->constraint_name = sym_cache_read_string(f);
            break;
        }
        default:
            break;
    }
    return ti;
}

// 序列化 ModuleSymbolTable 到文件
static int sym_cache_serialize(const char* cache_path, ModuleSymbolTable* table,
                               uint64_t src_hash, uint64_t src_size) {
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, cache_path, -1, NULL, 0);
    wchar_t* wp = (wchar_t*)malloc(wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, cache_path, -1, wp, wl);
    FILE* f = _wfopen(wp, L"wb");
    free(wp);
#else
    FILE* f = fopen(cache_path, "wb");
#endif
    if (!f) return -1;

    uint32_t magic = LENOSYMC_MAGIC;
    uint32_t version = LENOSYMC_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&src_hash, 8, 1, f);
    fwrite(&src_size, 8, 1, f);

    // funcs
    uint32_t fc = (uint32_t)table->func_count;
    fwrite(&fc, 4, 1, f);
    for (uint32_t i = 0; i < fc; i++) {
        sym_cache_write_string(f, table->funcs[i].name);
        uint8_t rt = (uint8_t)table->funcs[i].return_type;
        fwrite(&rt, 1, 1, f);
        sym_cache_write_type_info(f, table->funcs[i].return_type_info);
        sym_cache_write_string(f, table->funcs[i].return_struct_name);
        uint32_t tpc = (uint32_t)table->funcs[i].type_param_count;
        fwrite(&tpc, 4, 1, f);
    }

    // structs
    uint32_t sc = (uint32_t)table->struct_count;
    fwrite(&sc, 4, 1, f);
    for (uint32_t i = 0; i < sc; i++) {
        ModuleStructSymbol* ss = &table->structs[i];
        sym_cache_write_string(f, ss->name);
        uint32_t flc = (uint32_t)ss->field_count;
        fwrite(&flc, 4, 1, f);
        for (uint32_t j = 0; j < flc; j++) {
            sym_cache_write_string(f, ss->fields[j].name);
            uint8_t ft = (uint8_t)ss->fields[j].type;
            fwrite(&ft, 1, 1, f);
            uint8_t et = (uint8_t)ss->fields[j].element_type;
            fwrite(&et, 1, 1, f);
            uint8_t nl = (uint8_t)(ss->fields[j].nullable ? 1 : 0);
            fwrite(&nl, 1, 1, f);
            sym_cache_write_string(f, ss->fields[j].struct_name);
            sym_cache_write_string(f, ss->fields[j].element_struct_name);
        }
        uint32_t mc = (uint32_t)ss->method_count;
        fwrite(&mc, 4, 1, f);
        for (uint32_t j = 0; j < mc; j++) {
            ModuleStructMethod* sm = &ss->methods[j];
            sym_cache_write_string(f, sm->name);
            uint8_t rt = (uint8_t)sm->return_type;
            fwrite(&rt, 1, 1, f);
            sym_cache_write_type_info(f, sm->return_type_info);
            sym_cache_write_string(f, sm->return_struct_name);
            sym_cache_write_string(f, sm->return_type_param_name);
            uint32_t rgc = (uint32_t)sm->return_generic_count;
            fwrite(&rgc, 4, 1, f);
            for (uint32_t k = 0; k < rgc; k++) {
                sym_cache_write_string(f, sm->return_generic_param_names[k]);
            }
            uint32_t pc = (uint32_t)sm->param_count;
            fwrite(&pc, 4, 1, f);
            for (uint32_t k = 0; k < pc; k++) {
                uint8_t pt = (uint8_t)sm->param_types[k];
                fwrite(&pt, 1, 1, f);
            }
            // param_generic_names
            uint8_t has_pgn = sm->param_generic_names ? 1 : 0;
            fwrite(&has_pgn, 1, 1, f);
            if (has_pgn) {
                for (uint32_t k = 0; k < pc; k++) {
                    sym_cache_write_string(f, sm->param_generic_names[k]);
                }
            }
        }
        uint8_t is_cs = (uint8_t)ss->is_cstruct;
        fwrite(&is_cs, 1, 1, f);
        uint32_t ic = (uint32_t)ss->impl_count;
        fwrite(&ic, 4, 1, f);
        for (uint32_t j = 0; j < ic; j++) {
            sym_cache_write_string(f, ss->impl_names[j]);
        }
        uint32_t tpc = (uint32_t)ss->type_param_count;
        fwrite(&tpc, 4, 1, f);
        for (uint32_t j = 0; j < tpc; j++) {
            sym_cache_write_string(f, ss->type_param_names[j]);
        }
    }

    // enums
    uint32_t ec = (uint32_t)table->enum_count;
    fwrite(&ec, 4, 1, f);
    for (uint32_t i = 0; i < ec; i++) {
        sym_cache_write_string(f, table->enums[i].name);
        uint32_t emc = (uint32_t)table->enums[i].member_count;
        fwrite(&emc, 4, 1, f);
        for (uint32_t j = 0; j < emc; j++) {
            sym_cache_write_string(f, table->enums[i].member_names[j]);
            int64_t mv = table->enums[i].member_values[j];
            fwrite(&mv, 8, 1, f);
        }
    }

    // faces
    uint32_t fac = (uint32_t)table->face_count;
    fwrite(&fac, 4, 1, f);
    for (uint32_t i = 0; i < fac; i++) {
        sym_cache_write_string(f, table->faces[i].name);
        uint32_t fmc = (uint32_t)table->faces[i].method_count;
        fwrite(&fmc, 4, 1, f);
        for (uint32_t j = 0; j < fmc; j++) {
            sym_cache_write_string(f, table->faces[i].methods[j].name);
            uint8_t rt = (uint8_t)table->faces[i].methods[j].return_type;
            fwrite(&rt, 1, 1, f);
            sym_cache_write_string(f, table->faces[i].methods[j].return_struct_name);
            uint32_t pc = (uint32_t)table->faces[i].methods[j].param_count;
            fwrite(&pc, 4, 1, f);
        }
        uint32_t tpc = (uint32_t)table->faces[i].type_param_count;
        fwrite(&tpc, 4, 1, f);
    }

    // vars
    uint32_t vc = (uint32_t)table->var_count;
    fwrite(&vc, 4, 1, f);
    for (uint32_t i = 0; i < vc; i++) {
        sym_cache_write_string(f, table->vars[i].name);
        uint8_t vt = (uint8_t)table->vars[i].type;
        fwrite(&vt, 1, 1, f);
        sym_cache_write_string(f, table->vars[i].struct_name);
    }

    // aliases
    uint32_t ac = (uint32_t)table->alias_count;
    fwrite(&ac, 4, 1, f);
    for (uint32_t i = 0; i < ac; i++) {
        sym_cache_write_string(f, table->aliases[i].name);
        sym_cache_write_type_info(f, table->aliases[i].type_info);
    }

    // clibs
    uint32_t cc = (uint32_t)table->clib_count;
    fwrite(&cc, 4, 1, f);
    for (uint32_t i = 0; i < cc; i++) {
        sym_cache_write_string(f, table->clibs[i].name);
        uint32_t cfc = (uint32_t)table->clibs[i].func_count;
        fwrite(&cfc, 4, 1, f);
        for (uint32_t j = 0; j < cfc; j++) {
            ModuleClibFuncSymbol* cf = &table->clibs[i].funcs[j];
            sym_cache_write_string(f, cf->name);
            uint8_t rt = (uint8_t)cf->return_type;
            fwrite(&rt, 1, 1, f);
            // return_element_type (Ptr[T] 中的 T，TYPE_PTR 表示无)
            uint8_t ret_et = (uint8_t)cf->return_element_type;
            fwrite(&ret_et, 1, 1, f);
            sym_cache_write_string(f, cf->return_struct_name);
            uint32_t pc = (uint32_t)cf->param_count;
            fwrite(&pc, 4, 1, f);
            for (uint32_t k = 0; k < pc; k++) {
                uint8_t pt = (uint8_t)cf->param_types[k];
                fwrite(&pt, 1, 1, f);
            }
            // param_element_types
            if (cf->param_element_types && pc > 0) {
                uint8_t has_pet = 1;
                fwrite(&has_pet, 1, 1, f);
                for (uint32_t k = 0; k < pc; k++) {
                    uint8_t pet = (uint8_t)cf->param_element_types[k];
                    fwrite(&pet, 1, 1, f);
                }
            } else {
                uint8_t has_pet = 0;
                fwrite(&has_pet, 1, 1, f);
            }
            // param_struct_names
            uint8_t has_psn = cf->param_struct_names ? 1 : 0;
            fwrite(&has_psn, 1, 1, f);
            if (has_psn) {
                for (uint32_t k = 0; k < pc; k++) {
                    sym_cache_write_string(f, cf->param_struct_names[k]);
                }
            }
        }
    }

    fclose(f);
    return 0;
}

// 反序列化 ModuleSymbolTable 从缓存文件
// 返回 0=成功, -1=失败（缓存不存在/失效/损坏）
static int sym_cache_deserialize(const char* cache_path, ModuleSymbolTable* table,
                                  const char* full_path) {
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, cache_path, -1, NULL, 0);
    if (wl <= 0) return -1;
    wchar_t* wp = (wchar_t*)malloc(wl * sizeof(wchar_t));
    if (!wp) return -1;
    MultiByteToWideChar(CP_UTF8, 0, cache_path, -1, wp, wl);
    FILE* f = _wfopen(wp, L"rb");
    free(wp);
#else
    FILE* f = fopen(cache_path, "rb");
#endif
    if (!f) return -1;

    uint32_t magic, version;
    uint64_t src_hash, src_size;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&src_hash, 8, 1, f) != 1 || fread(&src_size, 8, 1, f) != 1) {
        fclose(f); return -1;
    }
    if (magic != LENOSYMC_MAGIC || version != LENOSYMC_VERSION) {
        fclose(f); return -1;
    }

    // 失效判定：先比对文件大小（O(1) stat），再比对内容哈希
    // Windows 上使用 _wstat 处理 UTF-8 中文路径
#ifdef _WIN32
    struct _stat st;
    {
        wchar_t* wp = utf8_to_utf16(full_path);
        if (!wp) { fclose(f); return -1; }
        int ret = _wstat(wp, &st);
        free(wp);
        if (ret != 0) { fclose(f); return -1; }
    }
#else
    struct stat st;
    if (stat(full_path, &st) != 0) { fclose(f); return -1; }
#endif
    if ((uint64_t)st.st_size != src_size) { fclose(f); return -1; }
    // 读源文件算哈希（文本模式，与 module_loader.c 的 read_file 一致）
#ifdef _WIN32
    {
        wchar_t* wp2 = utf8_to_utf16(full_path);
        if (!wp2) { fclose(f); return -1; }
        FILE* sf = _wfopen(wp2, L"r");
        free(wp2);
        if (!sf) { fclose(f); return -1; }
        fseek(sf, 0, SEEK_END); long sz = ftell(sf); fseek(sf, 0, SEEK_SET);
        char* src = (char*)malloc(sz + 1);
        if (!src) { fclose(sf); fclose(f); return -1; }
        size_t rd = fread(src, 1, sz, sf); src[rd] = '\0'; fclose(sf);
        uint64_t cur_hash = serialize_source_hash(src, strlen(src));
        free(src);
        if (cur_hash != src_hash) { fclose(f); return -1; }
    }
#else
    {
        FILE* sf = fopen(full_path, "r");
        if (!sf) { fclose(f); return -1; }
        fseek(sf, 0, SEEK_END); long sz = ftell(sf); fseek(sf, 0, SEEK_SET);
        char* src = (char*)malloc(sz + 1);
        if (!src) { fclose(sf); fclose(f); return -1; }
        size_t rd = fread(src, 1, sz, sf); src[rd] = '\0'; fclose(sf);
        uint64_t cur_hash = serialize_source_hash(src, strlen(src));
        free(src);
        if (cur_hash != src_hash) { fclose(f); return -1; }
    }
#endif

    // 验证通过，开始反序列化符号表

    // funcs
    uint32_t fc;
    if (fread(&fc, 4, 1, f) != 1) { fclose(f); return -1; }
    table->func_count = 0; table->func_capacity = fc > 0 ? fc : 0;
    table->funcs = fc > 0 ? (ModuleFuncSymbol*)calloc(fc, sizeof(ModuleFuncSymbol)) : NULL;
    for (uint32_t i = 0; i < fc; i++) {
        table->funcs[i].name = sym_cache_read_string(f);
        uint8_t rt; if (fread(&rt, 1, 1, f) != 1) goto fail;
        table->funcs[i].return_type = (TypeKind)rt;
        table->funcs[i].return_type_info = sym_cache_read_type_info(f);
        table->funcs[i].return_struct_name = sym_cache_read_string(f);
        uint32_t tpc; if (fread(&tpc, 4, 1, f) != 1) goto fail;
        table->funcs[i].type_param_count = (int)tpc;
        table->func_count++;
    }

    // structs
    uint32_t sc;
    if (fread(&sc, 4, 1, f) != 1) { fclose(f); return -1; }
    table->struct_count = 0; table->struct_capacity = sc > 0 ? sc : 0;
    table->structs = sc > 0 ? (ModuleStructSymbol*)calloc(sc, sizeof(ModuleStructSymbol)) : NULL;
    for (uint32_t i = 0; i < sc; i++) {
        ModuleStructSymbol* ss = &table->structs[i];
        ss->name = sym_cache_read_string(f);
        uint32_t flc; if (fread(&flc, 4, 1, f) != 1) goto fail;
        ss->field_count = (int)flc;
        ss->fields = flc > 0 ? (ModuleStructField*)calloc(flc, sizeof(ModuleStructField)) : NULL;
        for (uint32_t j = 0; j < flc; j++) {
            ss->fields[j].name = sym_cache_read_string(f);
            uint8_t ft, et, nl;
            if (fread(&ft, 1, 1, f) != 1 || fread(&et, 1, 1, f) != 1 || fread(&nl, 1, 1, f) != 1) goto fail;
            ss->fields[j].type = (TypeKind)ft;
            ss->fields[j].element_type = (TypeKind)et;
            ss->fields[j].nullable = nl ? 1 : 0;
            ss->fields[j].struct_name = sym_cache_read_string(f);
            ss->fields[j].element_struct_name = sym_cache_read_string(f);
        }
        uint32_t mc; if (fread(&mc, 4, 1, f) != 1) goto fail;
        ss->method_count = (int)mc;
        ss->methods = mc > 0 ? (ModuleStructMethod*)calloc(mc, sizeof(ModuleStructMethod)) : NULL;
        for (uint32_t j = 0; j < mc; j++) {
            ModuleStructMethod* sm = &ss->methods[j];
            sm->name = sym_cache_read_string(f);
            uint8_t rt; if (fread(&rt, 1, 1, f) != 1) goto fail;
            sm->return_type = (TypeKind)rt;
            sm->return_type_info = sym_cache_read_type_info(f);
            sm->return_struct_name = sym_cache_read_string(f);
            sm->return_type_param_name = sym_cache_read_string(f);
            uint32_t rgc; if (fread(&rgc, 4, 1, f) != 1) goto fail;
            sm->return_generic_count = (int)rgc;
            sm->return_generic_param_names = rgc > 0 ? (char**)calloc(rgc, sizeof(char*)) : NULL;
            for (uint32_t k = 0; k < rgc; k++) {
                sm->return_generic_param_names[k] = sym_cache_read_string(f);
            }
            uint32_t pc; if (fread(&pc, 4, 1, f) != 1) goto fail;
            sm->param_count = (int)pc;
            sm->param_types = pc > 0 ? (TypeKind*)calloc(pc, sizeof(TypeKind)) : NULL;
            for (uint32_t k = 0; k < pc; k++) {
                uint8_t pt; if (fread(&pt, 1, 1, f) != 1) goto fail;
                sm->param_types[k] = (TypeKind)pt;
            }
            uint8_t has_pgn; if (fread(&has_pgn, 1, 1, f) != 1) goto fail;
            if (has_pgn && pc > 0) {
                sm->param_generic_names = (char**)calloc(pc, sizeof(char*));
                for (uint32_t k = 0; k < pc; k++) {
                    sm->param_generic_names[k] = sym_cache_read_string(f);
                }
            }
        }
        uint8_t is_cs; if (fread(&is_cs, 1, 1, f) != 1) goto fail;
        ss->is_cstruct = is_cs;
        uint32_t ic; if (fread(&ic, 4, 1, f) != 1) goto fail;
        ss->impl_count = (int)ic;
        ss->impl_names = ic > 0 ? (char**)calloc(ic, sizeof(char*)) : NULL;
        for (uint32_t j = 0; j < ic; j++) {
            ss->impl_names[j] = sym_cache_read_string(f);
        }
        uint32_t tpc; if (fread(&tpc, 4, 1, f) != 1) goto fail;
        ss->type_param_count = (int)tpc;
        ss->type_param_names = tpc > 0 ? (char**)calloc(tpc, sizeof(char*)) : NULL;
        for (uint32_t j = 0; j < tpc; j++) {
            ss->type_param_names[j] = sym_cache_read_string(f);
        }
        table->struct_count++;
    }

    // enums
    uint32_t ec;
    if (fread(&ec, 4, 1, f) != 1) { fclose(f); return -1; }
    table->enum_count = 0; table->enum_capacity = ec > 0 ? ec : 0;
    table->enums = ec > 0 ? (ModuleEnumSymbol*)calloc(ec, sizeof(ModuleEnumSymbol)) : NULL;
    for (uint32_t i = 0; i < ec; i++) {
        table->enums[i].name = sym_cache_read_string(f);
        uint32_t emc; if (fread(&emc, 4, 1, f) != 1) goto fail;
        table->enums[i].member_count = (int)emc;
        table->enums[i].member_names = emc > 0 ? (char**)calloc(emc, sizeof(char*)) : NULL;
        table->enums[i].member_values = emc > 0 ? (int64_t*)calloc(emc, sizeof(int64_t)) : NULL;
        for (uint32_t j = 0; j < emc; j++) {
            table->enums[i].member_names[j] = sym_cache_read_string(f);
            int64_t mv; if (fread(&mv, 8, 1, f) != 1) goto fail;
            table->enums[i].member_values[j] = mv;
        }
        table->enum_count++;
    }

    // faces
    uint32_t fac;
    if (fread(&fac, 4, 1, f) != 1) { fclose(f); return -1; }
    table->face_count = 0; table->face_capacity = fac > 0 ? fac : 0;
    table->faces = fac > 0 ? (ModuleFaceSymbol*)calloc(fac, sizeof(ModuleFaceSymbol)) : NULL;
    for (uint32_t i = 0; i < fac; i++) {
        table->faces[i].name = sym_cache_read_string(f);
        uint32_t fmc; if (fread(&fmc, 4, 1, f) != 1) goto fail;
        table->faces[i].method_count = (int)fmc;
        table->faces[i].methods = fmc > 0 ? (ModuleFaceMethodSymbol*)calloc(fmc, sizeof(ModuleFaceMethodSymbol)) : NULL;
        for (uint32_t j = 0; j < fmc; j++) {
            table->faces[i].methods[j].name = sym_cache_read_string(f);
            uint8_t rt; if (fread(&rt, 1, 1, f) != 1) goto fail;
            table->faces[i].methods[j].return_type = (TypeKind)rt;
            table->faces[i].methods[j].return_struct_name = sym_cache_read_string(f);
            uint32_t pc; if (fread(&pc, 4, 1, f) != 1) goto fail;
            table->faces[i].methods[j].param_count = (int)pc;
        }
        uint32_t tpc; if (fread(&tpc, 4, 1, f) != 1) goto fail;
        table->faces[i].type_param_count = (int)tpc;
        table->face_count++;
    }

    // vars
    uint32_t vc;
    if (fread(&vc, 4, 1, f) != 1) { fclose(f); return -1; }
    table->var_count = 0; table->var_capacity = vc > 0 ? vc : 0;
    table->vars = vc > 0 ? (ModuleVarSymbol*)calloc(vc, sizeof(ModuleVarSymbol)) : NULL;
    for (uint32_t i = 0; i < vc; i++) {
        table->vars[i].name = sym_cache_read_string(f);
        uint8_t vt; if (fread(&vt, 1, 1, f) != 1) goto fail;
        table->vars[i].type = (TypeKind)vt;
        table->vars[i].struct_name = sym_cache_read_string(f);
        table->var_count++;
    }

    // aliases
    uint32_t ac;
    if (fread(&ac, 4, 1, f) != 1) { fclose(f); return -1; }
    table->alias_count = 0; table->alias_capacity = ac > 0 ? ac : 0;
    table->aliases = ac > 0 ? (ModuleAliasSymbol*)calloc(ac, sizeof(ModuleAliasSymbol)) : NULL;
    for (uint32_t i = 0; i < ac; i++) {
        table->aliases[i].name = sym_cache_read_string(f);
        table->aliases[i].type_info = sym_cache_read_type_info(f);
        table->alias_count++;
    }

    // clibs
    uint32_t cc;
    if (fread(&cc, 4, 1, f) != 1) { fclose(f); return -1; }
    table->clib_count = 0; table->clib_capacity = cc > 0 ? cc : 0;
    table->clibs = cc > 0 ? (ModuleClibSymbol*)calloc(cc, sizeof(ModuleClibSymbol)) : NULL;
    for (uint32_t i = 0; i < cc; i++) {
        table->clibs[i].name = sym_cache_read_string(f);
        uint32_t cfc; if (fread(&cfc, 4, 1, f) != 1) goto fail;
        table->clibs[i].func_count = (int)cfc;
        table->clibs[i].funcs = cfc > 0 ? (ModuleClibFuncSymbol*)calloc(cfc, sizeof(ModuleClibFuncSymbol)) : NULL;
        for (uint32_t j = 0; j < cfc; j++) {
            ModuleClibFuncSymbol* cf = &table->clibs[i].funcs[j];
            cf->name = sym_cache_read_string(f);
            uint8_t rt; if (fread(&rt, 1, 1, f) != 1) goto fail;
            cf->return_type = (TypeKind)rt;
            // return_element_type
            uint8_t ret_et; if (fread(&ret_et, 1, 1, f) != 1) goto fail;
            cf->return_element_type = (TypeKind)ret_et;
            cf->return_struct_name = sym_cache_read_string(f);
            uint32_t pc; if (fread(&pc, 4, 1, f) != 1) goto fail;
            cf->param_count = (int)pc;
            cf->param_types = pc > 0 ? (TypeKind*)calloc(pc, sizeof(TypeKind)) : NULL;
            for (uint32_t k = 0; k < pc; k++) {
                uint8_t pt; if (fread(&pt, 1, 1, f) != 1) goto fail;
                cf->param_types[k] = (TypeKind)pt;
            }
            // param_element_types
            uint8_t has_pet; if (fread(&has_pet, 1, 1, f) != 1) goto fail;
            if (has_pet && pc > 0) {
                cf->param_element_types = (TypeKind*)calloc(pc, sizeof(TypeKind));
                for (uint32_t k = 0; k < pc; k++) {
                    uint8_t pet; if (fread(&pet, 1, 1, f) != 1) goto fail;
                    cf->param_element_types[k] = (TypeKind)pet;
                }
            }
            uint8_t has_psn; if (fread(&has_psn, 1, 1, f) != 1) goto fail;
            if (has_psn && pc > 0) {
                cf->param_struct_names = (char**)calloc(pc, sizeof(char*));
                for (uint32_t k = 0; k < pc; k++) {
                    cf->param_struct_names[k] = sym_cache_read_string(f);
                }
            }
        }
        table->clib_count++;
    }

    fclose(f);
    return 0;

fail:
    fclose(f);
    // 注意：反序列化失败时，已部分填充的 table 会被 module_symbol_table_destroy 清理
    return -1;
}

// 计算符号表缓存文件路径（与模块编译缓存共享 .lenocache/ 目录）
// 使用不同后缀 .lenosymc 以区分
static char* sym_cache_path_for(const char* full_path) {
    if (!module_loader_is_cache_enabled()) return NULL;
    // 复用 module_cache_path_for 的逻辑，但改为 .lenosymc 后缀
    // 直接调用 module_cache_path_for，然后替换后缀
    extern char* module_cache_path_for(const char* full_path, const char* cache_dir);
    // 我们需要拿到 cache_dir — 通过 module_loader.h 获取
    // 简单方案：自己计算路径（与 module_cache_path_for 相同的 hash 逻辑）
    // 直接复用，然后替换扩展名
    // 但 module_cache_path_for 需要 cache_dir 参数，我们无法直接获取
    // 最简方案：让 module_loader 导出获取 cache_dir 的函数
    // 暂时用更简方案：基于 full_path hash 自行构建路径
    // 复用 serialize_source_hash 计算相同 hash
    uint64_t hash = serialize_source_hash(full_path, strlen(full_path));
    // 需要缓存目录 — 通过已有 API 获取
    // 暂时在 module_loader 中添加一个 getter
    extern const char* module_loader_get_cache_dir(void);
    const char* cache_dir = module_loader_get_cache_dir();
    if (!cache_dir) return NULL;
    size_t dir_len = strlen(cache_dir);
    int need_sep = 0;
    if (dir_len > 0) {
        char last = cache_dir[dir_len - 1];
#ifdef _WIN32
        if (last != '\\' && last != '/') need_sep = 1;
#else
        if (last != '/') need_sep = 1;
#endif
    }
    char sep_buf[2] = {0};
    if (need_sep) {
#ifdef _WIN32
        sep_buf[0] = '\\';
#else
        sep_buf[0] = '/';
#endif
    }
    size_t buf_size = dir_len + (need_sep ? 1 : 0) + 20 + strlen(".lenosymc") + 1;
    char* path = (char*)malloc(buf_size);
    if (!path) return NULL;
    snprintf(path, buf_size, "%s%s%llx.lenosymc",
             cache_dir, sep_buf, (unsigned long long)hash);
    return path;
}
