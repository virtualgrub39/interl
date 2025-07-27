local template = require("template")

function handle_index(request)
	local html, err = template.render_file("index.lt.html", {
		username = request.headers["name"] or "Hatsune Miku",
	})

	if not html then
		return {
			code = 500,
			body = "<html>Internal Server Error</html>",
		}
	end

	return {
		code = 200,
		body = html,
	}
end

function handle_hello(request)
	local name = request.headers["name"] or "Kasane Teto"
	return {
		code = 200,
		body = "<html><h1>Hello, " .. name .. "!</h1></html>",
	}
end

routes = {
	["/"] = handle_index,
	["/hello"] = handle_hello,
	["/time"] = function(_)
		return {
			code = 200,
			body = "<html>It's " .. os.date() .. "</html>",
		}
	end,
	["/info"] = function(request)
		if request.method == "GET" then
			return {
				code = 200,
				body = "<h1>I love Hatsune Miku</h1>"
			}
		else
			return {
				code = 404,
				body = "<h1>I HATE BALCEROWICZ</h1>"
			}
		end
	end,
}
