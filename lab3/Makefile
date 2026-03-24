CXX=g++
CXXFLAGS=-Wall -std=c++17 -pthread

all: server client

server:
	$(CXX) server_modified.cpp -o server $(CXXFLAGS)

client:
	$(CXX) client_modified.cpp -o client $(CXXFLAGS)

clean:
	rm -f server client
