//PacketServo browser endpoint
// Author: Packetservo
var server = null;
if(window.location.protocol === 'http:')
	server = "ws://" + window.location.hostname + ":8080/ws";
else
	server = "https://" + window.location.hostname + ":8080/ws";

var janus = null;
var gstsink = null;
var started = false;
var spinner = null;
var bandwidth = 1024 * 1024;
var avstream = new MediaStream();
var astream = null;
var vstream = null;

var myname = null;
var recording = false;
var playing = false;
var recordingId = null;
var selectedRecording = null;
var selectedRecordingInfo = null;

$(document).ready(function() {
	var mycanvas = document.getElementById("mycanvas");
	Janus.init({debug: "all", callback: function(){
		$('#start').click(function(){
			if(started)
				return;
			started = true;
			if(!Janus.isWebrtcSupported()){
				bootbox.alert("No WebRTC supported");
			}
			janus = new Janus({
				server: server,
				//iceServers: [{url: null}],
				success: function(){
					janus.attach(
					{
						plugin: "ps.plugin.gstsink",
						success: function(pluginHandle) {
							gstsink = pluginHandle;
							Janus.log("Plugin attached! (" + gstsink.getPlugin() + ", id=" + gstsink.getId() + ")");
							$('#start').removeAttr('disabled').html("Stop")
								.click(function(){
									gstsink.send({'message': {'request':'stop'}});
									//janus.destroy();
								});
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
											recordingId = id;
										}
									} else if (event === "stopped") {
										vstream.getTracks().forEach(function(track){track.stop();});
										//vstream = null;
										gstsink.hangup();
									}
								}
							} else {
								var error = msg["error"];
								bootbox.alert(error);
							}
						},
						onlocalstream: function(stream) {
							if (playing === true) {return;}
							Janus.debug(" ::: Got a local stream :::");
							//attachMediaStream($('#local').get(0), stream);
						},
						oncleanup: function() {
							Janus.log(" ::: Got a cleanup notification :::");
							playing = false;
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
				error: function(error){
					Janus.error(error);
					bootbox.alert(error, function() {
						window.location.reload();
					});
				},
				destroyed: function(){
					window.location.reload();
				}
			});
		});
	}});
});

function prepareStream(canvas) {
	start_cloth(canvas);
	vstream = canvas.captureStream(10);
	getUserMedia({audio: true, video: false},
		function(astream) {
			avstream.addTrack(astream.getAudioTracks()[0]);
			avstream.addTrack(vstream.getVideoTracks()[0]);
			Janus.log(avstream.getTracks());
			gstsink.createOffer({
				stream: avstream,
				media: {audio: true, video: true},
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
		function(error) {Janus.log("error: "+ error);}
	);
}
function startRecording() {
	if(recording) return;
	recording = true;
	$('#start').
	start_cloth(mycanvas);
	canvas_stream = mycanvas.captureStream(10);
	//gstsink.send({
		//'message': {
			//'request': 'configure',
			//'video-bitrate-max': bandwidth,
			//'video-keyframe-interval': 15000
		//}
	//});
	gstsink.createOffer({
		media: {stream: canvas_stream},
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
}
