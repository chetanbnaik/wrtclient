CFLAGS=`pkg-config --cflags glib-2.0 gstreamer-1.0 libwebsockets` -g
#CFLAGS += -I/home/osboxes/client/websocketpp/
LIBS=`pkg-config --libs glib-2.0 gstreamer-1.0 libwebsockets` 
#LIBS += -lboost_system -lboost_random -lpthread -lboost_timer -lboost_chrono -lrt

OBJECTS=client.o wsclient.o main.o
SOURCES=client.c client.h wsclient.c wsclient.h main.c 

all: webrtcinterface

webrtcinterface: $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LIBS)
      
client.o: client.c client.h
wsclient.o: wsclient.c wsclient.h
main.o: main.c client.h wsclient.h


clean:
	rm *.o webrtcinterface
