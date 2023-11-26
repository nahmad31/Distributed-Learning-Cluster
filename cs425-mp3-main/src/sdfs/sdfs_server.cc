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
#include <exception>
#include <deque>
#include <random>
#include <unordered_set>
#include <regex>

#define PORT "13338"
#define MAX_CONCURRENT_FAILURES 3

// File that represents DNS, containing the hostname of the master server
#define DNS_FILE "dns.config"
// File Directory where SDFS replicates are stored
#define SDFS_FILE_DIRECTORY "temp/"

// struct that contains global variables that are used by multiple threads
struct Environment {
    inline static std::list<std::string> membershipList;
    inline static std::unordered_map<std::string, std::list<std::string>::iterator> mapHosttoItr;
    inline static std::string hostName;
    inline static std::string logFile;
    inline static std::string membershipID;
    inline static std::mutex listMutex;
    inline static std::mutex logMutex;
    inline static std::condition_variable pingCV;
    inline static std::atomic<bool> recievdAck[MAX_CONCURRENT_FAILURES];
    inline static std::unordered_map<std::string, std::deque<std::pair<long long, std::string>>> mapFilesToLoc;
    inline static std::unordered_map<std::string, std::unordered_set<std::string>> mapFilesToReps;
    inline static std::atomic<int> Wacks;
    inline static std::atomic<int> Wnacks;
    inline static std::atomic<int> itr = 0;
    inline static std::atomic<bool> masterElected;
    inline static std::atomic<int> putStatus;
    inline static std::mutex dupMutex;
    inline static std::mutex fetchMutex;
};

/**
 * Writes a message to a log file and appends a timestamp
 * @param message - The string to log
*/
void logToFile(const std::string& message) {
    std::lock_guard<std::mutex> lock(Environment::logMutex);
    std::ofstream file(Environment::logFile, std::ios::out | std::ios::app);
    file << message << " [" << std::chrono::system_clock::now().time_since_epoch().count() << "]\n";
    file.close();
}

/**
 * Reads the entry in the DNS File
 * @return returns the hostname contained in the DNS file
*/
std::string readDns() {
    std::string line;
    std::ifstream dns(DNS_FILE);
    std::getline(dns, line);
    return line;
}

/**
 * Gets the master server which is the first server in the membership list if it has been elected
 * @return returns the hostname of the master server if elected, otherwise returns an empty string
*/
std::string getMaster() {
    if (!Environment::masterElected) {
        return "";
    } else {
        std::string master = Environment::membershipList.front();
        std::string ip = master.substr(0, master.find(":"));
        return ip;
    }
}

/**
 * Splits a string into individual words
 * @param input - the string to split
 * @return a vector of strings corresponding to the words in the input
*/
std::vector<std::string> splitString(const std::string& input) {
    std::istringstream ss(input);
	std::string word;
    std::vector<std::string> toRet;
    while(std::getline(ss, word, ' ')) {
		toRet.push_back(word);
	}
    return toRet;
}

/**
 * Sends a UDP message to destIP
 * @param message - The message to be sent
 * @param destIP - the IP to send the message to
*/
void sendUDPMessage(const std::string& message, const std::string& destIP) {
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
void sendUDPMessageToAll(const std::string& message, const std::string& toExclude = "") {
    std::vector<std::jthread> threads;
    Environment::listMutex.lock();
    for (const std::string& member: Environment::membershipList) {
        std::string ip = member.substr(0, member.find(":"));
        if (ip == Environment::hostName || ip == toExclude) {
            continue;
        }
        threads.emplace_back(sendUDPMessage, message, ip);
    }
    Environment::listMutex.unlock();
    for (std::jthread& thread: threads) {
        thread.join();
    }
    return;
}

/**
 * Contacts Master, over TCP, to receive membership list
 * @param master - Hostname of the master
 * @return - If master was succesfully contacted and membership list was obtained ✓
*/
bool contactMaster(const std::string& master) {
    struct addrinfo hints, *servinfo;;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (int status = getaddrinfo(master.c_str(), PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        return false;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        return false;
    }

    std::string message = "J\n" + Environment::hostName;
    send(socketFd, message.c_str(), message.size(), 0); // Sending join message with local hostname
    shutdown(socketFd, SHUT_WR);

    int bufferSize = 1024;
    char buffer[bufferSize];
    int numBytes = 0;
    std::string response;
    while ((numBytes = recv(socketFd, buffer, bufferSize, 0)) > 0) { // receiving new line separated membership list
        buffer[numBytes] = '\0';
        response += buffer;
    }
    if (response.size() < 1) {
        std::cerr << "Unable to receive response from Master ✗" << std::endl;
        close(socketFd);
        return false;
    }

    std::string line;
    std::stringstream resStream(response);
    Environment::listMutex.lock();
    while (std::getline(resStream, line)) {
        Environment::membershipList.push_back(line);
        Environment::mapHosttoItr[line] = std::prev(Environment::membershipList.end());
        logToFile("[Populating Membership List] " + line);
    }
    Environment::membershipID = Environment::membershipList.back();
    Environment::listMutex.unlock();

    sendUDPMessageToAll("J\n" + Environment::membershipID, master); // Send a message to all nodes from membership list that we have joined
    Environment::pingCV.notify_all(); // Notify the pinging threads to start pinging neighbor nodes if possible
    return true;
}

/**
 * Attempts to join the membership group
 * @return - True if successfully able to join
*/
bool joinGroup() {
    if (Environment::hostName == readDns()) {
        Environment::membershipID = Environment::hostName + ":" + 
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        Environment::listMutex.lock();
        Environment::membershipList.push_back(Environment::membershipID);
        Environment::mapHosttoItr[Environment::membershipID] = Environment::membershipList.begin();
        Environment::listMutex.unlock();
        logToFile("[Populating Membership List] " + Environment::membershipID);
        return true;
    } else {
        return contactMaster(readDns());
    }
}

/**
 * Sends a leave message to everyone in the membership group
*/
void leaveGroup() {
    sendUDPMessageToAll("L\n" + Environment::membershipID);
    logToFile("[LEAVE] " + Environment::membershipID);
}

/**
 * Searches through metadata (mapFilesToReps) to find what servers a file was replicated at
 * if the file is not contained in the metadata, randomly selects hostnames from the membership list
 * where the file should be replicated at.
 * @param hostName - a hostname that should be added to the list of replicates 
 * @param fileName - the name of the file being replicated
 * @return a vector of containing hostnames where the file is/should be replicated at.
*/
std::vector<std::string> selectWhereToReplicate(const std::string& hostName, const std::string& fileName) {
    std::vector<std::string> replicateHosts;
    if (Environment::mapFilesToReps.contains(fileName)) {
        for (const std::string& host: Environment::mapFilesToReps[fileName]) {
            replicateHosts.push_back(host);
        }
    } else {
        std::vector<std::string> randMembers;
        std::sample(Environment::membershipList.begin(), Environment::membershipList.end(), 
            std::back_inserter(randMembers), 4, std::mt19937{std::random_device{}()});
        replicateHosts.push_back(hostName);
        for (const std::string& member: randMembers) {
            std::string ip = member.substr(0, member.find(":"));
            if (ip == hostName) {
                continue;
            }
            replicateHosts.push_back(ip);
            if (replicateHosts.size() == 4) {
                break;
            }
        }
    }
    return replicateHosts;
}

/**
 * Gets the hostnames of where the replicates of a file stored in the SFDS is located at
 * @param sdfsfilename - the name of the file
 * @return - a new-line delimited string containing the host names of where the file is stored at
*/
std::string getReplicateLocations(const std::string& sdfsfilename) {
    std::string response;
    if (Environment::hostName == getMaster()) {
        if (Environment::mapFilesToReps.contains(sdfsfilename) && Environment::mapFilesToReps[sdfsfilename].size() >= 2) {
            for (const std::string& host: Environment::mapFilesToReps[sdfsfilename]) {
                response += (host + "\n");
            }
        } else {
            response += (std::string("DNE") + "\n");
        }
    } else {
        struct addrinfo hints, *servinfo;;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_STREAM; // TCP

        if (int status = getaddrinfo(getMaster().c_str(), PORT, &hints, &servinfo)) {
            std::cerr << "Error. Master may have not been elected yet." << gai_strerror(status) << std::endl;
            return "";
        }

        int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
            close(socketFd);
            return "";
        }

        std::string message = "G\n" + sdfsfilename + "\n";
        if (send(socketFd, message.c_str(), message.size(), 0) == -1) {
            close(socketFd);
            return "";
        }
        shutdown(socketFd, SHUT_WR);

        int bufferSize = 1024;
        char buffer[bufferSize];
        int numBytes = 0;
        while ((numBytes = recv(socketFd, buffer, bufferSize, 0)) > 0) { // receiving new line separated hostnames
            buffer[numBytes] = '\0';
            response += buffer;
        }
        if (response.size() < 1) {
            std::cerr << "Unable to receive response from Master" << std::endl;
            close(socketFd);
            return "";
        }
        close(socketFd);
    }
    return response;
}

/**
 * Copies a local file to the nodes SDFS_FILE_DIRECTORY and adds it information to local metadata (mapFilesToLoc)
 * @param localfilename - the file name that should be copied
 * @param fileId - a string containing the sdfs filename and timestamp of when the file was put in the system 
*/
void addLocalToSDFS(const std::string& localfilename, const std::string& fileId) {
    std::filesystem::copy_file(localfilename, SDFS_FILE_DIRECTORY + fileId, std::filesystem::copy_options::overwrite_existing);
    std::string sdfsfilename = fileId.substr(0, fileId.find(":"));
    long long timeStamp = std::stoll(fileId.substr(fileId.find(":") + 1));
    Environment::mapFilesToLoc[sdfsfilename].push_back(std::make_pair(timeStamp, fileId));
    std::sort(Environment::mapFilesToLoc[sdfsfilename].begin(), Environment::mapFilesToLoc[sdfsfilename].end());
    if (Environment::mapFilesToLoc[sdfsfilename].size() > 5) {
        std::filesystem::remove(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[sdfsfilename].front().second);
        Environment::mapFilesToLoc[sdfsfilename].pop_front();
    }
    logToFile("[PUT] " + localfilename);
}

/**
 * Replicates the file on a given node 
 * @param localfilename - the name of the local file to replicate
 * @param fileId - the fileID containing the SDFS filename and timestamp of when the master received the write request
 * @param hostName - the host name of the node to replicate the file on
 * @param itr - int corresponding to the instance of putFile that called replicateFile
 * @return true if the replication was successful or false if not 
*/
bool replicateFile(const std::string& localfilename, const std::string& fileId, const std::string& hostName, int itr) {
    struct addrinfo hints, *servinfo;;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        if (Environment::itr == itr) { Environment::Wnacks += 1; }
        return false;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        if (Environment::itr == itr) { Environment::Wnacks += 1; }
        return false;
    }

    std::string message = "R\n" + fileId + "\n";
    if (send(socketFd, message.c_str(), message.size(), 0) == -1) {
        if (Environment::itr == itr) { Environment::Wnacks += 1; }
        return false;
    }

    int bufferSize = 1024;
    char buffer[bufferSize];
    std::ifstream file(localfilename);
    file.read(buffer, bufferSize);
    while (file.gcount()) {
        if (send(socketFd, buffer, file.gcount(), 0) == -1) {
            if (Environment::itr == itr) { Environment::Wnacks += 1; }
            return false;
        }
        file.read(buffer, bufferSize);
    }
    close(socketFd);
    if (Environment::itr == itr) { Environment::Wacks += 1; }
    return true;
}

/**
 * Puts or Write a file on the SDFS. The file is replicated on 4 nodes, including one local copy.
 * Once at least three of the nodes acknowledge that the write is complete, the write is deemed successful
 * @param localfilename - the name of the local file to write
 * @param sdfsfilename - the name the file should have in SFDS
 * @return true if the write was sucessful or false if not 
*/
bool putFile(const std::string& localfilename, const std::string& sdfsfilename) {
    Environment::itr++;
    Environment::Wacks = 0;
    Environment::Wnacks = 0;

    if (Environment::hostName == getMaster()) {
        std::string fileID = sdfsfilename + ":" + 
                            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::vector<std::string> replicateHosts = selectWhereToReplicate(Environment::hostName, sdfsfilename);
        std::vector<std::jthread> threads;
        for (const std::string& repl: replicateHosts) {
            if (repl == Environment::hostName) {
                addLocalToSDFS(localfilename, fileID);
                Environment::Wacks++;
            } else {
                threads.emplace_back(replicateFile, localfilename, fileID, repl, (int) Environment::itr);
            }
        }

        // wait until we receive at least three positive acknowledgements or two negative acknowledgements
        while (Environment::Wacks < 3 && Environment::Wnacks < 2) { } 

        // if we received at least three positive acknowledgements add the file to the metadata 
        if (Environment::Wacks >= 3) {
            for (const std::string& rep: replicateHosts) {
                Environment::mapFilesToReps[sdfsfilename].insert(rep);
            }
            return true;
        }
        return false;
    }


    struct addrinfo hints, *servinfo;;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (int status = getaddrinfo(getMaster().c_str(), PORT, &hints, &servinfo)) {
        std::cerr << "Error. Master may have not been elected yet." << gai_strerror(status) << std::endl;
        return false;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        return false;
    }

    std::string message = "P\n" + Environment::hostName + "\n"+ sdfsfilename + "\n";
    if (send(socketFd, message.c_str(), message.size(), 0) == -1) {
        return false;
    }

    int bufferSize = 1024;
    char buffer[bufferSize];
    int numBytes = 0;
    std::string response;
    while ((numBytes = recv(socketFd, buffer, bufferSize, 0)) > 0) { // receiving new line separated hostnames
        buffer[numBytes] = '\0';
        response += buffer;
    }
    if (response.size() < 1) {
        std::cerr << "Unable to receive response from Master" << std::endl;
        close(socketFd);
        return false;
    }

    std::string line;
    std::stringstream resStream(response);
    std::getline(resStream, line);
    std::string fileID = line;
    std::vector<std::jthread> threads;
    while (std::getline(resStream, line)) {
        if (line == Environment::hostName) {
            addLocalToSDFS(localfilename, fileID);
            Environment::Wacks++;
        } else {
            threads.emplace_back(replicateFile, localfilename, fileID, line, (int) Environment::itr);
        }
    }

    // wait until we receive at least three positive acknowledgements or two negative acknowledgements
    while (Environment::Wacks < 3 && Environment::Wnacks < 2) { }

    // if we received at least three positive acknowledgements send a message back to the master indicating the put was successful
    if (Environment::Wacks >= 3) {
        send(socketFd, "PA", 2, 0);
        close(socketFd);
        return true;
    } else {
        close(socketFd);
        return false;
    }
}

/**
 * Fetches a file from a given node. Unless, there is only one copy of the file in HDFS
 * two nodes must respond to the fetch request for it to be successful. The node that reponded back 
 * with the latest timestamp of its last update to the file will be asked to continue sending the entire file.
 * If two nodes repond back with the same timestamp, a node holding a replicate on the local machine will be preferred
 * @param sdfsfilename - the name of the file in SDFS to fetch
 * @param localfilename - the name of the local file to write the fetched file into
 * @param hostName - the host name of the server we are fetching the file from
 * @param numVersions - number of versions of the file to get
 * @param responses - vector that store a pair containing (timestamp of write, if the replicate is local) 
 * of nodes that reponded to the fetch request
 * @param Rinstances - number of active fetch request to nodes that were sent out
 * @param successfulRead - set to true if the fetch was successful or false if not
 * @param isSingle - true if there is only one copy of the file in SDFS.
*/
void fetchFile(const std::string& sdfsfilename, const std::string& localfilename, const std::string& hostName, int numVersions, 
    std::vector<std::pair<long long, bool>>* responses, std::atomic<int>* Rinstances, std::atomic<bool>* successfulRead, bool isSingle) {
    if (hostName == Environment::hostName) {
        Environment::fetchMutex.lock();
        if (responses->size() == 2) {
            Environment::fetchMutex.unlock();
            return;
        }
        int curPos = responses->size();
        responses->push_back(std::make_pair(Environment::mapFilesToLoc[sdfsfilename].back().first, true));
        Environment::fetchMutex.unlock();
        
        // wait until there two responses from nodes or there is only one active fetch request  
        while (responses->size() != 2 && *Rinstances > 1) {}
        // if there was only one response, it is only allowed to continue if there is only one copy of the file in SDFS
        if (*Rinstances == 1) {
            if (!isSingle) {
                return;
            }
        } else if (responses->at(curPos).first < responses->at(curPos ^ 1).first) {
            return;
        }
        if (numVersions == 1 || Environment::mapFilesToLoc[sdfsfilename].size() == 1) {
            std::filesystem::copy_file(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[sdfsfilename].back().second, 
                localfilename, std::filesystem::copy_options::overwrite_existing);
        } else {
            std::ofstream file(localfilename);
            int bufferSize = 1024;
            char buffer[bufferSize];
            int delimSize = 100;
            std::string delim(delimSize, '-');

            for (int i = std::min(numVersions, (int) Environment::mapFilesToLoc[sdfsfilename].size()) - 1; i >= 0; i--) {
                std::ifstream inputFile(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[sdfsfilename][i].second);
                inputFile.read(buffer, bufferSize);
                while (inputFile.gcount()) {
                    file.write(buffer, inputFile.gcount());
                    inputFile.read(buffer, bufferSize);
                }
                file.write(("\n" + delim + "\n").c_str(), delimSize + 2);
            }
        }
        
    } else {
        struct addrinfo hints, *servinfo;;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_STREAM; // TCP

        if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
            std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
            (*Rinstances)--;
            return;
        }

        int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
            close(socketFd);
            (*Rinstances)--;
            return;
        }
        std::string message = "F\n" + sdfsfilename + "\n" + std::to_string(numVersions) + "\n";
        if (send(socketFd, message.c_str(), message.size(), 0) == -1) {
            close(socketFd);
            (*Rinstances)--;
            return;
        }

        int bufferSize = 1024;
        char buffer[bufferSize];
        int bytesRead = 0;
        bytesRead = recv(socketFd, buffer, bufferSize, 0);
        if (bytesRead <= 0) {
            close(socketFd);
            (*Rinstances)--;
            return;
        }
        buffer[bytesRead] = '\0';
        std::string response = buffer;
        long long timestamp = std::stol(response);

        Environment::fetchMutex.lock();
        if (responses->size() == 2) {
            Environment::fetchMutex.unlock();
            close(socketFd);
            return;
        }
        int curPos = responses->size();
        responses->push_back(std::make_pair(timestamp, false));
        Environment::fetchMutex.unlock();

        // wait until there two responses from nodes or there is only one active fetch request  
        while (responses->size() != 2 && *Rinstances > 1) {}
        // if there was only one response, it is only allowed to continue if there is only one copy of the file in SDFS
        if (*Rinstances == 1) {
            if (!isSingle) {
                close(socketFd);
                return;
            }
        } else if (responses->at(curPos).first <= responses->at(curPos ^ 1).first) {
            // Chosen var is only for the case when the two timestamps are equal, let the second element
            // decide who has the true variable If the second elements boolean == 0, then always let first element go,
            // otherwise it means that the second element has true and thus Chosen will chose it
            int chosenPos = responses->at(1).second;
            if (responses->at(curPos).first < responses->at(curPos ^ 1).first) { return; }
            if (chosenPos != curPos ) { return; }
        }
      
        if (send(socketFd, "FA", 2, 0) == -1) {
            close(socketFd);
            return;
        }

        if (numVersions == -1) {
            bytesRead = 0;
            while (bytesRead != bufferSize) {
                int read = recv(socketFd, buffer, bufferSize - bytesRead, 0); // reading header
                if (read <= 0) {
                    close(socketFd);
                    return;
                }
                bytesRead += read;
            }

            std::string header = buffer;
            std::string line;
            std::vector<std::string> files;
            std::vector<unsigned long> fileSizes;
            std::stringstream headerStream(header);
            while (std::getline(headerStream, line)) {
                if (line == "END") {
                    break;
                }
                files.push_back(line);
                std::getline(headerStream, line);
                fileSizes.push_back(std::stoul(line));
            }

            for (int i = 0; i < files.size(); i++) {
                std::ofstream file(SDFS_FILE_DIRECTORY + files[i]);
                bytesRead = 0;
                while (bytesRead != fileSizes[i]) {
                    int read = recv(socketFd, buffer, std::min(bufferSize, (int)(fileSizes[i] - bytesRead)), 0);
                    if (read <= 0) {
                        close(socketFd);
                        return;
                    }
                    file.write(buffer, read);
                    bytesRead += read;
                }
                long long timeStamp = std::stoll(files[i].substr(files[i].find(":") + 1));
                Environment::mapFilesToLoc[sdfsfilename].push_back(std::make_pair(timeStamp, files[i]));
            }
        } else {
            std::ofstream file(localfilename);
            while ((bytesRead = recv(socketFd, buffer, bufferSize, 0)) > 0) {
                file.write(buffer, bytesRead);
            }
            if (bytesRead == -1) {
                close(socketFd);
                return;
            }
        }
    }
    logToFile("[GET] " + sdfsfilename + " from " + hostName);
    *successfulRead = true;
    return;
}

/**
 * Puts or Write a file on the SDFS. The file is replicated on 4 nodes, including one local copy.
 * Once at least three of the nodes acknowledge that the write is complete, the write is deemed successful
 * @param sdfsfilename - the name the file stored in SFDS to get
 * @param localfilename - the name of the local file in which to write to
 * @param repLocs - a new-line delimited string containing the host names of the nodes holding replicates of the file
 * @param numVersions - number of versions of the file to get. If numVersions > 1 then the versions will be delimited by dashes
 * and the most recent versions will appear first. If numVersions = -1, then all the versions will be written individually to 
 * the SDFS directory rather than the local file.
 * @return true if the read was successful or false if not 
*/
bool getFile(const std::string& sdfsfilename, const std::string& localfilename, const std::string& repLocs, int numVersions = 1) {
    if (repLocs == "") {
        return false;
    }

    std::string line;
    std::stringstream resStream(repLocs);
    std::vector<std::string> reps;
    while (std::getline(resStream, line)) {
        if (line == "DNE") {
            return false;
        }
        reps.push_back(line);
    }

    std::vector<std::jthread> threads;
    std::vector<std::pair<long long, bool>> responses;
    std::atomic<int> Rinstances = reps.size();
    bool isSingle = (Rinstances == 1);
    std::atomic<bool> successfulRead = false;

    for (const std::string& rep: reps) {
        threads.emplace_back(fetchFile, sdfsfilename, localfilename, rep, numVersions, &responses, &Rinstances, &successfulRead, isSingle);
    }

    for (std::jthread& thread: threads) {
        thread.join();
    }
    return successfulRead; 
}

/**
 * Sends a message to a node storing a file asking for it to be deleted
 * @param sdfsfilename - the sdfs file name of the file to be deleted
 * @param hostName - the host name of the node which is being asked to delete the file
*/
bool requestToDelete(const std::string& sdfsfilename, const std::string& hostName) {
    if (hostName == Environment::hostName) {
        if (Environment::mapFilesToLoc.contains(sdfsfilename)) {
            for (const auto& file: Environment::mapFilesToLoc[sdfsfilename]) {
                std::filesystem::remove(SDFS_FILE_DIRECTORY + file.second);
            }
            Environment::mapFilesToLoc.erase(sdfsfilename);
        }
        if (hostName == getMaster() && Environment::mapFilesToReps.contains(sdfsfilename)) {
            Environment::mapFilesToReps.erase(sdfsfilename);
        }
        logToFile("[DELETE] " + sdfsfilename);
    } else {
        struct addrinfo hints, *servinfo;;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_STREAM; // TCP

        if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
            std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
            return false;
        }

        int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
            close(socketFd);
            return false;
        }

        std::string message = "D\n" + sdfsfilename + "\n";
        if (send(socketFd, message.c_str(), message.size(), 0) == -1) {
            close(socketFd);
            return false;
        }
        close(socketFd);
    }
    return true;
}

/**
 * Deletes a file from SDFS by sending messages to all files containg the replicate
 * requesting that they delete the file and sending a message to the master node
 * requesting that it deletes the file from its metadata.
 * @param sdfsfilename - the sdfs file name of the file to be deleted
 * @return whether the file was successfully deleted
*/
bool deleteFile(const std::string& sdfsfilename) {
    std::string repLocs = getReplicateLocations(sdfsfilename);
    if (repLocs == "") {
        return false;
    }

    std::string line;
    std::stringstream resStream(repLocs);
    std::vector<std::jthread> threads;
    bool removeFromMaster = false;
    while (std::getline(resStream, line)) {
        if (line == "DNE") {
            return false;
        }
        threads.emplace_back(requestToDelete, sdfsfilename, line);
        if (line == getMaster()) {
            removeFromMaster = true;
        }
    }
    if (!removeFromMaster) { //call requestToDelete on master even if it was not part of replicate locations to remove the file from metadata
        threads.emplace_back(requestToDelete, sdfsfilename, getMaster());
    }
    for (std::jthread& thread: threads) {
        thread.join();
    }
    return true;
}

/**
 * Sends a message to a node containg the file name and 
 * locations of where it is stored so that the node can duplicate that file 
 * @param file - the file to duplicate
 * @param hostName - the host name of the node where the file should be duplicated
*/
void requestToDuplicate(const std::string& file, const std::string& hostName) {
    std::string fileLocs;
    for (const std::string& fileLoc: Environment::mapFilesToReps[file]) {
        fileLocs += (fileLoc + "\n");
    }
    if (Environment::hostName == hostName) {
        if (getFile(file, "", fileLocs, -1)) {
            Environment::mapFilesToReps[file].insert(hostName);
        }
    } else {
        struct addrinfo hints, *servinfo;;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_STREAM; // TCP

        if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
            std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
            return;
        }

        int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
            close(socketFd);
            return;
        }
        
        std::string message = "T\n" + file + "\n" + fileLocs;
        send(socketFd, message.c_str(), message.size(), 0); // Sending Dupe message
        shutdown(socketFd, SHUT_WR);

        char buffer[3];
        int bytesRead = recv(socketFd, buffer, 3, 0);
        if (bytesRead > 0) {
            buffer[bytesRead] = 0;
            if (strcmp(buffer, "TA") == 0) {
                Environment::mapFilesToReps[file].insert(hostName);
            }
        } 
        close(socketFd);
    }
}

/**
 * Find nodes where the file can be duplicted so that 
 * there are four replicates of the file in the system
 * and sends messages that request nodes to duplicate a file 
 * @param file - the file to duplicate
*/
void duplicateFile(const std::string& file) {
    Environment::dupMutex.lock();
    if (Environment::mapFilesToReps[file].size() == 4) {
        Environment::dupMutex.unlock();
        return;
    } 

    std::vector<std::string> randMembers;
    std::vector<std::string> newReplicates;
    std::sample(Environment::membershipList.begin(), Environment::membershipList.end(), std::back_inserter(randMembers), 4, std::mt19937{std::random_device{}()});
    for (const std::string& member : randMembers) {
        std::string ip = member.substr(0, member.find(":"));
        if (Environment::mapFilesToReps[file].contains(ip)) {
            continue;
        }
        newReplicates.push_back(ip);
        if (newReplicates.size() + Environment::mapFilesToReps[file].size() == 4) {
            break;
        }
    }

    std::vector<std::jthread> threads;
    for (const std::string& repl: newReplicates) {
        threads.emplace_back(requestToDuplicate, file, repl);
        logToFile("[REPLICATION] " + file + " to " + repl);
    }

    for (std::jthread& thread: threads) {
        thread.join();
    }
    Environment::dupMutex.unlock();
    // wait for 3 seconds and retry duplicateFile in case a node went down while duplicating the first time
    // which caused there to be less still than 4 replicas of a file in the system
    std::this_thread::sleep_for(std::chrono::seconds(3));
    duplicateFile(file);
    return;
}

/**
 * Repairs the replicates of a file by iterating throught the metadata to find
 * files that have less than 4 replicates stored of it and then sending 
 * requests to duplicate that file on other nodes
*/
void repairReplicates() {
    for (const auto &kv: Environment::mapFilesToReps) {
        if (kv.second.size() < 4) {
            duplicateFile(kv.first);
        }
    }
}

/**
 * Sends a message to a node asking it what files it has stored in its sdfs directory
 * and then updates metadata (mapFilesToReps) from that information
 * @param hostName - the host name of the node that is being asked for file information
*/
void askForFiles(const std::string& hostName) {
    if (hostName == Environment::hostName) {
        for (const auto &kv: Environment::mapFilesToLoc) {
            Environment::mapFilesToReps[kv.first].insert(hostName);
        }
    } else {
        struct addrinfo hints, *servinfo;;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;       // IPv4
        hints.ai_socktype = SOCK_STREAM; // TCP

        if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
            std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
            return;
        }

        int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
            close(socketFd);
            return;
        }

        std::string message = "L\n";
        send(socketFd, message.c_str(), message.size(), 0); // Sending list message with local hostname
        shutdown(socketFd, SHUT_WR);

        int bufferSize = 1024;
        char buffer[bufferSize];
        int numBytes = 0;
        std::string response;
        while ((numBytes = recv(socketFd, buffer, bufferSize, 0)) > 0) { // receiving new line separated files
            buffer[numBytes] = '\0';
            response += buffer;
        }
        if (response.size() < 1) {
            std::cerr << "Unable to receive response" << std::endl;
            close(socketFd);
            return;
        }

        std::string line;
        std::stringstream resStream(response);
        while (std::getline(resStream, line)) {
            if (line == "NONE") {
                break;
            }
            Environment::mapFilesToReps[line].insert(hostName);
        }
    }
    return;
}

/**
 * Sends a TCP message to a server indicating that the election is complete. 
 * @param hostName - the hostName of the server to send the message to
*/
void sendElectionComplete(const std::string& hostName) {
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (int status = getaddrinfo(hostName.c_str(), PORT, &hints, &servinfo)) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << std::endl;
        return;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        return;
    }

    std::string message = "W\n";
    send(socketFd, message.c_str(), message.size(), 0); // Sending Win Election Message
    close(socketFd);
    return;
}

/**
 * Starts an election by first sending a message to every node in the group asking
 * for what files it stores in order to rebuild the metadata (mapFilesToReps)
 * Then repairs replicates and sends a message to all of the nodes letting them know
 * the introducer has been re-elected and that they can resume file operations
*/
void startElection() {
    std::vector<std::jthread> threads;
    for (const std::string& member: Environment::membershipList) {
        std::string ip = member.substr(0, member.find(":"));
        threads.emplace_back(askForFiles, ip);
    }

    for (std::jthread& thread: threads) {
        thread.join();
    }
    threads.clear();
    repairReplicates();

    for (const std::string& member: Environment::membershipList) {
        std::string ip = member.substr(0, member.find(":"));
        threads.emplace_back(sendElectionComplete, ip);
    }
    Environment::masterElected = true;

    for (std::jthread& thread: threads) {
        thread.join();
    }
}

/**
 * Pings neighbor node every 5 seconds to check whether they are up
 * if a node fails the master replicates the files it stored on to other nodes
 * if the master fails, the next node at the start of the membership list starts an election 
 * @param nodeToPing - Relative position of the iᵗʰ neighbor node in the membership list/ring
 * 0 = neighbor, 1 = neighbor+1, 2 = neighbor+2
*/
void pingNodes(int nodeToPing) {
    while (1) {
        std::unique_lock<std::mutex> lck(Environment::listMutex);
        while (Environment::membershipList.size() <= nodeToPing + 1) { // if membership list too small for neighbor to exist, wait
            Environment::pingCV.wait(lck);
            lck.unlock();
            // wait for five seconds before starting to ping. 
            // This helps reduce false positive since by not waiting we might ping a node before it is ready to respond.
            std::this_thread::sleep_for(std::chrono::seconds(5)); 
            lck.lock();
        }

        auto toPingItr = Environment::mapHosttoItr[Environment::membershipID];
        for (int i = 0; i <= nodeToPing; i++) { // if end of membership list reached before obtaining neighbor, loop around
            toPingItr++;
            if (toPingItr == Environment::membershipList.end()) {
                toPingItr = Environment::membershipList.begin();
            }
        }

        std::string toPingStr = *toPingItr;
        lck.unlock();
        std::string ip = toPingStr.substr(0, toPingStr.find(":"));
        // Send ping message three times for extra reliability in case a UDP message was lost
        sendUDPMessage("P\n" + std::to_string(nodeToPing), ip);
        sendUDPMessage("P\n" + std::to_string(nodeToPing), ip);
        sendUDPMessage("P\n" + std::to_string(nodeToPing), ip);

        std::this_thread::sleep_for(std::chrono::seconds(5)); // wait 5 second to receive ack from node that is pinged
        if (!Environment::recievdAck[nodeToPing]) {
            lck.lock();
            if (Environment::mapHosttoItr.contains(toPingStr)) { // make sure the member still exists in local membership list
                if (toPingStr == Environment::membershipList.front()) {
                    Environment::masterElected = false;
                }
                Environment::membershipList.erase(toPingItr);
                Environment::mapHosttoItr.erase(toPingStr);
                lck.unlock();
                if (Environment::hostName == getMaster()) {
                    for (auto& kv: Environment::mapFilesToReps) { // find files that the node the failed was holding and duplicate them on other nodes
                        if (kv.second.contains(ip)) {
                            kv.second.erase(ip);
                            std::jthread duplicationThread(duplicateFile, kv.first);
                            duplicationThread.detach();
                        }
                    }
                } else if (Environment::membershipID == Environment::membershipList.front()) {
                    std::jthread electionThread(startElection); // the node at the start of the membership list starts the election
                    electionThread.detach();
                }
                logToFile("Did not recieve acknowledgement from " + toPingStr + " Sending failure messages");
                sendUDPMessageToAll("F\n" + toPingStr); // Sends a fail message with the membership ID of the node that did not respond
                logToFile("[FAILURE] " + toPingStr);
            } else {
                lck.unlock();
            }
        }
        Environment::recievdAck[nodeToPing] = false; // reset status of neighbor acknowledgement
    }
}

/**
 * Listens for UDP™ messages from other nodes in membership group and processes them
*/
void listenForUDPMessages() {
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
            Environment::listMutex.lock();
            Environment::membershipList.push_back(line);
            Environment::mapHosttoItr[line] = std::prev(Environment::membershipList.end());
            Environment::pingCV.notify_all();
            Environment::listMutex.unlock();
            logToFile("[JOIN] " + line);
        } else if (line == "L" || line == "F") { //Received {L||F}'\n'MembershipID, remove MembershipID from local membership list
            bool isLeave = (line == "L");
            std::getline(recStream, line);
            Environment::listMutex.lock();
            if (Environment::mapHosttoItr.contains(line)) {
                if (line == Environment::membershipList.front()) {
                    Environment::masterElected = false;
                }
                Environment::membershipList.erase(Environment::mapHosttoItr[line]);
                Environment::mapHosttoItr.erase(line);
                Environment::listMutex.unlock();
                if (isLeave) {
                    logToFile("[LEAVE] " + line);
                } else {
                    logToFile("[FAILURE] " + line);
                }
                if (Environment::hostName == getMaster()) {
                    std::string ip = line.substr(0, line.find(":"));
                    for (auto& kv: Environment::mapFilesToReps) {
                        if (kv.second.contains(ip)) {
                            kv.second.erase(ip);
                            std::jthread duplicationThread(duplicateFile, kv.first);
                            duplicationThread.detach();
                        }
                    }
                } else if (Environment::membershipID == Environment::membershipList.front()) {
                    std::jthread electionThread(startElection);
                    electionThread.detach();
                }
            } else {
                Environment::listMutex.unlock();
            }
        } else if (line == "P") { // Received P'\n'nodeToPing, send ack message back to sender
            std::getline(recStream, line);
            // Send three acknowledgments for extra reliability in case a UDP message was lost
            sendUDPMessage("A\n" + line, inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));
            sendUDPMessage("A\n" + line, inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));
            sendUDPMessage("A\n" + line, inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));
        } else if (line == "A") { // Received A'\n'nodeToPing, set received ack'ed to true
            std::getline(recStream, line);
            Environment::recievdAck[stoi(line)] = true;
        }
    }
    return;
}

/**
 * Processes TCP messages from other nodes
 * @param clientFd - the file descriptor of the node that is being communicated with though TCP
*/
void processTCPMessage(int clientFd) {
    int bytesRead;
    int bufferSize = 1024;
    char buffer[bufferSize];
    std::string message;

    bytesRead = recv(clientFd, buffer, bufferSize, 0);
    buffer[bytesRead] = '\0';
    message += buffer;

    std::stringstream recStream(message);
    std::string line;
    std::getline(recStream, line);
    if (line == "J") { // Process Join message
        std::getline(recStream, line);
        // construct membership id for joining node with format hostname:{TIME_STAMP}
        std::string id = line + ":" + 
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); 
        Environment::listMutex.lock();
        Environment::membershipList.push_back(id); // add the joining node to local membership list
        Environment::mapHosttoItr[id] = std::prev(Environment::membershipList.end());
        for (const std::string& member : Environment::membershipList) { //sending the joining node the membership list separated by new lines
            std::string to_send = member+"\n";
            send(clientFd, to_send.c_str(), to_send.size(), 0);
        }
        Environment::pingCV.notify_all(); // Notify the pinging threads to start pinging neighbor nodes if possible
        Environment::listMutex.unlock();
        logToFile("[JOIN] " + id);
    } else if (line == "P") { // Process Put message
        std::getline(recStream, line); // hostname
        std::string hostName = line;
        std::getline(recStream, line); // filename
        std::string filename = line;
        std::string fileID = filename + ":" + 
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::vector<std::string> replicateHosts = selectWhereToReplicate(hostName, filename);
        std::string replicateHostsStr = fileID + "\n";
        for (const std::string& rep: replicateHosts) {
            replicateHostsStr += (rep + "\n");
        }
        if (send(clientFd, replicateHostsStr.c_str(), replicateHostsStr.size(), 0) < 0) {
            close(clientFd);
            return;
        }
        shutdown(clientFd, SHUT_WR);
        bytesRead = recv(clientFd, buffer, bufferSize, 0);
        if (bytesRead != -1) {
            buffer[bytesRead] = '\0';
            if (strcmp(buffer, "PA") == 0) {
                for (const std::string& rep: replicateHosts) {
                    Environment::mapFilesToReps[filename].insert(rep);
                }
            }
        }
    } else if (line == "R") { // Process replicate Message
        std::getline(recStream, line); // fileId
        std::string fileId = line;
        std::string fileName = fileId.substr(0, fileId.find(":"));
        long long timeStamp = std::stoll(fileId.substr(fileId.find(":") + 1));
        std::ofstream file(SDFS_FILE_DIRECTORY + fileId);

        recStream.read(buffer, bufferSize);
        if (recStream.gcount()) {
            file.write(buffer, recStream.gcount());
        }
        while ((bytesRead = recv(clientFd, buffer, bufferSize, 0)) > 0) {
            file.write(buffer, bytesRead);
        }
        Environment::mapFilesToLoc[fileName].emplace_back(std::make_pair(timeStamp, fileId));
        std::sort(Environment::mapFilesToLoc[fileName].begin(), Environment::mapFilesToLoc[fileName].end());
        if (Environment::mapFilesToLoc[fileName].size() > 5) {
            std::filesystem::remove(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[fileName].front().second);
            Environment::mapFilesToLoc[fileName].pop_front();
        }
        logToFile("[PUT] " + fileName);
    } else if (line == "G") { // Process Get Replicate Locations message
        std::getline(recStream, line); // filename
        std::string filename = line;
        std::string response = "";
        if (Environment::mapFilesToReps.contains(filename) && Environment::mapFilesToReps[filename].size() >= 2) {
            for (const std::string& host: Environment::mapFilesToReps[filename]) {
                response += (host + "\n");
            }
        } else {
            response += (std::string("DNE") + "\n");
        }
        send(clientFd, response.c_str(), response.size(), 0);
    } else if (line == "F") { // Process Fetch message
        std::getline(recStream, line); // filename
        std::string filename = line;
        std::getline(recStream, line); // num versions
        int numVersions = std::stoi(line);

        std::string timestamp = std::to_string(Environment::mapFilesToLoc[filename].back().first);
        send(clientFd, timestamp.c_str(), timestamp.size(), 0);
        bytesRead = recv(clientFd, buffer, bufferSize, 0);
        if (bytesRead != -1) {
            buffer[bytesRead] = '\0';
            if (strcmp(buffer, "FA") == 0) {
                if (numVersions == -1) { // if numVersions = -1 then all version of the file will be sent
                    memset(buffer, 0, bufferSize);
                    // send a header containing filenames and size of all the versions of a file
                    std::string header;
                    for (int i = 0; i < Environment::mapFilesToLoc[filename].size(); i++) {
                        header += (Environment::mapFilesToLoc[filename][i].second + "\n" + std::to_string(std::filesystem::file_size(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[filename][i].second)) + "\n");
                    }
                    header += "END\n";
                    strcpy(buffer, header.c_str());
                    send(clientFd, buffer, bufferSize, 0);

                    for (int i = 0; i < Environment::mapFilesToLoc[filename].size(); i++) {
                        std::ifstream file(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[filename][i].second);
                        file.read(buffer, bufferSize);
                        while (file.gcount()) {
                            send(clientFd, buffer, file.gcount(), 0);
                            file.read(buffer, bufferSize);
                        }
                    }
                } else {
                    int delimSize = 100;
                    std::string delim(delimSize, '-');
                    for (int i = std::min(numVersions, (int) Environment::mapFilesToLoc[filename].size()) - 1; i >= 0; i--) {
                        std::ifstream file(SDFS_FILE_DIRECTORY + Environment::mapFilesToLoc[filename][i].second);
                        file.read(buffer, bufferSize);
                        while (file.gcount()) {
                            send(clientFd, buffer, file.gcount(), 0);
                            file.read(buffer, bufferSize);
                        }
                        if (numVersions != 1 && Environment::mapFilesToLoc[filename].size() != 1) {
                            send(clientFd, ("\n" + delim + "\n").c_str(), delimSize + 2, 0);
                        }
                    }
                }
            }
        }
    } else if (line == "D") { // Process Delete Message
        std::getline(recStream, line); // filename
        std::string filename = line;
        if (Environment::mapFilesToLoc.contains(filename)) {
            for (const auto& file: Environment::mapFilesToLoc[filename]) {
                std::filesystem::remove(SDFS_FILE_DIRECTORY + file.second);
            }
            Environment::mapFilesToLoc.erase(filename);
        }
        if (Environment::hostName == getMaster() && Environment::mapFilesToReps.contains(filename)) {
            Environment::mapFilesToReps.erase(filename);
        }
        logToFile("[DELETE] " + filename);
    } else if (line == "L") { // Process LS message
        std::string res;
        if (Environment::mapFilesToLoc.size() > 0) {
            for (const auto &kv: Environment::mapFilesToLoc) {
                res += (kv.first + "\n");
            }
        } else {
            res = "NONE\n";
        }
        send(clientFd, res.c_str(), res.size(), 0);
    } else if (line == "T") { // Process Try to Duplicate message 
        std::getline(recStream, line);  // filename
        std::string filename = line;
        std::string fileLocs;
        while (std::getline(recStream, line)) {
            fileLocs += (line + "\n");
        }
        if (getFile(filename, "", fileLocs, -1)) {
            send(clientFd, "TA", 2, 0);
        }

    } else if (line == "W") { // Process Won Election Message
        Environment::masterElected = true;
    }

    close(clientFd);
}

/**
 * Listens for TCP messages from other nodes in membership group and processes them
*/
void listenForTCPMessages() {
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
    while (1) { // accept loop
        clientFd = accept(socketFd, NULL, NULL);
        std::jthread processTCPThread(processTCPMessage, clientFd);
        processTCPThread.detach();
    }
}

/**
 * Print out membership list
*/
void printMembershipList() {
    std::lock_guard<std::mutex> lock(Environment::listMutex);
    for (const std::string& member : Environment::membershipList) {
        std::cout << member << std::endl;
    }
}

/**
 * Reads commands from the user
*/
void readInput() {
    std::string input;
    std::jthread udpThread;
    std::jthread tcpThread;
    bool isListening = false;
    bool hasJoined = false;
    while (1) {
        getline(std::cin, input);
        if (input.size() == 0) {
            continue;
        }
        std::vector<std::string> inputVec = splitString(input);
        if (input == "join") {
            if (hasJoined) {
                std::cout << "Already Joined" << std::endl;
                continue;
            }
            if (!isListening) { // start threads to listen for introductions and listen for messages
                tcpThread = std::jthread(listenForTCPMessages);
                udpThread = std::jthread(listenForUDPMessages);
                std::cout << "Started Listening For Introductions & Group Messages" << std::endl;
                isListening = true;
            }
            logToFile("JOINING GROUP");
            if (joinGroup()) {
                hasJoined = true;
                Environment::masterElected = true;
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
                std::cout << Environment::membershipID << std::endl;
            }
        } else if (input == "store") {
            if (Environment::mapFilesToLoc.empty()) {
                std::cout << "No files stored" << std::endl;
            } else {
                for (const auto &kv : Environment::mapFilesToLoc) {
                    std::cout << kv.first << std::endl;
                }
            }
        } else if (inputVec[0] == "put") {
            if (inputVec.size() != 3) {
                std::cout << "Invalid. Usage: put localfilename sdfsfilename" << std::endl;
            } else if (!hasJoined) {
                std::cout << "Join in order to put files" << std::endl;
            } else if (Environment::membershipList.size() < 4) {
                std::cout << "There must be at least 4 nodes in the system to put files" << std::endl;
            } else if (putFile(inputVec[1], std::regex_replace(inputVec[2], std::regex("/"), "|"))) {
                std::cout << "Successfully stored file" << std::endl;
            } else {
                std::cout << "There was a problem. Try Again!" << std::endl;
            }
        } else if (inputVec[0] == "get") {
            if (inputVec.size() != 3) {
                std::cout << "Invalid. Usage: get sdfsfilename localfilename" << std::endl;
            } else if (!hasJoined) {
                std::cout << "Join in order to get files" << std::endl;
            } else if (getFile(std::regex_replace(inputVec[1], std::regex("/"), "|"), 
                inputVec[2], getReplicateLocations(std::regex_replace(inputVec[1], std::regex("/"), "|")))) {
                std::cout << "Successfully got file" << std::endl;
            } else {
                std::cout << "There was a problem or the file may not exist. Try Again!" << std::endl;
            }
        } else if (inputVec[0] == "delete") {
            if (inputVec.size() != 2) {
                std::cout << "Invalid. Usage: delete sdfsfilename" << std::endl;
            } else if (!hasJoined) {
                std::cout << "Join in order to delete files" << std::endl;
            } else if (deleteFile(std::regex_replace(inputVec[1], std::regex("/"), "|"))) {
                std::cout << "Successfully deleted file" << std::endl;
            } else {
                std::cout << "There was a problem or the file may not exist. Try Again!" << std::endl;
            }
        } else if (inputVec[0] == "ls") {
            if (inputVec.size() != 2) {
                std::cout << "Invalid. Usage: ls sdfsfilename" << std::endl;
                continue;
            } else if (!hasJoined) {
                std::cout << "Join in order to ls files" << std::endl;
                continue;
            }
            std::string sdfsfilename = std::regex_replace(inputVec[1], std::regex("/"), "|");
            std::string repLocs = getReplicateLocations(sdfsfilename);
            if (repLocs == "") {
                std::cout << "There was a problem. Try Again!" << std::endl;
            } else if (repLocs == "DNE\n") {
                std::cout << "The file does not exist!" << std::endl;
            } else {
                std::cout << repLocs;
            }
        } else if (inputVec[0] == "get-versions") {
            if (inputVec.size() != 4) {
                std::cout << "Invalid. Usage: get-versions sdfsfilename numversions localfilename" << std::endl;
            } else if (!hasJoined) {
                std::cout << "Join in order to get files" << std::endl;
            } else {
                int numVersions;
                try {
                    numVersions = std::stoi(inputVec[2]);
                    if (numVersions < 1 || numVersions > 5) {
                        throw std::invalid_argument("");
                    }
                } catch (...) {
                    std::cout << "Invalid. Usage: get-versions sdfsfilename numversions localfilename (where 1 <= numversions <= 5)" << std::endl;
                    continue;
                }
                std::string sdfsfilename = std::regex_replace(inputVec[1], std::regex("/"), "|");
                if (getFile(sdfsfilename, inputVec[3], getReplicateLocations(sdfsfilename), numVersions)) {
                    std::cout << "Successfully got file" << std::endl;
                } else {
                    std::cout << "There was a problem or the file may not exist. Try Again!" << std::endl;
                }
            }
        } else {
            std::cout << "Invalid. Try Again" << std::endl;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cout << "Usage ./sdfs_server [HOST_NAME] [LOG_FILE]" << std::endl;
        return 1;
    }
    Environment::hostName = argv[1];
    Environment::logFile = argv[2];

    std::filesystem::remove_all(SDFS_FILE_DIRECTORY);
    std::filesystem::create_directory(SDFS_FILE_DIRECTORY);

    std::jthread inputThread(readInput); // start thread to read input
    std::vector<std::jthread> pingThreads;
    for (int i = 0; i < MAX_CONCURRENT_FAILURES; i++) {
        Environment::recievdAck[i] = false;
        pingThreads.emplace_back(pingNodes, i); // setup threads that will ping neighbors if possible
    }
    inputThread.join();
    quick_exit(0); // quick_exit to immediately kill all threads and exit the program
}
