/* Stealable-HOL specimen: busy-wait under the process-wide pages exclusive.
 * Mix with static MISS so fallthrough is not under that lock. Delay: ?ms=N. */
let hits = 0;

function delay_ms(query) {
  let ms = 20;
  let q = query || "";
  let i = q.indexOf("ms=");
  if (i >= 0) {
    let n = parseInt(q.slice(i + 3), 10);
    if (n > 0 && n < 5000) ms = n;
  }
  return ms;
}

export function GET(request) {
  hits++;
  let ms = delay_ms(request.query);
  let t0 = Date.now();
  while (Date.now() - t0 < ms) {
  }
  return new Response("slow " + hits + " " + ms + "ms\n", {
    headers: { "content-type": "text/plain; charset=utf-8" },
  });
}
