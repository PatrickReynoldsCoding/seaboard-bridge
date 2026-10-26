mpe-clean: mpe-clean.cpp
	g++ -O2 -std=c++17 -Wall -Wextra -o $@ $< -lasound -lpthread

clean:
	rm -f mpe-clean
