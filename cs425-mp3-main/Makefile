COMPILERFLAGS = -std=c++20 -g -Wall -Wextra -Wno-sign-compare -pthread

GREPSERVEROBJECTS = obj/grep_server.o
GREPCLIENTOBJECTS = obj/grep_client.o
SDFSSERVEROBJECTS = obj/sdfs_server.o

.PHONY: all clean

all : obj grep_server grep_client sdfs_server

grep_server: $(GREPSERVEROBJECTS)
	clang++ -O3 $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
grep_client: $(GREPCLIENTOBJECTS)
	clang++ -O3 $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
sdfs_server: $(SDFSSERVEROBJECTS)
	clang++ -O3 $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
	
clean :
	$(RM) -r obj grep_server grep_client sdfs_server

obj/%.o: src/*/%.cc
	clang -O3 $(COMPILERFLAGS) -c -o $@ $<
obj:
	mkdir -p obj
