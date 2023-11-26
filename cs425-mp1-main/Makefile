COMPILERFLAGS = -std=c++20 -g -Wall -Wextra -Wno-sign-compare -pthread

SERVEROBJECTS = obj/server.o
CLIENTOBJECTS = obj/client.o

.PHONY: all clean

all : obj server client

server: $(SERVEROBJECTS)
	clang++ $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)
client: $(CLIENTOBJECTS)
	clang++ $(COMPILERFLAGS) $^ -o $@ $(LINKLIBS)

clean :
	$(RM) -r obj server client

obj/%.o: src/%.cc
	clang $(COMPILERFLAGS) -c -o $@ $<
obj:
	mkdir -p obj
