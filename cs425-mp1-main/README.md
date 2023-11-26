# CS425-MP1
Karan Sodhi (ksodhi2) & Nashwaan Ahmad (nahmad31)


## Compilation
To compile the project a version of clang supporing C++20 must be installed on the machine. Then simply run the following command. 
```
make
```
This will compile binaries for both the client and server.

## Usage 
After compilation run the server program on all machines holding log files.
```
./server
```
Then run the client process on any one of the machines and type in a command into the terminal.

To run a grep query all files ending in .log:
```
./client
>> grep -c pattern *.log
```
Additionally, the client process allows you to run distributed unit tests.

To run all unit tests:
```
./client
>> test all
```

Note: If you wish to run the server processes on machines other than fa22-cs425-53**.cs.illinois.edu, make sure to update the ip.config file. 
Entries in the config file are line formatted as {ip_address}:{1 if server is running otherwise 0}.
