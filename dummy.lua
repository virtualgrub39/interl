local template = require("template")

function handle_index(request)
  local html, err = template.render_file("index.lt.html", {
    username = request.headers["name"] or "Anon"
  })

  if not html then
    return 404, err
  end

  return 200, html
end

function handle_hello(request)
	local name = request.headers["name"] or "Anon"
	return 200, "<html><h1>Hello, " .. name .. "!</h1></html>"
end

routes = {
	["/"] = handle_index,
	["/hello"] = handle_hello,
	["/time"] = function(_)
		return 200, "<html>It's " .. os.date() .. "</html>"
	end,
}
