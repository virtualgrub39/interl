#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <time.h>
// #include <sys/time.h>

#include "ketopt.h"

#include "config.h"

struct server_cfg {
    const char* lua_cfg_path;
    const char* static_dir_path;
};

static lua_State* L = NULL;
pthread_mutex_t luaLock = PTHREAD_MUTEX_INITIALIZER;

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
http_error(struct MHD_Connection* connection, int code, const char* body)
{
    struct MHD_Response* response = MHD_create_response_from_buffer(
      strlen(body), (void*)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text");
    enum MHD_Result retval = MHD_queue_response(connection, code, response);
    MHD_destroy_response(response);
    return retval;
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

    pthread_mutex_lock(&luaLock);

    // routes[url]
    lua_pushstring(L, url);
    lua_gettable(L, -2);

    if (!lua_isfunction(L, -1)) {
        retval = http_error(connection, MHD_HTTP_NOT_FOUND, "Route not found");
        lua_pop(L, 1);
        goto http_response_lua_unlock;
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

    lua_pushstring(L, "body");
    lua_pushstring(L, upload_data);
    lua_settable(L, -3);

    /* request.method */

    lua_pushstring(L, "method");
    lua_pushstring(L, method);
    lua_settable(L, -3);

    /* request.url */

    lua_pushstring(L, "url");
    lua_pushstring(L, url);
    lua_settable(L, -3);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // stack: [routes, handler_fn, request]

    // handler_fn(request) -> {status: number, body: string}
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fprintf(
          stderr, "Lua error in handler function: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        retval = http_error(
          connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
        goto http_response_lua_unlock;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Lua returned in %.9fs\n", elapsed);

    // stack: [routes, response]

    if (!lua_istable(L, -1)) {
        fprintf(stderr,
                "Invalid handler return value - expected table, got %s\n",
                lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 1);
        retval = http_error(
          connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
        goto http_response_lua_unlock;
    }

    lua_getfield(L, -1, "code");

    if (!lua_isinteger(L, -1)) {
        fprintf(stderr,
                "Invalid response 'code' field - expected integer, got %s\n",
                lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 2);
        retval = http_error(
          connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
        goto http_response_lua_unlock;
    }

    int status = lua_tointeger(L, -1);
    lua_pop(L, 1);

    // stack: [routes, response]

    lua_getfield(L, -1, "body");

    if (!lua_isstring(L, -1)) {
        fprintf(stderr,
                "Invalid response 'body' field - expected string, got %s\n",
                lua_typename(L, lua_type(L, -1)));
        lua_pop(L, 2);
        retval = http_error(
          connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error");
        goto http_response_lua_unlock;
    }

    const char* body = lua_tostring(L, -1);
    lua_pop(L, 1);

    // stack: [routes, response]

    response = MHD_create_response_from_buffer(
      strlen(body), (void*)body, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(
      response,
      MHD_HTTP_HEADER_CONTENT_TYPE,
      "text/html"); // TODO: detect mime type / get it from response table

    retval = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);

    lua_pop(L, 1);

    // stack: [routes]

http_response_lua_unlock:
    pthread_mutex_unlock(&luaLock);

    return retval;
}

static void
reload_lua(lua_State* L, const char* path)
{
    lua_settop(L, 0);

    if (luaL_dofile(L, path) != LUA_OK) {
        fprintf(stderr, "Error reloading Lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getglobal(L, "routes");
    if (!lua_istable(L, -1)) {
        fprintf(stderr, "`routes` is not a table!\n");
    }
}

static void*
watcher_thread(void* arg)
{
    struct server_cfg* wa = arg;
    int in_fd = inotify_init1(IN_NONBLOCK);
    if (in_fd < 0) {
        perror("inotify_init1");
        return NULL;
    }

    // Watch for writes + close (so temporary files don’t trigger)
    int wd = inotify_add_watch(in_fd, wa->lua_cfg_path, IN_CLOSE_WRITE);
    if (wd < 0) {
        perror("inotify_add_watch");
        close(in_fd);
        return NULL;
    }

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (1) {
        ssize_t len = read(in_fd, buf, sizeof(buf));
        if (len <= 0) {
            // EAGAIN if nothing to read; sleep briefly
            usleep(200000);
            continue;
        }

        for (char* ptr = buf; ptr < buf + len;) {
            struct inotify_event* evt = (void*)ptr;
            if (evt->mask & IN_CLOSE_WRITE) {
                // File was updated—reload
                printf("%s updated - reloading...\n", wa->lua_cfg_path);
                pthread_mutex_lock(&luaLock);
                reload_lua(L, wa->lua_cfg_path);
                pthread_mutex_unlock(&luaLock);
            }
            ptr += sizeof(*evt) + evt->len;
        }
    }

    // (never reached)
    inotify_rm_watch(in_fd, wd);
    close(in_fd);
    return NULL;
}

static void
usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  -h, --help           Show this message\n"
            "  -s, --static         Directory containing static files\n"
            "  -f, --lua FILE       Path to lua config file\n",
            prog);
}

enum {
    ko_help = 256,
    ko_file,
    ko_static,
};

static const ko_longopt_t longopts[] = {
    { "help", ko_no_argument, ko_help },
    { "lua", ko_required_argument, ko_file },
    { "static", ko_required_argument, ko_file },
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
    char* static_dir = NULL;

    ketopt_t s = KETOPT_INIT;
    int c;
    const char* ostr = "hf:s:";

    while ((c = ketopt(&s, argc, argv, true, ostr, longopts)) != -1) {
        switch (c) {
            case 'h':
            case ko_help:
                usage(argv[0]);
                return 0;

            case 'f':
            case ko_file:
                lua_file = s.arg;
                break;

            case 's':
            case ko_static:
                static_dir = s.arg;
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

    pthread_mutex_lock(&luaLock);
    reload_lua(L, lua_file);
    pthread_mutex_unlock(&luaLock);

    pthread_t tid;
    struct server_cfg wa = {
        .lua_cfg_path = lua_file,
        .static_dir_path = static_dir,
    };

    if (pthread_create(&tid, NULL, watcher_thread, &wa) != 0) {
        perror("pthread_create");
        return 1;
    }

    struct MHD_Daemon* http_daemon =
      MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
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

// TODO: C pre-dispatch "middleware" for static file streaming
// TODO: Expose some kind of API to lua script (ex. mime_type_for(path),
// send_file(path), etc.)
// TODO: Add more response fields (Headers, Cookies, etc.)
// TODO: Parse URL queries before passing URL to lua; Add query field to request
// table.
// TODO: Allow for url-params somehow? (as in /user/<id>)
// TODO: Make some parameters in response table optional
