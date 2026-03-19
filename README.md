
# C++ Simple Chat Room (Libevent & Multi-threading)

A high-performance, lightweight chat application featuring a **Libevent-based asynchronous server** and a **multi-threaded C++ client**. The system supports user registration, real-time broadcasting, private messaging, and graceful shutdowns.

## Features

### Functional Features
*   **User Registration**: Automatically registers users upon connection using the `Hello <name>` protocol.
*   **Dynamic User List**: Request a list of all online users using the `LIST` command.
*   **Private Messaging**: Send direct messages to specific users using the `PVT` command.
*   **Global Broadcast**: Send messages to everyone in the chat room by default.
*   **Duplicate Name Handling**: The server automatically appends suffixes (e.g., `user_1`) if a username is already taken.

### Technical Highlights
*   **Asynchronous I/O**: The server utilizes `Libevent` for efficient handling of multiple concurrent connections without overhead.
*   **Dual-Threaded Client**: Separate threads for sending (UI/Input) and receiving (Network) to ensure a non-blocking user experience.
*   **Graceful Shutdown**: Robust signal handling (`SIGINT`) on the client side to ensure sockets are closed properly and the background receiver thread is interrupted and joined gracefully, preventing process hangs.
*   **Memory Safety**: Efficient buffer management using Libevent's `evbuffer` and `bufferevent`.

---

## Command Logic

The server parses incoming strings to determine the intended action:

| Command | Logic | Description |
| :--- | :--- | :--- |
| **Registration** | `Hello <username>` | Must be the first message sent. Maps the connection to a unique name. |
| **List Users** | `LIST` | Returns a space-separated string of all currently online users. |
| **Private Chat** | `PVT <to> <msg>` | Looks up the target in a hash map and forwards the message only to them. |
| **Broadcast** | `<any other text>` | Iterates through the global user list and sends the message to everyone except the sender. |

---

## Client Architecture

### Dual-Threading Model
The client is designed to solve the "blocking input" problem:
1.  **Main Thread (Send)**: Handles `std::getline` from `stdin`. It blocks waiting for user input but doesn't prevent the UI from updating because receiving happens elsewhere.
2.  **Background Thread (Receive)**: Runs a continuous loop calling `recv()`. When data arrives, it is parsed and printed to the console immediately.

### Signal Handling (`csignal`)
To prevent "zombie" connections or socket hang-ups:
*   A global `std::atomic<bool> is_running` flag controls the lifecycle of both threads.
*   When `Ctrl+C` (SIGINT) is pressed, the `signal_handler` toggles the flag.
*   The client performs a `shutdown()` and `join()` to ensure the receiving thread terminates cleanly before the process exits.

---

## Prerequisites

*   **Compiler**: GCC/Clang with C++17 support.
*   **Build System**: CMake 3.10+.
*   **Libraries**: 
    *   `libevent` (core, extra, and pthreads).
    *   `Threads` (POSIX threads).

On Ubuntu/Debian:
```bash
sudo apt-get install libevent-dev cmake build-essential
```

---

## Build and Run

1.  **Clone the repository**:
    ```bash
    git clone https://github.com/yourusername/chat-system.git
    cd chat-system
    ```

2.  **Build using CMake**:
    ```bash
    mkdir build && cd build
    cmake ..
    make
    ```

3.  **Run the Server**:
    ```bash
    ./server
    ```

4.  **Run the Client** (Open multiple terminals to test):
    ```bash
    ./client
    ```

---

## Usage Example

**Client A:**
```text
Enter username: Alice
[System] you have logged in as Alice
Users: Alice
Enter message: Hello Bob
```

**Client B:**
```text
Enter username: Bob
[System] you have logged in as Bob
Users: Alice Bob
[System] Bob has logged in
Alice: Hello Bob
Enter message: PVT Alice Hey Alice, this is private!
```

---

## Project Structure

*   `server.cpp`: Libevent-based event loop, user management, and command parsing.
*   `client.cpp`: Multi-threaded socket client with signal handling.
*   `CMakeLists.txt`: Build configuration for linking Libevent and Threads.


---

# C++ 简易聊天室 (基于 Libevent 与多线程)

这是一个高性能、轻量级的聊天室系统，由基于 **Libevent 的异步服务端**和 **C++ 多线程客户端**组成。系统支持用户注册、实时广播、私聊功能，并具备完善的退出机制。

## 功能特性

### 业务功能
*   **用户注册**：连接后通过 `Hello <name>` 协议自动注册。
*   **动态用户列表**：输入 `LIST` 命令即可获取当前所有在线用户信息。
*   **私聊功能**：使用 `PVT <to> <msg>` 命令向指定用户发送私密消息。
*   **全局广播**：默认发送的消息将广播给聊天室内的所有在线用户。
*   **重名处理**：若用户名已被占用，服务端会自动添加后缀（例如 `user_1`）。

### 技术亮点
*   **异步 I/O**：服务端利用 `Libevent` 高效处理大量并发连接，无需为每个连接创建线程。
*   **双线程客户端**：发送（用户输入）与接收（网络监听）分属不同线程，确保界面非阻塞，响应实时。
*   **安全停机**：客户端实现了完善的信号处理（`SIGINT`），确保按下 `Ctrl+C` 时能够正确关闭 Socket、中断阻塞中的接收线程并安全回收（Join）资源，防止程序挂起。
*   **内存安全**：利用 Libevent 的 `evbuffer` 和 `bufferevent` 进行高效且安全的缓冲区管理。

---

## 指令逻辑

服务端通过解析输入字符串的前缀来判断操作类型：

| 指令 | 逻辑 | 描述 |
| :--- | :--- | :--- |
| **注册** | `Hello <username>` | 必须是连接后的第一条消息。将连接映射到唯一用户名。 |
| **在线列表** | `LIST` | 返回当前所有在线用户的空格分隔列表。 |
| **私聊** | `PVT <to> <msg>` | 在哈希表中查找目标用户，并将消息仅转发给对方。 |
| **广播** | `<其他任何文本>` | 遍历全局用户列表，将消息转发给除发送者以外的所有人。 |

---

## 客户端架构

### 双线程模型
客户端设计旨在解决“输入阻塞”问题：
1.  **主线程 (发送)**：处理来自 `stdin` 的 `std::getline`。即使它在等待用户输入时阻塞，也不会影响消息的接收。
2.  **后台线程 (接收)**：持续循环调用 `recv()`。一旦数据到达，立即解析并将其打印到控制台。

### 信号处理 (`csignal`)
为了防止“僵尸”连接或 Socket 挂起：
*   使用全局 `std::atomic<bool> is_running` 标志控制双线程生命周期。
*   当触发 `Ctrl+C` (SIGINT) 时，信号处理函数会翻转标志位。
*   客户端执行 `shutdown()` 操作打破 `recv` 的阻塞状态，并执行 `join()` 确保后台线程在进程结束前完全退出。

---

## 环境要求

*   **编译器**：支持 C++17 的 GCC 或 Clang。
*   **构建系统**：CMake 3.10+。
*   **依赖库**：
    *   `libevent` (core, extra, 以及 pthreads)。
    *   `Threads` (POSIX 线程库)。

在 Ubuntu/Debian 上安装：
```bash
sudo apt-get install libevent-dev cmake build-essential
```

---

## 编译与运行

1.  **克隆仓库**：
    ```bash
    git clone https://github.com/yourusername/chat-system.git
    cd chat-system
    ```

2.  **使用 CMake 编译**：
    ```bash
    mkdir build && cd build
    cmake ..
    make
    ```

3.  **运行服务端**：
    ```bash
    ./server
    ```

4.  **运行客户端**（打开多个终端进行测试）：
    ```bash
    ./client
    ```

---

## 使用示例

**用户 A:**
```text
Enter username: Alice
[System] you have logged in as Alice
Users: Alice
Enter message: Hello Bob
```

**用户 B:**
```text
Enter username: Bob
[System] you have logged in as Bob
Users: Alice Bob
[System] Bob has logged in
Alice: Hello Bob
Enter message: PVT Alice Hey Alice, this is private!
```

---

## 项目结构

*   `server.cpp`：基于 Libevent 的事件循环、用户管理及指令解析逻辑。
*   `client.cpp`：支持信号处理的多线程 Socket 客户端。
*   `CMakeLists.txt`：用于链接 Libevent 和线程库的构建配置。

---
## License
This project is open-source and available under the [MIT License](LICENSE).
