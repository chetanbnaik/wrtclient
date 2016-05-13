CFLAGS=`pkg-config --cflags glib-2.0 gstreamer-1.0 libwebsockets jansson` -g
#CFLAGS += -I/home/osboxes/client/websocketpp/
LIBS=`pkg-config --libs glib-2.0 gstreamer-1.0 libwebsockets jansson` 
LIBS+= -lpthread -ldl
#LIBS += -lboost_system -lboost_random -lpthread -lboost_timer -lboost_chrono -lrt

OBJECTS=log.o psclient.o
SOURCES=log.c log.h debug.h main.c 

all: webrtcinterface

.PHONY: transport

transport:
	$(MAKE) -C transport

webrtcinterface: $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LIBS)

log.o: log.c log.h debug.h
psclient.o: psclient.c ps_websockets.h


clean:
	rm *.o webrtcinterface
