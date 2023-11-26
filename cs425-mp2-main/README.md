# CS425-MP2
Karan Sodhi (ksodhi2) & Nashwaan Ahmad (nahmad31)


## Compilation
To compile the project a version of clang supporting C++20 must be installed on the machine. Then simply run the following command. 
```
make
```
This will compile binaries for the membership_server, grep_server and grep_client.

## Membership Server Usage 
To run the membership server run the following
```
./membership_server [HOST_NAME] [LOG_FILE]
```
For example, to run the membership_server on fa22-cs425-5301.cs.illinois.edu and log events on vm1.log run
```
./membership_server fa22-cs425-5301.cs.illinois.edu vm1.log
```

Then type "join" to join the group, "leave" to voluntarily leave the group, "list_mem" to print out the membership list, or "list_self" to print out the node's membership ID.

Note: You are unable to join the group if the Primary Introducer has not already joined the group. Currently the Primary Introducer is set to "fa22-cs425-5301.cs.illinois.edu".

## Grep Usage 

Run the grep_server program on all machines holding log files.
```
./grep_server
```
Then run the client process on any one of the machines and type in a command into the terminal.

To run a grep query all files ending in .log:
```
./grep_client
>> grep -c pattern *.log

```
Note: If you wish to run the server processes on machines other than fa22-cs425-53**.cs.illinois.edu, make sure to update the ip.config file. 
Entries in the config file are line formatted as {ip_address}:{1 if server is running otherwise 0}.
