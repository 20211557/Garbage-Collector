#include "avl_tree.h"
#include <stdlib.h>

static int get_height(AVLNode* n) { return n ? n->height : 0; }
static int max(int a, int b) { return (a > b) ? a : b; }
static int get_balance(AVLNode* n) { return n ? get_height(n->left) - get_height(n->right) : 0; }

static AVLNode* create_node(void* ptr, size_t size, size_t id) {
    AVLNode* node = (AVLNode*)calloc(1, sizeof(AVLNode));
    if (!node) return NULL;
    node->start_ptr = ptr;
    node->size = size;
    node->id = id;
    node->height = 1;
    return node;
}

// --- 회전 로직 (이전과 동일하지만 안정성 재확인) ---
static AVLNode* rotate_right(AVLNode* y) {
    AVLNode* x = y->left;
    AVLNode* T2 = x->right;
    x->right = y;
    y->left = T2;
    y->height = max(get_height(y->left), get_height(y->right)) + 1;
    x->height = max(get_height(x->left), get_height(x->right)) + 1;
    return x;
}

static AVLNode* rotate_left(AVLNode* x) {
    AVLNode* y = x->right;
    AVLNode* T2 = y->left;
    y->left = x;
    x->right = T2;
    x->height = max(get_height(x->left), get_height(x->right)) + 1;
    y->height = max(get_height(y->left), get_height(y->right)) + 1;
    return y;
}

// --- 삽입 (O(\log N)) ---
AVLNode* avl_insert(AVLNode* node, void* ptr, size_t size, size_t id) {
    if (!node) return create_node(ptr, size, id);
    if (ptr < node->start_ptr) node->left = avl_insert(node->left, ptr, size, id);
    else if (ptr > node->start_ptr) node->right = avl_insert(node->right, ptr, size, id);
    else return node;

    node->height = 1 + max(get_height(node->left), get_height(node->right));
    int balance = get_balance(node);

    if (balance > 1 && ptr < node->left->start_ptr) return rotate_right(node);
    if (balance < -1 && ptr > node->right->start_ptr) return rotate_left(node);
    if (balance > 1 && ptr > node->left->start_ptr) {
        node->left = rotate_left(node->left);
        return rotate_right(node);
    }
    if (balance < -1 && ptr < node->right->start_ptr) {
        node->right = rotate_right(node->right);
        return rotate_left(node);
    }
    return node;
}

AVLNode* avl_find_range(AVLNode* root, void* ptr) {
    if (!root) return NULL;
    char* start = (char*)root->start_ptr;
    char* end = start + root->size;

    if ((char*)ptr >= start && (char*)ptr < end) return root;
    if (ptr < root->start_ptr) return avl_find_range(root->left, ptr);
    return avl_find_range(root->right, ptr);
}

// --- 삭제 (가장 안전한 방식) ---
static AVLNode* min_value_node(AVLNode* node) {
    AVLNode* current = node;
    while (current->left != NULL) current = current->left;
    return current;
}

AVLNode* avl_delete(AVLNode* root, void* ptr) {
    if (!root) return NULL;

    if (ptr < root->start_ptr) root->left = avl_delete(root->left, ptr);
    else if (ptr > root->start_ptr) root->right = avl_delete(root->right, ptr);
    else {
        // 노드 발견!
        if (!root->left || !root->right) {
            AVLNode* temp = root->left ? root->left : root->right;
            if (!temp) {
                // 자식이 없는 경우
                free(root);
                return NULL;
            } else {
                // 자식이 하나인 경우: 값 복사가 아니라 노드 자체를 교체
                AVLNode* res = temp;
                free(root);
                return res; 
            }
        } else {
            // 자식이 둘인 경우: 오른쪽 서브트리에서 최소값 노드 찾기
            AVLNode* temp = min_value_node(root->right);
            root->start_ptr = temp->start_ptr;
            root->size = temp->size;
            root->id = temp->id;
            // 후계자 노드 삭제
            root->right = avl_delete(root->right, temp->start_ptr);
        }
    }

    // 높이 갱신 및 재균형
    root->height = 1 + max(get_height(root->left), get_height(root->right));
    int balance = get_balance(root);

    if (balance > 1 && get_balance(root->left) >= 0) return rotate_right(root);
    if (balance > 1 && get_balance(root->left) < 0) {
        root->left = rotate_left(root->left);
        return rotate_right(root);
    }
    if (balance < -1 && get_balance(root->right) <= 0) return rotate_left(root);
    if (balance < -1 && get_balance(root->right) > 0) {
        root->right = rotate_right(root->right);
        return rotate_left(root);
    }
    return root;
}

void avl_free_all(AVLNode* node) {
    if (!node) return;
    avl_free_all(node->left);
    avl_free_all(node->right);
    free(node->start_ptr);
    free(node);
}