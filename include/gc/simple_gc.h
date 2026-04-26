#ifndef SIMPLE_GC_H
#define SIMPLE_GC_H

#include <stddef.h>

void GC_init(void);

/* 컴파일 시 -DUSE_THREAD_LOCAL_GC 옵션으로 제어합니다. */
#ifdef USE_THREAD_LOCAL_GC

    /* =========================================
     * [신버전] Thread-Local GC (투트랙 API)
     * ========================================= */
    void* GC_malloc(size_t size, int is_shared);
    void GC_write_barrier(void* obj_ptr, void** field_ptr, void* value);
    void GC_collect_local(void);
    void GC_collect_global(void);
    
    /* 구버전 코드와의 호환성을 위한 매크로 매핑 */
    #define GC_collect() GC_collect_global()

#else

    /* =========================================
     * [구버전] Global Parallel GC (단일 API)
     * ========================================= */
    void* GC_malloc(size_t size);
    void GC_collect(void);
    
    /* 구버전에서는 쓰기 장벽이 필요 없으므로 단순 대입으로 무효화(Bypass) */
    #define GC_write_barrier(obj_ptr, field_ptr, value) (*(field_ptr) = (value))

#endif

#endif // SIMPLE_GC_H
