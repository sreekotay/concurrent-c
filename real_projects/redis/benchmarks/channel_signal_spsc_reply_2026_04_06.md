# Channel Signal Benchmark With SPSC Reply Channel 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Idiomatic port: `6395`
- Upstream port: `6396`
- Baseline source: `benchmark_baseline.txt`

## Summary vs Baseline

| Case | Baseline idiomatic rps | New idiomatic rps | Delta | Baseline upstream rps | New upstream rps | New idiomatic/upstream | Baseline idiomatic p50 | New idiomatic p50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 218240.95 | 126711.24 | 0.581x | 211895.10 | 95235.22 | 1.331x | 0.127 | 0.215 |
| `GET P=1` | 220589.90 | 124802.56 | 0.566x | 217867.68 | 92573.43 | 1.348x | 0.127 | 0.226 |
| `SET P=16` | 2873674.42 | 978884.46 | 0.341x | 2373438.58 | 1061900.82 | 0.922x | 0.226 | 0.407 |
| `GET P=16` | 2958748.58 | 1804747.88 | 0.610x | 2843606.92 | 1323980.06 | 1.363x | 0.151 | 0.330 |
| `INCR P=16` | 1805532.59 | 994790.44 | 0.551x | 2700274.42 | 1355505.85 | 0.734x | 0.271 | 0.436 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 141743.44 requests per second, p50=0.199 msec`
- `GET P=1`: `GET: 126342.39 requests per second, p50=0.223 msec`
- `SET P=16`: `SET: 29463.76 requests per second, p50=0.415 msec`
- `GET P=16`: `GET: 1980198.00 requests per second, p50=0.303 msec`
- `INCR P=16`: `INCR: 1612903.25 requests per second, p50=0.415 msec`

### upstream
- `SET P=1`: `SET: 86281.27 requests per second, p50=0.295 msec`
- `GET P=1`: `GET: 53662.46 requests per second, p50=0.383 msec`
- `SET P=16`: `SET: 1117318.38 requests per second, p50=0.511 msec`
- `GET P=16`: `GET: 1652892.62 requests per second, p50=0.351 msec`
- `INCR P=16`: `INCR: 1694915.25 requests per second, p50=0.351 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 122850.12 requests per second, p50=0.231 msec`
- `GET P=1`: `GET: 126262.62 requests per second, p50=0.223 msec`
- `SET P=16`: `SET: 1600000.00 requests per second, p50=0.359 msec`
- `GET P=16`: `GET: 1694915.25 requests per second, p50=0.351 msec`
- `INCR P=16`: `INCR: 1351351.38 requests per second, p50=0.455 msec`

### upstream
- `SET P=1`: `SET: 91491.30 requests per second, p50=0.303 msec`
- `GET P=1`: `GET: 130293.16 requests per second, p50=0.183 msec`
- `SET P=16`: `SET: 1600000.00 requests per second, p50=0.391 msec`
- `GET P=16`: `GET: 1785714.25 requests per second, p50=0.327 msec`
- `INCR P=16`: `INCR: 1801801.75 requests per second, p50=0.335 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 115540.16 requests per second, p50=0.215 msec`
- `GET P=1`: `GET: 121802.68 requests per second, p50=0.231 msec`
- `SET P=16`: `SET: 1307189.62 requests per second, p50=0.447 msec`
- `GET P=16`: `GET: 1739130.38 requests per second, p50=0.335 msec`
- `INCR P=16`: `INCR: 20116.68 requests per second, p50=0.439 msec`

### upstream
- `SET P=1`: `SET: 107933.08 requests per second, p50=0.223 msec`
- `GET P=1`: `GET: 93764.66 requests per second, p50=0.207 msec`
- `SET P=16`: `SET: 468384.09 requests per second, p50=1.015 msec`
- `GET P=16`: `GET: 533333.31 requests per second, p50=0.759 msec`
- `INCR P=16`: `INCR: 569800.56 requests per second, p50=0.823 msec`

