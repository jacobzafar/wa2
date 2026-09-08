CC_FLAGS= -Wall -I.
LD_FLAGS= -Wall -L./ 


all: libcalc test tcpserver udpserver

tcpservermain.o: tcpservermain.cpp
	$(CXX)  $(CC_FLAGS) $(CFLAGS) -c tcpservermain.cpp

udpservermain.o: udpservermain.cpp
	$(CXX)  $(CC_FLAGS) $(CFLAGS) -c udpservermain.cpp 

main.o: main.cpp
	$(CXX) $(CC_FLAGS) $(CFLAGS) -c main.cpp 


test: main.o calcLib.o
	$(CXX) $(LD_FLAGS) -o test main.o -lcalc

tcpserver: tcpservermain.o calcLib.o
	$(CXX) $(LD_FLAGS) -o tcpserver tcpservermain.o -lcalc

udpserver: udpservermain.o calcLib.o
	$(CXX) $(LD_FLAGS) -o udpserver udpservermain.o -lcalc


calcLib.o: calcLib.c calcLib.h
	gcc -Wall -fPIC -c calcLib.c

libcalc: calcLib.o
	ar -rc libcalc.a -o calcLib.o

clean:
	rm *.o *.a test tcpserver udpserver 
