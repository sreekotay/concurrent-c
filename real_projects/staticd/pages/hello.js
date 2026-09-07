export function GET(request) {
  return new Response(`hello ${request.path}\n`, {
    headers: { "content-type": "text/plain; charset=utf-8" },
  });
}
