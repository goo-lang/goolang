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

int list_add(List* list, void* item) {
    if (!list) return -1;
    if (list->size >= list->capacity) {
        size_t new_capacity = list->capacity * 2;
        void** tmp = realloc(list->items, sizeof(void*) * new_capacity);
        if (!tmp) return -1;
        list->items = tmp;
        list->capacity = new_capacity;
    }
    list->items[list->size++] = item;
    return 0;
}

void* list_get(List* list, size_t index) {
    if (!list || index >= list->size) return NULL;
    return list->items[index];
}

size_t list_size(List* list) {
    if (!list) return 0;
    return list->size;
}
