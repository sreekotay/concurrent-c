package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

/*
#include <time.h>
// Raw nanosleep — blocks the calling OS thread (M) in the kernel.
// Go's runtime detaches P from the blocked M so other Gs can still run,
// but the M itself is parked until the syscall returns.
static void raw_nanosleep(long sec, long nsec) {
    struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
    nanosleep(&ts, NULL);
}
*/
import "C"

const (
	NUM_KIDNAPPERS = 100
	TEST_DURATION  = 3 * time.Second
)

var (
	heartbeats     int64
	stop           int32
	kidnappersDone int64
)

func kidnapper() {
	// One kidnap unit of work: raw nanosleep(2s) blocks this M in the kernel.
	C.raw_nanosleep(2, 0)
	atomic.AddInt64(&kidnappersDone, 1)
}

func heartbeat() {
	// Raw nanosleep — every tick blocks the heartbeat's M in the kernel.
	// If Go's runtime can promote a fresh M when one is blocked, this still ticks.
	for atomic.LoadInt32(&stop) == 0 {
		C.raw_nanosleep(0, 100*1000*1000) // 100ms
		atomic.AddInt64(&heartbeats, 1)
		fmt.Printf("[Heartbeat] Tick %d\n", atomic.LoadInt64(&heartbeats))
	}
}

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO SYSCALL KIDNAPPING CHALLENGE\n")
	fmt.Printf("Kidnappers: %d | Duration: %v\n", NUM_KIDNAPPERS, TEST_DURATION)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	go heartbeat()

	fmt.Printf("\n!!! Unleashing Kidnappers (raw syscall.Nanosleep) !!!\n")
	for i := 0; i < NUM_KIDNAPPERS; i++ {
		go kidnapper()
	}

	time.Sleep(TEST_DURATION)
	atomic.StoreInt32(&stop, 1)

	finalBeats := atomic.LoadInt64(&heartbeats)
	finalDone := atomic.LoadInt64(&kidnappersDone)
	fmt.Printf("\n=================================================================\n")
	fmt.Printf("FINAL RESULTS\n")
	fmt.Printf("Total Heartbeats:     %d\n", finalBeats)
	fmt.Printf("Kidnappers Completed: %d / %d\n", finalDone, NUM_KIDNAPPERS)
	fmt.Printf("=================================================================\n")
}
