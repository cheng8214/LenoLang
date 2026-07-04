// ============================================================================
// .leno 模块初始化操作码 (OP_INIT_LENOMODULE)
// ============================================================================
// import foo.leno 时生成此操作码，运行时执行模块的 init_chunk
// 行为：检查栈顶模块是否已初始化，若未初始化则执行其 init_chunk
// ============================================================================

OPCODE(OP_INIT_LENOMODULE) {
    frame = &vm.frames[vm.frame_cnt - 1];

    Value module_val = vm.stack[vm.sp - 1];

    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_MODULE) {
        runtime_error("OP_INIT_LENOMODULE: 栈顶不是模块对象", GET_CURRENT_LINE());
        return -1;
    }

    ObjModule* module = (ObjModule*)val_as_obj(module_val);

    if (!module->initialized && module->init_chunk) {
        module->initialized = 1;

        if (module->native_imports && module->native_import_count > 0) {
            for (int i = 0; i < module->native_import_count; i++) {
                native_init_module(module->native_imports[i]);
            }
        }

        extern ModuleFrame* module_frame_new(ObjModule* module);
        extern void module_frame_enter(ModuleFrame* frame);
        extern void module_frame_exit(void);
        extern int vm_run_chunk(Chunk* chunk);

        int saved_sp = vm.sp;

        ModuleFrame* mod_frame = module_frame_new(module);
        module_frame_enter(mod_frame);
        int result = vm_run_chunk(module->init_chunk);
        module_frame_exit();

        vm.sp = saved_sp;

        if (result < 0) {
            return -1;
        }

        if (module->export_mappings && module->export_mapping_count > 0) {
            for (int i = 0; i < module->export_mapping_count; i++) {
                ExportGlobalMapping* mapping = &module->export_mappings[i];
                ObjString* key = str_copy(mapping->name, (int)strlen(mapping->name));
                Value current_val = dict_get(module->exports, val_obj((Object*)key));
                if (val_is_null(current_val)) {
                    if (mapping->global_index >= 0 && mapping->global_index < module->global_count) {
                        Value actual_val = module->globals[mapping->global_index];
                        if (!val_is_null(actual_val)) {
                            dict_set(module->exports, val_obj((Object*)key), actual_val);
                        }
                    } else if (mapping->global_index == -2) {
                        extern ObjStructDef* struct_def_find(const char* name);
                        ObjStructDef* def = struct_def_find(mapping->name);
                        if (def) {
                            dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        }
                    }
                }
            }
        }

        // 将 use 导入的类型也添加到 exports（供 import B as b 后 b.TypeName 访问）
        if (module->use_reexport_names && module->use_reexport_count > 0) {
            extern ObjEnumDef* enum_def_find(const char* name);
            extern ObjStructDef* struct_def_find(const char* name);
            for (int i = 0; i < module->use_reexport_count; i++) {
                const char* name = module->use_reexport_names[i];
                TypeKind kind = (TypeKind)module->use_reexport_kinds[i];
                ObjString* key = str_copy(name, (int)strlen(name));
                Value current_val = dict_get(module->exports, val_obj((Object*)key));
                if (val_is_null(current_val)) {
                    if (kind == TYPE_ENUM) {
                        ObjEnumDef* def = enum_def_find(name);
                        if (def) {
                            dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        }
                    } else if (kind == TYPE_STRUCT) {
                        ObjStructDef* def = struct_def_find(name);
                        if (def) {
                            dict_set(module->exports, val_obj((Object*)key), val_obj((Object*)def));
                        }
                    }
                }
            }
        }
    } else if (!module->initialized && !module->init_chunk) {
        module->initialized = 1;

        if (module->native_imports && module->native_import_count > 0) {
            for (int i = 0; i < module->native_import_count; i++) {
                native_init_module(module->native_imports[i]);
            }
        }
    }

    DISPATCH;
}
