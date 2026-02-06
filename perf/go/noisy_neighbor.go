package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

const (
	NUM_HOGS              = 15
	HEARTBEAT_INTERVAL_MS = 100
	TEST_DURATION_SEC     = 5
)

var (
	heartbeats int64
	hogsActive int64
	stop       int32
)

func heartbeat() {
	fmt.Printf("[Heartbeat] Started\n")
	for atomic.LoadInt32(&stop) == 0 {
		time.Sleep(HEARTBEAT_INTERVAL_MS * time.Millisecond)
		val := atomic.AddInt64(&heartbeats, 1)
		fmt.Printf("[Heartbeat] Tick %d\n", val)
	}
}

func hog(id int) {
	atomic.AddInt64(&hogsActive, 1)
	fmt.Printf("[Hog %d] Started CPU-intensive loop...\n", id)

	x := 1.1
	for atomic.LoadInt32(&stop) == 0 {
		// Tight loop: no syscalls, no yields.
		// Go 1.14+ should preempt this asynchronously.
		for i := 0; i < 1000000; i++ {
			x = x * x
			if x > 1000000.0 {
				x = 1.1
			}
		}
	}

	fmt.Printf("[Hog %d] Stopped\n", id)
	atomic.AddInt64(&hogsActive, -1)
}

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO NOISY NEIGHBOR CHALLENGE\n")
	fmt.Printf("Workers: %d | CPU Hogs: %d\n", runtime.NumCPU(), NUM_HOGS)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	go heartbeat()

	// Give heartbeat a head start
	time.Sleep(500 * time.Millisecond)
	fmt.Printf("Initial heartbeats: %d (Healthy)\n", atomic.LoadInt64(&heartbeats))

	fmt.Printf("\n!!! Unleashing CPU Hogs !!!\n")
	for i := 0; i < NUM_HOGS; i++ {
		go hog(i)
	}

	time.Sleep(TEST_DURATION_SEC * time.Second)
	atomic.StoreInt32(&stop, 1)

	// Wait for hogs to stop
	for atomic.LoadInt64(&hogsActive) > 0 {
		time.Sleep(10 * time.Millisecond)
	}

	finalBeats := atomic.LoadInt64(&heartbeats)
	fmt.Printf("\n=================================================================\n")
	fmt.Printf("FINAL RESULTS\n")
	fmt.Printf("Total Heartbeats: %d\n", finalBeats)

	expected := (TEST_DURATION_SEC * 1000 / HEARTBEAT_INTERVAL_MS)
	if finalBeats >= int64(float64(expected)*0.8) {
		fmt.Printf("RESULT: PASS - Go is fair even with CPU hogs!\n")
	} else {
		fmt.Printf("RESULT: FAIL - Go was starved by CPU hogs.\n")
		fmt.Printf("Heartbeat efficiency: %.1f%%\n", float64(finalBeats)*100.0/float64(expected))
	}
	fmt.Printf("=================================================================\n")
}
