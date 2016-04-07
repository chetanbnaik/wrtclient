/**
 * To compile the websocket cpp code, use the following command
 * g++ echo_client.cpp -I /home/osboxes/client/websocketpp/ \
 * -lboost_system -lboost_random -lpthread -lboost_timer \
 * -lboost_chrono -lrt
 * 
 * */
#include "client.h"

gboolean bus_msg (GstBus * bus, GstMessage * msg, gpointer loop){
	GError * err, * warn;
	gchar * debug_info, * warn_info;
	g_print ("Got %s message\n", GST_MESSAGE_TYPE_NAME (msg));
	
	switch (GST_MESSAGE_TYPE (msg)){
		case GST_MESSAGE_ERROR:
			gst_message_parse_error (msg, &err, &debug_info);
			g_printerr ("Error received from element %s: %s\n",
			  GST_OBJECT_NAME (msg->src), err->message);
			g_printerr ("debugging information: %s\n", debug_info ? debug_info : "none");
			g_clear_error (&err);
			g_free (debug_info);
			
			g_main_loop_quit (loop);
			break;
			
		case GST_MESSAGE_EOS:
			g_print ("End-of-stream reached.\n");
			
			g_main_loop_quit (loop);
			break;
			
		case GST_MESSAGE_WARNING:
			gst_message_parse_warning (msg, &warn, &warn_info);
			g_printerr ("Warning received from element %s: %s\n",
			  GST_OBJECT_NAME (msg->src), warn->message);
			g_printerr ("debugging information: %s\n", warn_info ? warn_info : "none");
			g_clear_error (&err);
			g_free (debug_info);
			break;
			
		default:
			g_printerr ("Unexpected message received.\n");
	}
	return TRUE;
}
