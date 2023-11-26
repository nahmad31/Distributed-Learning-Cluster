#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>
#include <random>

#define PORT "13337"  // the port server is listening on
#define TEST_FILE_SIZE 500000 // Approximate file size to generate

/**
 * Spawns process to run grep query
 * @param query - The grep query to run from client
 * @return - The output of the query
*/
FILE* processQuery(std::string query) {
    FILE* output = NULL;
    query = "grep -H " + query;
    std::cout << "This following query was run: " << query << std::endl;
    output = popen(query.c_str(), "r");
    return output;
}

/**
 * Generates logging files for testing, contens of the file differ on id
 * @param filename - The name of the file to generate
 * @param numLines - Number of lines to generate inside the file, not counting id-specific lines
 * @param id - id of the server
*/
void generateLogFile(const std::string& fileName, int numLines, int id) { 
    std::ofstream outfile(fileName);
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist(1000000000,9999999999); 

    for (int i = 0; i < numLines; i++) {
        if (i % 50000 == 0) {
            outfile << "[ERROR] This is an ERROR Log" << std::endl;
            if (id < 1) { outfile << "[LOG] Pineapple" << std::endl; }
            if (id < 2) { outfile << "[LOG] Orange" << std::endl; }
            if (id < 3) { outfile << "[LOG] Apple" << std::endl; }
            if (id < 4) { outfile << "[LOG] Strawberry" << std::endl; }
            if (id < 5) { outfile << "[LOG] Coconut" << std::endl; }
            if (id < 6) { outfile << "[LOG] Raspberry" << std::endl; }
            if (id < 7) { outfile << "[LOG] Kiwi" << std::endl; }
            if (id < 8) { outfile << "[LOG] Lychee" << std::endl; }
            if (id < 9) { outfile << "[LOG] Blackberry" << std::endl; }
            if (id < 10){ outfile << "[LOG] Banana" << std::endl; }
        } else if (i % 5000 == 0) {
            outfile << "[WARNING] This is an WARNING Log" << std::endl;
        } else if (i % 500 == 0) {
            outfile << "[INFO] This is an INFO Log" << std::endl;
        } else {
            outfile << "Generated Random Number:" << dist(rng) << std::endl;
        }
    }
    outfile.close();
}

/**
 * Runs server and accepts connections and queries from client
*/
int main() {
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPV4
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE; // Set to local IP, server only

    if (int status = getaddrinfo(NULL, PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        exit(1);
    }
    
    int socketFd = socket(AF_INET, SOCK_STREAM, 0); // Make socket for server

    int value = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value))) { // Allows us to immediately reuse a port
        std::cerr << "setsockopt error" << std::endl;
        exit(1);
    }

    if (bind(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) != 0) { // Binding server socket to a network interface and port
        std::cerr << "bind error" << std::endl;
        exit(1);
    }

    freeaddrinfo(servinfo);

    if (listen(socketFd, 5) == -1) { // Prepare to accept connections on socket fd
        std::cerr << "listen error" << std::endl;
        exit(1);
    }

    std::cout << "Waiting for Queries..." << std::endl;

    while(1) { // accept loop
        int clientFd;
        int bytesRead;
        int headerSize = 4;
        char header[headerSize + 1];
        int bufferSize = 1024;
        char buffer[bufferSize + 1];

        clientFd = accept(socketFd, NULL, NULL);
        if ((bytesRead = recv(clientFd, header, headerSize, 0)) != headerSize) {
            std::cerr << "invalid header" << std::endl;
            close(clientFd);
            continue;
        }
        header[headerSize] = '\0';

        if (!strcmp(header, "grep")) {
            std::string query;
            while ((bytesRead = recv(clientFd, buffer, bufferSize, 0))) {
                buffer[bytesRead] = '\0';
                query += buffer;
            }
            if (query.size() == 0) {
                std::cerr << "invalid body" << std::endl;
                close(clientFd);
                continue;
            }
            FILE* queryOutput = processQuery(query);
            if (queryOutput == NULL) {
                std::cerr << "invalid grep" << std::endl;
                close(clientFd);
                continue;
            }
            
            while (fgets(buffer, bufferSize, queryOutput)) {
                if (send(clientFd, buffer, strlen(buffer), 0) == -1) {
                    break;
                }
            }  
            pclose(queryOutput);
            close(clientFd);

        } else if (!strcmp(header, "test")) {
            std::string str_id;
            while ((bytesRead = recv(clientFd, buffer, bufferSize, 0))) {
                buffer[bytesRead] = '\0';
                str_id += buffer; // receiving an integer ID
            }
            int id;
            try {
                id = std::stoi(str_id);
            } catch (...) {
                std::cerr << "invalid header" << std::endl;
                close(clientFd);
                continue;
            }
            generateLogFile("test" + std::to_string(id+1) + ".log", TEST_FILE_SIZE, id);
            close(clientFd);
            continue;
        } else {
            std::cerr << "invalid header" << std::endl;
            close(clientFd);
            continue;
        }
    }   

    return 0;
}
