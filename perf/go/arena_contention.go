package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

const (
	NUM_GOROUTINES    = 16
	ALLOCS_PER_ROUTINE = 62500
)

var (
	successCount int64
)

func mallocWorker() {
	// Go doesn't have arenas, so we just allocate on the heap.
	// This is the "Convenience" path.
	for i := 0; i < ALLOCS_PER_ROUTINE; i++ {
		// Allocate 16 bytes
		_ = make([]byte, 16)
		atomic.AddInt64(&successCount, 1)
	}
}

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO HEAP ALLOCATION CHALLENGE\n")
	fmt.Printf("Goroutines: %d | Allocs per routine: %d\n", NUM_GOROUTINES, ALLOCS_PER_ROUTINE)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	start := time.Now()
	done := make(chan bool, NUM_GOROUTINES)
	for i := 0; i < NUM_GOROUTINES; i++ {
		go func() {
			mallocWorker()
			done <- true
		}()
	}

	for i := 0; i < NUM_GOROUTINES; i++ {
		<-done
	}
	duration := time.Since(start)

	success := atomic.LoadInt64(&successCount)
	fmt.Printf("Results:\n")
	fmt.Printf("  Success: %d\n", success)
	fmt.Printf("  Time:    %.2f ms\n", float64(duration.Nanoseconds())/1e6)
	fmt.Printf("  Throughput: %.2f M allocs/sec\n", float64(success)/duration.Seconds()/1e6)
	fmt.Printf("=================================================================\n")
}
