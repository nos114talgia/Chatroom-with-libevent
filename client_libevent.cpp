#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <arpa/inet.h>
#include <string>
#include <cstring>
#include <iostream>
#include <csignal>
#include <unistd.h>

// global context
struct clientCtx {
    struct event_base* base;
    struct bufferevent* bev;
    struct event* stdin_event;
    bool registered;
};

static clientCtx g_ctx{};
static volatile bool g_running = true;

// --- write a line to server ---
static void send_line(struct bufferevent* bev, const std::string& msg) {
    struct evbuffer* output = bufferevent_get_output(bev);
    evbuffer_add(output, msg.c_str(), msg.length());
    evbuffer_add(output, "\n", 1);
}

// --- read_cb: receive data from server ---
static void read_cb(struct bufferevent* bev, void* context) {
    struct evbuffer* input = bufferevent_get_input(bev);
    size_t len;
    char* line = nullptr;

    while ((line = evbuffer_readln(input, &len, EVBUFFER_EOL_LF)) != nullptr) {
        std::string msg(line, len);
        free(line);
        std::cout << msg << std::endl;
    }
}

// --- event_cb: handle connection events ---
static void event_cb(struct bufferevent* bev, short events, void* context) {
    if (events & BEV_EVENT_CONNECTED) {
        std::cout << "[System] Connected to server" << std::endl;

        // prompt for username and register
        std::cout << "Enter username: " << std::flush;
        std::string username;
        if (std::getline(std::cin, username)) {
            send_line(bev, "Hello " + username);
            g_ctx.registered = true;
        }
    }
    if (events & BEV_EVENT_ERROR) {
        std::cerr << "[System] Connection error: "
                  << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())
                  << std::endl;
        g_running = false;
        event_base_loopexit(g_ctx.base, nullptr);
    }
    if (events & BEV_EVENT_EOF) {
        std::cout << "[System] Server closed connection" << std::endl;
        g_running = false;
        event_base_loopexit(g_ctx.base, nullptr);
    }
}

// --- stdin_cb: read user input from stdin ---
static void stdin_cb(evutil_socket_t fd, short what, void* context) {
    std::string message;
    if (!std::getline(std::cin, message)) {
        // EOF or error on stdin (e.g., after SIGINT closes fd)
        g_running = false;
        event_base_loopexit(g_ctx.base, nullptr);
        return;
    }
    if (message.empty()) return;
    if (!g_running || !g_ctx.registered) return;

    send_line(g_ctx.bev, message);
}

// --- signal handler ---
static void signal_handler(int sig) {
    if (sig == SIGINT) {
        g_running = false;
        event_base_loopexit(g_ctx.base, nullptr);
    }
}

int main() {
    std::signal(SIGINT, signal_handler);

    g_ctx.base = event_base_new();
    if (!g_ctx.base) {
        std::cerr << "[System] Failed to create event base" << std::endl;
        return 1;
    }

    // create bufferevent for TCP connection
    g_ctx.bev = bufferevent_socket_new(g_ctx.base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!g_ctx.bev) {
        std::cerr << "[System] Failed to create bufferevent" << std::endl;
        event_base_free(g_ctx.base);
        return 1;
    }

    bufferevent_setcb(g_ctx.bev, read_cb, nullptr, event_cb, nullptr);
    bufferevent_enable(g_ctx.bev, EV_READ | EV_WRITE);

    // set up stdin as an event source for libevent
    g_ctx.stdin_event = event_new(g_ctx.base, STDIN_FILENO,
                                  EV_READ | EV_PERSIST, stdin_cb, nullptr);

    // connect to server
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    if (bufferevent_socket_connect(g_ctx.bev,
            (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        std::cerr << "[System] Connection failed" << std::endl;
        bufferevent_free(g_ctx.bev);
        event_base_free(g_ctx.base);
        return 1;
    }

    // start watching stdin after connection is established
    // we activate stdin_event in event_cb on BEV_EVENT_CONNECTED,
    // but we add it here so the event loop knows about it
    event_add(g_ctx.stdin_event, nullptr);

    std::cout << "[System] Connecting to server..." << std::endl;
    event_base_dispatch(g_ctx.base);

    // cleanup
    event_free(g_ctx.stdin_event);
    bufferevent_free(g_ctx.bev);
    event_base_free(g_ctx.base);
    std::cout << "[System] Exited." << std::endl;

    return 0;
}