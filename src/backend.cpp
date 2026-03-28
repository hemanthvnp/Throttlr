#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./backend <port>\n";
        return 1;
    }

    int port = atoi(argv[1]);

    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);

    std::cout << "Backend running on port " << port << "\n";

while (true) {
    client_socket = accept(server_fd, (struct sockaddr*)&address,
                           (socklen_t*)&addrlen);

    char buffer[1024] = {0};
    ssize_t bytes = read(client_socket, buffer, sizeof(buffer));
    (void)bytes;

    std::cout << "Received request:\n" << buffer << "\n";

    std::string body = "Response from backend port " + std::to_string(port) + "\n";

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" +
        body;

    send(client_socket, response.c_str(), response.size(), 0);

    close(client_socket);
}
    return 0;
}
