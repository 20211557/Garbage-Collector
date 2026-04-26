#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t* bits;
    size_t size; // 관리할 수 있는 최대 비트 수
} Bitmap;

Bitmap* bitmap_create(size_t max_elements);
void bitmap_set(Bitmap* b, size_t index);
int bitmap_get(Bitmap* b, size_t index);
void bitmap_clear_all(Bitmap* b);
void bitmap_destroy(Bitmap* b);

#endif