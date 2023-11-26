# CS425-MP4
Karan Sodhi (ksodhi2) & Nashwaan Ahmad (nahmad31)


## Compilation
To compile the project a version of clang supporting C++20, cmake, and [tensorflow_cc](https://github.com/FloopCZ/tensorflow_cc) must be installed on the machine. Then simply run the following commands. 
```
mkdir build
cmake ..
make
```
This will compile binaries for the idunno_server, grep_server and grep_client.

## IDunno Server Usage 
To run the IDunno Server run the following
```
./idunno_server [HOST_NAME] [LOG_FILE]
```
For example, to run the idunno_server on fa22-cs425-5301.cs.illinois.edu and log events on vm1.log run
```
./idunno_server fa22-cs425-5301.cs.illinois.edu vm1.log
```

Then type "join" to join the distributed file system, "leave" to voluntarily leave the distributed file system, "list_mem" to print out the membership list, or "list_self" to print out the node's membership ID.

Note: You are unable to join the distributed file system if the introducer the dns.config file is set to has not already joined the group. Currently the dns.config file is set to "fa22-cs425-5301.cs.illinois.edu".

## IDunno Operations
To train a model (currently only [InceptionV3](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/examples/label_image) and [Multibox](https://github.com/tensorflow/tensorflow/tree/master/tensorflow/examples/multibox_detector) supported):
```
train [multibox/inception] [localmodelfilename] [localpriorsname]
```

To load-data:
```
load-data [multibox/inception] datadirectory
```

To start-job:
```
start-job [multibox/inception] batchsize(images per query)
```

To kill-job:
```
kill-job [multibox/inception]
```

To get Job Stats:
```
job-stats [multibox/inception]
```

To show results:
```
show-results [multibox/inception]
```

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
