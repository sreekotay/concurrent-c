package main

import (
	"fmt"
	"runtime"
	"time"
)

const (
	ITERATIONS = 1000000
	NUM_TRIALS = 3
)

func producer(ch chan int, count int) {
	for i := 0; i < count; i++ {
		ch <- i
	}
}

func consumer(ch chan int, count int) {
	for i := 0; i < count; i++ {
		<-ch
	}
}

func main() {
	fmt.Printf("=================================================================\n")
	fmt.Printf("GO CHANNEL CONTENTION CHALLENGE\n")
	fmt.Printf("Iterations: %d | Trials: %d\n", ITERATIONS, NUM_TRIALS)
	fmt.Printf("=================================================================\n\n")

	runtime.GOMAXPROCS(runtime.NumCPU())

	ch1 := make(chan int, 1024)
	ch2 := make(chan int, 1024)

	for trial := 1; trial <= NUM_TRIALS; trial++ {
		fmt.Printf("Trial %d:\n", trial)

		// 1. Baseline: Run Channel 1 alone
		start := time.Now()
		done := make(chan bool, 2)
		go func() { producer(ch1, ITERATIONS); done <- true }()
		go func() { consumer(ch1, ITERATIONS); done <- true }()
		<-done
		<-done
		baselineElapsed := time.Since(start)
		baselineOpsSec := float64(ITERATIONS) / baselineElapsed.Seconds()
		fmt.Printf("  Baseline (Ch1 only):  %8.2f ms (%8.0f ops/sec)\n", baselineElapsed.Seconds()*1000, baselineOpsSec)

		// 2. Contention: Run Channel 1 while Channel 2 is hammered
		start = time.Now()
		done = make(chan bool, 4)
		go func() { producer(ch1, ITERATIONS); done <- true }()
		go func() { consumer(ch1, ITERATIONS); done <- true }()
		go func() { producer(ch2, ITERATIONS); done <- true }()
		go func() { consumer(ch2, ITERATIONS); done <- true }()
		<-done
		<-done
		<-done
		<-done
		contentionElapsed := time.Since(start)
		contentionOpsSec := float64(ITERATIONS*2) / contentionElapsed.Seconds()
		fmt.Printf("  Contention (Ch1+Ch2): %8.2f ms (%8.0f ops/sec total)\n", contentionElapsed.Seconds()*1000, contentionOpsSec)

		throughputDrop := (baselineOpsSec - (contentionOpsSec / 2.0)) / baselineOpsSec * 100.0
		fmt.Printf("  Throughput Drop:      %8.2f%%\n\n", throughputDrop)
	}
}
