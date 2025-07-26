#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "config.h"
#include "ketopt.h"

static lua_State* L = NULL;

#define runtime_assert(condition)                                              \
    if (!(condition)) {                                                        \
        fprintf(stderr,                                                        \
                "%s:%d Runtime assertion failed: " #condition "\n",            \
                __FILE__,                                                      \
                __LINE__);                                                     \
        abort();                                                               \
    }
#define TODO(msg)                                                              \
    do {                                                                       \
        fprintf(stderr, "%s:%u TODO: %s\n", __FILE__, __LINE__, msg);          \
        abort();                                                               \
    } while (0)
#define UNUSED(arg) ((void)arg)

// double
// f(double x)
// {
//     double y = 0.0;

//     lua_rawgeti(L, LUA_REGISTRYINDEX, ref_f);
//     lua_pushnumber(L, x);

//     if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
//         fprintf(stderr, "Lua error in f(): %s\n", lua_tostring(L, -1));
//         lua_pop(L, 1);
//         return -1.0;
//     }

//     if (!lua_isnumber(L, -1)) {
//         fprintf(stderr, "Lua f() didn’t return a number\n");
//         lua_pop(L, 1);
//         return -1.0;
//     }

//     y = lua_tonumber(L, -1);
//     lua_pop(L, 1);
//     return y;
// }

enum MHD_Result
http_on_client_connect(void* cls,
                       const struct sockaddr* addr,
                       socklen_t addrlen)
{
    UNUSED(cls);
    UNUSED(addrlen);

    if (addr->sa_family == AF_INET6)
        return MHD_NO;

    uint32_t addr_raw = ((struct sockaddr_in*)addr)->sin_addr.s_addr;

    const char* addr_str = inet_ntoa((struct in_addr){ addr_raw });

    printf("%s REQUESTS: ", addr_str);

    return MHD_YES;
}

enum MHD_Result
add_header_to_current_table(void* cls,
                            enum MHD_ValueKind kind,
                            const char* key,
                            const char* value)
{
    UNUSED(cls);
    UNUSED(kind);

    // stack: [routes, handler_fn, request, "headers", headers]
    // request.headers[key] = value
    lua_pushstring(L, key);   // stack: [..., headers, "key"]
    lua_pushstring(L, value); // stack: [..., headers, "key", "value"]
    // headers.key = value:
    lua_settable(L, -3); // stack [..., headers]

    return MHD_YES;
}

enum MHD_Result
http_answer(void* cls,
            struct MHD_Connection* connection,
            const char* url,
            const char* method,
            const char* version,
            const char* upload_data,
            size_t* upload_data_size,
            void** req_cls)
{
    UNUSED(cls);
    UNUSED(req_cls);
    UNUSED(upload_data_size);

    printf("%s %s %s\n", version, method, url);

    struct MHD_Response* response = NULL;
    enum MHD_Result retval = MHD_YES;

    // stack: [routes]

    // routes[url]
    lua_pushstring(L, url);
    lua_gettable(L, -2); // TODO: Why -2?

    if (!lua_isfunction(L, -1)) {
        const char* e404 = "<html>404 Route not found</html>";
        response = MHD_create_response_from_buffer(
          strlen(e404), (void*)e404, MHD_RESPMEM_PERSISTENT);
        retval = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        return retval;
    }

    // stack: [routes, handler_fn]

    lua_newtable(L); // request

    /* request.headers */

    lua_pushstring(L, "headers");
    lua_newtable(L);

    MHD_get_connection_values(
      connection, MHD_HEADER_KIND, &add_header_to_current_table, NULL);

    // stack: [routes, handler_fn, request, "headers", headers]

    // request["headers"] = headers
    lua_settable(L, -3);

    /* request.body */
    // stack: [routes, handler_fn, request]

    lua_pushstring(L, "body");
    lua_pushstring(L, upload_data);
    // request["body"] = upload_data
    lua_settable(L, -3);


    if (lua_pcall(L, 1, 2, 0) != LUA_OK) { // handler_fn(request) -> 2 ret-vals.
        fprintf(stderr, "Lua error in handler function: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);

        const char* e500 = "<html>500 Internal Server Error</html>";
        response = MHD_create_response_from_buffer(
          strlen(e500), (void*)e500, MHD_RESPMEM_PERSISTENT);
        retval = MHD_queue_response(
          connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return retval;
    }

    // stack: [routes, status_code, body_string]

    int status = lua_tointeger(L, -2);
    const char* body = lua_tostring(L, -1);

    response = MHD_create_response_from_buffer(
      strlen(body), (void*)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(
      response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/html");

    retval = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);

    lua_pop(L, 2);

    // stack: [routes]

    return retval;
}

static void
usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  -h, --help           Show this help\n"
            "  -f, --lua FILE       Path to lua file\n",
            prog);
}

enum {
    ko_help = 256,
    ko_file,
};

static const ko_longopt_t longopts[] = {
    { "help", ko_no_argument, ko_help },
    { "lua", ko_required_argument, ko_file },
    { NULL, 0, 0 }
};

int
main(int argc, char* argv[])
{
    if (argc == 1) {
        usage(argv[0]);
        return 1;
    }

    char* lua_file = NULL;

    ketopt_t s = KETOPT_INIT;
    int c;
    const char* ostr = "hf:";

    while ((c = ketopt(&s, argc, argv, true, ostr, longopts)) != -1) {
        switch (c) {
            case 'h':
            case ko_help:
                usage(argv[0]);
                return 0;
                // errno = 0;
                // char* end;
                // unsigned long v = strtoul(s.arg, &end, 10);
                // if (errno || *end != '\0') {
                //     fprintf(stderr, "Invalid table length: %s\n", s.arg);
                //     usage(argv[0]);
                //     return 1;
                // }

            case 'f':
            case ko_file:
                lua_file = s.arg;
                break;

            case '?':
                fprintf(stderr, "Unknown option: %s\n", argv[s.ind]);
                usage(argv[0]);
                return 1;

            case ':':
                fprintf(
                  stderr, "Option requires an argument: %s\n", argv[s.ind]);
                usage(argv[0]);
                return 1;
        }
    }

    for (int i = s.ind; i < argc; i++) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

    if (!lua_file) {
        fprintf(stderr,
                "You have to provide path to lua file defining routes.\n");
        usage(argv[0]);
        return 1;
    }

    L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "Failed to create lua context.\n");
        return 1;
    }

    luaL_openlibs(L);

    if (luaL_dofile(L, lua_file)) {
        fprintf(stderr, "Failed to load script: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    lua_getglobal(L, "routes");
    if (!lua_istable(L, -1)) {
        fprintf(stderr, "Lua script must define `routes` table.\n");
        lua_close(L);
        return 1;
    }

    struct MHD_Daemon* http_daemon =
      MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,
                       HTTP_PORT,
                       &http_on_client_connect,
                       NULL,
                       &http_answer,
                       NULL,
                       MHD_OPTION_END);

    if (!http_daemon) {
        fprintf(stderr, "Failed to start microhttpd server.\n");
        lua_close(L);
        return 1;
    }

    getchar();

    MHD_stop_daemon(http_daemon);

    if (L) {
        lua_close(L);
        L = NULL;
    }

    return 0;
}
