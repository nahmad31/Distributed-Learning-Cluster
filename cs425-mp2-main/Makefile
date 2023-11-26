COMPILERFLAGS = -std=c++20 -g -Wall -Wextra -Wno-sign-compare -pthread

GREPSERVEROBJECTS = obj/grep_server.o
GREPCLIENTOBJECTS = obj/grep_client.o
MEMBERSHIPSERVEROBJECTS = obj/membership_server.o

.PHONY: all clean

all : obj grep_server grep_client membership_server

grep_server: $(GREPSERVEROBJECTS)
	clang++ $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
grep_client: $(GREPCLIENTOBJECTS)
	clang++ $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
membership_server: $(MEMBERSHIPSERVEROBJECTS)
	clang++ $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
	
clean :
	$(RM) -r obj grep_server grep_client membership_server

obj/%.o: src/*/%.cc
	clang $(COMPILERFLAGS) -c -o $@ $<
obj:
	mkdir -p obj
