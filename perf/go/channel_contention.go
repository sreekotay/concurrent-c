package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const (
	DefaultMessages  = 1000000
	DefaultTrials    = 15
	DefaultProducers = 8
	DefaultConsumers = 8
)

var sink int64

func envIntOrDefault(name string, fallback int, minValue int) int {
	v := os.Getenv(name)
	if v == "" {
		return fallback
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < minValue {
		return fallback
	}
	return n
}

func benchMessages() int {
	return envIntOrDefault("CC_CONTENTION_ITERATIONS", DefaultMessages, 1)
}

func benchTrials() int {
	return envIntOrDefault("CC_CONTENTION_TRIALS", DefaultTrials, 1)
}

func benchProducers() int {
	return envIntOrDefault("CC_CONTENTION_PRODUCERS", DefaultProducers, 1)
}

func benchConsumers() int {
	return envIntOrDefault("CC_CONTENTION_CONSUMERS", DefaultConsumers, 1)
}

func workShare(total int, idx int, workers int) int {
	base := total / workers
	rem := total % workers
	if idx < rem {
		return base + 1
	}
	return base
}

func runSharedCase(producers int, consumers int, messages int) float64 {
	ch := make(chan int, 1024)
	sums := make([]int64, consumers)

	start := time.Now()

	var producerWG sync.WaitGroup
	var consumerWG sync.WaitGroup
	producerWG.Add(producers)
	consumerWG.Add(consumers)

	for c := 0; c < consumers; c++ {
		idx := c
		go func() {
			defer consumerWG.Done()
			var local int64
			for v := range ch {
				local += int64(v)
			}
			sums[idx] = local
		}()
	}

	for p := 0; p < producers; p++ {
		producerID := p
		sendCount := workShare(messages, p, producers)
		go func() {
			defer producerWG.Done()
			for i := 0; i < sendCount; i++ {
				ch <- ((producerID + 1) ^ (i << 1) ^ (i >> 16))
			}
		}()
	}

	go func() {
		producerWG.Wait()
		close(ch)
	}()

	consumerWG.Wait()
	for _, sum := range sums {
		atomic.AddInt64(&sink, sum)
	}
	return time.Since(start).Seconds() * 1000
}

func main() {
	messages := benchMessages()
	trials := benchTrials()
	producers := benchProducers()
	consumers := benchConsumers()

	fmt.Printf("=================================================================\n")
	fmt.Printf("GO SHARED CHANNEL CONTENTION\n")
	fmt.Printf("Messages: %d | Trials: %d | Contention: %dx%d\n", messages, trials, producers, consumers)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	baselineTimes := make([]float64, trials)
	contentionTimes := make([]float64, trials)

	for trial := 1; trial <= trials; trial++ {
		baselineTimes[trial-1] = runSharedCase(1, 1, messages)
		contentionTimes[trial-1] = runSharedCase(producers, consumers, messages)
		fmt.Printf("  Trial %d:  baseline=%6.2f ms  contention=%6.2f ms\n",
			trial, baselineTimes[trial-1], contentionTimes[trial-1])
	}

	bestBaseline := baselineTimes[0]
	bestContention := contentionTimes[0]
	for i := 1; i < trials; i++ {
		if baselineTimes[i] < bestBaseline {
			bestBaseline = baselineTimes[i]
		}
		if contentionTimes[i] < bestContention {
			bestContention = contentionTimes[i]
		}
	}

	interference := (bestContention - bestBaseline) / bestBaseline * 100.0

	fmt.Printf("\n")
	fmt.Printf("  Best baseline:    %6.2f ms  (%8.0f msgs/sec)\n",
		bestBaseline, float64(messages)/bestBaseline*1000)
	fmt.Printf("  Best contention:  %6.2f ms  (%8.0f msgs/sec)\n",
		bestContention, float64(messages)/bestContention*1000)
	fmt.Printf("\n")
	fmt.Printf("Interference: %.2f%%  (best-of-%d)\n", interference, trials)
}
