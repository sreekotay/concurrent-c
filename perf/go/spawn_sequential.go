package main

import (
	"fmt"
	"runtime"
	"time"
)

const ITERATIONS = 10000

func timeNowMs() float64 {
	return float64(time.Now().UnixNano()) / 1000000.0
}

func simpleTask(x int) int {
	return x * 2
}

func main() {
	fmt.Println("spawn_sequential_go: measuring sequential spawn+join throughput")

	runtime.GOMAXPROCS(1) // Sequential execution

	start := timeNowMs()
	total := 0

	for i := 0; i < ITERATIONS; i++ {
		// Simulate sequential goroutine spawn and join
		done := make(chan int, 1)
		go func(val int) {
			done <- simpleTask(val)
		}(i)
		total += <-done
	}

	elapsed := timeNowMs() - start
	spawnsPerSec := float64(ITERATIONS) / (elapsed / 1000.0)

	fmt.Printf("  sequential spawns: %.0f spawns/sec (%.1f ms, sum=%d)\n",
		spawnsPerSec, elapsed, total)

	fmt.Println("spawn_sequential_go: DONE")
}