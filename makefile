CC          =   gcc
CFLAGS      =   -std=c99 -g -Wall
DIR 		=	./
INCDIR      =   $(DIR)include/
LIBDIR      =   $(DIR)lib/
LIBSRC		=   $(LIBDIR)lib-src/
LIBS        =	-lmyutils -lpthread -lmypoll -lconcurrent_queue
LDFLAGS     = 	-Wl,-rpath,$(LIBDIR)
SRCDIR      =   $(DIR)src/
OUTPUTFILE  = 	out.txt

TARGETS     =   server13_1 \
				server13_2 \
				client

.PHONY: all clean test13_1 test13_2

all: $(TARGETS)

%: %.c
	$(CC) $(CFLAGS) -I $(INCDIR) -L $(LIBDIR) $(LDFLAGS) $(LIBS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -I $(INCDIR) -L $(LIBDIR) $(LIBS) -c -o $@ $<
	

server13_1: $(SRCDIR)server13_1.c  $(INCDIR)protocollo.h $(INCDIR)mysocket.h $(INCDIR)mypthread.h  \
			$(LIBDIR)libmypoll.so $(LIBDIR)libmyutils.so $(LIBDIR)libconcurrent_queue.so
	$(CC) $(CFLAGS) -I $(INCDIR) -L $(LIBDIR) $(LDFLAGS) $(LIBS) -o $@ $<

server13_2: $(SRCDIR)server13_2.c $(INCDIR)protocollo.h $(INCDIR)mysocket.h $(INCDIR)mypthread.h  \
			$(LIBDIR)libmypoll.so $(LIBDIR)libmyutils.so $(LIBDIR)libconcurrent_queue.so
	$(CC) $(CFLAGS) -I $(INCDIR) -L $(LIBDIR) $(LDFLAGS) $(LIBS) -o $@ $<

client: $(SRCDIR)client.c $(INCDIR)protocollo.h $(INCDIR)mysocket.h
	$(CC) $(CFLAGS) -I $(INCDIR) -L $(LIBDIR) $(LDFLAGS) $(LIBS) -o $@ $<

$(LIBDIR)libmypoll.so: $(LIBSRC)mypoll.c $(INCDIR)mypoll.h
	-rm -f mypoll.o
	$(CC) $(CFLAGS) -I $(INCDIR) -c -fPIC -o mypoll.o $(LIBSRC)mypoll.c
	$(CC) -shared -o $@ mypoll.o
	rm -f mypoll.o

$(LIBDIR)libmyutils.so: $(LIBSRC)myutils.c $(INCDIR)myutils.h
	-rm -f myutils.o
	$(CC) $(CFLAGS) -I $(INCDIR) -c -fPIC -o myutils.o $(LIBSRC)myutils.c
	$(CC) -shared -o $@ myutils.o
	rm -f myutils.o

$(LIBDIR)libconcurrent_queue.so: $(LIBSRC)concurrent_queue.c $(INCDIR)concurrent_queue.h
	-rm -f concurrent_queue.o
	$(CC) $(CFLAGS) -I $(INCDIR) -c -fPIC -o concurrent_queue.o $(LIBSRC)concurrent_queue.c
	$(CC) -shared -o $@ concurrent_queue.o
	rm -f concurrent_queue.o

test13_1:
	chmod +x ./test_server13_1; \
	./test_server13_1 20 $(OUTPUTFILE) 1000

test13_2:
	chmod +x ./test_server13_2; \
	./test_server13_2 20 $(OUTPUTFILE) 1000

clean:
	-rm -f $(TARGETS)
	-rm -f vgcore*
	-rm -f $(LIBDIR)*.so
	-rm -f $(OUTPUTFILE)
