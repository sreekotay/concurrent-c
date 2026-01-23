package main

import (
	"fmt"
	"runtime"
	"time"
)

const ITERATIONS = 100000

func timeNowMs() float64 {
	return float64(time.Now().UnixNano()) / 1000000.0
}

func benchSingleThread() {
	ch := make(chan int, 1000)

	start := timeNowMs()

	// Alternate send/recv to avoid filling buffer
	for i := 0; i < ITERATIONS; i++ {
		ch <- i
		<-ch
	}

	elapsed := timeNowMs() - start
	opsPerSec := (ITERATIONS * 2.0) / (elapsed / 1000.0)

	fmt.Printf("  single-thread (no contention): %.0f ops/sec (%.1f ms)\n",
		opsPerSec, elapsed)

	close(ch)
}

func benchBuffered() {
	ch := make(chan int, 1000)

	start := timeNowMs()

	done := make(chan bool, 2)

	// Producer
	go func() {
		for i := 0; i < ITERATIONS; i++ {
			ch <- i
		}
		done <- true
	}()

	// Consumer
	go func() {
		for i := 0; i < ITERATIONS; i++ {
			<-ch
		}
		done <- true
	}()

	// Wait for both
	<-done
	<-done

	elapsed := timeNowMs() - start
	opsPerSec := (ITERATIONS * 2.0) / (elapsed / 1000.0) // send + recv

	fmt.Printf("  buffered (cap=1000): %.0f ops/sec (%.1f ms for %d pairs)\n",
		opsPerSec, elapsed, ITERATIONS)

	close(ch)
}

func benchUnbuffered() {
	ch := make(chan int) // unbuffered
	iterations := ITERATIONS / 10

	start := timeNowMs()

	done := make(chan bool, 2)

	// Producer
	go func() {
		for i := 0; i < iterations; i++ {
			ch <- i
		}
		done <- true
	}()

	// Consumer
	go func() {
		for i := 0; i < iterations; i++ {
			<-ch
		}
		done <- true
	}()

	// Wait for both
	<-done
	<-done

	elapsed := timeNowMs() - start
	opsPerSec := (float64(iterations) * 2.0) / (elapsed / 1000.0)

	fmt.Printf("  unbuffered (rendezvous): %.0f ops/sec (%.1f ms for %d pairs)\n",
		opsPerSec, elapsed, iterations)
}

func main() {
	fmt.Println("channel_throughput_go: measuring channel performance")

	runtime.GOMAXPROCS(runtime.NumCPU())

	benchSingleThread()
	benchBuffered()
	benchUnbuffered()

	fmt.Println("channel_throughput_go: DONE")
}