package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

const ITERATIONS = 100000

func timeNowMs() float64 {
	return float64(time.Now().UnixNano()) / 1000000.0
}

func main() {
	fmt.Println("spawn_nursery_go: nursery spawn + par-for increment")

	runtime.GOMAXPROCS(runtime.NumCPU())

	start := timeNowMs()
	counter := int64(0)

	for batch := 0; batch < ITERATIONS/1000; batch++ {
		// Join bag: goroutines + done chan (not a nursery).
		done := make(chan bool, 1000)
		for i := 0; i < 1000; i++ {
			go func() {
				atomic.AddInt64(&counter, 1)
				done <- true
			}()
		}
		for i := 0; i < 1000; i++ {
			<-done
		}
	}

	elapsed := timeNowMs() - start
	totalSpawns := atomic.LoadInt64(&counter)
	spawnsPerSec := float64(totalSpawns) / (elapsed / 1000.0)

	fmt.Printf("  nursery spawns: %.0f spawns/sec (%.1f ms, total=%d)\n",
		spawnsPerSec, elapsed, totalSpawns)

	// Twin of CC `@parallel for (i in 0..N)`: P contiguous spans, same
	// shared atomic. Not a goroutine per increment.
	var parCounter int64
	n := ITERATIONS
	p := runtime.GOMAXPROCS(0)
	chunk := (n + p - 1) / p
	start = timeNowMs()
	var wg sync.WaitGroup
	for w := 0; w < p; w++ {
		lo := w * chunk
		hi := lo + chunk
		if hi > n {
			hi = n
		}
		if lo >= n {
			break
		}
		wg.Add(1)
		go func(lo, hi int) {
			defer wg.Done()
			for i := lo; i < hi; i++ {
				atomic.AddInt64(&parCounter, 1)
			}
		}(lo, hi)
	}
	wg.Wait()
	elapsed = timeNowMs() - start
	parTotal := atomic.LoadInt64(&parCounter)
	if parTotal != int64(n) {
		fmt.Printf("  par for: expected %d got %d\n", n, parTotal)
		return
	}
	parPerSec := float64(parTotal) / (elapsed / 1000.0)
	fmt.Printf("  par for: %.0f ops/sec (%.1f ms, total=%d)\n",
		parPerSec, elapsed, parTotal)

	fmt.Println("spawn_nursery_go: DONE")
}
