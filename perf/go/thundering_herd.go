package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

const (
	NUM_WAITERS = 1000
	NUM_SAMPLES = 5
)

func waiter(ch chan int, count *int64) {
	<-ch
	atomic.AddInt64(count, 1)
}

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO THUNDERING HERD CHALLENGE\n")
	fmt.Printf("Waiters: %d | Samples: %d\n", NUM_WAITERS, NUM_SAMPLES)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	ch := make(chan int)
	var count int64

	for sample := 1; sample <= NUM_SAMPLES; sample++ {
		atomic.StoreInt64(&count, 0)

		for i := 0; i < NUM_WAITERS; i++ {
			go waiter(ch, &count)
		}

		// Give them time to block
		time.Sleep(100 * time.Millisecond)

		start := time.Now()
		// Send one item. Go should wake exactly one.
		ch <- 42

		// Wait for the first one to finish
		for atomic.LoadInt64(&count) < 1 {
			runtime.Gosched()
		}
		latency := time.Since(start)
		fmt.Printf("Sample %d: Latency to wake 1st waiter: %8.4f ms\n", sample, float64(latency.Nanoseconds())/1e6)

		// Flush the rest
		for i := 1; i < NUM_WAITERS; i++ {
			ch <- i
		}
		// Wait for all to finish before next sample
		for atomic.LoadInt64(&count) < int64(NUM_WAITERS) {
			runtime.Gosched()
		}
	}
}
