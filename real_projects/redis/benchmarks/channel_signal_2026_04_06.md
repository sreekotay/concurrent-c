# Channel Signal Benchmark 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Idiomatic port: `6386`
- Upstream port: `6387`
- Baseline source: `benchmark_baseline.txt`

## Summary vs Baseline

| Case | Baseline idiomatic rps | New idiomatic rps | Delta | Baseline upstream rps | New upstream rps | New idiomatic/upstream | Baseline idiomatic p50 | New idiomatic p50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 218240.95 | 173924.25 | 0.797x | 211895.10 | 131137.49 | 1.326x | 0.127 | 0.151 |
| `GET P=1` | 220589.90 | 170275.31 | 0.772x | 217867.68 | 191450.17 | 0.889x | 0.127 | 0.156 |
| `SET P=16` | 2873674.42 | 2118543.29 | 0.737x | 2373438.58 | 1956965.62 | 1.083x | 0.226 | 0.292 |
| `GET P=16` | 2958748.58 | 1529730.81 | 0.517x | 2843606.92 | 1908764.98 | 0.801x | 0.151 | 0.223 |
| `INCR P=16` | 1805532.59 | 1827697.25 | 1.012x | 2700274.42 | 2378958.25 | 0.768x | 0.271 | 0.340 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 203252.03 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 213903.75 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2739726.00 requests per second, p50=0.231 msec`
- `GET P=16`: `GET: 35733.43 requests per second, p50=0.183 msec`
- `INCR P=16`: `INCR: 2173913.00 requests per second, p50=0.303 msec`

### upstream
- `SET P=1`: `SET: 123456.79 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 211193.23 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2020202.00 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 909090.94 requests per second, p50=0.567 msec`
- `INCR P=16`: `INCR: 2325581.25 requests per second, p50=0.263 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 125470.52 requests per second, p50=0.183 msec`
- `GET P=1`: `GET: 177304.97 requests per second, p50=0.151 msec`
- `SET P=16`: `SET: 2325581.25 requests per second, p50=0.279 msec`
- `GET P=16`: `GET: 2666666.50 requests per second, p50=0.207 msec`
- `INCR P=16`: `INCR: 2222222.25 requests per second, p50=0.287 msec`

### upstream
- `SET P=1`: `SET: 187617.27 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 197044.34 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2197802.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2666666.50 requests per second, p50=0.231 msec`
- `INCR P=16`: `INCR: 2564102.50 requests per second, p50=0.247 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 193050.19 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 119617.22 requests per second, p50=0.191 msec`
- `SET P=16`: `SET: 1290322.62 requests per second, p50=0.367 msec`
- `GET P=16`: `GET: 1886792.50 requests per second, p50=0.279 msec`
- `INCR P=16`: `INCR: 1086956.50 requests per second, p50=0.431 msec`

### upstream
- `SET P=1`: `SET: 82338.41 requests per second, p50=0.295 msec`
- `GET P=1`: `GET: 166112.95 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 1652892.62 requests per second, p50=0.343 msec`
- `GET P=16`: `GET: 2150537.50 requests per second, p50=0.263 msec`
- `INCR P=16`: `INCR: 2247191.00 requests per second, p50=0.271 msec`

