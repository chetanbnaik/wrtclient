/*
 * gstreamer pipeline:
 * gst-launch-1.0 videotestsrc ! 
 * video/x-raw,format=RGB,width=320,height=240,framerate=30/1 ! 
 * videoconvert ! vp8enc ! rtpvp8pay ! appsink
 * 
 * CAPS:
 * application/x-rtp, 
 * media=(string)video, 
 * clock-rate=(int)90000, 
 * encoding-name=(string)VP8-DRAFT-IETF-01, 
 * payload=(int)96, ssrc=(uint)2754073203, 
 * timestamp-offset=(uint)689101461, 
 * seqnum-offset=(uint)38579
 * 
 * gst-launch-1.0 -v v4l2src ! 
 * video/x-raw,format=YUY2,width=320,height=240,framerate=15/1 ! 
 * videoconvert ! autovideosink
 * */
#include <gst/gst.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <inttypes.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include "../rtp.h"

#define PS_GSTREAMER_VP8  0
#define PS_GSTREAMER_H264 1
#define PS_GSTREAMER_VP9  2

static GMainLoop * loop;

typedef struct ps_gstreamer_rtp_source {
	GstElement * pipeline, * sink, * filter;
	GstCaps * filtercaps;
} ps_gstreamer_rtp_source;

typedef struct ps_gstreamer_rtp_sink {
	GstElement * pipeline, * src, * filter;
	GstCaps * filtercaps;
	gboolean isCapsSet;
} ps_gstreamer_rtp_sink;

typedef struct ps_gstreamer_rtp_relay_packet {
	rtp_header * data;
	gint length;
	gint is_video;
	gint is_keyframe;
	uint32_t timestamp;
	uint16_t seq_number;
} ps_gstreamer_rtp_relay_packet;

static GstFlowReturn new_sample (GstElement * appsink, ps_gstreamer_rtp_sink * sink) {
	GstCaps * caps;
	GstFlowReturn ret;
	GstSample * sample, * feedsample;
	GstBuffer * buffer, * feedbuffer, * app_buffer;
	GstMapInfo * map;
	gpointer framedata;
	gsize dest_size;
	
	g_signal_emit_by_name (appsink, "pull-sample", &sample, NULL);
	buffer = gst_sample_get_buffer (sample);
	app_buffer = gst_buffer_copy (buffer);
	
	gst_buffer_extract_dup (app_buffer, 0, -1, &framedata, &dest_size);
	
	rtp_header * rtp = (rtp_header *) framedata;
	if (!sink->isCapsSet){
		caps = gst_sample_get_caps (sample);
		g_print ("CAPS: %s\n", gst_caps_to_string(caps));
		//if (caps != NULL) gst_caps_unref (caps);
		g_print ("ssrc: %d, timestamp: %d, seq-num: %d\n",ntohl(rtp->ssrc), ntohl(rtp->timestamp), ntohs(rtp->seq_number));	
		sink->filtercaps = gst_caps_new_simple ("application/x-rtp", 
				"media", G_TYPE_STRING, "video",
				"clock-rate", G_TYPE_INT, 90000,
				"encoding-name", G_TYPE_STRING, "VP8-DRAFT-IETF-01",
				"payload", G_TYPE_INT, 96,
				"ssrc", G_TYPE_UINT, ntohl(rtp->ssrc),
				"timestamp-offset", G_TYPE_UINT, ntohl(rtp->timestamp),
				"seqnum-offset", G_TYPE_INT, ntohs(rtp->seq_number),
				NULL);
		g_object_set (sink->src, "caps", sink->filtercaps, NULL);
		gst_caps_unref (sink->filtercaps);
		sink->isCapsSet = TRUE;
		caps = NULL;
	}
	gst_sample_unref (sample);
	feedbuffer = gst_buffer_new_wrapped (framedata, dest_size);
	ret = gst_app_src_push_buffer (GST_APP_SRC(sink->src), feedbuffer);
	return ret;
	/*if (ps_gstreamer_is_keyframe (PS_GSTREAMER_VP8, framedata, dest_size)) {
		g_print ("Key frame received\n");
	}*/
	//if (caps != NULL) gst_caps_unref (caps);
	//g_signal_emit_by_name (appsink, "pull-sample", &sample, NULL);
	//sample = gst_app_sink_pull_sample (GST_APP_SINK(appsink));
	/*g_print ("framedata ssrc -> %u\n", rtp->ssrc);
	g_print ("framedata type -> %u\n", rtp->type);*/
	
	/*g_print ("framedata ts -> %u, buffer pts: %"G_GUINT64_FORMAT"\n", rtp->timestamp, GST_BUFFER_PTS (buffer));
	g_print ("framedata ntohl(ts) -> %u, buffer pts: %"G_GUINT64_FORMAT"\n", ntohl(rtp->timestamp), GST_BUFFER_PTS (buffer));
	g_print ("framedata sequence -> %u, ntohs(seq) -> %u\n", rtp->seq_number,ntohs(rtp->seq_number));
	g_print ("Buffer dts: %"G_GUINT64_FORMAT"\n", GST_BUFFER_DTS (buffer));
	g_print ("Buffer duration: %"G_GUINT64_FORMAT"\n", GST_BUFFER_DURATION (buffer));
	
	if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
		g_print ("Buffer map: 
		gst_buffer_unmap (buffer, &map);
	}
	if (caps != NULL) gst_caps_unref (caps);
	g_print ("here-> %p\n",sample);*/
	
	//g_print ("Buffer len: %d, gsize: %"G_GSIZE_FORMAT"\n", gst_buffer_get_size (app_buffer), dest_size);
	
	//feedsample = gst_sample_new (feedbuffer, caps, NULL, NULL);
	/*if ((ret = gst_app_src_push_buffer (GST_APP_SRC(sink->src), feedbuffer)) == GST_FLOW_OK) {
		g_print ("Sample pushed\n");
	} else {
		g_print ("Sample not pushed\n");
	}*/
	//g_free (framedata);
	
	//return ret;
}

/* Signal Handler */
static void ps_handle_signal(int signum) {
	g_main_loop_quit(loop);
}

gint main (int argc, char * argv[]) {
	/* to allow core dumps */
	struct rlimit core_limits;
	core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &core_limits);
	
	ps_gstreamer_rtp_source * data = NULL;
	ps_gstreamer_rtp_sink * sink = NULL;
	GstElement * source, * videorate, * videoscale;
	GstElement * convert, * encoder, * rtppay, * toverlay;
	GstBus * bus;
	
	loop = g_main_loop_new (NULL, FALSE);
	
	signal(SIGINT, ps_handle_signal);
	signal(SIGTERM, ps_handle_signal);
	
	gst_init (&argc, &argv);
	data = (ps_gstreamer_rtp_source *)g_malloc0(sizeof(ps_gstreamer_rtp_source));
	char * vsrcname = "videotestsrc";
	source = gst_element_factory_make (vsrcname,"source");
	videorate = gst_element_factory_make ("videorate", "videorate");
	data->filter = gst_element_factory_make ("capsfilter","filter");
	videoscale = gst_element_factory_make ("videoscale", "videoscale");
	convert = gst_element_factory_make ("videoconvert", "convert");
	toverlay = gst_element_factory_make ("timeoverlay","toverlay");
	encoder = gst_element_factory_make ("vp8enc","encoder");
	rtppay = gst_element_factory_make ("rtpvp8pay","rtppay");
	data->sink = gst_element_factory_make ("appsink","sink");
	g_object_set (data->sink, "emit-signals", TRUE, "sync", FALSE, NULL);
	g_object_set (encoder, "error-resilient", TRUE, NULL);
	g_object_set (encoder, "keyframe-max-dist", 30, NULL);
	g_object_set (encoder, "keyframe-mode", 0, NULL);
	
	
	data->filtercaps = gst_caps_new_simple ("video/x-raw", 
				"format", G_TYPE_STRING, "RGB",
				"width", G_TYPE_INT, 320,
				"height", G_TYPE_INT, 240,
				"framerate", GST_TYPE_FRACTION, 30, 1,
				NULL);
	g_print ("filtercaps--> %s\n", gst_caps_to_string(data->filtercaps));
	g_object_set (data->filter, "caps", data->filtercaps, NULL);
	gst_caps_unref (data->filtercaps);
	
	data->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (data->pipeline), source, data->filter, videoscale, videorate, convert, toverlay, encoder, rtppay, data->sink, NULL);
	if (!gst_element_link_many (source, data->filter, videoscale, videorate, convert, toverlay, encoder, rtppay, data->sink, NULL)) {
		g_warning ("failed to link elements\n");
	}
	
	g_print ("pipeline created successfully\n");
	
	
	sink = (ps_gstreamer_rtp_sink *) g_malloc0(sizeof(ps_gstreamer_rtp_sink));
	sink->isCapsSet = FALSE;
	GstElement * rtpdepay, * decoder, * sinkconvert, * display;
	sink->src = gst_element_factory_make ("appsrc","src");
	sink->filtercaps = NULL; //gst_caps_new_simple("application/x-rtp",NULL,NULL);
	//g_object_set(sink->src, "caps", sink->filtercaps, NULL);
	rtpdepay = gst_element_factory_make ("rtpvp8depay","rtpdepay");
	decoder = gst_element_factory_make ("vp8dec", "decoder");
	sinkconvert = gst_element_factory_make ("videoconvert", "sinkconvert");
	display = gst_element_factory_make ("autovideosink", "display");
	sink->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN(sink->pipeline), sink->src, rtpdepay, decoder, sinkconvert, display, NULL);
	if (!gst_element_link_many(sink->src, rtpdepay, decoder, sinkconvert, display, NULL)) {
		g_warning ("failed to link elements in rtp_sink\n");
	}
	
	g_signal_connect (data->sink, "new-sample", G_CALLBACK (new_sample), sink);
	
	
	gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
	gst_element_set_state (sink->pipeline, GST_STATE_PLAYING);
	
	g_main_loop_run (loop);
	
	/* Exit nice and cleanly */
	g_print ("Exiting...\n");
	/* gst_object_unref (data->sink); */
	gst_element_set_state (data->pipeline, GST_STATE_NULL);
	gst_object_unref (GST_OBJECT(data->pipeline));
	gst_element_set_state (sink->pipeline, GST_STATE_NULL);
	gst_object_unref (GST_OBJECT(sink->pipeline));
	g_main_loop_unref (loop);
	g_free (data);
	g_free (sink);
	g_print ("Exiting...\n");
	return 0;
}
