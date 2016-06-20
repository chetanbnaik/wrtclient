// PacketServo browser endpoint
// Author: Packetservo
var server = null;
if(window.location.protocol === 'http:')
	server = "ws://" + window.location.hostname + ":8080/ws";
else
	server = "https://" + window.location.hostname + ":8080/ws";

var gstsink = null;
var gstsrc = null;
var spinner = null;
var bandwidth = 1024 * 1024;

var localPlaying = false;
var remotePlaying = false;

function StopRemote () {
	$('#remoteVideo').hide();
	gstsrc.send({'message': {'request':'stop'}});
	$('#remoteButton').html("Start").click(StartRemote);
	remotePlaying = false;
}

function StartRemote () {
	$('#remoteVideo').show();
	if(remotePlaying)
		return;
	
	janus = new Janus({
		server: server,
		success: function () {
			janus.attach({
				plugin: "ps.plugin.gstreamer",
				success: function(pluginHandle) {
					gstsrc = pluginHandle;
					Janus.log("Plugin attached! (" + gstsrc.getPlugin() + ", id=" + gstsrc.getId() + ")");
					var body = {"request": "list"};
					gstsrc.send({"message": body, success: function(result){
						if (result === null || result === undefined) {
							bootbox.alert("Got no response to our query for available streams");
							return;
						}
						if(result["list"] !== undefined && result["list"] !== null) {
							var list = result["list"];
							Janus.debug(list);
							var body = {"request":"watch",id:list[0]["id"]};
							gstsrc.send({"message": body});
						}
					}});
					$('#remoteButton').html("Stop").click(StopRemote);
				},
				error: function(error) {
					Janus.error(" -- Error attaching plugin.. ", error);
					bootbox.alert("Error attaching plugin... " + error);
				},
				onmessage: function(msg, jsep) {
					Janus.debug(" ::: got a message :::");
					Janus.debug(JSON.stringify(msg));
					var result = msg["result"];
					if (result !== null && result !== undefined) {
						var status = result["status"];
						if (status === 'starting'){
							Janus.debug(status);
						} else if(status === 'started') {
							Janus.debug(status);
						} else if(status === 'stopping') {
							Janus.debug(status);
							gstsrc.hangup();
							remotePlaying = false;
							janus.destroy();
						}
					} else if (msg["error"] !== undefined && msg["error"] !== null) {
						bootbox.alert(msg["error"]);
						return;
					}
					if (jsep !== undefined && jsep !== null) {
						Janus.debug("Handling SDP as well");
						Janus.debug(jsep);
						gstsrc.createAnswer({
							jsep: jsep,
							media: {audioSend: false, videoSend: false, data: true},
							success: function (jsep) {
								Janus.debug("Got SDP!");
								Janus.debug(jsep);
								var body = {"request": "start"};
								gstsrc.send({"message": body, "jsep": jsep});
							},
							error: function(error) {
								Janus.error("WebRTC error:", error);
								bootbox.alert("WebRTC error... " + JSON.stringify(error));
							}
						});
					}
				},
				onremotestream: function(stream) {
					if (remotePlaying === true) {return;}
					remotePlaying = true;
					Janus.debug(" ::: Got a remote stream :::");
					Janus.debug(JSON.stringify(stream));
					attachMediaStream($('#remoteVideo').get(0), stream);
				},
				oncleanup: function() {
					Janus.log(" ::: Got a cleanup notification :::");
					remotePlaying = false;
				}
			});
		},
		error: function(error) {
			Janus.error(error);
			bootbox.alert(error, function(){window.location.reload();});
		},
		destroyed: function() {
			Janus.log("Destroyed!!");
			remotePlaying = false;
			window.location.reload();
		}
	});
}

function StopLocal() {
	$('#mycanvas').hide()
	gstsink.send({'message': {'request':'stop'}});
	$('#localButton').html("Start").click(StartLocal);
	localPlaying = false;
}

function StartLocal() {
	$('#mycanvas').show()
	if(localPlaying)
		return;
	
	janus = new Janus({
		server: server,
		success: function() {
			janus.attach({
				plugin: "ps.plugin.gstsink",
				success: function(pluginHandle) {
					gstsink = pluginHandle;
					Janus.log("Plugin attached! (" + gstsink.getPlugin() + ", id=" + gstsink.getId() + ")");
					$('#localButton').removeAttr('disabled').html("Stop").click(StopLocal);
					gstsink.send({
						'message': {
							'request': 'configure',
							'video-bitrate-max': bandwidth,
							'video-keyframe-interval': 15000
						}
					});
					//prepareStream(mycanvas); 
					start_cloth(mycanvas);
					vstream = canvas.captureStream(10);
					gstsink.createOffer({
						stream: vstream,
						//media: {data: true},
						success: function(jsep) {
							Janus.debug("Got SDP!");
							Janus.debug(jsep);
							var body = {"request": "start"};
							gstsink.send({"message": body, "jsep": jsep});
						},
						error: function(error) {
							Janus.error("WebRTC error...", error);
							bootbox.alert("WebRTC error..." + error);
							gstsink.hangup();
						}
					});
				},
				error: function(error) {
					Janus.error(" -- Error attaching plugin --", error);
					bootbox.alert(" -- Error attaching plugin --" + error);
				},
				onmessage: function (msg, jsep){
					Janus.debug(" ::: Got a message :::");
					Janus.debug(JSON.stringify(msg));
					var result = msg["result"];
					if(result !== null && result !== undefined) {
						if(result["status"] !== undefined && result["status"] !== null) {
							var event = result["status"];
							if (event === "recording") {
								if (jsep !== null & jsep !== undefined) {
									gstsink.handleRemoteJsep({jsep: jsep});
								}
								var id = result["id"];
								if(id !== null && id !== undefined){
									Janus.log("The ID is "+id);
								}
							} else if (event === "stopped") {
								vstream.getTracks().forEach(function(track){track.stop();});
								//vstream = null;
								localPlaying = false;
								gstsink.hangup();
								janus.destroy();
							}
						}
					} else {
						var error = msg["error"];
						bootbox.alert(error);
					}
				},
				onlocalstream: function(stream) {
					if (localPlaying === true) {return;}
					localPlaying = true;
					Janus.debug(" ::: Got a local stream :::");
					//attachMediaStream($('#local').get(0), stream);
				},
				oncleanup: function() {
					Janus.log(" ::: Got a cleanup notification :::");
					localPlaying = false;
				},
				consentDialog: function(on) {
					Janus.debug("Consent dialog should be " + (on ? "on": "off") + " now");
					if (on) {
						$.blockUI({
							message: '<div>See up</div>',
							css: {
								border: 'none',
								padding: '15px',
								backgroundColor: 'transparent',
								color: '#aaa',
								top: '10px',
								left: (navigator.mozGetUserMedia ? '-100px' : '300px')
							}
						});
					} else {
						$.unblockUI();
					}
				}
			});			
		},
		error: function(error) {
			Janus.error(error);
			bootbox.alert(error, function() {window.location.reload();});
		},
		destroyed: function() {
			Janus.log("Destroyed!!");
			localPlaying = false;
			window.location.reload();
		}
	});
}

$(document).ready(function() {
	
	if(!Janus.isWebrtcSupported()){
		bootbox.alert("No WebRTC supported");
	}
	
	var mycanvas = document.getElementById("mycanvas");
	$('#mycanvas').hide();
	$('#remoteVideo').hide();
	Janus.init({debug: "all"});
	$('#localButton').click(StartLocal);
	$('#remoteButton').click(StartRemote);
	$('#sendData').click(sendData);
});

function sendData(){
	if (gstsrc !== null && gstsrc !== undefined) {
		gstsrc.data({
			text: "test",
			error: function(reason){bootbox.alert(reason);},
			success: function() {Janus.debug("'test' data send");}
		});
	} else {
		Janus.debug("Not connected! 'gstsrc' found null");
	}
}

$(document).keydown(function(e){
	if (gstsrc !== null && gstsrc !== undefined) {
		var theCode = e.keyCode ? e.KeyCode : e.which ? e.which : e.charCode;
		switch(theCode) {
			case 76: 	//"l" key, for right command
				Janus.debug(e);
				gstsrc.data({
					text: "right",
					error: function(reason) {bootbox.alert(reason);},
					success: function(){},
				});
				break;
			case 74:	//"j" key, for left command
				gstsrc.data({
					text: "left",
					error: function(reason) {bootbox.alert(reason);},
					success: function(){},
				});
				break;
			case 75:	//"k" key, for back command
				gstsrc.data({
					text: "back",
					error: function(reason) {bootbox.alert(reason);},
					success: function(){},
				});
				break;
			case 73:	//"i" key, for fwd command
				gstsrc.data({
					text: "fwd",
					error: function(reason) {bootbox.alert(reason);},
					success: function(){},
				});
				break;
			default: return;
		}
		e.preventDefault();
	}
});
