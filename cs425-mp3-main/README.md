# CS425-MP3
Karan Sodhi (ksodhi2) & Nashwaan Ahmad (nahmad31)


## Compilation
To compile the project a version of clang supporting C++20 must be installed on the machine. Then simply run the following command. 
```
make
```
This will compile binaries for the sdfs_server, grep_server and grep_client.

## SDFS Server Usage 
To run the sdfs server run the following
```
./sdfs_server [HOST_NAME] [LOG_FILE]
```
For example, to run the sdfs_server on fa22-cs425-5301.cs.illinois.edu and log events on vm1.log run
```
./sdfs_server fa22-cs425-5301.cs.illinois.edu vm1.log
```

Then type "join" to join the distributed file system, "leave" to voluntarily leave the distributed file system, "list_mem" to print out the membership list, or "list_self" to print out the node's membership ID.

Note: You are unable to join the distributed file system if the introducer the dns.config file is set to has not already joined the group. Currently the dns.config file is set to "fa22-cs425-5301.cs.illinois.edu".

## File Operations
To put/update a file in the system:
```
put [localfilename] [sdfsfilename]
```
To get a file:
```
get [sdfsfilename] [localfilename]
```
To get multiple versions of a file:
```
get-versions [sdfsfilename] [numversions] [localfilename]
```
To delete a file:
```
delete [sdfsfilename]
```
To list all machine (VM) addresses where a file is currently being stored:
```
ls [sdfsfilename]
```
To list all files currently being stored at the machine:
```
store
```

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
