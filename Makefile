PROG=netlink
#CPP=g++
CPP=clang++
CPPFLAGS=-Wall
CFLAGS=-g
INCLUDES=-I${HOME}/c++ -I/usr/include/boost -I/usr/local/include/clang
LIBS=-L/usr/lib -lboost_thread -lboost_system -lboost_program_options -lpthread  -L/usr/lib/x86_64-linux-gnu/ -lssl

%.o : %.cpp
	$(CPP) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(PROG): $(PROG).o
	$(CPP) $< -o $@ $(LIBS)

.PHONY: clean
clean:
	@rm -f *.o
	@rm -f $(PROG)

