#include "include/lenolang.h"

#define MAX_FACE_DEFS 256

static THREAD_LOCAL ObjFaceDef* face_def_table[MAX_FACE_DEFS];
static THREAD_LOCAL int face_def_count = 0;

ObjFaceDef* face_def_new(const char* name, int method_count) {
    ObjFaceDef* def = (ObjFaceDef*)gc_alloc(sizeof(ObjFaceDef), OBJ_FACE_DEF);
    if (!def) return NULL;

    def->name = strdup(name);
    def->method_count = method_count;
    def->type_param_count = 0;
    def->type_param_names = NULL;
    def->type_param_constraints = NULL;

    if (method_count > 0) {
        def->methods = (FaceMethodInfo*)calloc(method_count, sizeof(FaceMethodInfo));
    } else {
        def->methods = NULL;
    }

    return def;
}

void face_def_set_type_params(ObjFaceDef* def, int count, char** names, char** constraints) {
    if (!def || count <= 0) return;
    def->type_param_count = count;
    def->type_param_names = (char**)malloc(sizeof(char*) * count);
    def->type_param_constraints = (char**)calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        def->type_param_names[i] = strdup(names[i]);
        if (constraints && constraints[i]) {
            def->type_param_constraints[i] = strdup(constraints[i]);
        }
    }
}

void face_def_register(ObjFaceDef* def) {
    if (face_def_count >= MAX_FACE_DEFS) {
        error_add(ERR_RUNTIME, 0, "face 定义数量超过上限");
        return;
    }

    for (int i = 0; i < face_def_count; i++) {
        if (strcmp(face_def_table[i]->name, def->name) == 0) {
            face_def_table[i] = def;
            return;
        }
    }

    face_def_table[face_def_count++] = def;
}

ObjFaceDef* face_def_find(const char* name) {
    for (int i = 0; i < face_def_count; i++) {
        if (strcmp(face_def_table[i]->name, name) == 0) {
            return face_def_table[i];
        }
    }
    return NULL;
}

int struct_implements_face(ObjStructDef* struct_def, ObjFaceDef* face_def) {
    if (!struct_def || !face_def) return 0;

    // 名义类型检查：必须通过 impl 显式声明实现 face
    for (int i = 0; i < struct_def->impl_count; i++) {
        if (strcmp(struct_def->impl_names[i], face_def->name) == 0) {
            return 1;
        }
    }

    return 0;
}

int struct_def_has_face_method(const char* struct_name, const char* method_name, int* out_param_count, TypeKind* out_return_type) {
    if (!struct_name || !method_name) return 0;

    ObjStructDef* sdef = struct_def_find(struct_name);
    if (!sdef || sdef->impl_count <= 0) return 0;

    for (int i = 0; i < sdef->impl_count; i++) {
        ObjFaceDef* fdef = face_def_find(sdef->impl_names[i]);
        if (fdef) {
            for (int j = 0; j < fdef->method_count; j++) {
                if (strcmp(fdef->methods[j].name, method_name) == 0) {
                    if (out_param_count) *out_param_count = fdef->methods[j].param_count;
                    if (out_return_type) *out_return_type = fdef->methods[j].return_type ? fdef->methods[j].return_type->kind : TYPE_ANY;
                    return 1;
                }
            }
        }
    }

    return 0;
}

void face_def_mark_all(void) {
    extern void gc_mark_object(Object* obj);
    for (int i = 0; i < face_def_count; i++) {
        gc_mark_object((Object*)face_def_table[i]);
    }
}
