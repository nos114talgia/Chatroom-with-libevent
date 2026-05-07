# 技术文档：集成线程池的 Libevent 聊天室服务器

## 1. 项目概述

这是一个基于 **libevent** 的聊天室服务器，最初采用单线程事件循环模型。为提升并发处理能力，本项目集成了一个轻量级 C++ 线程池（`Thread_pool.hpp`），实现了 **I/O 与业务逻辑分离** 的架构。

### 关键特性
- 基于 libevent 的高效事件驱动网络 I/O
- 4 线程的消息处理线程池
- 支持用户注册、在线列表、私聊、广播等完整聊天功能
- 通过 mutex 和 libevent 线程安全机制保证数据一致性

---

## 2. 架构设计

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                     主线程（事件循环）                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ 接受连接    │  │ 读取数据    │  │ 处理连接关闭        │  │
│  │ accept_cb   │  │ read_cb     │  │ event_cb            │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                   │
│         │         ┌──────▼──────┐                           │
│         │         │ 提取消息行  │                           │
│         │         │ (buffer→str)│                           │
│         │         └──────┬──────┘                           │
│         │                │                                   │
│         │         ┌──────▼──────────────────────┐           │
│         │         │ pool.enqueue(process_message)│           │
│         │         └──────────────────────────────┘           │
└─────────┼───────────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────────┐
│                   线程池（4 个工作线程）                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │ Worker 1     │  │ Worker 2     │  │ Worker 3     │       │
│  │ process_msg  │  │ process_msg  │  │ process_msg  │       │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
│         │                 │                  │               │
│  ┌──────▼─────────────────▼──────────────────▼───────┐      │
│  │              共享状态（需要 mutex 保护）            │      │
│  │  • userlist        在线用户 bufferevent 列表       │      │
│  │  • name_to_buffer  用户名 → bufferevent 映射       │      │
│  │  • buffer_to_name  bufferevent → 用户名映射        │      │
│  └───────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 设计模式

采用 **Reactor + Half-Sync/Half-Async** 模式：

| 层级 | 职责 | 执行位置 |
|------|------|----------|
| **Async Layer** | 网络 I/O 事件监听、连接管理 | 主线程事件循环 |
| **Sync Layer** | 业务逻辑处理（消息路由、广播、注册） | 线程池工作线程 |

这种模式结合了事件驱动的高并发 I/O 和多线程的高效业务处理。

---

## 3. 核心组件详解

### 3.1 `serverCtx` — 全局服务上下文

```cpp
struct serverCtx{
    struct event_base* base;                                    // libevent 事件基
    std::mutex mtx;                                            // 共享状态互斥锁
    ThreadPool pool{4};                                        // 线程池（4 个工作者线程）
    std::vector<struct bufferevent*> userlist;                 // 在线用户列表
    std::unordered_map<std::string, struct bufferevent*> name_to_buffer;  // 用户名→连接
    std::unordered_map<struct bufferevent*, std::string> buffer_to_name;  // 连接→用户名
};
```

`serverCtx` 是整个服务器的**共享上下文**，管理所有在线用户的连接和身份信息。新增了 `mtx`（互斥锁）和 `pool`（线程池）成员，这是从单线程模型转向多线程模型的关键改变。

---

### 3.2 `accept_cb` — 新连接回调

**作用**：当有新客户端连接时被调用。

```cpp
void accept_cb(...){
    // 创建支持跨线程安全访问的 bufferevent
    struct bufferevent* bev = bufferevent_socket_new(base, fd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);

    bufferevent_setcb(bev, read_cb, nullptr, event_cb, ctx);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    // 加锁保护，将新连接加入用户列表
    {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->userlist.push_back(bev);
    }
}
```

**关键点**：
- `BEV_OPT_THREADSAFE` 标志使 bufferevent 内部使用锁，允许从不同线程安全地读写
- 新连接加入 `userlist` 时使用 mutex 保护

---

### 3.3 `read_cb` — 读取回调（I/O 层）

**作用**：从 libevent 缓冲区中读取消息行，并投递到线程池处理。

```cpp
void read_cb(struct bufferevent* bev, void* context){
    struct serverCtx* ctx = static_cast<serverCtx*>(context);
    struct evbuffer* input = bufferevent_get_input(bev);

    size_t len;
    char* line = nullptr;
    // 循环读取缓冲区中的所有行
    while((line = evbuffer_readln(input, &len, EVBUFFER_EOL_LF)) != nullptr){
        std::string msg(line, len);
        free(line);
        // 投递到线程池
        ctx->pool.enqueue(process_message, ctx, bev, msg);
    }
}
```

**职责边界**：
- ✅ 从缓冲区提取消息行（I/O 操作）
- ✅ 将消息投递到线程池（任务分发）
- ❌ **不处理**业务逻辑（注册、广播、私聊等）

这种分离确保了事件循环不会被业务逻辑阻塞，保证了 I/O 的高效性。

---

### 3.4 `process_message` — 消息处理（业务层）

**作用**：解析消息协议并执行对应的业务逻辑。在线程池工作线程中运行。

```cpp
void process_message(struct serverCtx* ctx, struct bufferevent* bev, std::string msg){
    std::lock_guard<std::mutex> lock(ctx->mtx);  // 获取锁

    // 1. 检查连接是否仍然有效
    bool is_registered = ctx->buffer_to_name.count(bev);
    if(!is_registered && !is_in_userlist(bev)) return;  // 连接已关闭

    // 2. 注册协议：Hello <name>
    if(!is_registered && msg.starts_with("Hello ")){
        // 分配唯一用户名（自动去重：user → user_2 → user_3 ...）
        // 通知其他用户
    }

    // 3. 在线列表协议：LIST
    if(msg == "LIST"){
        send_userlist_locked(ctx, bev);
    }

    // 4. 私聊协议：PVT <to> <msg>
    if(msg.starts_with("PVT ")){
        // 解析目标用户和消息内容
        // 查找目标用户的 bufferevent 并发送
    }

    // 5. 默认：广播给所有其他用户
    broadcast_locked(ctx, bev, msg);
}
```

**消息协议**：

| 协议 | 格式 | 说明 |
|------|------|------|
| 注册 | `Hello <name>` | 用户首次连接时发送，服务器分配唯一用户名 |
| 在线列表 | `LIST` | 查询当前在线用户 |
| 私聊 | `PVT <to> <msg>` | 向指定用户发送私聊消息 |
| 广播 | `<msg>` | 向所有其他用户广播消息 |

---

### 3.5 `event_cb` — 连接断开回调

**作用**：当客户端断开连接时，清理相关资源。

```cpp
void event_cb(struct bufferevent* bev, short events, void* context){
    struct serverCtx* ctx = static_cast<serverCtx*>(context);

    // 加锁清理共享状态
    std::lock_guard<std::mutex> lock(ctx->mtx);

    // 1. 从 userlist 中移除
    ctx->userlist.erase(std::remove(...), ctx->userlist.end());

    // 2. 获取用户名并通知其他用户
    std::string name = ctx->buffer_to_name[bev];
    if(!name.empty()){
        ctx->name_to_buffer.erase(name);
        // 广播 "xxx has logged out"
    }

    // 3. 清理映射关系
    ctx->buffer_to_name.erase(bev);

    // 4. 释放 bufferevent（BEV_OPT_CLOSE_ON_FREE 会自动关闭 socket）
    bufferevent_free(bev);
}
```

---

### 3.6 `send_line` / `broadcast_locked` / `send_userlist_locked` — 工具函数

| 函数 | 作用 | 调用条件 |
|------|------|----------|
| `send_line(bev, msg)` | 向指定 bufferevent 发送一行消息 | 无需锁（BEV_OPT_THREADSAFE 保证） |
| `broadcast_locked(ctx, sender, msg)` | 广播消息给除发送者外的所有用户 | 调用者需持有 `ctx->mtx` |
| `send_userlist_locked(ctx, to)` | 发送在线用户列表 | 调用者需持有 `ctx->mtx` |

---

## 4. 线程安全机制

### 4.1 三层防护

```
┌─────────────────────────────────────────────────────────────┐
│  第 1 层：BEV_OPT_THREADSAFE                                │
│  libevent bufferevent 内部加锁，支持跨线程安全读写           │
├─────────────────────────────────────────────────────────────┤
│  第 2 层：std::mutex (serverCtx::mtx)                       │
│  保护共享数据结构（userlist, name_to_buffer, buffer_to_name）│
├─────────────────────────────────────────────────────────────┤
│  第 3 层：设计约束                                           │
│  read_cb 仅做 I/O 提取，不修改共享状态                      │
│  所有共享状态修改都在 process_message / event_cb 中进行       │
│  且必须持有 ctx->mtx                                        │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 锁的使用规则

- **主锁 `ctx->mtx`**：保护所有对 `userlist`、`name_to_buffer`、`buffer_to_name` 的读写
- **读取缓冲区**：在 `read_cb` 中完成，不需要锁（仅操作 libevent 内部缓冲区）
- **写入网络**：由 `BEV_OPT_THREADSAFE` 保证 bufferevent 的线程安全

---

## 5. 本次修改详解

### 5.1 CMakeLists.txt

```diff
- target_link_libraries(server PRIVATE ${LIBEVENT_LIBRARIES})
+ target_link_libraries(server PRIVATE ${LIBEVENT_LIBRARIES} Threads::Threads)
```

**原因**：线程池使用了 `std::thread`、`std::mutex` 等 POSIX 线程原语，需要链接 `pthread` 库。

---

### 5.2 server.cpp 修改汇总

| 修改项 | 修改前 | 修改后 | 原因 |
|--------|--------|--------|------|
| 头文件 | 无 | `<event2/thread.h>`, `"Thread_pool.hpp"` | 启用 libevent 线程支持 + 引入线程池 |
| `serverCtx` | 无同步机制 | 添加 `std::mutex mtx`, `ThreadPool pool{4}` | 保护共享状态 + 线程池实例 |
| `main()` | 直接创建 event_base | 先调用 `evthread_use_pthreads()` | 让 libevent 使用 pthreads，启用内部线程安全 |
| `accept_cb` | `BEV_OPT_CLOSE_ON_FREE` | 添加 `BEV_OPT_THREADSAFE` 标志 | 允许跨线程安全访问 bufferevent |
| `read_cb` | 内联处理所有业务逻辑 | 仅提取消息行，投递到线程池 | I/O 与业务逻辑分离，避免阻塞事件循环 |
| 新增 `process_message` | 无 | 从 read_cb 提取的业务逻辑 | 线程池工作线程执行的独立函数 |
| 所有共享状态访问 | 无保护 | 加 `std::lock_guard<std::mutex>` | 多线程并发访问需要同步 |

### 5.3 线程池工作流程

```
1. 客户端发送消息
       ↓
2. 主线程事件循环检测到可读事件
       ↓
3. read_cb 被调用，从 libevent 缓冲区读取消息行
       ↓
4. 调用 ctx->pool.enqueue(process_message, ctx, bev, msg)
       ↓
5. 线程池中的某个空闲工作线程取出任务
       ↓
6. 工作线程执行 process_message()
   - 获取 ctx->mtx 锁
   - 解析消息协议
   - 执行对应业务逻辑（注册/广播/私聊等）
   - 释放锁
       ↓
7. 主线程事件循环继续处理下一个事件
```

---

## 6. 运行与编译

```bash
# 编译
mkdir build && cd build
cmake ..
make

# 运行服务器（默认监听 0.0.0.0:8080）
./server

# 运行客户端
./client
```

**输出示例**：
```
[System] Server started (thread pool: 4 workers)
[System] New connection
[System] New connection
```

---

## 7. 依赖项

| 依赖 | 版本 | 用途 |
|------|------|------|
| libevent | 2.1.12-stable | 事件驱动网络 I/O |
| pthreads | - | 线程池、mutex、condition_variable |
| C++17 | - | 结构化绑定、invoke_result 等 |

---

## 8. 性能特点

- **I/O 不阻塞**：事件循环始终保持响应，即使某个消息处理任务较慢
- **并发处理**：多个客户端消息可以被线程池并行处理
- **资源可控**：线程池大小固定为 4，避免了过多线程导致的上下文切换开销
- **扩展性强**：后续可以轻松将数据库查询、消息持久化等 CPU 密集任务放入线程池