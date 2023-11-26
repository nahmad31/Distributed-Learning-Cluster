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
#include <syncstream>
#include <thread>
#include <fstream>
#include <vector>
#include <unordered_set>

#define PORT "13337" // The port client connects to
#define CONFIG_FILE "ip.config" // file that stores machine ip addresses and status
#define MAX_MACHINES 100

struct serverRecords { // Struct that holds ip and status from config file
    serverRecords(std::string ip_, bool isEnabled_) {
        ip = ip_;
        isEnabled = isEnabled_;
    }
    std::string ip;
    bool isEnabled;
};

/**
 * Reads and parses the config file into a vector of serverRecord structs
 * @param configFile - File name of the config file
 * @return - Vector of server records
**/
std::vector<serverRecords> getServerRecords(const std::string& configFile) {
    std::vector<serverRecords> recs;

    std::ifstream infile(configFile);
    std::string line;
    while (std::getline(infile, line)) {
        int pos = line.find(":");
        std::string ip = line.substr(0, pos);
        bool isEnabled = std::stoi(line.substr(pos+1));
        recs.emplace_back(ip, isEnabled);
    }

    infile.close();
    return recs;
}

/**
 * Updates the config file with new server failures from retVals
 * @param configFile - config file
 * @param retVals - Pointer containing server statuses, if ith value is -1 then that machine has failed. 
**/
void writeFailures(const std::string& configFile, int* retVals) {
    std::fstream outfile(configFile);

    std::string line;
    int lineNum = 0;
    while (std::getline(outfile, line)) {
        if (retVals[lineNum] == -1) {
            long pos = outfile.tellp();
            outfile.seekp(pos - 2); 
            outfile.write("0", 1);
            outfile.seekp(pos); 
        }
        lineNum++;
    }
    outfile.close();
}

/**
 * Connects to server and sends the given message
 * @param ipAddr - Server ip adress
 * @param message - Message to send to server to either grep or test
 * @param id - server id, corresponds to server position in config file
 * @param retVals - Array to either store line counts of grep or -1, id determines position in retVals
 * @param getLineCount - True if grep is called with the -c option 
 * @return - If message was successfully sent return 1 otherwise return 0
**/
int connectAndSend(const std::string& ipAddr, const std::string& message, int id, int* retVals, bool getLineCount) {
    // thread safe output stream to print to console
    std::osyncstream syncedOut(std::cout);
    
    struct addrinfo hints, *servinfo;;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(ipAddr.c_str(), PORT, &hints, &servinfo)) {
        syncedOut << "getaddrinfo failure for " << ipAddr << std::endl;
        retVals[id] = -1;
        return 0;
    }

    int socketFd = socket(AF_INET, SOCK_STREAM, 0);

    if (connect(socketFd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(socketFd);
        syncedOut << "Unable To Connect To " << ipAddr << std::endl;
        retVals[id] = -1;
        return 0;
    }

    send(socketFd, message.c_str(), message.size(), 0);
    shutdown(socketFd, SHUT_WR);

    int buffer_size = 1024;
    char buffer[buffer_size + 1];
    int numBytes = 0;
    int linecount = 0;
    while ((numBytes = recv(socketFd, buffer, buffer_size, 0)) > 0) {
        buffer[numBytes] = '\0';
        if (getLineCount) {
            linecount = atoi(strrchr(buffer, ':') + 1); // Parses the grep output, ex: machine.i.log:70, and gets 70 as linecount
        }
        syncedOut << buffer;
    }
    if (numBytes == -1) {
        syncedOut << "The Machine with ip: " << ipAddr << " may have failed and its results may be incomplete" << std::endl;
        close(socketFd);
        retVals[id] = -1;
        return 0;
    }

    close(socketFd);
    retVals[id] = linecount;
    return 1;

}

/**
 * Sends a message in parallel to all running machines
 * @param recs - Vector of serverRecords
 * @param message - The message
 * @param appendID - Whether or not to append the ID (Position of machine in recs) to the message
 * @param retVals - Array to store return values of each thread, will contain server statuses or linecounts
 * @param getLineCount - True if grep is called with the -c option
 * @return - True if all messages were successfully sent
**/
bool sendMessageToAllmachines(const std::vector<serverRecords>& recs, const std::string& message, bool appendID, int* retVals, bool getLineCount) {
    int id = 0;
    std::vector<std::thread> threads;
    for (auto& rec: recs) {
        if (rec.isEnabled) {
            if (appendID) {
                std::string messageWithId = message + std::to_string(id);
                threads.emplace_back(std::thread(connectAndSend, rec.ip, messageWithId, id++, retVals, getLineCount));
            } else {
                threads.emplace_back(std::thread(connectAndSend, rec.ip, message, id++, retVals, getLineCount));
            }
        } else {
            retVals[id++] = -2; // server was down before message was sent
        }
    }
    for (auto& th : threads) {
        th.join();
    }

    bool success = true;
    int linecount = 0;
    for (int i = 0; i < id; i++) {
        if (retVals[i] == -1) {
            success = false;
        } else if (retVals[i] > 0) {
            linecount += retVals[i];
        }
    }
    if (getLineCount) {
        std::cout << "Total Number of Matching Lines " << linecount << std::endl;
    }
    return success;
}

/**
 * The main testing library, which runs a specified grep query on all machines and compares it to the expected output
 * @param recs - Vector of Server Records
 * @param retVals - Array of server status of machines
 * @param grepQuery - The Query we are testing
 * @param getLineCount - True if grep is called with the -c option
 * @param expectedResultFile - File containing the sorted expected output of the grep query
 * @return - return 0 if tests fail and 1 otherwise
**/
int test(const std::vector<serverRecords>& recs, int* retVals, const std::string& grepQuery, bool getLineCount, const std::string& expectedResultFile) {
    std::stringstream buffer;
    std::streambuf* prevcoutbuf = std::cout.rdbuf(buffer.rdbuf()); // capturing printed output, to be stored in buffer
    if (!sendMessageToAllmachines(recs, "grep" + grepQuery, false, retVals, getLineCount)) {
        std::cout << "Failed To Get Results" << std::endl;
        return 0;
    }

    std::vector<std::string> printOut;
    std::string line;
    while (std::getline(buffer, line)) {
        printOut.push_back(line);
    }
    std::sort(printOut.begin(), printOut.end()); // Since the printed output could be in any order, sort output
    
    int i = 0;
    std::ifstream resFile(expectedResultFile);
    while (i < printOut.size() && std::getline(resFile, line)) {
        if (line != printOut[i]) {
            break;
        }
        i++;
    }

    resFile.close();
    std::cout.rdbuf(prevcoutbuf);

    return (i == printOut.size());
}

// Reads input which is either a grep command or test command and sends it to the machines
int main() {
    std::string input;
    getline(std::cin, input);
    std::stringstream inputStream(input);

    std::string command;
    getline(inputStream, command, ' ');

    std::string option;
    std::string allOptions;
    bool getLineCount = false;
    bool endFlags = false; // if -- appears then no more flags are going to appear

    while (getline(inputStream, option, ' ')) { // This loop is used to get the query and all the flags specified by the user, sets getLineCount if c flag specified
        if (!endFlags && option[0] == '-' && option.size() > 1) {
            if (option[1] != '-') {
                for (int j = 1; j < option.size(); j++) {
                    if (option[j] == 'c') {
                        getLineCount = true;
                    }
                }
            } else {
                if (option == "--") {
                    endFlags = true;
                }
            }
        }
        allOptions += option;
        allOptions += " ";
    }
    allOptions.pop_back(); // Gets rid of trailing space
    
    int retVals[MAX_MACHINES]; // -2 means server is down / not enabled, -1 means thread failure, other values corresspond to matching lines

    if (command == "grep") {
        std::vector<serverRecords> recs = getServerRecords(CONFIG_FILE);
        std::string message = command + allOptions;
        if (!sendMessageToAllmachines(recs, message, false, retVals, getLineCount)) {
            writeFailures(CONFIG_FILE, retVals);
        }
    } else if (command == "test") {
        std::unordered_set<std::string> validOptions{"1", "2", "3", "4", "5", "6", "all"}; // The numbers represent Test Cases, all selects all
        if (!validOptions.contains(allOptions)) {
            std::cout << "Use Valid Option" << std::endl;
            return 1;
        }

        std::vector<serverRecords> recs = getServerRecords(CONFIG_FILE);
        for (auto& rec: recs) {
            if (!rec.isEnabled) {
                std::cout << "One or More Machines Is Down. Restart Machines, Update config and Try Again." << std::endl;
                return 1;
            }
        }
        if (recs.size() != 10) {
            std::cout << "The Tests are only designed to work with 10 running servers. Update config and Try Again." << std::endl;
            return 1;
        }

        // Sending a message to machines to tell them to create test files 
        if (!sendMessageToAllmachines(recs, command, true, retVals, false)) {
            std::cout << "Failed To Create Logs on all Machines" << std::endl;
            return 1;
        }

        // Testing a frequent pattern
        if (allOptions == "all" || allOptions == "1") {
            if (!test(recs, retVals, "-c \"Generated Random Number\" test*", true, "expected_results/test1_expected.txt")) {
                std::cout << "Test One Failed!" << std::endl;
            } else {
                std::cout << "Test One Success!" << std::endl;
            }
        }

        // Testing a somewhat frequent pattern
        if (allOptions == "all" || allOptions == "2") {
            if (!test(recs, retVals, "-Ec \"[[A-Za-z]*]\" test*", true, "expected_results/test2_expected.txt")) {
                std::cout << "Test Two Failed!" << std::endl;
            } else {
                std::cout << "Test Two Success!" << std::endl;
            }
        }
        // Testing a rare pattern
        if (allOptions == "all" || allOptions == "3") {
            if (!test(recs, retVals, "-Ec \"WARNING|ERROR\" test*", true, "expected_results/test3_expected.txt")) {
                std::cout << "Test Three Failed!" << std::endl;
            } else {
                std::cout << "Test Three Success!" << std::endl;
            }
        }

        // Testing a pattern appears only in some logs
        if (allOptions == "all" || allOptions == "4") {
            if (!test(recs, retVals, "-Ec \"[[:alpha:]]{10,12}\" test*", true, "expected_results/test4_expected.txt")) {
                std::cout << "Test Four Failed!" << std::endl;
            } else {
                std::cout << "Test Four Success!" << std::endl;
            }
        }

        // Testing a pattern that does not appear in any logs
        if (allOptions == "all" || allOptions == "5") {
            if (!test(recs, retVals, "-Ec \"^[A-Z].*[.,]$\" test*", true, "expected_results/test5_expected.txt")) {
                std::cout << "Test Five Failed!" << std::endl;
            } else {
                std::cout << "Test Five Success!" << std::endl;
            }
        }

        // Testing a grep query that outputs lines instead of counts
        if (allOptions == "all" || allOptions == "6") {
            if (!test(recs, retVals, "Raspberry test*", false, "expected_results/test6_expected.txt")) {
                std::cout << "Test Six Failed!" << std::endl;
            } else {
                std::cout << "Test Six Success!" << std::endl;
            }
        }
        
    } else {
        std::cout << "Incorrect command supplied. Command should either start with grep or test" << std::endl;
    }

    return 0;
}
