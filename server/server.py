#!/usr/bin/python

import sys, json
from twisted.internet import reactor
from twisted.web.server import Site
from twisted.web.static import File
from twisted.web.resource import Resource

from autobahn.twisted.websocket import WebSocketServerFactory, \
     WebSocketServerProtocol

from autobahn.twisted.resource import WebSocketResource

class PSProto(WebSocketServerProtocol):
    def onConnect(self,request):
        print("Client connecting: {0}".format(request.peer))

    def sendHello(self):
		msg = json.dumps({'cmd':'play'}, ensure_ascii=False).encode('utf8')
		self.sendMessage(msg)
		#reactor.callLater(1,self.sendHello)

    def onOpen(self):
        print("WebSocket connection open.")
        self.factory.register(self)
        #self.sendHello()

    def onMessage(self, payload, isBinary):
		self.factory.forward(payload, self.peer, isBinary)
		if isBinary:
			print("Binary message received: {0} bytes".format(len(payload)))
		else:
			print("JSON message received: {0}".format(json.loads(payload.decode('utf8'))))
        

    def connectionLost(self,reason):
		WebSocketServerProtocol.connectionLost(self,reason)
		self.factory.unregister(self)
		print("Websocket connection lost: {0}".format(reason))
		
    def onClose(self, wasClean, code, reason):
		self.factory.unregister(self)
		print("WebSocket connection closed: {0}".format(reason))


class PSFactory(WebSocketServerFactory):
    def __init__(self):
        WebSocketServerFactory.__init__(self)
        self.clients = []

    def register(self, client):
        if client not in self.clients:
            print("registered client {}".format(client.peer))
            self.clients.append(client)

    def unregister(self, client):
        if client in self.clients:
            print("unregistered client {}".format(client.peer))
            self.clients.remove(client)
    
    def forward(self, payload, sender, isBinary):
		for c in self.clients:
			if c.peer != sender:
				c.sendMessage(payload, isBinary)
		


if __name__ == '__main__':
    from twisted.python import log
    log.startLogging(sys.stdout)
    
    wsfactory = PSFactory()
    wsfactory.protocol = PSProto
    wsfactory.startFactory()
    wsres = WebSocketResource(wsfactory)

    root = File(".")
    root.putChild("ws",wsres)

    site = Site(root)
    reactor.listenTCP(8080,site)
    reactor.run()



