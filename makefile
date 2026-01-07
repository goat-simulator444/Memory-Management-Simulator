# Makefile to build src/mainn.cpp

CXX := g++
CXXFLAGS := -std=c++17 -O2 -w

SRC := src/mainn.cpp
TARGET := mainn

.PHONY: all clean run

all: $(TARGET)
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<
run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)