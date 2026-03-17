#ifndef LIST_H
#define LIST_H

#include <stddef.h>

typedef struct List {
    void** items;
    size_t size;
    size_t capacity;
} List;

List* list_new();
void list_free(List* list);
int list_add(List* list, void* item);
void* list_get(List* list, size_t index);
size_t list_size(List* list);

#endif // LIST_H
