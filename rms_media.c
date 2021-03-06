/*
 * Copyright (C) 2017 Julien Chavanton jchavanton@gmail.com
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "../../core/sr_module.h"
#include "../../core/mem/shm.h"
#include "rms_media.h"
#include "rtp_media_server_call.h"

inline static void* ptr_shm_malloc(size_t size) {
	return shm_malloc(size);
}
inline static void* ptr_shm_realloc(void *ptr, size_t size) {
	return shm_realloc(ptr, size);
}
inline static void ptr_shm_free(void *ptr) {
	shm_free(ptr);
}

int rms_media_init() {
	OrtpMemoryFunctions ortp_memory_functions;
	ortp_memory_functions.malloc_fun = ptr_shm_malloc;
	ortp_memory_functions.realloc_fun = ptr_shm_realloc;
	ortp_memory_functions.free_fun = ptr_shm_free;
	ortp_set_memory_functions(&ortp_memory_functions);
	ortp_init();
	return 1;
}

static MSFactory * rms_create_factory() {
	MSFactory *ms_factory = ms_factory_new_with_voip();
	ms_factory_enable_statistics(ms_factory, TRUE);
	ms_factory_reset_statistics(ms_factory);
	return ms_factory;
}
//	ms_factory_destroy
static MSTicker * rms_create_ticker(char *name) {
	MSTickerParams params;
	params.name = name;
	params.prio = MS_TICKER_PRIO_NORMAL;
	return ms_ticker_new_with_params(&params);
}
//	ms_ticker_destroy(ms_tester_ticker);

void rms_media_destroy() {
//	ms_factory_destroy(ms_factory);
}

int create_call_leg_media(call_leg_media_t *m, str *callid){
	m->ms_factory = rms_create_factory();
	m->callid = callid;
	// create caller RTP session
	m->rtps = ms_create_duplex_rtp_session(m->local_ip, m->local_port, m->local_port+1, ms_factory_get_mtu(m->ms_factory));
	rtp_session_set_remote_addr_full(m->rtps, m->remote_ip, m->remote_port, m->remote_ip, m->remote_port+1);
	rtp_session_set_payload_type(m->rtps, m->pt->type);
	rtp_session_enable_rtcp(m->rtps,FALSE);
	// create caller filters : rtprecv1/rtpsend1/encoder1/decoder1
	m->ms_rtprecv = ms_factory_create_filter(m->ms_factory, MS_RTP_RECV_ID);
	m->ms_rtpsend = ms_factory_create_filter(m->ms_factory, MS_RTP_SEND_ID);
	m->ms_encoder = ms_factory_create_encoder(m->ms_factory, m->pt->mime_type);
	m->ms_decoder = ms_factory_create_decoder(m->ms_factory, m->pt->mime_type);
	/* set filter params */
	ms_filter_call_method(m->ms_rtpsend, MS_RTP_SEND_SET_SESSION, m->rtps);
	ms_filter_call_method(m->ms_rtprecv, MS_RTP_RECV_SET_SESSION, m->rtps);
	return 1;
}

int rms_bridge(call_leg_media_t *m1, call_leg_media_t *m2) {
	MSConnectionHelper h;
	m1->ms_ticker = rms_create_ticker(NULL);

	// direction 1
	ms_connection_helper_start(&h);
	ms_connection_helper_link(&h, m1->ms_rtprecv, -1, 0);
	ms_connection_helper_link(&h, m2->ms_rtpsend, 0, -1);

	// direction 2
	ms_connection_helper_start(&h);
	ms_connection_helper_link(&h, m2->ms_rtprecv, -1, 0);
	ms_connection_helper_link(&h, m1->ms_rtpsend, 0, -1);

	ms_ticker_attach_multiple(m1->ms_ticker, m1->ms_rtprecv, m2->ms_rtprecv, NULL);

	return 1;
}

#define MS_UNUSED(x) ((void)(x))
static void rms_player_eof(void *user_data, MSFilter *f, unsigned int event, void *event_data) {
	if (event == MS_FILE_PLAYER_EOF) {
		call_leg_media_t *m = (call_leg_media_t *) user_data;
		rms_hangup_call(m->callid);
	}
	MS_UNUSED(f), MS_UNUSED(event_data);
}

int rms_playfile(call_leg_media_t *m, char* file_name) {
	MSConnectionHelper h;
	m->ms_ticker = rms_create_ticker(NULL);
	m->ms_player = ms_factory_create_filter(m->ms_factory, MS_FILE_PLAYER_ID);
	//m->ms_recorder = ms_factory_create_filter(m->ms_factory, MS_FILE_PLAYER_ID);
	m->ms_voidsink = ms_factory_create_filter(m->ms_factory, MS_VOID_SINK_ID);
	//ms_filter_add_notify_callback(m->ms_player, (MSFilterNotifyFunc) rms_player_eof, m, TRUE);
	ms_filter_add_notify_callback(m->ms_player, rms_player_eof, m, TRUE);
	ms_filter_call_method(m->ms_player, MS_FILE_PLAYER_OPEN, (void *) file_name);
	int channels = 1;
	ms_filter_call_method(m->ms_player, MS_FILTER_SET_OUTPUT_NCHANNELS, &channels);
	ms_filter_call_method_noarg(m->ms_player, MS_FILE_PLAYER_START);

	// sending graph
	ms_connection_helper_start(&h);
	ms_connection_helper_link(&h, m->ms_player, -1, 0);
	ms_connection_helper_link(&h, m->ms_encoder, 0, 0);
	ms_connection_helper_link(&h, m->ms_rtpsend, 0, -1);
	//ms_ticker_attach(m->ms_ticker, m->ms_player);

	// receiving graph
	ms_connection_helper_start(&h);
	ms_connection_helper_link(&h, m->ms_rtprecv, -1, 0);
	//ms_connection_helper_link(&h, m->ms_decoder, 0, 0);
	ms_connection_helper_link(&h, m->ms_voidsink, 0, -1);

	ms_ticker_attach_multiple(m->ms_ticker, m->ms_player, m->ms_rtprecv, NULL);

	return 1;
}

int rms_stop_bridge(call_leg_media_t *m1, call_leg_media_t *m2) {
	if (!m1->ms_ticker)
		return -1;
	MSConnectionHelper h;
	if (m1->ms_rtpsend) ms_ticker_detach(m1->ms_ticker, m1->ms_rtpsend);
	if (m1->ms_rtprecv) ms_ticker_detach(m1->ms_ticker, m1->ms_rtprecv);
	if (m2->ms_rtpsend) ms_ticker_detach(m1->ms_ticker, m2->ms_rtpsend);
	if (m2->ms_rtprecv) ms_ticker_detach(m1->ms_ticker, m2->ms_rtprecv);
	rtp_stats_display(rtp_session_get_stats(m1->rtps)," AUDIO BRIDGE offer RTP STATISTICS ");
	rtp_stats_display(rtp_session_get_stats(m2->rtps)," AUDIO BRIDGE answer RTP STATISTICS ");
	ms_factory_log_statistics(m1->ms_factory);

	ms_connection_helper_start(&h);
	if (m1->ms_rtprecv) ms_connection_helper_unlink(&h, m1->ms_rtprecv,-1,0);
	if (m2->ms_rtpsend) ms_connection_helper_unlink(&h, m2->ms_rtpsend,0,-1);

	ms_connection_helper_start(&h);
	if (m2->ms_rtprecv) ms_connection_helper_unlink(&h, m2->ms_rtprecv,-1,0);
	if (m1->ms_rtpsend) ms_connection_helper_unlink(&h, m1->ms_rtpsend,0,-1);

	if (m1->ms_rtpsend) ms_filter_destroy(m1->ms_rtpsend);
	if (m1->ms_rtprecv) ms_filter_destroy(m1->ms_rtprecv);
	if (m2->ms_rtpsend) ms_filter_destroy(m2->ms_rtpsend);
	if (m2->ms_rtprecv) ms_filter_destroy(m2->ms_rtprecv);
	return 1;
}

int rms_stop_media(call_leg_media_t *m) {
	if (!m->ms_ticker)
		return -1;
	MSConnectionHelper h;
	if (m->ms_player) ms_ticker_detach(m->ms_ticker, m->ms_player);
	if (m->ms_encoder) ms_ticker_detach(m->ms_ticker, m->ms_encoder);
	if (m->ms_rtpsend) ms_ticker_detach(m->ms_ticker, m->ms_rtpsend);
	if (m->ms_rtprecv) ms_ticker_detach(m->ms_ticker, m->ms_rtprecv);
	if (m->ms_voidsink) ms_ticker_detach(m->ms_ticker, m->ms_voidsink);

	rtp_stats_display(rtp_session_get_stats(m->rtps)," AUDIO SESSION'S RTP STATISTICS ");
	ms_factory_log_statistics(m->ms_factory);

	/*dismantle the sending graph*/
	ms_connection_helper_start(&h);
	if (m->ms_player) ms_connection_helper_unlink(&h, m->ms_player,-1,0);
	if (m->ms_encoder) ms_connection_helper_unlink(&h, m->ms_encoder,0,0);
	if (m->ms_rtpsend) ms_connection_helper_unlink(&h, m->ms_rtpsend,0,-1);
	/*dismantle the receiving graph*/
	ms_connection_helper_start(&h);
	if (m->ms_rtprecv) ms_connection_helper_unlink(&h, m->ms_rtprecv,-1,0);
	if (m->ms_voidsink) ms_connection_helper_unlink(&h, m->ms_voidsink,0,-1);

	if (m->ms_player) ms_filter_destroy(m->ms_player);
	if (m->ms_encoder) ms_filter_destroy(m->ms_encoder);
	if (m->ms_rtpsend) ms_filter_destroy(m->ms_rtpsend);
	if (m->ms_rtprecv) ms_filter_destroy(m->ms_rtprecv);
	if (m->ms_voidsink) ms_filter_destroy(m->ms_voidsink);
	return 1;
}
