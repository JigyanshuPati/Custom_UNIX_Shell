CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -O0
TARGET := shell

SOURCES := shell.cpp parser.cpp executor.cpp builtins.cpp jobs.cpp
OBJECTS := $(SOURCES:.cpp=.o)
HEADERS := parser.h executor.h builtins.h jobs.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)
