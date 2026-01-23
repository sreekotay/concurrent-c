/*
 * Test for fiber context switching.
 * Compile: cc -o fiber_ctx_test fiber_ctx.c fiber_ctx_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include "fiber_ctx.h"

#define STACK_SIZE (64 * 1024)

static fiber_ctx main_ctx;
static fiber_ctx fiber1_ctx;
static fiber_ctx fiber2_ctx;

static void* stack1;
static void* stack2;

static void fiber1_entry(void* arg) {
    int* counter = (int*)arg;
    printf("fiber1: starting, counter=%d\n", *counter);
    
    for (int i = 0; i < 5; i++) {
        (*counter)++;
        printf("fiber1: counter=%d, switching to fiber2\n", *counter);
        fiber_ctx_switch(&fiber1_ctx, &fiber2_ctx);
        printf("fiber1: resumed, counter=%d\n", *counter);
    }
    
    printf("fiber1: done, switching to main\n");
    fiber_ctx_switch(&fiber1_ctx, &main_ctx);
}

static void fiber2_entry(void* arg) {
    int* counter = (int*)arg;
    printf("fiber2: starting, counter=%d\n", *counter);
    
    for (int i = 0; i < 5; i++) {
        (*counter) += 10;
        printf("fiber2: counter=%d, switching to fiber1\n", *counter);
        fiber_ctx_switch(&fiber2_ctx, &fiber1_ctx);
        printf("fiber2: resumed, counter=%d\n", *counter);
    }
    
    printf("fiber2: done, switching to main\n");
    fiber_ctx_switch(&fiber2_ctx, &main_ctx);
}

int main(void) {
    int counter = 0;
    
    printf("main: allocating stacks\n");
    stack1 = malloc(STACK_SIZE);
    stack2 = malloc(STACK_SIZE);
    
    if (!stack1 || !stack2) {
        fprintf(stderr, "Failed to allocate stacks\n");
        return 1;
    }
    
    printf("main: initializing fiber contexts\n");
    fiber_ctx_init(&fiber1_ctx, stack1, STACK_SIZE, fiber1_entry, &counter);
    fiber_ctx_init(&fiber2_ctx, stack2, STACK_SIZE, fiber2_entry, &counter);
    
    printf("main: switching to fiber1\n");
    fiber_ctx_switch(&main_ctx, &fiber1_ctx);
    
    printf("main: returned from fibers, counter=%d\n", counter);
    printf("main: expected counter=55 (5 increments of 1 + 5 increments of 10)\n");
    
    free(stack1);
    free(stack2);
    
    if (counter == 55) {
        printf("SUCCESS!\n");
        return 0;
    } else {
        printf("FAILED: counter=%d, expected 55\n", counter);
        return 1;
    }
}
