/*
 * gstreamer pipeline:
 * gst-launch-1.0 videotestsrc ! 
 * video/x-raw,format=RGB,width=320,height=240,framerate=30/1 ! 
 * videoconvert ! vp8enc ! rtpvp8pay ! appsink
 * 
 * */
#include <gst/gst.h>
#include <inttypes.h>
#include <gst/app/gstappsink.h>

#include "../rtp.h"

#define PS_GSTREAMER_VP8  0
#define PS_GSTREAMER_H264 1
#define PS_GSTREAMER_VP9  2

static gboolean ps_gstreamer_is_keyframe (gint codec, char * buffer, int len);

typedef struct ps_gstreamer_rtp_source {
	GstElement * pipeline, * sink, * filter;
	GstCaps * filtercaps;
} ps_gstreamer_rtp_source;

static GstFlowReturn new_sample (GstElement * appsink, ps_gstreamer_rtp_source * source) {
	//GstCaps * caps;
	GstSample * sample;
	GstBuffer * buffer;
	GstMapInfo * map;
	gpointer framedata;
	gsize dest_size;
	
	sample = gst_app_sink_pull_sample (GST_APP_SINK(source->sink));
	//g_signal_emit_by_name (appsink, "pull-sample", &sample, NULL);
	if (sample != NULL) {
		//caps = gst_sample_get_caps (sample);
		//g_print ("CAPS: %s\n", gst_caps_to_string(caps));
		buffer = gst_sample_get_buffer (sample);
		gst_buffer_extract_dup (buffer, 0, -1, &framedata, &dest_size);
		//rtp_header * rtp = (rtp_header *) framedata;
		if (ps_gstreamer_is_keyframe (PS_GSTREAMER_VP8, framedata, dest_size)) {
			g_print ("Key frame received\n");
		}
		/*g_print ("framedata ssrc -> %u\n", rtp->ssrc);
		g_print ("framedata type -> %u\n", rtp->type);*/
		g_print ("Buffer len: %d, gsize: %"G_GSIZE_FORMAT"\n", gst_buffer_get_size (buffer), dest_size);
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
		
		g_free (framedata);
		gst_sample_unref (sample);
	} else {
		g_warning ("Sample not received\n");
	}
		
	return GST_FLOW_OK;
}

/* Helpers to check if frame is a key frame (see post processor code) */
#if defined(__ppc__) || defined(__ppc64__)
	# define swap2(d)  \
	((d&0x000000ff)<<8) |  \
	((d&0x0000ff00)>>8)
#else
	# define swap2(d) d
#endif

static gboolean ps_gstreamer_is_keyframe (gint codec, char * buffer, int len) {
	if(codec == PS_GSTREAMER_VP8) {
		/* VP8 packet */
		if(!buffer || len < 28)
			return FALSE;
		/* Parse RTP header first */
		rtp_header *header = (rtp_header *)buffer;
		guint32 timestamp = ntohl(header->timestamp);
		guint16 seq = ntohs(header->seq_number);
		/*g_print("Checking if VP8 packet (size=%d, seq=%"SCNu16", ts=%"SCNu32") is a key frame...\n",
			len, seq, timestamp);*/
		uint16_t skip = 0;
		if(header->extension) {
			janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buffer+12);
			g_print("  -- RTP extension found (type=%"SCNu16", length=%"SCNu16")\n",
				ntohs(ext->type), ntohs(ext->length));
			skip = 4 + ntohs(ext->length)*4;
		}
		buffer += 12+skip;
		
		/* Parse VP8 header now */
		uint8_t vp8pd = *buffer;
		//g_print ("vp8pd ->%d\n", vp8pd);
		uint8_t xbit = (vp8pd & 0x80);
		//g_print ("xbit ->%"PRIu8"\n", xbit);
		uint8_t sbit = (vp8pd & 0x10);
		if(xbit) {
			g_print("  -- X bit is set!\n");
			/* Read the Extended control bits octet */
			buffer++;
			vp8pd = *buffer;
			uint8_t ibit = (vp8pd & 0x80);
			uint8_t lbit = (vp8pd & 0x40);
			uint8_t tbit = (vp8pd & 0x20);
			uint8_t kbit = (vp8pd & 0x10);
			if(ibit) {
				g_print("  -- I bit is set!\n");
				/* Read the PictureID octet */
				buffer++;
				vp8pd = *buffer;
				uint16_t picid = vp8pd, wholepicid = picid;
				uint8_t mbit = (vp8pd & 0x80);
				if(mbit) {
					g_print("  -- M bit is set!\n");
					memcpy(&picid, buffer, sizeof(uint16_t));
					wholepicid = ntohs(picid);
					picid = (wholepicid & 0x7FFF);
					buffer++;
				}
				g_print("  -- -- PictureID: %"SCNu16"\n", picid);
			}
			if(lbit) {
				g_print("  -- L bit is set!\n");
				/* Read the TL0PICIDX octet */
				buffer++;
				vp8pd = *buffer;
			}
			if(tbit || kbit) {
				g_print("  -- T/K bit is set!\n");
				/* Read the TID/KEYIDX octet */
				buffer++;
				vp8pd = *buffer;
			}
			buffer++;	/* Now we're in the payload */
			if(sbit) {
				g_print("  -- S bit is set!\n");
				unsigned long int vp8ph = 0;
				memcpy(&vp8ph, buffer, 4);
				vp8ph = ntohl(vp8ph);
				uint8_t pbit = ((vp8ph & 0x01000000) >> 24);
				if(!pbit) {
					g_print("  -- P bit is NOT set!\n");
					/* It is a key frame! Get resolution for debugging */
					unsigned char *c = (unsigned char *)buffer+3;
					/* vet via sync code */
					if(c[0]!=0x9d||c[1]!=0x01||c[2]!=0x2a) {
						g_print("First 3-bytes after header not what they're supposed to be?\n");
					} else {
						int vp8w = swap2(*(unsigned short*)(c+3))&0x3fff;
						int vp8ws = swap2(*(unsigned short*)(c+3))>>14;
						int vp8h = swap2(*(unsigned short*)(c+5))&0x3fff;
						int vp8hs = swap2(*(unsigned short*)(c+5))>>14;
						g_print("Got a VP8 key frame: %dx%d (scale=%dx%d)\n", vp8w, vp8h, vp8ws, vp8hs);
						return TRUE;
					}
				}
			}
		}
		/* If we got here it's not a key frame */
		return FALSE;
	} else if(codec == PS_GSTREAMER_H264) {
		/* Parse RTP header first */
		rtp_header *header = (rtp_header *)buffer;
		guint32 timestamp = ntohl(header->timestamp);
		guint16 seq = ntohs(header->seq_number);
		g_print("Checking if H264 packet (size=%d, seq=%"SCNu16", ts=%"SCNu32") is a key frame...\n",
			len, seq, timestamp);
		uint16_t skip = 0;
		if(header->extension) {
			janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buffer+12);
			g_print("  -- RTP extension found (type=%"SCNu16", length=%"SCNu16")\n",
				ntohs(ext->type), ntohs(ext->length));
			skip = 4 + ntohs(ext->length)*4;
		}
		buffer += 12+skip;
		/* Parse H264 header now */
		uint8_t fragment = *buffer & 0x1F;
		uint8_t nal = *(buffer+1) & 0x1F;
		uint8_t start_bit = *(buffer+1) & 0x80;
		g_print("Fragment=%d, NAL=%d, Start=%d\n", fragment, nal, start_bit);
		if(fragment == 5 ||
				((fragment == 28 || fragment == 29) && nal == 5 && start_bit == 128)) {
			g_print("Got an H264 key frame\n");
			return TRUE;
		}
		/* If we got here it's not a key frame */
		return FALSE;
	} else {
		/* FIXME Not a clue */
		return FALSE;
	}
}

gint main (int argc, char * argv[]) {
	ps_gstreamer_rtp_source * data = NULL;
	GstElement * source, * videorate, * videoscale;
	GstElement * convert, * encoder, * rtppay;
	GstBus * bus;
	GMainLoop * loop;
	loop = g_main_loop_new (NULL, FALSE);
	
	gst_init (&argc, &argv);
	data = (ps_gstreamer_rtp_source *)g_malloc0(sizeof(ps_gstreamer_rtp_source));
	char * vsrcname = "videotestsrc";
	source = gst_element_factory_make (vsrcname,"source");
	videorate = gst_element_factory_make ("videorate", "videorate");
	data->filter = gst_element_factory_make ("capsfilter","filter");
	videoscale = gst_element_factory_make ("videoscale", "videoscale");
	convert = gst_element_factory_make ("videoconvert", "convert");
	encoder = gst_element_factory_make ("vp8enc","encoder");
	rtppay = gst_element_factory_make ("rtpvp8pay","rtppay");
	data->sink = gst_element_factory_make ("appsink","sink");
	g_object_set (data->sink, "emit-signals", TRUE, "sync", FALSE, NULL);
	g_object_set (encoder, "error-resilient", TRUE, NULL);
	g_object_set (encoder, "keyframe-max-dist", 30, NULL);
	g_object_set (encoder, "keyframe-mode", 0, NULL);
	g_signal_connect (data->sink, "new-sample", G_CALLBACK (new_sample), data);
	
	data->filtercaps = gst_caps_new_simple ("video/x-raw", 
				"format", G_TYPE_STRING, "RGB",
				"width", G_TYPE_INT, 480,
				"height", G_TYPE_INT, 320,
				"framerate", GST_TYPE_FRACTION, 15, 1,
				NULL);
	g_print ("filtercaps--> %s\n", gst_caps_to_string(data->filtercaps));
	g_object_set (data->filter, "caps", data->filtercaps, NULL);
	gst_caps_unref (data->filtercaps);
	
	data->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (data->pipeline), source, data->filter, videoscale, videorate, convert, encoder, rtppay, data->sink, NULL);
	if (!gst_element_link_many (source, data->filter, videoscale, videorate, convert, encoder, rtppay, data->sink, NULL)) {
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
