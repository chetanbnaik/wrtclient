/*
 * gstreamer pipeline:
 * gst-launch-1.0 videotestsrc ! 
 * video/x-raw,format=RGB,width=320,height=240,framerate=30/1 ! 
 * videoconvert ! vp8enc ! rtpvp8pay ! appsink
 * 
 * */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "../rtp.h"

typedef struct ps_gstreamer_rtp_source {
	GstElement * pipeline, * sink, * filter;
	GstCaps * filtercaps;
} ps_gstreamer_rtp_source;

static GstFlowReturn new_sample (GstElement * appsink, ps_gstreamer_rtp_source * source) {
	GstCaps * caps;
	GstSample * sample;
	GstBuffer * buffer;
	GstMapInfo * map;
	gpointer framedata;
	gsize dest_size;
	
	sample = gst_app_sink_pull_sample (GST_APP_SINK(source->sink));
	//g_signal_emit_by_name (appsink, "pull-sample", &sample, NULL);
	if (sample != NULL) {
		caps = gst_sample_get_caps (sample);
		g_print ("CAPS: %s\n", gst_caps_to_string(caps));
		buffer = gst_sample_get_buffer (sample);
		gst_buffer_extract_dup (buffer, 0, gst_buffer_get_size (buffer), &framedata, &dest_size);
		rtp_header * rtp = (rtp_header *) framedata;
		g_print ("framedata ssrc -> %u\n", rtp->ssrc);
		g_print ("framedata type -> %u\n", rtp->type);
		g_print ("Buffer len: %d, gsize: %"G_GSIZE_FORMAT"\n", gst_buffer_get_size (buffer), dest_size);
		g_print ("framedata ts -> %u, buffer pts: %"G_GUINT64_FORMAT"\n", rtp->timestamp, GST_BUFFER_PTS (buffer));
		g_print ("framedata ntohl(ts) -> %u, buffer pts: %"G_GUINT64_FORMAT"\n", ntohl(rtp->timestamp), GST_BUFFER_PTS (buffer));
		g_print ("framedata sequence -> %u, ntohs(seq) -> %u\n", rtp->seq_number,ntohs(rtp->seq_number));
		g_print ("Buffer dts: %"G_GUINT64_FORMAT"\n", GST_BUFFER_DTS (buffer));
		g_print ("Buffer duration: %"G_GUINT64_FORMAT"\n", GST_BUFFER_DURATION (buffer));
		/*
		if (gst_buffer_map (buffer, &map, GST_MAP_READ)) {
			g_print ("Buffer map: 
			gst_buffer_unmap (buffer, &map);
		}
		if (caps != NULL) gst_caps_unref (caps);
		g_print ("here-> %p\n",sample);*/
		
		g_free (framedata);
		gst_sample_unref (sample);
	} else {
		g_warning ("Sample not received\n");
	}
		
	return GST_FLOW_OK;
}

gint main (int argc, char * argv[]) {
	ps_gstreamer_rtp_source * data = NULL;
	GstElement * source;
	GstElement * convert, * encoder, * rtppay;
	GstBus * bus;
	GMainLoop * loop;
	loop = g_main_loop_new (NULL, FALSE);
	
	gst_init (&argc, &argv);
	data = (ps_gstreamer_rtp_source *)g_malloc0(sizeof(ps_gstreamer_rtp_source));
	char * vsrcname = "videotestsrc";
	source = gst_element_factory_make (vsrcname,"source");
	data->filter = gst_element_factory_make ("capsfilter","filter");
	convert = gst_element_factory_make ("videoconvert", "convert");
	encoder = gst_element_factory_make ("vp8enc","encoder");
	rtppay = gst_element_factory_make ("rtpvp8pay","rtppay");
	data->sink = gst_element_factory_make ("appsink","sink");
	g_object_set (data->sink, "emit-signals", TRUE, "sync", FALSE, NULL);
	g_signal_connect (data->sink, "new-sample", G_CALLBACK (new_sample), data);
	
	data->filtercaps = gst_caps_new_simple ("video/x-raw", 
				"format", G_TYPE_STRING, "RGB",
				"width", G_TYPE_INT, 480,
				"height", G_TYPE_INT, 320,
				"framerate", GST_TYPE_FRACTION, 30, 1,
				NULL);
	g_print ("filtercaps--> %s\n", gst_caps_to_string(data->filtercaps));
	g_object_set (data->filter, "caps", data->filtercaps, NULL);
	gst_caps_unref (data->filtercaps);
	
	data->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (data->pipeline), source, data->filter, convert, encoder, rtppay, data->sink, NULL);
	if (!gst_element_link_many (source, data->filter, convert, encoder, rtppay, data->sink, NULL)) {
		g_warning ("failed to link elements\n");
	}
	
	g_print ("pipeline created successfully\n");
	gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
	g_main_loop_run (loop);
	
	/* Exit nice and cleanly */
	g_print ("Exiting...\n");
	/* gst_object_unref (data->sink); */
	gst_element_set_state (data->pipeline, GST_STATE_NULL);
	gst_object_unref (GST_OBJECT(data->pipeline));
	g_main_loop_unref (loop);
	g_free (data);
	g_print ("Exiting...\n");
	return 0;
}
