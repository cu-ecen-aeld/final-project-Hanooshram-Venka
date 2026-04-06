//FIle name - socket_daemon.c
//Description - This file is the socket daemon file that allws us to send commands to the Raspberry Pi 4b, usin gthe Netcat tool at port 9000
// Author - Hanooshram Venkateswaran
//References:
// 1) Used a similar code structure to aesdsocket.c from previous assignments
// 2) FOund Beej's Guide to Network programming to be useful regarding socket server concepts 
// 3) Used Linux Man Pages often with syslog.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <fcntl.h>

#define PORT 9000
#define BUFFER_SIZE 1024

// This section helps in detaching the process from the terminal, to run in the background
void daemonize() {
    pid_t pid = fork(); // forks the parent process
    if (pid < 0) exit(EXIT_FAILURE); // fork failed
    if (pid > 0) exit(EXIT_SUCCESS); // Parent terminates, child continues
    if (setsid() < 0) exit(EXIT_FAILURE); // Creates a new session for the child

    // Close standard file descriptors to run in background
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    // Initializes Syslog for logging commands and tracking daemon activity in /var/log/messages
    openlog("audiod-daemon", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "Audio Daemon starting on port %d", PORT);

    // Creates the TCP Socket (IPv4, TCP Stream)
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        syslog(LOG_ERR, "Socket failed");
        return EXIT_FAILURE;
    }

    // Forcefully attaching socket to the port 9000, this prevents "bind failed" error if daemon is restarted quickly
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configures the server address structure
    address.sin_family = AF_INET; // IPv4
    address.sin_addr.s_addr = INADDR_ANY; // Accepts connections from any interface
    address.sin_port = htons(PORT); //to ensure correct byte order for the port

    // Binds  the socket to the port and IP, and Listens
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        syslog(LOG_ERR, "Bind failed");
        return EXIT_FAILURE;
    }
    // Socket will listen to incoming connection requests (will queue upto 3 requests)
    if (listen(server_fd, 3) < 0) {
        syslog(LOG_ERR, "Listen failed");
        return EXIT_FAILURE;
    }

    // Run persistently in the background (Daemonize)
    daemonize();

    // Main Server Loop - will continuously wait for and  handle new connections
    while (1) {
        // accept() will block execution until a client connects through Netcat or a script (if used)
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) continue; // if connection fails, it waits for a new one

        // Reads the incoming string from the client
        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread > 0) {
            // Log the received command to syslog foro verification
            syslog(LOG_INFO, "Received command: %s", buffer);
            memset(buffer, 0, BUFFER_SIZE); // Clears the buffer to ensure old data does not corrupt next command
        }

        close(new_socket);
    }

    closelog();
    return 0;
}
