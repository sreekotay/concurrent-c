package main

import (
	"fmt"
	"runtime"
	"sort"
	"sync/atomic"
	"time"
)

const (
	ITERATIONS = 1000000
	NUM_TRIALS = 7
)

// Prevent dead-code elimination of consumer work.
var sink int64

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO CHANNEL ISOLATION CHALLENGE\n")
	fmt.Printf("Iterations: %d | Trials: %d\n", ITERATIONS, NUM_TRIALS)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	ch1 := make(chan int, 1024)
	ch2 := make(chan int, 1024)

	baselineTimes := make([]float64, NUM_TRIALS)
	contentionTimes := make([]float64, NUM_TRIALS)

	for trial := 1; trial <= NUM_TRIALS; trial++ {
		// Baseline: one P/C pair.
		start := time.Now()
		done := make(chan bool, 2)
		var sum1 int64
		go func() {
			for i := 0; i < ITERATIONS; i++ {
				ch1 <- i ^ (i >> 16)
			}
			done <- true
		}()
		go func() {
			var s int64
			for i := 0; i < ITERATIONS; i++ {
				s += int64(<-ch1)
			}
			atomic.AddInt64(&sum1, s)
			done <- true
		}()
		<-done
		<-done
		atomic.AddInt64(&sink, sum1)
		baselineTimes[trial-1] = time.Since(start).Seconds() * 1000

		// Contention: two independent P/C pairs.
		start = time.Now()
		done = make(chan bool, 4)
		var sum2, sum3 int64
		go func() {
			for i := 0; i < ITERATIONS; i++ {
				ch1 <- i ^ (i >> 16)
			}
			done <- true
		}()
		go func() {
			var s int64
			for i := 0; i < ITERATIONS; i++ {
				s += int64(<-ch1)
			}
			atomic.AddInt64(&sum2, s)
			done <- true
		}()
		go func() {
			for i := 0; i < ITERATIONS; i++ {
				ch2 <- i ^ (i >> 16)
			}
			done <- true
		}()
		go func() {
			var s int64
			for i := 0; i < ITERATIONS; i++ {
				s += int64(<-ch2)
			}
			atomic.AddInt64(&sum3, s)
			done <- true
		}()
		<-done
		<-done
		<-done
		<-done
		atomic.AddInt64(&sink, sum2+sum3)
		contentionTimes[trial-1] = time.Since(start).Seconds() * 1000

		fmt.Printf("  Trial %d:  baseline=%6.2f ms  contention=%6.2f ms\n",
			trial, baselineTimes[trial-1], contentionTimes[trial-1])
	}

	bestBaseline := baselineTimes[0]
	bestContention := contentionTimes[0]
	for i := 1; i < NUM_TRIALS; i++ {
		if baselineTimes[i] < bestBaseline {
			bestBaseline = baselineTimes[i]
		}
		if contentionTimes[i] < bestContention {
			bestContention = contentionTimes[i]
		}
	}

	interference := (bestContention - bestBaseline) / bestBaseline * 100.0

	fmt.Printf("\n")
	fmt.Printf("  Best baseline:    %6.2f ms  (%8.0f ops/sec)\n",
		bestBaseline, float64(ITERATIONS)/bestBaseline*1000)
	fmt.Printf("  Best contention:  %6.2f ms  (%8.0f ops/sec per channel)\n",
		bestContention, float64(ITERATIONS)/bestContention*1000)
	fmt.Printf("\n")
	fmt.Printf("Interference: %.2f%%  (best-of-%d)\n", interference, NUM_TRIALS)
}
