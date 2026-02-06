package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

/*
#include <unistd.h>
*/
import "C"

const (
	NUM_KIDNAPPERS = 100
	TEST_DURATION  = 5 * time.Second
)

var (
	heartbeats int64
	stop       int32
)

func kidnapper() {
	for atomic.LoadInt32(&stop) == 0 {
		// Use C.usleep to block the OS thread directly.
		// Go's time.Sleep is runtime-aware and doesn't block the thread.
		C.usleep(100000) // 100ms
	}
}

func heartbeat() {
	for atomic.LoadInt32(&stop) == 0 {
		time.Sleep(100 * time.Millisecond)
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

	// Give heartbeat a head start
	time.Sleep(500 * time.Millisecond)

	fmt.Printf("\n!!! Unleashing Kidnappers (raw syscall.Nanosleep) !!!\n")
	for i := 0; i < NUM_KIDNAPPERS; i++ {
		go kidnapper()
	}

	time.Sleep(TEST_DURATION)
	atomic.StoreInt32(&stop, 1)

	finalBeats := atomic.LoadInt64(&heartbeats)
	fmt.Printf("\n=================================================================\n")
	fmt.Printf("FINAL RESULTS\n")
	fmt.Printf("Total Heartbeats: %d\n", finalBeats)
	fmt.Printf("=================================================================\n")
}
