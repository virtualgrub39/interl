#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
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

struct watch_args {
    const char* path;
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
        fprintf(
          stderr, "Lua error in handler function: %s\n", lua_tostring(L, -1));
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
    struct watch_args* wa = arg;
    int in_fd = inotify_init1(IN_NONBLOCK);
    if (in_fd < 0) {
        perror("inotify_init1");
        return NULL;
    }

    // Watch for writes + close (so temporary files don’t trigger)
    int wd = inotify_add_watch(in_fd, wa->path, IN_CLOSE_WRITE);
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
                printf("%s updated - reloading...\n", wa->path);
                pthread_mutex_lock(&luaLock);
                reload_lua(L, wa->path);
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

    pthread_mutex_lock(&luaLock);
    reload_lua(L, lua_file);
    pthread_mutex_unlock(&luaLock);

    pthread_t tid;
    struct watch_args wa = {
        .path = lua_file,
    };

    if (pthread_create(&tid, NULL, watcher_thread, &wa) != 0) {
        perror("pthread_create");
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
