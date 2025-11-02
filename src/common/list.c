#include "common/list.h"
#include <stdlib.h>

List* list_new() {
    List* list = malloc(sizeof(List));
    if (!list) return NULL;
    list->size = 0;
    list->capacity = 16;
    list->items = malloc(sizeof(void*) * list->capacity);
    if (!list->items) {
        free(list);
        return NULL;
    }
    return list;
}

void list_free(List* list) {
    if (!list) return;
    free(list->items);
    free(list);
}

void list_add(List* list, void* item) {
    if (!list) return;
    if (list->size >= list->capacity) {
        list->capacity *= 2;
        list->items = realloc(list->items, sizeof(void*) * list->capacity);
    }
    list->items[list->size++] = item;
}

void* list_get(List* list, size_t index) {
    if (!list || index >= list->size) return NULL;
    return list->items[index];
}

size_t list_size(List* list) {
    if (!list) return 0;
    return list->size;
}
