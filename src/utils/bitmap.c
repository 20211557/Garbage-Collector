#include "bitmap.h"
#include <stdlib.h>
#include <string.h>

Bitmap* bitmap_create(size_t max_elements) {
    Bitmap* b = (Bitmap*)malloc(sizeof(Bitmap));
    b->size = max_elements;
    // 64비트 단위로 메모리 할당
    size_t num_words = (max_elements + 63) / 64;
    b->bits = (uint64_t*)calloc(num_words, sizeof(uint64_t));
    return b;
}

void bitmap_set(Bitmap* b, size_t index) {
    if (index >= b->size) return;
    b->bits[index / 64] |= (1ULL << (index % 64));
}

int bitmap_get(Bitmap* b, size_t index) {
    if (index >= b->size) return 0;
    return (b->bits[index / 64] & (1ULL << (index % 64))) != 0;
}

void bitmap_clear_all(Bitmap* b) {
    size_t num_words = (b->size + 63) / 64;
    memset(b->bits, 0, num_words * sizeof(uint64_t));
}

void bitmap_destroy(Bitmap* b) {
    free(b->bits);
    free(b);
}