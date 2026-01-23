package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"time"
)

const ITERATIONS = 100000

func timeNowMs() float64 {
	return float64(time.Now().UnixNano()) / 1000000.0
}

func main() {
	fmt.Println("spawn_nursery_go: measuring nursery spawn throughput")

	runtime.GOMAXPROCS(runtime.NumCPU())

	start := timeNowMs()
	counter := int64(0)

	for batch := 0; batch < ITERATIONS/1000; batch++ {
		// Simulate nursery with goroutines
		done := make(chan bool, 1000)
		for i := 0; i < 1000; i++ {
			go func() {
				atomic.AddInt64(&counter, 1)
				done <- true
			}()
		}
		// Wait for all goroutines in this batch
		for i := 0; i < 1000; i++ {
			<-done
		}
	}

	elapsed := timeNowMs() - start
	totalSpawns := atomic.LoadInt64(&counter)
	spawnsPerSec := float64(totalSpawns) / (elapsed / 1000.0)

	fmt.Printf("  nursery spawns: %.0f spawns/sec (%.1f ms, total=%d)\n",
		spawnsPerSec, elapsed, totalSpawns)

	fmt.Println("spawn_nursery_go: DONE")
}