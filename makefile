CXXFLAGS ?= -std=c++20 -Wall -Wextra -g

all: tree

%: %.c
	$(CXX) $(CFLAGS) $^ -o $@

clean:

distclean: clean
	$(RM) tree

.PHONY: all clean distclean
