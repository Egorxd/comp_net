CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
TARGETS = server client

all: $(TARGETS)

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f $(TARGETS)

re: clean all

.PHONY: all clean re
