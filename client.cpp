#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <csignal>
#include <algorithm>

// global mark
std::atomic<bool> is_running(true);
std::atomic<bool> signal_received(false);

void signal_handler(int signal){
    if(signal == SIGINT){
        std::cout << "\n[System] Exiting..." << std::endl;
        is_running = false;
        signal_received = true;
    }
}

void receive_message(int client_socket) {
    std::string recv_buffer;
    char temp_buf[1024];

    while (is_running) {
        ssize_t len = recv(client_socket, temp_buf, sizeof(temp_buf), 0);
        
        if (len > 0) {
            recv_buffer.append(temp_buf, len);
            size_t pos;
            while ((pos = recv_buffer.find('\n')) != std::string::npos) {
                std::string message = recv_buffer.substr(0, pos);
                std::cout << message << std::endl; 
                recv_buffer.erase(0, pos + 1);
            }
        } else {
            if (is_running) {
                std::cout << "\n[System] Connection lost." << std::endl;
                is_running = false;
            }
            break;
        }
    }
}

void send_message(int client_socket) {
    while (is_running) {
        std::string message;
        std::cout << "Enter message: " << std::flush;
        if (!std::getline(std::cin, message)){
            if(signal_received){
                break;
            }
            break;
        }
        if (message.empty()) continue;
        if (!is_running) break;

        message += "\n";
        if (send(client_socket, message.c_str(), message.length(), 0) < 0) {
            std::cout << "Error sending message" << std::endl;
            is_running = false;
            break;
        }
    }
}

int main() {
    std::signal(SIGINT, signal_handler);

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

    if (connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("Connect failed");
        return -1;
    }

    // register and start receiving thread
    std::string username;
    std::cout << "Enter username: ";
    std::getline(std::cin, username);
    std::string reg_msg = "Hello " + username + "\n";

    std::thread t_recv(receive_message, client_socket);

    send(client_socket, reg_msg.c_str(), reg_msg.length(), 0);

    // start sending
    send_message(client_socket);

    is_running = false;
    shutdown(client_socket, SHUT_RDWR);
    
    if (t_recv.joinable()) {
        t_recv.join();
    }

    close(client_socket);

    return 0;
}