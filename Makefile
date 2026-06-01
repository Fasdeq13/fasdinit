CXX = g++
CXXFLAGS = -std=c++20 -O3 -march=native -pipe -pthread
PREFIX = /usr/local
SRC_DIR = src

TARGETS = fasdinit fasdscan fasdsupervisor fasdctl

all: $(TARGETS)

fasdinit: $(SRC_DIR)/fasdinit.cpp
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/fasdinit.cpp -o fasdinit

fasdscan: $(SRC_DIR)/fasdscan.cpp
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/fasdscan.cpp -o fasdscan

fasdsupervisor: $(SRC_DIR)/fasdsupervisor.cpp
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/fasdsupervisor.cpp -o fasdsupervisor

fasdctl: $(SRC_DIR)/fasdctl.cpp
	$(CXX) $(CXXFLAGS) $(SRC_DIR)/fasdctl.cpp -o fasdctl

clean:
	rm -f $(TARGETS)

install: all
	mkdir -p $(DESTDIR)/sbin
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp fasdinit $(DESTDIR)/sbin/fasdinit
	cp fasdscan $(DESTDIR)/sbin/fasdscan
	cp fasdsupervisor $(DESTDIR)/sbin/fasdsupervisor
	cp fasdctl $(DESTDIR)$(PREFIX)/bin/fasdctl

.PHONY: all clean install
