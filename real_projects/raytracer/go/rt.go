// Go port of the Shirley weekend final scene.
//
// Same algorithm as rt.c / rt.ccs. Per-pixel LCG so row goroutines
// match sequential. RT_SEQ=1 runs an ordinary for.
//
//	RT_WIDTH / RT_SAMPLES / RT_DEPTH   image (default 400 / 10 / 20)
//	RT_SMOKE=1                         48 x 27, 2 spp, depth 8
//	RT_PPM=path                        write a binary P6 PPM
//
// Book: https://raytracing.github.io/books/RayTracingInOneWeekend.html
package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
	"sync"
	"time"
)

const (
	worldCap = 512
	tMin     = 0.001
	tMax     = 1.0e30
)

type vec3 struct{ x, y, z float64 }

func v3(x, y, z float64) vec3 { return vec3{x, y, z} }
func (a vec3) add(b vec3) vec3 {
	return vec3{a.x + b.x, a.y + b.y, a.z + b.z}
}
func (a vec3) sub(b vec3) vec3 {
	return vec3{a.x - b.x, a.y - b.y, a.z - b.z}
}
func (a vec3) hadamard(b vec3) vec3 {
	return vec3{a.x * b.x, a.y * b.y, a.z * b.z}
}
func (a vec3) scale(t float64) vec3 { return vec3{a.x * t, a.y * t, a.z * t} }
func (a vec3) neg() vec3            { return vec3{-a.x, -a.y, -a.z} }
func (a vec3) dot(b vec3) float64   { return a.x*b.x + a.y*b.y + a.z*b.z }
func (a vec3) cross(b vec3) vec3 {
	return vec3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}
}
func (a vec3) len2() float64 { return a.dot(a) }
func (a vec3) len() float64  { return math.Sqrt(a.len2()) }
func (a vec3) unit() vec3    { return a.scale(1.0 / a.len()) }
func (a vec3) nearZero() bool {
	const s = 1.0e-8
	return math.Abs(a.x) < s && math.Abs(a.y) < s && math.Abs(a.z) < s
}
func (v vec3) reflect(n vec3) vec3 {
	return v.sub(n.scale(2.0 * v.dot(n)))
}
func (uv vec3) refract(n vec3, etaiOverEtat float64) vec3 {
	cosTheta := uv.neg().dot(n)
	if cosTheta > 1.0 {
		cosTheta = 1.0
	}
	rOutPerp := uv.add(n.scale(cosTheta)).scale(etaiOverEtat)
	par := 1.0 - rOutPerp.len2()
	if par < 0.0 {
		par = 0.0
	}
	return rOutPerp.add(n.scale(-math.Sqrt(par)))
}

type ray struct{ o, d vec3 }

func rayAt(o, d vec3) ray { return ray{o, d} }
func (r ray) pos(t float64) vec3 {
	return r.o.add(r.d.scale(t))
}

type rng struct{ s uint64 }

func (r *rng) u32() uint32 {
	r.s = r.s*6364136223846793005 + 1
	return uint32(r.s >> 32)
}
func (r *rng) f() float64 { return float64(r.u32()) * (1.0 / 4294967296.0) }
func (r *rng) rngRange(lo, hi float64) float64 {
	return lo + (hi-lo)*r.f()
}
func (r *rng) vec() vec3 { return v3(r.f(), r.f(), r.f()) }
func (r *rng) vecRange(lo, hi float64) vec3 {
	return v3(r.rngRange(lo, hi), r.rngRange(lo, hi), r.rngRange(lo, hi))
}
func (r *rng) unit() vec3 {
	for {
		p := r.vecRange(-1.0, 1.0)
		lensq := p.len2()
		if 1.0e-160 < lensq && lensq <= 1.0 {
			return p.scale(1.0 / math.Sqrt(lensq))
		}
	}
}
func (r *rng) inUnitDisk() vec3 {
	for {
		p := v3(r.rngRange(-1.0, 1.0), r.rngRange(-1.0, 1.0), 0.0)
		if p.len2() < 1.0 {
			return p
		}
	}
}

func pixelSeed(x, y int) uint64 {
	s := uint64(0x9E3779B97F4A7C15)
	s ^= uint64(uint32(x)) * 0xBF58476D1CE4E5B9
	s ^= uint64(uint32(y)) * 0x94D049BB133111EB
	return s | 1
}

type matKind int

const (
	matLambert matKind = 0
	matMetal   matKind = 1
	matGlass   matKind = 2
)

type material struct {
	kind   matKind
	albedo vec3
	fuzz   float64
	ir     float64
}

type sphere struct {
	center vec3
	radius float64
	mat    material
}

type hit struct {
	p, n  vec3
	t     float64
	front bool
	mat   material
}

type camera struct {
	center, pixel00, du, dv vec3
	defocusU, defocusV      vec3
	defocusAngle            float64
}

type renderJob struct {
	fb     []byte
	w, h   int
	spp    int
	depth  int
	cam    camera
	world  []sphere
	nworld int
}

func sphereHit(s *sphere, r ray, tmin, tmax float64, rec *hit) bool {
	oc := s.center.sub(r.o)
	a := r.d.len2()
	h := r.d.dot(oc)
	c := oc.len2() - s.radius*s.radius
	disc := h*h - a*c
	if disc < 0.0 {
		return false
	}
	sqrtd := math.Sqrt(disc)
	root := (h - sqrtd) / a
	if root <= tmin || root >= tmax {
		root = (h + sqrtd) / a
		if root <= tmin || root >= tmax {
			return false
		}
	}
	rec.t = root
	rec.p = r.pos(rec.t)
	outward := rec.p.sub(s.center).scale(1.0 / s.radius)
	rec.front = r.d.dot(outward) < 0.0
	if rec.front {
		rec.n = outward
	} else {
		rec.n = outward.neg()
	}
	rec.mat = s.mat
	return true
}

func worldHit(world []sphere, r ray, rec *hit) bool {
	var tmp hit
	hitAny := false
	closest := tMax
	for i := range world {
		if sphereHit(&world[i], r, tMin, closest, &tmp) {
			hitAny = true
			closest = tmp.t
			*rec = tmp
		}
	}
	return hitAny
}

func reflectance(cosine, ri float64) float64 {
	r0 := (1.0 - ri) / (1.0 + ri)
	r0 = r0 * r0
	x := 1.0 - cosine
	x2 := x * x
	return r0 + (1.0-r0)*x2*x2*x
}

func scatter(rIn ray, rec *hit, rnd *rng, attn *vec3, scattered *ray) bool {
	m := rec.mat
	if m.kind == matLambert {
		dir := rec.n.add(rnd.unit())
		if dir.nearZero() {
			dir = rec.n
		}
		*scattered = rayAt(rec.p, dir)
		*attn = m.albedo
		return true
	}
	if m.kind == matMetal {
		reflected := rIn.d.reflect(rec.n).unit()
		reflected = reflected.add(rnd.unit().scale(m.fuzz))
		*scattered = rayAt(rec.p, reflected)
		*attn = m.albedo
		return scattered.d.dot(rec.n) > 0.0
	}
	ri := m.ir
	if rec.front {
		ri = 1.0 / m.ir
	}
	unitD := rIn.d.unit()
	cosTheta := unitD.neg().dot(rec.n)
	if cosTheta > 1.0 {
		cosTheta = 1.0
	}
	sinTheta := math.Sqrt(1.0 - cosTheta*cosTheta)
	cannot := ri*sinTheta > 1.0
	var direction vec3
	if cannot || reflectance(cosTheta, ri) > rnd.f() {
		direction = unitD.reflect(rec.n)
	} else {
		direction = unitD.refract(rec.n, ri)
	}
	*scattered = rayAt(rec.p, direction)
	*attn = v3(1.0, 1.0, 1.0)
	return true
}

func rayColor(r ray, depth int, world []sphere, rnd *rng) vec3 {
	if depth <= 0 {
		return v3(0, 0, 0)
	}
	var rec hit
	if worldHit(world, r, &rec) {
		var scattered ray
		var attn vec3
		if scatter(r, &rec, rnd, &attn, &scattered) {
			return attn.hadamard(rayColor(scattered, depth-1, world, rnd))
		}
		return v3(0, 0, 0)
	}
	u := r.d.unit()
	a := 0.5 * (u.y + 1.0)
	return v3(1, 1, 1).scale(1.0 - a).add(v3(0.5, 0.7, 1.0).scale(a))
}

func cameraInit(w, h int) camera {
	lookfrom := v3(13, 2, 3)
	lookat := v3(0, 0, 0)
	vup := v3(0, 1, 0)
	vfov := 20.0
	focus := 10.0
	defocus := 0.6
	const rtPi = 3.14159265358979323846
	theta := vfov * rtPi / 180.0
	hh := math.Tan(theta / 2.0)
	viewportH := 2.0 * hh * focus
	viewportW := viewportH * (float64(w) / float64(h))
	ww := lookfrom.sub(lookat).unit()
	uu := vup.cross(ww).unit()
	vv := ww.cross(uu)
	viewportU := uu.scale(viewportW)
	viewportV := vv.neg().scale(viewportH)
	defocusRadius := focus * math.Tan((defocus/2.0)*rtPi/180.0)
	cam := camera{center: lookfrom, defocusAngle: defocus}
	cam.du = viewportU.scale(1.0 / float64(w))
	cam.dv = viewportV.scale(1.0 / float64(h))
	upperLeft := cam.center.sub(ww.scale(focus)).sub(viewportU.scale(0.5)).sub(viewportV.scale(0.5))
	cam.pixel00 = upperLeft.add(cam.du.add(cam.dv).scale(0.5))
	cam.defocusU = uu.scale(defocusRadius)
	cam.defocusV = vv.scale(defocusRadius)
	return cam
}

func (cam *camera) getRay(i, j int, rnd *rng) ray {
	ox := rnd.f() - 0.5
	oy := rnd.f() - 0.5
	pixel := cam.pixel00.add(cam.du.scale(float64(i) + ox)).add(cam.dv.scale(float64(j) + oy))
	origin := cam.center
	if cam.defocusAngle > 0.0 {
		p := rnd.inUnitDisk()
		origin = cam.center.add(cam.defocusU.scale(p.x)).add(cam.defocusV.scale(p.y))
	}
	return rayAt(origin, pixel.sub(origin))
}

func toByte(linear float64) byte {
	g := 0.0
	if linear > 0.0 {
		g = math.Sqrt(linear)
	}
	if g < 0.0 {
		g = 0.0
	}
	if g > 0.999 {
		g = 0.999
	}
	return byte(256.0 * g)
}

func renderRow(job *renderJob, y int) {
	row := job.fb[y*job.w*3:]
	for x := 0; x < job.w; x++ {
		rnd := rng{s: pixelSeed(x, y)}
		col := v3(0, 0, 0)
		for s := 0; s < job.spp; s++ {
			r := job.cam.getRay(x, y, &rnd)
			col = col.add(rayColor(r, job.depth, job.world, &rnd))
		}
		col = col.scale(1.0 / float64(job.spp))
		row[x*3+0] = toByte(col.x)
		row[x*3+1] = toByte(col.y)
		row[x*3+2] = toByte(col.z)
	}
}

func makeLambert(albedo vec3) material {
	return material{kind: matLambert, albedo: albedo, fuzz: 0, ir: 1}
}
func makeMetal(albedo vec3, fuzz float64) material {
	if fuzz > 1.0 {
		fuzz = 1.0
	}
	return material{kind: matMetal, albedo: albedo, fuzz: fuzz, ir: 1}
}
func makeGlass(ir float64) material {
	return material{kind: matGlass, albedo: v3(1, 1, 1), fuzz: 0, ir: ir}
}

func addSphere(world []sphere, n *int, c vec3, r float64, m material) {
	if *n >= worldCap {
		return
	}
	world[*n] = sphere{center: c, radius: r, mat: m}
	*n++
}

func buildWorld(world []sphere) int {
	rnd := rng{s: 1}
	n := 0
	addSphere(world, &n, v3(0, -1000, 0), 1000, makeLambert(v3(0.5, 0.5, 0.5)))
	for a := -11; a < 11; a++ {
		for b := -11; b < 11; b++ {
			choose := rnd.f()
			center := v3(float64(a)+0.9*rnd.f(), 0.2, float64(b)+0.9*rnd.f())
			if center.sub(v3(4, 0.2, 0)).len() <= 0.9 {
				continue
			}
			if choose < 0.8 {
				albedo := rnd.vec().hadamard(rnd.vec())
				addSphere(world, &n, center, 0.2, makeLambert(albedo))
			} else if choose < 0.95 {
				albedo := rnd.vecRange(0.5, 1.0)
				fuzz := rnd.rngRange(0.0, 0.5)
				addSphere(world, &n, center, 0.2, makeMetal(albedo, fuzz))
			} else {
				addSphere(world, &n, center, 0.2, makeGlass(1.5))
			}
		}
	}
	addSphere(world, &n, v3(0, 1, 0), 1, makeGlass(1.5))
	addSphere(world, &n, v3(-4, 1, 0), 1, makeLambert(v3(0.4, 0.2, 0.1)))
	addSphere(world, &n, v3(4, 1, 0), 1, makeMetal(v3(0.7, 0.6, 0.5), 0))
	return n
}

func checksumRGB(fb []byte) uint64 {
	h := uint64(14695981039346656037)
	for _, b := range fb {
		h ^= uint64(b)
		h *= 1099511628211
	}
	return h
}

func envTruth(k string) bool {
	s := os.Getenv(k)
	return s != "" && s != "0"
}

func envInt(k string, def int) int {
	s := os.Getenv(k)
	if s == "" {
		return def
	}
	n, err := strconv.Atoi(s)
	if err != nil {
		return def
	}
	return n
}

func writePPM(path string, fb []byte, w, h int) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := fmt.Fprintf(f, "P6\n%d %d\n255\n", w, h); err != nil {
		return err
	}
	_, err = f.Write(fb)
	return err
}

func main() {
	smoke := envTruth("RT_SMOKE")
	w := envInt("RT_WIDTH", 400)
	spp := envInt("RT_SAMPLES", 10)
	depth := envInt("RT_DEPTH", 20)
	if smoke {
		if os.Getenv("RT_WIDTH") == "" {
			w = 48
		}
		if os.Getenv("RT_SAMPLES") == "" {
			spp = 2
		}
		if os.Getenv("RT_DEPTH") == "" {
			depth = 8
		}
	}
	seq := envTruth("RT_SEQ")
	if w < 1 {
		w = 1
	}
	if spp < 1 {
		spp = 1
	}
	if depth < 1 {
		depth = 1
	}
	h := int(float64(w) / (16.0 / 9.0))
	if h < 1 {
		h = 1
	}

	world := make([]sphere, worldCap)
	nworld := buildWorld(world)
	world = world[:nworld]
	fb := make([]byte, w*h*3)
	job := renderJob{
		fb: fb, w: w, h: h, spp: spp, depth: depth,
		cam: cameraInit(w, h), world: world, nworld: nworld,
	}

	t0 := time.Now()
	if seq {
		for y := 0; y < h; y++ {
			renderRow(&job, y)
		}
	} else {
		var wg sync.WaitGroup
		wg.Add(h)
		for y := 0; y < h; y++ {
			y := y
			go func() {
				renderRow(&job, y)
				wg.Done()
			}()
		}
		wg.Wait()
	}
	ms := float64(time.Since(t0).Microseconds()) / 1000.0

	sum := checksumRGB(fb)
	if ppm := os.Getenv("RT_PPM"); ppm != "" {
		if err := writePPM(ppm, fb, w, h); err != nil {
			fmt.Fprintf(os.Stderr, "rt: failed to write %s: %v\n", ppm, err)
			os.Exit(1)
		}
	}
	fmt.Printf("rt impl=go seq=%d width=%d height=%d spp=%d depth=%d spheres=%d checksum=0x%016x time_ms=%.2f\n",
		boolInt(seq), w, h, spp, depth, nworld, sum, ms)
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}
