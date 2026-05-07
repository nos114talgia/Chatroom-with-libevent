#include <event2/event.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/thread.h>

#include <arpa/inet.h>
#include <vector>
#include <string>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <mutex>

#include "Thread_pool.hpp"

// global context(user list, chat room list)
struct serverCtx{
    struct event_base* base;
    std::mutex mtx;                     // protect shared state
    ThreadPool pool{4};                 // thread pool with 4 workers
    std::vector<struct bufferevent*> userlist;
    std::unordered_map<std::string, struct bufferevent*> name_to_buffer;
    std::unordered_map<struct bufferevent*, std::string> buffer_to_name;
};

// send one message (thread-safe via BEV_OPT_THREADSAFE)
static inline void send_line(struct bufferevent* bev, const std::string& msg){
    struct evbuffer* output = bufferevent_get_output(bev);
    evbuffer_add(output, msg.c_str(), msg.length());
    evbuffer_add(output, "\n", 1);
}

// broadcast message (caller must hold ctx->mtx)
void broadcast_locked(struct serverCtx* ctx, struct bufferevent* sender, const std::string& msg){
    auto it = ctx->buffer_to_name.find(sender);
    bool has_name = (it != ctx->buffer_to_name.end());
    std::string payload;

    if(has_name){
        payload = it->second + ": " + msg;
    }else{
        payload = msg;
    }

    for(auto* user : ctx->userlist){
        if(sender == user){
            continue;
        }
        send_line(user, payload);
    }
}

// send the online userlist to the new user (caller must hold ctx->mtx)
static inline void send_userlist_locked(struct serverCtx* ctx, struct bufferevent* to){
    int len = 6 + ctx->name_to_buffer.size() * 10;
    std::string payload;
    payload.reserve(len);
    payload += "Users:";
    for(const auto& [name, bev] : ctx->name_to_buffer){
        if(!name.empty()){
            payload.append(" ").append(name);
        }
    }
    send_line(to, payload);
}

// process a single message (runs in thread pool worker)
// 1. register              Hello <name>
// 2. return online list    LIST
// 3. private chat          PVT <to> <msg>
// 4. broadcast             <msg>
void process_message(struct serverCtx* ctx, struct bufferevent* bev, std::string msg){
    std::lock_guard<std::mutex> lock(ctx->mtx);

    // check if this connection is still alive
    bool is_registered = false;
    auto it = ctx->buffer_to_name.find(bev);
    if(it != ctx->buffer_to_name.end()){
        is_registered = true;
    }else{
        // if not registered AND not in userlist, connection was already closed
        if(std::find(ctx->userlist.begin(), ctx->userlist.end(), bev) == ctx->userlist.end()){
            return;
        }
    }

    // register
    if(!is_registered && (msg.length() >= 7 && msg.substr(0, 6) == "Hello ")){
        std::string name = msg.substr(6);
        if(name.empty()){
            name = "user";
        }

        std::string real = name;
        int suffix = 1;
        while(ctx->name_to_buffer.count(real)){
            real = name + "_" + std::to_string(++suffix);
        }
        ctx->buffer_to_name.emplace(bev, real);
        ctx->name_to_buffer.emplace(real, bev);

        // echo the username
        send_line(bev, "[System] you have logged in as " + real);
        send_userlist_locked(ctx, bev);
        // notify other users
        for(auto* other : ctx->userlist){
            if(other != bev){
                send_line(other, "[System] " + real + " has logged in");
            }
        }
        return;
    }
    // send online list
    if(msg == "LIST"){
        send_userlist_locked(ctx, bev);
        return;
    }
    // private chat
    if(msg.rfind("PVT ", 0) == 0){
        std::stringstream ss(msg);
        std::string cmd, to;
        ss >> cmd >> to;

        std::string mesg;
        std::getline(ss, mesg);
        if(!mesg.empty() && mesg[0] == ' '){
            mesg.erase(0, 1);
        }
        auto it = ctx->name_to_buffer.find(to);
        if(it != ctx->name_to_buffer.end()){
            send_line(it->second, "[PVT] " + ctx->buffer_to_name[bev] + ": " + mesg);
        }else{
            send_line(bev, "[System] " + to + " is not online");
        }
        return;
    }
    // default: broadcast
    broadcast_locked(ctx, bev, msg);
}

// read_cb: extract lines from buffer and enqueue to thread pool
void read_cb(struct bufferevent* bev, void* context){
    struct serverCtx* ctx = static_cast<serverCtx*>(context);
    struct evbuffer* input = bufferevent_get_input(bev);

    size_t len;
    char* line = nullptr;
    while((line = evbuffer_readln(input, &len, EVBUFFER_EOL_LF)) != nullptr){
        std::string msg(line, len);
        free(line);
        // dispatch message processing to thread pool
        ctx->pool.enqueue(process_message, ctx, bev, msg);
    }
}

// event_cb: handle logout
void event_cb(struct bufferevent* bev, short events, void* context){
    struct serverCtx* ctx = static_cast<serverCtx*>(context);
    if(events & BEV_EVENT_ERROR){
        std::cerr << "[System] Connection error" << std::endl;
    }
    if(events & BEV_EVENT_EOF){
        std::cerr << "[System] Connection closed" << std::endl;
    }

    std::lock_guard<std::mutex> lock(ctx->mtx);

    auto it = std::find(ctx->userlist.begin(), ctx->userlist.end(), bev);
    if(it != ctx->userlist.end()){
        ctx->userlist.erase(it);
    }

    std::string name = ctx->buffer_to_name[bev];
    if(!name.empty()){
        ctx->name_to_buffer.erase(name);
        for(auto* other : ctx->userlist){
            send_line(other, "[System] " + name + " has logged out");
        }
    }

    ctx->buffer_to_name.erase(bev);

    bufferevent_free(bev);
}

// accept_cb
void accept_cb(struct evconnlistener* listener, evutil_socket_t fd,
                struct sockaddr* addr, int socklen, void* context){
    struct serverCtx* ctx = static_cast<serverCtx*>(context);
    struct event_base* base = evconnlistener_get_base(listener);

    // BEV_OPT_THREADSAFE enables safe cross-thread bufferevent access
    struct bufferevent* bev = bufferevent_socket_new(base, fd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);

    bufferevent_setcb(bev, read_cb, nullptr, event_cb, ctx);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->userlist.push_back(bev);
    }
    std::cout<< "[System] New connection" << std::endl;
}

int main(int argc, char** argv){
    // enable pthreads support for libevent (must be called before event_base_new)
    evthread_use_pthreads();

    serverCtx ctx{};
    ctx.base = event_base_new();
    if(!ctx.base){
        std::cerr << "[System] Failed to create event base" << std::endl;
        return 1;
    }

    // ip and port
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8080);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    struct evconnlistener* listener = evconnlistener_new_bind(
        ctx.base,
        accept_cb,
        &ctx,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        -1,
        (struct sockaddr*) &sin,
        sizeof(sin)
    );
    if(!listener){
        std::cerr << "[System] Failed to create listener" << std::endl;
        return 1;
    }
    std::cout<< "[System] Server started (thread pool: 4 workers)" << std::endl;
    event_base_dispatch(ctx.base);
    evconnlistener_free(listener);
    event_base_free(ctx.base);
    return 0;
}