#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

typedef struct {
    int x;
    int y;
} Point;

int main(void) {
    CC_ARENA_STACK(a, 1024);
    
    CCArenaPool p;
    cc_arena_pool_init(&p, &a, sizeof(Point));
    
    // Allocate a few
    Point* p1 = (Point*)cc_arena_pool_alloc(&p);
    Point* p2 = (Point*)cc_arena_pool_alloc(&p);
    
    if (!p1 || !p2 || p1 == p2) {
        printf("FAIL: initial pool allocs\n");
        return 1;
    }
    
    p1->x = 10; p1->y = 20;
    p2->x = 30; p2->y = 40;
    
    // Free one
    cc_arena_pool_free(&p, p1);
    
    // Re-allocate - should get p1 back
    Point* p3 = (Point*)cc_arena_pool_alloc(&p);
    if (p3 != p1) {
        printf("FAIL: pool reuse\n");
        return 1;
    }
    
    if (p3->x != 10 || p3->y != 20) {
        // Note: pool doesn't zero memory, but it should be the same memory
    }
    
    // Test UFCS (via ccc build)
    // We'll use a .ccs file for that if needed, but we can test the C API here.
    
    printf("arena_pool_smoke ok\n");
    return 0;
}
