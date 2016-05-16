/*
 * gstreamer pipeline:
 * gst-launch-1.0 videotestsrc ! 
 * video/x-raw,width=320,height=240,framerate=30/1 ! 
 * vp8enc ! rtpvp8pay ! appsink
 * 
 * */
#include <gst/gst.h>
#include <glib.h>

typedef struct ps_gstreamer_rtp_source {
	GstElement * pipeline, * sink;
} ps_gstreamer_rtp_source;

static GstFlowReturn new_sample (GstElement * appsink, ps_gstreamer_rtp_source * source) {
	GstCaps * caps;
	GstSample * sample = NULL;
	
	g_signal_emit_by_name (appsink, "pull-sample", &sample, NULL);
	if (sample) {
		caps = gst_sample_get_caps (sample);
		g_print ("CAPS: %s\n", gst_caps_to_string(caps));
		gst_caps_unref (caps);
		gst_sample_unref (sample);
	} else {
		g_warning ("Sample not received\n");
	}
		
	return GST_FLOW_OK;
}

gint main (int argc, char * argv[]) {
	ps_gstreamer_rtp_source * data;
	GstElement * source, * filter;
	GstCaps * filtercaps;
	GstElement * convert, * encoder, * rtppay;
	GstBus * bus;
	GMainLoop * loop;
	loop = g_main_loop_new (NULL, FALSE);
	
	gst_init (&argc, &argv);
	data = (ps_gstreamer_rtp_source *)g_malloc0(sizeof(ps_gstreamer_rtp_source));
	
	source = gst_element_factory_make ("videotestsrc","source");
	filter = gst_element_factory_make ("capsfilter","filter");
	convert = gst_element_factory_make ("videoconvert", "convert");
	encoder = gst_element_factory_make ("vp8enc","encoder");
	rtppay = gst_element_factory_make ("rtpvp8pay","rtppay");
	data->sink = gst_element_factory_make ("appsink","sink");
	g_object_set (data->sink, "emit-signals", TRUE, "sync", FALSE, NULL);
	
	filtercaps = gst_caps_new_simple ("video/x-raw", 
				"format", G_TYPE_STRING, "RGB",
				"width", G_TYPE_INT, 480,
				"height", G_TYPE_INT, 320,
				"framerate", GST_TYPE_FRACTION, 30, 1,
				NULL);
	g_object_set (filter, "caps", filtercaps, NULL);
	gst_caps_unref (filtercaps);
	
	data->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (data->pipeline), source, filter, convert, encoder, rtppay, data->sink, NULL);
	if (!gst_element_link_many (source, filter, convert, encoder, rtppay, data->sink, NULL)) {
		g_warning ("failed to link elements\n");
	}
	g_signal_connect (data->sink, "new-sample", G_CALLBACK (new_sample), &data);
	
	
	g_print ("pipeline created successfully\n");
	gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
	g_main_loop_run (loop);
	
	/* Exit nice and cleanly */
	gst_object_unref (data->sink);
	gst_element_set_state (data->pipeline, GST_STATE_NULL);
	gst_object_unref (GST_OBJECT(data->pipeline));
	g_main_loop_unref (loop);
	g_print ("Exiting...\n");
	return 0;
}
