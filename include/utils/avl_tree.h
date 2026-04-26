#ifndef AVL_TREE_H
#define AVL_TREE_H

#include <stddef.h>

typedef struct AVLNode {
    void* start_ptr;
    size_t size;
    size_t id;         // 비트맵 마킹을 위한 고유 인덱스
    int height;
    struct AVLNode *left;
    struct AVLNode *right;
} AVLNode;

// id 매개변수가 추가된 insert 함수
AVLNode* avl_insert(AVLNode* node, void* ptr, size_t size, size_t id);
AVLNode* avl_delete(AVLNode* root, void* ptr);
AVLNode* avl_find_range(AVLNode* root, void* ptr);
void avl_free_all(AVLNode* node);

#endif