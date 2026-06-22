void visit_list(Semantic* s, AstList* list) {
    for (int i = 0; i < list->count; i++) {
        visit(s, list->items[i]);
    }
}