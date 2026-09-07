def GET(request):
    return Response(f"hello {request.path}\n", content_type="text/plain; charset=utf-8")
