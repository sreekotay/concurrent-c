// Go peer for stress/compare_arena_memory.sh — fair bump arena protocol.
// Slab/overflow backing uses C.malloc/C.free so reset/reclaim is timed honestly
// (not deferred to the Go GC).
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
	"unsafe"
)

func envZU(k string, d uint64) uint64 {
	v := os.Getenv(k)
	if v == "" {
		return d
	}
	n, err := strconv.ParseUint(v, 10, 64)
	if err != nil {
		return d
	}
	return n
}

func cAlloc(n int) []byte {
	if n <= 0 {
		return nil
	}
	p := C.malloc(C.size_t(n))
	if p == nil {
		panic("malloc")
	}
	return unsafe.Slice((*byte)(p), n)
}

func cFree(b []byte) {
	if len(b) == 0 {
		return
	}
	C.free(unsafe.Pointer(unsafe.SliceData(b)))
}

type slab struct {
	base []byte
	off  int
	prev *slab
}

type bump struct {
	cur      *slab
	nslabs   int
	blockMax int
	ovf      [][]byte
	ovfBytes int
	live     int
}

func (b *bump) init(root int, blockMax int) {
	b.cur = &slab{base: cAlloc(root)}
	b.nslabs = 1
	b.blockMax = blockMax
	b.ovf = nil
	b.ovfBytes = 0
	b.live = 0
}

func (b *bump) reset() {
	for _, p := range b.ovf {
		cFree(p)
	}
	b.ovf = nil
	b.ovfBytes = 0
	for b.cur != nil && b.cur.prev != nil {
		s := b.cur
		b.cur = s.prev
		cFree(s.base)
		b.nslabs--
	}
	if b.cur != nil {
		b.cur.off = 0
	}
	b.live = 0
}

func (b *bump) free() {
	b.reset()
	if b.cur != nil {
		cFree(b.cur.base)
		b.cur = nil
	}
	b.nslabs = 0
}

func alignUp(v, a int) int { return (v + a - 1) & ^(a - 1) }

func (b *bump) grow(need int) bool {
	if b.blockMax > 0 && b.nslabs >= b.blockMax {
		return false
	}
	cap := len(b.cur.base) + len(b.cur.base)/2
	if cap < need {
		cap = need
	}
	if cap < 4096 {
		cap = 4096
	}
	ns := &slab{base: cAlloc(cap), prev: b.cur}
	b.cur = ns
	b.nslabs++
	return true
}

func (b *bump) alloc(n, align int) []byte {
	off := alignUp(b.cur.off, align)
	end := off + n
	if end > len(b.cur.base) {
		if b.grow(n + align) {
			off = alignUp(b.cur.off, align)
			end = off + n
		} else {
			p := cAlloc(n)
			b.ovf = append(b.ovf, p)
			b.ovfBytes += n
			b.live += n
			return p
		}
	}
	p := b.cur.base[off:end:end]
	b.cur.off = end
	b.live += n
	return p
}

func (b *bump) realloc(ptr []byte, oldN, newN, align int, moves *uint64) []byte {
	if ptr == nil {
		return b.alloc(newN, align)
	}
	base := b.cur.base
	if len(base) > 0 && len(ptr) > 0 {
		start := int(uintptr(unsafe.Pointer(unsafe.SliceData(ptr))) - uintptr(unsafe.Pointer(unsafe.SliceData(base))))
		if start >= 0 && start < len(base) && start+oldN == b.cur.off {
			end := start + newN
			if newN <= oldN || end <= len(base) {
				b.cur.off = end
				b.live = b.live - oldN + newN
				return base[start:end:end]
			}
		}
	}
	np := b.alloc(newN, align)
	copy(np, ptr[:oldN])
	*moves++
	return np
}

func (b *bump) gross() int {
	g := b.ovfBytes
	for s := b.cur; s != nil; s = s.prev {
		g += len(s.base)
	}
	return g
}

func bulkSize(i int) int { return 16 + (i*17)%240 }

func main() {
	tipRounds := int(envZU("ARENA_MEM_TIP_ROUNDS", 400))
	tipSteps := int(envZU("ARENA_MEM_TIP_STEPS", 5000))
	tipRoot := int(envZU("ARENA_MEM_TIP_ROOT", 8*1024*1024))
	bulkN := int(envZU("ARENA_MEM_BULK", 2000000))
	bulkRoot := int(envZU("ARENA_MEM_BULK_ROOT", 64*1024*1024))
	ovfN := int(envZU("ARENA_MEM_OVF", 200000))
	ovfRoot := int(envZU("ARENA_MEM_OVF_ROOT", 4096))
	churnN := int(envZU("ARENA_MEM_CHURN", 1000))
	churnEach := int(envZU("ARENA_MEM_CHURN_EACH", 128))
	blockMax := int(envZU("ARENA_MEM_BLOCK_MAX", 4))

	fmt.Printf("arena_memory_bench(go): tip=%dx%d bulk=%d ovf=%d block_max=%d\n",
		tipRounds, tipSteps, bulkN, ovfN, blockMax)

	var sink byte
	var moves uint64
	var a bump
	a.init(tipRoot, blockMax)
	t0 := time.Now()
	for r := 0; r < tipRounds; r++ {
		var p []byte
		sz := 0
		for i := 0; i < tipSteps; i++ {
			want := sz + 16 + (i % 17)
			p = a.realloc(p, sz, want, 8, &moves)
			p[sz] = byte(i)
			sz = want
		}
		sink ^= p[0]
		a.reset()
	}
	a.free()
	tipMs := float64(time.Since(t0).Microseconds()) / 1000.0

	// bulk_ms / ovf_ms = allocate + reclaim (full ownership).
	a.init(bulkRoot, blockMax)
	t0 = time.Now()
	for i := 0; i < bulkN; i++ {
		q := a.alloc(bulkSize(i), 8)
		q[0] = byte(i)
		sink ^= q[0]
	}
	bulkGross := a.gross()
	bulkOvf := a.ovfBytes
	tReset := time.Now()
	a.reset()
	resetMs := float64(time.Since(tReset).Microseconds()) / 1000.0
	a.free()
	bulkMs := float64(time.Since(t0).Microseconds()) / 1000.0

	a.init(ovfRoot, blockMax)
	t0 = time.Now()
	for i := 0; i < ovfN; i++ {
		n := 24 + (i*17)%512
		q := a.alloc(n, 8)
		q[0] = byte(i)
		sink ^= q[0]
	}
	ovfGross := a.gross()
	ovfBytes := a.ovfBytes
	a.free() // reclaim extents + overflow inside ovf_ms
	ovfMs := float64(time.Since(t0).Microseconds()) / 1000.0

	t0 = time.Now()
	for i := 0; i < churnN; i++ {
		var c bump
		c.init(16*1024, blockMax)
		for j := 0; j < churnEach; j++ {
			_ = c.alloc(16+(j*31)%256, 8)
		}
		c.free()
	}
	churnMs := float64(time.Since(t0).Microseconds()) / 1000.0
	runtime.KeepAlive(sink)

	fmt.Printf("RESULT lang=go tip_ms=%.3f tip_moves=%d bulk_ms=%.3f bulk_gross=%d bulk_ovf=%d "+
		"ovf_ms=%.3f ovf_gross=%d ovf_bytes=%d reset_ms=%.3f churn_ms=%.3f sink=%d\n",
		tipMs, moves, bulkMs, bulkGross, bulkOvf,
		ovfMs, ovfGross, ovfBytes, resetMs, churnMs, sink)
}
