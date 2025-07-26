function handle_hello(request)
    body = request.headers["name"] or "Anon"
    return 200, "<html><h1>Hello, "..body.."!</h1></html>"
end

routes = {
    ["/hello"] = handle_hello,
    ["/time"] = function(_) return 200, "<html>It's "..os.date().."</html>" end
}
