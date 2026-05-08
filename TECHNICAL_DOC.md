# 技术文档：集成线程池的 Libevent 聊天室服务器

## 1. 项目概述

这是一个基于 **libevent** 的聊天室项目，包含两个服务器版本和两个客户端版本：

**服务器：**

| 版本 | 源文件 | 线程模型 | 说明 |
|------|--------|----------|------|
| 单线程版 | `server.cpp` | 单线程事件循环 | 所有 I/O 和业务逻辑在主线程完成 |
| 线程池版 | `server_withThreadPool.cpp` | 主线程 I/O + 线程池业务处理 | I/O 与业务逻辑分离 |

**客户端：**

| 版本 | 源文件 | 线程模型 | 说明 |
|------|--------|----------|------|
| 多线程版 | `client.cpp` | 双线程（发送+接收） | 原始 POSIX 套接字，独立接收线程 |
| libevent 版 | `client_libevent.cpp` | 单线程事件循环 | 基于 libevent，stdin 和网络统一事件驱动 |

### 关键特性
- 基于 libevent 的高效事件驱动网络 I/O
- 4 线程的消息处理线程池（线程池版服务器）
- 支持用户注册、在线列表、私聊、广播等完整聊天功能
- 自定义文本行协议（`\n` 分隔）
- 通过 mutex 和 libevent 线程安全机制保证数据一致性（线程池版）

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

### 2.3 自定义消息协议

本项目采用**基于文本行的自定义协议**，所有消息以 `\n`（换行符）作为分隔符，使用 ASCII 文本编码。

#### 2.3.1 传输层

```
┌──────────────────────────────────────────────────────────────┐
│  传输格式：ASCII 文本 + LF (\n) 行分隔                        │
│                                                              │
│  物理帧示例（TCP 字节流）：                                    │
│  [48 65 6C 6C 6F 20 41 6C 69 63 65 0A]                      │
│  [48 65 6C 6C 6F 20 42 6F 62 0A]                             │
│   ├─ "Hello Alice\n" ─┘ ├─ "Hello Bob\n" ─┘                  │
│                                                              │
│  提取方式：evbuffer_readln(input, &len, EVBUFFER_EOL_LF)     │
│  libevent 自动从 TCP 字节流中按 '\n' 切割出完整消息行          │
└──────────────────────────────────────────────────────────────┘
```

#### 2.3.2 协议消息类型

| # | 消息类型 | 方向 | 格式 | 示例 |
|---|---------|------|------|------|
| 1 | **用户注册** | C→S | `Hello <name>` | `Hello Alice` |
| 2 | **在线列表请求** | C→S | `LIST` | `LIST` |
| 3 | **私聊消息** | C→S | `PVT <target> <content>` | `PVT Bob 你好！` |
| 4 | **广播消息** | C→S | `<任意文本>` | `大家好` |

#### 2.3.3 服务器响应消息

| # | 响应类型 | 方向 | 前缀 | 格式 | 示例 |
|---|---------|------|------|------|------|
| 1 | **系统通知** | S→C | `[System]` | `[System] <通知内容>` | `[System] you have logged in as Alice` |
| 2 | **在线列表** | S→C | `Users:` | `Users: <name1> <name2> ...` | `Users: Alice Bob Charlie` |
| 3 | **私聊消息** | S→C | `[PVT]` | `[PVT] <sender>: <content>` | `[PVT] Bob: 你好！` |
| 4 | **广播消息** | S→C | 无前缀 | `<sender>: <content>` | `Alice: 大家好` |

#### 2.3.4 协议详细说明

**① 用户注册协议 `Hello <name>`**

```
客户端 → 服务器:  Hello Alice\n

服务器处理逻辑：
  1. 检查该连接是否已注册
  2. 如果未注册且用户名已存在，自动追加后缀：Alice → Alice_2 → Alice_3 ...
  3. 在 name_to_buffer / buffer_to_name 中建立映射

服务器 → 当前用户:  [System] you have logged in as Alice\n
                    Users: Alice Bob Charlie\n
服务器 → 其他用户:  [System] Alice has logged in\n
```

- 用户名允许的字符：任意非空字符串
- 长度限制：`Hello ` 前缀占 6 字节，用户名最少 1 字节，因此有效消息最小长度为 7 字节
- 重名处理：自动追加 `_2`、`_3`、... 后缀直到唯一

**② 在线列表请求协议 `LIST`**

```
客户端 → 服务器:  LIST\n

服务器 → 当前用户:  Users: Alice Bob Charlie\n
```

- 无参数，精确匹配字符串 `LIST`
- 返回当前所有已注册用户的用户名，空格分隔

**③ 私聊协议 `PVT <target> <content>`**

```
客户端 → 服务器:  PVT Bob 你好！\n

服务器处理逻辑：
  1. 用空格分割出命令 "PVT" 和目标用户名 "Bob"
  2. 剩余部分作为消息内容 "你好！"
  3. 查找目标用户的 bufferevent

服务器 → 目标用户:  [PVT] Alice: 你好！\n
  或（用户不在线）
服务器 → 当前用户:  [System] Bob is not online\n
```

- `PVT` 和目标用户名之间用**单个空格**分隔
- 目标用户名和消息内容之间用**单个空格**分隔
- 消息内容可以包含任意字符（包括空格），但不能包含换行符

**④ 广播协议（默认）**

```
客户端 → 服务器:  大家好\n

服务器处理逻辑：
  1. 检查消息不匹配任何特殊命令
  2. 遍历所有在线用户，跳过发送者自身

服务器 → 其他所有用户:  Alice: 大家好\n
```

- 当消息不以 `Hello `、`LIST`、`PVT ` 开头时，视为广播消息
- 服务器自动在消息前添加 `<发送者用户名>: ` 前缀

#### 2.3.5 协议交互时序图

```
  Client A                    Server                    Client B
    │                          │                          │
    │──── Hello Alice ────────→│                          │
    │                          │                          │
    │←── [System] you have ───│                          │
    │    logged in as Alice    │                          │
    │←── Users: Alice ────────│                          │
    │                          │── [System] Alice has ──→│
    │                          │   logged in              │
    │                          │                          │
    │                          │←──── Hello Bob ─────────│
    │                          │                          │
    │←── [System] Bob has ────│←── [System] you have ───│
    │    logged in             │   logged in as Bob       │
    │                          │←── Users: Alice Bob ────│
    │                          │                          │
    │──── PVT Bob 你好 ──────→│                          │
    │                          │── [PVT] Alice: 你好 ───→│
    │                          │                          │
    │                          │←──────── Hi! ───────────│
    │←── Bob: Hi! ────────────│                          │
    │                          │                          │
    │──── LIST ───────────────→│                          │
    │←── Users: Alice Bob ────│                          │
    │                          │                          │
    │==== (TCP 断开) ═════════│                          │
    │                          │── [System] Alice has ──→│
    │                          │   logged out             │
```

#### 2.3.6 客户端实现要点

```cpp
// 发送消息时，客户端必须在消息末尾追加 '\n'
message += "\n";
send(client_socket, message.c_str(), message.length(), 0);

// 接收消息时，客户端按 '\n' 分割接收到的字节流
// 支持 TCP 粘包：一条 recv() 可能包含多条消息
while ((pos = recv_buffer.find('\n')) != std::string::npos) {
    std::string message = recv_buffer.substr(0, pos);
    // 处理完整消息行
    recv_buffer.erase(0, pos + 1);
}
```

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

## 5. 从单线程版到线程池版的修改详解

### 5.1 项目文件结构

```
Chatroom-with-libevent/
├── server.cpp                  # 单线程版服务器（原版）
├── server_withThreadPool.cpp   # 线程池版服务器（基于原版改造）
├── client.cpp                  # 多线程客户端（原始 POSIX 套接字）
├── client_libevent.cpp         # libevent 客户端（单线程事件驱动）
├── Thread_pool.hpp             # 线程池实现（header-only）
├── CMakeLists.txt              # 构建配置
└── TECHNICAL_DOC.md            # 本文档
```

### 5.2 CMakeLists.txt 修改

```cmake
# 单线程版：仅链接 libevent
target_link_libraries(server PRIVATE ${LIBEVENT_LIBRARIES})

# 线程池版：链接 libevent + pthreads（线程池需要 std::thread）
target_link_libraries(server_withThreadPool PRIVATE ${LIBEVENT_LIBRARIES} Threads::Threads)
```

**原因**：线程池使用了 `std::thread`、`std::mutex` 等 POSIX 线程原语，需要链接 `pthread` 库。单线程版 `server.cpp` 保持不变，不需要额外链接。

---

### 5.3 server_withThreadPool.cpp 相对于 server.cpp 的修改

| 修改项 | server.cpp（原版） | server_withThreadPool.cpp（线程池版） | 原因 |
|--------|-------------------|--------------------------------------|------|
| 头文件 | 无 | 新增 `<event2/thread.h>`, `"Thread_pool.hpp"` | 启用 libevent 线程支持 + 引入线程池 |
| `serverCtx` | 无同步机制 | 新增 `std::mutex mtx`, `ThreadPool pool{4}` | 保护共享状态 + 线程池实例 |
| `main()` | 直接创建 event_base | 先调用 `evthread_use_pthreads()` | 让 libevent 使用 pthreads，启用内部线程安全 |
| `accept_cb` | `BEV_OPT_CLOSE_ON_FREE` | 添加 `BEV_OPT_THREADSAFE` 标志 | 允许跨线程安全访问 bufferevent |
| `accept_cb` | 直接 push_back | 加 `std::lock_guard` 保护 userlist 写入 | 多线程安全 |
| `read_cb` | 内联处理所有业务逻辑 | 仅提取消息行，投递到线程池 | I/O 与业务逻辑分离，避免阻塞事件循环 |
| 新增 `process_message` | 无 | 从 read_cb 提取的业务逻辑 | 线程池工作线程执行的独立函数 |
| `event_cb` | 无锁保护 | 加 `std::lock_guard<std::mutex>` | 退出回调也需要线程安全保护 |

### 5.4 线程池工作流程

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

### 5.5 `client_libevent.cpp` 技术说明

#### 5.5.1 设计目标

与 `client.cpp`（多线程版）功能相同，但采用**纯事件驱动**架构，无需额外线程：

| 对比项 | `client.cpp`（多线程版） | `client_libevent.cpp`（libevent 版） |
|--------|-------------------------|--------------------------------------|
| 线程数 | 2（主线程 + 接收线程） | 1（单线程事件循环） |
| 网络 I/O | `recv()` 阻塞调用 | `bufferevent` 异步回调 |
| stdin 读取 | `std::getline` 阻塞 | libevent 监听 `STDIN_FILENO` 事件 |
| 退出机制 | `shutdown()` + `join()` + `close(STDIN_FILENO)` | `event_base_loopexit()` 终止事件循环 |
| 依赖 | POSIX sockets + pthreads | libevent |

#### 5.5.2 架构

```
┌────────────────────────────────────────────────────────────┐
│                   单线程事件循环（event_base）               │
│                                                            │
│  ┌──────────────────┐  ┌──────────────────┐               │
│  │ 网络 bufferevent  │  │ stdin event      │               │
│  │                  │  │ (EV_PERSIST)     │               │
│  │ • read_cb        │  │ • stdin_cb       │               │
│  │   收到服务器消息  │  │   读取用户输入    │               │
│  │   打印到控制台    │  │   发送给服务器    │               │
│  │                  │  │                  │               │
│  │ • event_cb       │  └──────────────────┘               │
│  │   连接成功→注册   │                                    │
│  │   断开/错误→退出  │                                    │
│  └──────────────────┘                                    │
└────────────────────────────────────────────────────────────┘
```

#### 5.5.3 核心组件

**`clientCtx` — 客户端上下文**

```cpp
struct clientCtx {
    struct event_base* base;       // 事件循环
    struct bufferevent* bev;       // 网络连接（bufferevent 封装了 socket 读写）
    struct event* stdin_event;     // 标准输入事件
    bool registered;               // 是否已完成注册
};
```

**`read_cb` — 网络数据回调**

从 `bufferevent` 的输入缓冲区按 `\n` 分割消息行，打印到控制台。使用 `evbuffer_readln()` 自动处理 TCP 粘包。

**`event_cb` — 连接事件回调**

- `BEV_EVENT_CONNECTED`：连接成功后提示用户输入用户名，发送 `Hello <name>` 注册
- `BEV_EVENT_ERROR` / `BEV_EVENT_EOF`：连接断开或出错时，设置退出标志并终止事件循环

**`stdin_cb` — 标准输入回调**

libevent 将 `STDIN_FILENO` 注册为事件源（`EV_READ | EV_PERSIST`）。当用户在终端输入并按回车时，触发此回调，读取一行文本并通过 `bufferevent` 发送给服务器。

**`signal_handler` — 信号处理**

收到 `SIGINT`（Ctrl+C）时调用 `event_base_loopexit()`，优雅终止事件循环。

#### 5.5.4 关键实现细节

```cpp
// stdin 作为 libevent 事件源
// STDIN_FILENO 是标准的文件描述符 0，libevent 可以监听它的可读事件
g_ctx.stdin_event = event_new(g_ctx.base, STDIN_FILENO,
                              EV_READ | EV_PERSIST, stdin_cb, nullptr);

// 异步 TCP 连接
bufferevent_socket_connect(g_ctx.bev, (struct sockaddr*)&sin, sizeof(sin));

// 统一事件循环：网络和 stdin 在同一个 event_base 中处理
event_base_dispatch(g_ctx.base);
```

#### 5.5.5 退出流程

```
用户按下 Ctrl+C
       ↓
signal_handler() 设置 g_running = false
       ↓
调用 event_base_loopexit() 通知事件循环退出
       ↓
event_base_dispatch() 返回
       ↓
释放 stdin_event → 释放 bev → 释放 base
       ↓
程序退出
```

---

## 6. 运行与编译

```bash
# 编译
mkdir build && cd build
cmake ..
make

# 运行单线程服务器（默认监听 0.0.0.0:8080）
./server

# 运行线程池版服务器
./server_withThreadPool

# 运行多线程客户端
./client

# 运行 libevent 客户端
./client_libevent
```

**输出示例**：
```
# 单线程版服务器
[System] Server started
[System] New connection

# 线程池版服务器
[System] Server started (thread pool: 4 workers)
[System] New connection

# libevent 客户端
[System] Connecting to server...
[System] Connected to server
Enter username: Alice
[System] you have logged in as Alice
Users: Alice
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