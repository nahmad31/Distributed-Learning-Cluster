#include <iostream>
#include <thread>
#include <atomic>
#include <list>
#include <unordered_map>
#include <chrono>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <condition_variable>

#define PRIMARY_INTODUCER_HOST_NAME "fa22-cs425-5301.cs.illinois.edu"
#define PORT "13338"
#define MAX_CONCURRENT_FAILURES 3

#define NUM_RECENT_TO_LOG 3
#define RECENTLY_JOINED_NODES_FILE "recent_nodes.log"

// struct that contains global variables that are used by multiple threads
struct Enviorment {
    inline static bool isIntroducer = false;
    inline static std::list<std::string> membershipList;
    inline static std::unordered_map<std::string, std::list<std::string>::iterator> mapHosttoItr;
    inline static std::string hostName;
    inline static std::string logFile;
    inline static std::string membershipID;
    inline static std::mutex listMutex;
    inline static std::mutex logMutex;
    inline static std::condition_variable pingCV;
    inline static std::atomic<bool> recievdAck[MAX_CONCURRENT_FAILURES];
};

/**
 * Writes a message to a log file and appends a time stamp
 * @param message - The string to log
*/
void logToFile(const std::string& message) {
    std::lock_guard<std::mutex> lock(Enviorment::logMutex);
    std::ofstream file(Enviorment::logFile, std::ios::out | std::ios::app);
    file << message << " [" << std::chrono::system_clock::now().time_since_epoch().count() << "]\n";
    file.close();
}

/**
 * Logs the ip of the latest {NUM_RECENT_TO_LOG} nodes that have joined
 * File is read by the primary introducer if it ever goes down and needs to rejoin the group
 * @param ip - The ip of the node that has joined
*/
void logRecentlyJoinedNodes(const std::string& ip) {
    std::ifstream inputFile(RECENTLY_JOINED_NODES_FILE);
    std::string line;
    std::vector<std::string> recNodes;
    while (std::getline(inputFile, line)) {
        recNodes.push_back(line);
    }
    inputFile.close();

    recNodes.push_back(ip);
    int i = 0;
    if (recNodes.size() > NUM_RECENT_TO_LOG) { // makes sure to only log the latest {NUM_RECENT_TO_LOG} nodes
        i = 1;
    }
    std::ofstream file(RECENTLY_JOINED_NODES_FILE, std::ios::out | std::ios::trunc); // re-open and clear the file to write the latest nodes
    for (;i < recNodes.size(); i++) {
        file << recNodes[i] << '\n';
    }
    file.close();
}

/**
 * Sends a UDP message to destIP
 * @param message - The message to be sent
 * @param destIP - the IP to send the message to
*/
void sendMessage(const std::string& message, const std::string& destIP) {
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET; // IPv4
	hints.ai_socktype = SOCK_DGRAM; // UDP

    if (int status = getaddrinfo(destIP.c_str(), PORT, &hints, &servinfo)) {
		std::cerr << "getaddrinfo error, exiting process " << gai_strerror(status) << std::endl;
        exit(1);
	}
    int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sendto(socketFd, message.c_str(), message.size(), 0, servinfo->ai_addr, servinfo->ai_addrlen);
    close(socketFd); // Close UDP sockets even though it is connectionless, clean up resources (File Descriptors)
    return;
}

/**
 * Sends message to all known nodes in the membership list in parallel
 * @param message - The message to be sent
 * @param toExclude - Node not to send the message to
*/
void sendMessageToAll(const std::string& message, std::string toExclude = "") {
    std::vector<std::jthread> threads;
    Enviorment::listMutex.lock();
    for (const std::string& member: Enviorment::membershipList) {
        std::string ip = member.substr(0, member.find(":"));
        if (ip == Enviorment::hostName || ip == toExclude) {
            continue;
        }
        threads.emplace_back(sendMessage, message, ip);
    }
    Enviorment::listMutex.unlock();
    for (std::jthread& thread: threads) {
        thread.join();
    }
    return;
}

/**
 * Pings neighbor node every 5 seconds to check whether they are up
 * @param nodeToPing - Relative position of the iᵗʰ neighbor node in the membership list/ring
 * 0 = neighbor, 1 = neighbor+1, 2 = neighbor+2
*/
void pingNodes(int nodeToPing) {
    while (1) {
        std::unique_lock<std::mutex> lck(Enviorment::listMutex);
        while (Enviorment::membershipList.size() <= nodeToPing + 1) { // if membership list too small for neighbor to exist, wait
            Enviorment::pingCV.wait(lck);
        }

        auto toPingItr = Enviorment::mapHosttoItr[Enviorment::membershipID];
        for (int i = 0; i <= nodeToPing; i++) { // if end of membership list reached before obtaining neighbor, loop around
            toPingItr++;
            if (toPingItr == Enviorment::membershipList.end()) {
                toPingItr = Enviorment::membershipList.begin();
            }
        }

        std::string toPingStr = *toPingItr;
        lck.unlock();
        std::string ip = toPingStr.substr(0, toPingStr.find(":"));
        sendMessage("P\n" + std::to_string(nodeToPing), ip);

        std::this_thread::sleep_for(std::chrono::seconds(1)); // wait 1 second to receive ack from node that is pinged
        if (!Enviorment::recievdAck[nodeToPing]) {
            lck.lock();
            if (Enviorment::mapHosttoItr.contains(toPingStr)) { // make sure the member still exists in local membership list
                Enviorment::membershipList.erase(toPingItr);
                Enviorment::mapHosttoItr.erase(toPingStr);
                lck.unlock();
                logToFile("Did not recieve acknowledgement from " + toPingStr + " Sending failure messages");
                sendMessageToAll("F\n" + toPingStr); // Sends a fail message with the membership ID of the node that did not respond
                logToFile("[FAILURE] " + toPingStr);
            } else {
                lck.unlock();
            }
        } else {
            // wait for 3 more seconds before re-pinging node. Guarantees we will be able to detect if the node is down before 5 seconds.
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } 
        Enviorment::recievdAck[nodeToPing] = false; // reset status of neighbor acknowledgement
    }
}

/**
 * Contacts Introducer, over tcp, to receive membership list
 * @param introducer - Hostname of the introducer
 * @return - If introducer was succesfully contacted and membership list was obtained ✓
*/
bool contactIntroducer(const std::string& introducer) {
    struct addrinfo hints, *servinfo;;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (int status = getaddrinfo(introducer.c_str(), PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        return false;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        return false;
    }

    std::string message = "J\n" + Enviorment::hostName;
    send(socketFd, message.c_str(), message.size(), 0); // Sending join message with local hostname
    shutdown(socketFd, SHUT_WR);

    int buffer_size = 1024;
    char buffer[buffer_size + 1];
    int numBytes = 0;
    std::string response;
    while ((numBytes = recv(socketFd, buffer, buffer_size, 0)) > 0) { // receiving new line seperated membership list
        buffer[numBytes] = '\0';
        response += buffer;
    }
    if (response.size() < 1) {
        std::cerr << "Unable to receive response from Introducer ✗" << std::endl;
        close(socketFd);
        return false;
    }

    std::string line;
    std::stringstream resStream(response);
    Enviorment::listMutex.lock();
    while (std::getline(resStream, line)) {
        Enviorment::membershipList.push_back(line);
        Enviorment::mapHosttoItr[line] = std::prev(Enviorment::membershipList.end());
        logToFile("[Populating Membership List] " + line);
    }
    Enviorment::membershipID = Enviorment::membershipList.back();
    Enviorment::listMutex.unlock();

    sendMessageToAll("J\n" + Enviorment::membershipID, introducer); // Send a message to all nodes from membership list that we have joined
    Enviorment::pingCV.notify_all(); // Notify the pinging threads to start pinging neighbor nodes if possible
    return true;
}

/**
 * If the primary introducer holds a file of recently joined nodes, try to contact those nodes
 * to rejoin the group and re-populate the membership list
 * @return - Returns true if successfully able to contact a recent node to rejoin the group
*/
bool tryContactingRecentNodes() {
    if (!std::filesystem::exists(RECENTLY_JOINED_NODES_FILE)) {
        return false;
    }
    std::ifstream file(RECENTLY_JOINED_NODES_FILE);
    std::string line;
    while (std::getline(file, line)) {
        if (contactIntroducer(line)) {
            return true;
        }
    }
    return false;
}

/**
 * Attempts to join the membership group 
 * @return - True if successfully able to join
*/
bool joinGroup() {
    if (Enviorment::isIntroducer) {
        // Try to rejoin group through recent nodes
        if (tryContactingRecentNodes()) {
            std::cout << "Rejoined Group" << std::endl;
            return true;
        }
        // Start new group if not able to join an existing one
        Enviorment::membershipID = Enviorment::hostName + ":" + 
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        Enviorment::listMutex.lock();
        Enviorment::membershipList.push_back(Enviorment::membershipID);
        Enviorment::mapHosttoItr[Enviorment::membershipID] = Enviorment::membershipList.begin();
        Enviorment::listMutex.unlock();
        logToFile("[Populating Membership List] " + Enviorment::membershipID);
        return true;
    } else {
        return contactIntroducer(PRIMARY_INTODUCER_HOST_NAME);
    }
}

/**
 * Sends a leave message to everyone in the membership group
*/
void leaveGroup() {
    sendMessageToAll("L\n" + Enviorment::membershipID); // Sends an L to everyone
    logToFile("[LEAVE] " + Enviorment::membershipID);
}

/**
 * Listens for introduction messages from joining nodes and responds back with local membership list
*/
void listenForIntroductions() {
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPV4
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // Set to local IP, server only

    if (int status = getaddrinfo(NULL, PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo error, exiting process" << gai_strerror(status) << std::endl;
        quick_exit(1);
    }
    
    int socketFd = socket(AF_INET, SOCK_STREAM, 0); // Make socket for server

    int value = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value))) { // Allows us to immediately reuse a port
        std::cerr << "setsockopt error, exiting process" << std::endl;
        quick_exit(1);
    }

    if (bind(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) != 0) { // Binding server socket to a network interface and port
        std::cerr << "bind error, exiting process" << std::endl;
        quick_exit(1);
    }

    freeaddrinfo(servinfo);

    if (listen(socketFd, 100) == -1) { // Prepare to accept connections on socket fd
        std::cerr << "listen error, exiting process" << std::endl;
        quick_exit(1);
    }

    int clientFd;
    int bytesRead;
    int bufferSize = 1024;
    char buffer[bufferSize + 1];
    while (1) { // accept loop
        clientFd = accept(socketFd, NULL, NULL);
        std::string message;
        while ((bytesRead = recv(clientFd, buffer, bufferSize, 0))) {
            buffer[bytesRead] = '\0';
            message += buffer;
        }

        std::stringstream recStream(message);
        std::string line;
        std::getline(recStream, line);
        if (line == "J") {
            std::getline(recStream, line);
            // construct membership id for joining node with format hostname:{TIME_STAMP}
            std::string id = line + ":" + 
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); 
            Enviorment::listMutex.lock();
            Enviorment::membershipList.push_back(id); // add the joining node to local membership list
            Enviorment::mapHosttoItr[id] = std::prev(Enviorment::membershipList.end());
            for (const std::string& member : Enviorment::membershipList) { //sending the joining node the membership list seperated by new lines
                std::string to_send = member+"\n";
                send(clientFd, to_send.c_str(), to_send.size(), 0);
            }
            Enviorment::pingCV.notify_all(); // Notify the pinging threads to start pinging neighbor nodes if possible
            Enviorment::listMutex.unlock();
            logToFile("[JOIN] " + id);
            if (Enviorment::isIntroducer) {
                logRecentlyJoinedNodes(line);
            }
        }
        close(clientFd);
    }
}

/**
 * Listens for UDP™ messages from other nodes in membership group and processes them
*/
void listenForMessages() {
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;       // IPv4
	hints.ai_socktype = SOCK_DGRAM;  // UDP
	hints.ai_flags = AI_PASSIVE;     // use my IP

    if (int status = getaddrinfo(NULL, PORT, &hints, &servinfo)) {
		std::cerr << "getaddrinfo fail, ending process" << gai_strerror(status) << std::endl;
        quick_exit(1);
	}
    int socketFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // Make socket to listen for messages

    int value = 1;
    if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value))) { // Allows us to immediately reuse a port
        std::cerr << "setsockopt error, ending process" << std::endl;
        quick_exit(1);
    }

    if (bind(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) != 0) { // Binding server socket to a network interface and port
        std::cerr << "bind error, ending process" << std::endl;
        quick_exit(1);
    }

    freeaddrinfo(servinfo);

    int bufferSize = 256;
    char buf[bufferSize];
    int numbytes;
    struct sockaddr_storage their_addr;
    socklen_t addr_len = sizeof(their_addr);
    while (1) {
        std::string recivedMessage;
        numbytes = recvfrom(socketFd, buf, bufferSize - 1, 0, (struct sockaddr *)&their_addr, &addr_len);
        buf[numbytes] = '\0';
        recivedMessage += buf;

        std::string line;
        std::stringstream recStream(recivedMessage);
        std::getline(recStream, line);
        if (line == "J") { // Received J'\n'MembershipID, add MembershipID to local membership list
            std::getline(recStream, line);
            Enviorment::listMutex.lock();
            Enviorment::membershipList.push_back(line);
            Enviorment::mapHosttoItr[line] = std::prev(Enviorment::membershipList.end());
            Enviorment::pingCV.notify_all();
            Enviorment::listMutex.unlock();
            logToFile("[JOIN] " + line);
        } else if (line == "L" || line == "F") { //Received {L||F}'\n'MembershipID, remove MembershipID from local membership list
            bool isLeave = (line == "L");
            std::getline(recStream, line);
            Enviorment::listMutex.lock();
            if (Enviorment::mapHosttoItr.contains(line)) {
                Enviorment::membershipList.erase(Enviorment::mapHosttoItr[line]);
                Enviorment::mapHosttoItr.erase(line);
                Enviorment::listMutex.unlock();
                if (isLeave) {
                    logToFile("[LEAVE] " + line);
                } else {
                    logToFile("[FAILURE] " + line);
                }
            } else {
                Enviorment::listMutex.unlock();
            }
        } else if (line == "P") { // Received P'\n'nodeToPing, send ack message back to sender
            std::getline(recStream, line);
            sendMessage("A\n" + line, inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));
        } else if (line == "A") { // Received A'\n'nodeToPing, set received ack'ed to true
            std::getline(recStream, line);
            Enviorment::recievdAck[stoi(line)] = true;
        }
    }
    return;
}

/**
 * Print out membership list
*/
void printMembershipList() {
    std::lock_guard<std::mutex> lock(Enviorment::listMutex);
    for (const std::string& member : Enviorment::membershipList) {
        std::cout << member << std::endl;
    }
}

/**
 * Reads commands from the user https://tinyurl.com/2u5vyamz 
*/
void readInput() {
    std::string input;
    std::jthread messageThread;
    std::jthread introducerThread;
    bool isListening = false;
    bool hasJoined = false;
    while (1) {
        getline(std::cin, input);
        if (input == "join") {
            if (hasJoined) {
                std::cout << "Already Joined" << std::endl;
                continue;
            }
            if (!isListening) { // start threads to listen for introductions and listen for messages
                introducerThread = std::jthread(listenForIntroductions);
                messageThread = std::jthread(listenForMessages);
                std::cout << "Started Listening For Introductions & Group Messages" << std::endl;
                isListening = true;
            }
            logToFile("JOINING GROUP");
            if (joinGroup()) {
                hasJoined = true;
                std::cout << "Joined Group" << std::endl;
            } else {
                logToFile("FAILED TO JOIN GROUP");
                std::cout << "Problem Joining Group, Try Again" << std::endl;
            }

        } else if (input == "leave") {
            if (hasJoined) {
                leaveGroup();
            }
            std::cout << "Left Group!" << std::endl;
            quick_exit(0);
        } else if (input == "list_mem") {
            if (!hasJoined) {
                std::cout << "Join in order to mantain membership list" << std::endl;
            } else {
                printMembershipList();
            }
        } else if (input == "list_self") {
            if (!hasJoined) {
                std::cout << "Join in order to generate ID" << std::endl;
            } else {
                std::cout << Enviorment::membershipID << std::endl;
            }
        } else {
            std::cout << "Unvalid. Try Again" << std::endl;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Usage ./membership_server [HOST_NAME] [LOG_FILE]" << std::endl;
        return 1;
    }
    Enviorment::hostName = argv[1];
    Enviorment::logFile = argv[2];
    if (Enviorment::hostName == PRIMARY_INTODUCER_HOST_NAME) {
        Enviorment::isIntroducer = true;
    }
    std::jthread inputThread(readInput); // start thread to read input

    std::vector<std::jthread> pingThreads;
    for (int i = 0; i < MAX_CONCURRENT_FAILURES; i++) {
        Enviorment::recievdAck[i] = false;
        pingThreads.emplace_back(pingNodes, i); // setup threads that will ping neighbors if possible
    }
    inputThread.join();
    quick_exit(0); // quick_exit to immediately kill all threads and exit the program
}
