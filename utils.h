#ifndef __PS_UTILS_H__
#define __PS_UTILS_H__

#include <stdint.h>
#include <glib.h>
#include <netinet/in.h>


gint64 ps_get_monotonic_time(void);


gint64 ps_get_real_time(void);


char *ps_string_replace(char *message, const char *old_string, const char *new_string) G_GNUC_WARN_UNUSED_RESULT;


gboolean ps_is_true(const char *value);


gboolean ps_strcmp_const_time(const void *str1, const void *str2);


typedef uint32_t ps_flags;


void ps_flags_reset(ps_flags *flags);


void ps_flags_set(ps_flags *flags, uint32_t flag);


void ps_flags_clear(ps_flags *flags, uint32_t flag);


gboolean ps_flags_is_set(ps_flags *flags, uint32_t flag);

int ps_mkdir(const char *dir, mode_t mode);


int ps_get_opus_pt(const char *sdp);


int ps_get_isac32_pt(const char *sdp);


int ps_get_isac16_pt(const char *sdp);


int ps_get_pcmu_pt(const char *sdp);


int ps_get_pcma_pt(const char *sdp);


int ps_get_vp8_pt(const char *sdp);


int ps_get_vp9_pt(const char *sdp);

int ps_get_h264_pt(const char *sdp);

gboolean ps_is_ip_valid(const char *ip, int *family);

char *ps_address_to_ip(struct sockaddr *address);

int ps_pidfile_create(const char *file);

int ps_pidfile_remove(void);
#endif
