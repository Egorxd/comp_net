CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

all: server client

server: server_modified.cpp message.h
	$(CXX) $(CXXFLAGS) server_modified.cpp -o server

client: client_modified.cpp message.h
	$(CXX) $(CXXFLAGS) client_modified.cpp -o client

clean:
	rm -f server client
