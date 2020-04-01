/* GSM Mobile Radio Interface Layer 3 messages on the A-bis interface
 * 3GPP TS 04.08 version 7.21.0 Release 1998 / ETSI TS 100 940 V7.21.0 */

/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2008-2012 by Holger Hans Peter Freyther <zecke@selfish.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>
#include <regex.h>
#include <sys/types.h>

#include "bscconfig.h"

#include <openbsc/auth.h>
#include <openbsc/db.h>
#include <openbsc/debug.h>
#include <openbsc/gsm_data.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/gsm_04_11.h>
#include <openbsc/gsm_04_08.h>
#include <openbsc/gsm_04_80.h>
#include <openbsc/gsm_04_14.h>
#include <openbsc/abis_rsl.h>
#include <openbsc/chan_alloc.h>
#include <openbsc/paging.h>
#include <openbsc/signal.h>
#include <osmocom/abis/trau_frame.h>
#include <openbsc/trau_mux.h>
#include <openbsc/rtp_proxy.h>
#include <openbsc/transaction.h>
#include <openbsc/ussd.h>
#include <openbsc/silent_call.h>
#include <openbsc/bsc_api.h>
#include <openbsc/osmo_msc.h>
#include <openbsc/handover.h>
#include <openbsc/mncc_int.h>
#include <osmocom/abis/e1_input.h>
#include <osmocom/core/bitvec.h>

#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/gsm0480.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/tlv.h>

#include <assert.h>
#include "server.h"
#include "hex.h"
#include "client.h"
void *tall_locop_ctx;
void *tall_authciphop_ctx;

static int tch_rtp_signal(struct gsm_lchan *lchan, int signal);

static int gsm0408_loc_upd_acc(struct gsm_subscriber_connection *conn);
static int gsm48_tx_simple(struct gsm_subscriber_connection *conn,
			   uint8_t pdisc, uint8_t msg_type);
static void schedule_reject(struct gsm_subscriber_connection *conn);
static void release_anchor(struct gsm_subscriber_connection *conn);

static int apply_codec_restrictions(struct gsm_bts *bts,
	struct gsm_mncc_bearer_cap *bcap)
{
	int i, j;

	/* remove unsupported speech versions from list */
	for (i = 0, j = 0; bcap->speech_ver[i] >= 0; i++) {
		if (bcap->speech_ver[i] == GSM48_BCAP_SV_FR)
			bcap->speech_ver[j++] = GSM48_BCAP_SV_FR;
		if (bcap->speech_ver[i] == GSM48_BCAP_SV_EFR && bts->codec.efr)
			bcap->speech_ver[j++] = GSM48_BCAP_SV_EFR;
		if (bcap->speech_ver[i] == GSM48_BCAP_SV_AMR_F && bts->codec.amr)
			bcap->speech_ver[j++] = GSM48_BCAP_SV_AMR_F;
		if (bcap->speech_ver[i] == GSM48_BCAP_SV_HR && bts->codec.hr)
			bcap->speech_ver[j++] = GSM48_BCAP_SV_HR;
		if (bcap->speech_ver[i] == GSM48_BCAP_SV_AMR_H && bts->codec.amr)
			bcap->speech_ver[j++] = GSM48_BCAP_SV_AMR_H;
	}
	bcap->speech_ver[j] = -1;

	return 0;
}

static uint32_t new_callref = 0x80000001;

void cc_tx_to_mncc(struct gsm_network *net, struct msgb *msg)
{
	net->mncc_recv(net, msg);
}

static int gsm48_conn_sendmsg(struct msgb *msg, struct gsm_subscriber_connection *conn,
			      struct gsm_trans *trans)
{
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msg->data;

	/* if we get passed a transaction reference, do some common
	 * work that the caller no longer has to do */
	if (trans) {
		gh->proto_discr = trans->protocol | (trans->transaction_id << 4);
		msg->lchan = trans->conn->lchan;
	}

	if (msg->lchan) {
		struct e1inp_sign_link *sign_link =
				msg->lchan->ts->trx->rsl_link;

		msg->dst = sign_link;
		if (gsm48_hdr_pdisc(gh) == GSM48_PDISC_CC)
			DEBUGP(DCC, "(bts %d trx %d ts %d ti %02x) "
				"Sending '%s' to MS.\n",
				sign_link->trx->bts->nr,
				sign_link->trx->nr, msg->lchan->ts->nr,
				gh->proto_discr & 0xf0,
				gsm48_cc_msg_name(gh->msg_type));
		else
			DEBUGP(DCC, "(bts %d trx %d ts %d pd %02x) "
				"Sending 0x%02x to MS.\n",
				sign_link->trx->bts->nr,
				sign_link->trx->nr, msg->lchan->ts->nr,
				gh->proto_discr, gh->msg_type);
	}

	return gsm0808_submit_dtap(conn, msg, 0, 0);
}

int gsm48_cc_tx_notify_ss(struct gsm_trans *trans, const char *message)
{
	struct gsm48_hdr *gh;
	struct msgb *ss_notify;

	ss_notify = gsm0480_create_notifySS(message);
	if (!ss_notify)
		return -1;

	gsm0480_wrap_invoke(ss_notify, GSM0480_OP_CODE_NOTIFY_SS, 0);
	uint8_t *data = msgb_push(ss_notify, 1);
	data[0] = ss_notify->len - 1;
	gh = (struct gsm48_hdr *) msgb_push(ss_notify, sizeof(*gh));
	gh->msg_type = GSM48_MT_CC_FACILITY;
	return gsm48_conn_sendmsg(ss_notify, trans->conn, trans);
}

void release_security_operation(struct gsm_subscriber_connection *conn)
{
	if (!conn->sec_operation)
		return;

	talloc_free(conn->sec_operation);
	conn->sec_operation = NULL;
	msc_release_connection(conn);
}

void allocate_security_operation(struct gsm_subscriber_connection *conn)
{
	conn->sec_operation = talloc_zero(tall_authciphop_ctx,
	                                  struct gsm_security_operation);
}

int gsm48_secure_channel(struct gsm_subscriber_connection *conn, int key_seq,
                         gsm_cbfn *cb, void *cb_data)
{
	struct gsm_network *net = conn->network;
	struct gsm_subscriber *subscr = conn->subscr;
	struct gsm_security_operation *op;
	struct gsm_auth_tuple atuple;
	int status = -1, rc;

	/* Check if we _can_ enable encryption. Cases where we can't:
	 *  - Encryption disabled in config
	 *  - Channel already secured (nothing to do)
	 *  - Subscriber equipment doesn't support configured encryption
	 */
	if (!net->a5_encryption) {
		status = GSM_SECURITY_NOAVAIL;
	} else if (conn->lchan->encr.alg_id > RSL_ENC_ALG_A5(0)) {
		DEBUGP(DMM, "Requesting to secure an already secure channel");
		status = GSM_SECURITY_ALREADY;
	} else if (!ms_cm2_a5n_support(subscr->equipment.classmark2,
	                               net->a5_encryption)) {
		DEBUGP(DMM, "Subscriber equipment doesn't support requested encryption");
		status = GSM_SECURITY_NOAVAIL;
	}

	/* If not done yet, try to get info for this user */
	if (status < 0) {
		rc = auth_get_tuple_for_subscr(&atuple, subscr, key_seq);
		if (rc <= 0)
			status = GSM_SECURITY_NOAVAIL;
	}

	/* Are we done yet ? */
	if (status >= 0)
		return cb ?
			cb(GSM_HOOK_RR_SECURITY, status, NULL, conn, cb_data) :
			0;

	/* Start an operation (can't have more than one pending !!!) */
	if (conn->sec_operation)
		return -EBUSY;

	allocate_security_operation(conn);
	op = conn->sec_operation;
	op->cb = cb;
	op->cb_data = cb_data;
	memcpy(&op->atuple, &atuple, sizeof(struct gsm_auth_tuple));

		/* FIXME: Should start a timer for completion ... */

	/* Then do whatever is needed ... */
	if (rc == AUTH_DO_AUTH_THEN_CIPH) {
		/* Start authentication */
		return gsm48_tx_mm_auth_req(conn, op->atuple.vec.rand, NULL,
					    op->atuple.key_seq);
	} else if (rc == AUTH_DO_CIPH) {
		/* Start ciphering directly */
		return gsm0808_cipher_mode(conn, net->a5_encryption,
		                           op->atuple.vec.kc, 8, 0);
	}

	return -EINVAL; /* not reached */
}

static bool subscr_regexp_check(const struct gsm_network *net, const char *imsi)
{
	if (!net->authorized_reg_str)
		return false;

	if (regexec(&net->authorized_regexp, imsi, 0, NULL, 0) != REG_NOMATCH)
		return true;

	return false;
}

static int authorize_subscriber(struct gsm_loc_updating_operation *loc,
				struct gsm_subscriber *subscriber)
{
	if (!subscriber)
		return 0;

	/*
	 * Do not send accept yet as more information should arrive. Some
	 * phones will not send us the information and we will have to check
	 * what we want to do with that.
	 */
	if (loc && (loc->waiting_for_imsi || loc->waiting_for_imei))
		return 0;

	switch (subscriber->group->net->auth_policy) {
	case GSM_AUTH_POLICY_CLOSED:
		return subscriber->authorized;
	case GSM_AUTH_POLICY_REGEXP:
		if (subscriber->authorized)
			return 1;
		if (subscr_regexp_check(subscriber->group->net,
					subscriber->imsi))
			subscriber->authorized = 1;
		return subscriber->authorized;
	case GSM_AUTH_POLICY_TOKEN:
		if (subscriber->authorized)
			return subscriber->authorized;
		return (subscriber->flags & GSM_SUBSCRIBER_FIRST_CONTACT);
	case GSM_AUTH_POLICY_ACCEPT_ALL:
		return 1;
	default:
		return 0;
	}
}

static void _release_loc_updating_req(struct gsm_subscriber_connection *conn, int release)
{
	if (!conn->loc_operation)
		return;

	/* No need to keep the connection up */
	release_anchor(conn);

	osmo_timer_del(&conn->loc_operation->updating_timer);
	talloc_free(conn->loc_operation);
	conn->loc_operation = NULL;
	if (release)
		msc_release_connection(conn);
}

static void loc_updating_failure(struct gsm_subscriber_connection *conn, int release)
{
	if (!conn->loc_operation)
		return;
	LOGP(DMM, LOGL_ERROR, "Location Updating failed for %s\n",
	     subscr_name(conn->subscr));
	rate_ctr_inc(&conn->network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_FAILED]);
	_release_loc_updating_req(conn, release);
}

static void loc_updating_success(struct gsm_subscriber_connection *conn, int release)
{
	if (!conn->loc_operation)
		return;
	LOGP(DMM, LOGL_INFO, "Location Updating completed for %s\n",
	     subscr_name(conn->subscr));
	rate_ctr_inc(&conn->network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_COMPLETED]);
	_release_loc_updating_req(conn, release);
}

static void allocate_loc_updating_req(struct gsm_subscriber_connection *conn)
{
	if (conn->loc_operation)
		LOGP(DMM, LOGL_ERROR, "Connection already had operation.\n");
	loc_updating_failure(conn, 0);

	conn->loc_operation = talloc_zero(tall_locop_ctx,
					   struct gsm_loc_updating_operation);
}

static int finish_lu(struct gsm_subscriber_connection *conn)
{
	int rc = 0;
	int avoid_tmsi = conn->network->avoid_tmsi;

	/* We're all good */
	if (avoid_tmsi) {
		conn->subscr->tmsi = GSM_RESERVED_TMSI;
		db_sync_subscriber(conn->subscr);
	} else {
		db_subscriber_alloc_tmsi(conn->subscr);
	}

	rc = gsm0408_loc_upd_acc(conn);
	if (conn->network->send_mm_info) {
		/* send MM INFO with network name */
		rc = gsm48_tx_mm_info(conn);
	}

	/* call subscr_update after putting the loc_upd_acc
	 * in the transmit queue, since S_SUBSCR_ATTACHED might
	 * trigger further action like SMS delivery */
	subscr_update(conn->subscr, conn->bts,
		      GSM_SUBSCRIBER_UPDATE_ATTACHED);

	/*
	 * The gsm0408_loc_upd_acc sends a MI with the TMSI. The
	 * MS needs to respond with a TMSI REALLOCATION COMPLETE
	 * (even if the TMSI is the same).
	 * If avoid_tmsi == true, we don't send a TMSI, we don't
	 * expect a reply and Location Updating is done.
	 */
	if (avoid_tmsi)
		loc_updating_success(conn, 1);

	return rc;
}

static int _gsm0408_authorize_sec_cb(unsigned int hooknum, unsigned int event,
                                     struct msgb *msg, void *data, void *param)
{
	struct gsm_subscriber_connection *conn = data;
	int rc = 0;

	switch (event) {
		case GSM_SECURITY_AUTH_FAILED:
			loc_updating_failure(conn, 1);
			break;

		case GSM_SECURITY_ALREADY:
			LOGP(DMM, LOGL_ERROR, "We don't expect LOCATION "
				"UPDATING after CM SERVICE REQUEST\n");
			/* fall through */

		case GSM_SECURITY_NOAVAIL:
		case GSM_SECURITY_SUCCEEDED:
			rc = finish_lu(conn);
			break;

		default:
			rc = -EINVAL;
	};

	return rc;
}

static int gsm0408_authorize(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	if (!conn->loc_operation)
		return 0;

	if (authorize_subscriber(conn->loc_operation, conn->subscr))
		return gsm48_secure_channel(conn,
			conn->loc_operation->key_seq,
			_gsm0408_authorize_sec_cb, NULL);
	return 0;
}

void gsm0408_clear_request(struct gsm_subscriber_connection *conn, uint32_t cause)
{
	struct gsm_trans *trans, *temp;

	/* avoid someone issuing a clear */
	conn->in_release = 1;

	/*
	 * Cancel any outstanding location updating request
	 * operation taking place on the subscriber connection.
	 */
	loc_updating_failure(conn, 0);

	/* We might need to cancel the paging response or such. */
	if (conn->sec_operation && conn->sec_operation->cb) {
		conn->sec_operation->cb(GSM_HOOK_RR_SECURITY, GSM_SECURITY_AUTH_FAILED,
					NULL, conn, conn->sec_operation->cb_data);
	}

	release_security_operation(conn);
	release_anchor(conn);

	/*
	 * Free all transactions that are associated with the released
	 * connection. The transaction code will inform the CC or SMS
	 * facilities that will send the release indications. As part of
	 * the CC REL_IND the remote leg might be released and this will
	 * trigger the call to trans_free. This is something the llist
	 * macro can not handle and we will need to re-iterate the list.
	 *
	 * TODO: Move the trans_list into the subscriber connection and
	 * create a pending list for MT transactions. These exist before
	 * we have a subscriber connection.
	 */
restart:
	llist_for_each_entry_safe(trans, temp, &conn->network->trans_list, entry) {
		if (trans->conn == conn) {
			trans_free(trans);
			goto restart;
		}
	}
}

void gsm0408_clear_all_trans(struct gsm_network *net, int protocol)
{
	struct gsm_trans *trans, *temp;

	LOGP(DCC, LOGL_NOTICE, "Clearing all currently active transactions!!!\n");

	llist_for_each_entry_safe(trans, temp, &net->trans_list, entry) {
		if (trans->protocol == protocol) {
			trans->callref = 0;
			trans_free(trans);
		}
	}
}

/* Chapter 9.2.14 : Send LOCATION UPDATING REJECT */
int gsm0408_loc_upd_rej(struct gsm_subscriber_connection *conn, uint8_t cause)
{
	struct gsm_bts *bts = conn->bts;
	struct msgb *msg;

	msg = gsm48_create_loc_upd_rej(cause);
	if (!msg) {
		LOGP(DMM, LOGL_ERROR, "Failed to create msg for LOCATION UPDATING REJECT.\n");
		return -1;
	}

	msg->lchan = conn->lchan;

	LOGP(DMM, LOGL_INFO, "Subscriber %s: LOCATION UPDATING REJECT "
	     "LAC=%u BTS=%u\n", subscr_name(conn->subscr),
	     bts->location_area_code, bts->nr);

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

/* Chapter 9.2.13 : Send LOCATION UPDATE ACCEPT */
static int gsm0408_loc_upd_acc(struct gsm_subscriber_connection *conn)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 LOC UPD ACC");
	struct gsm48_hdr *gh;
	struct gsm48_loc_area_id *lai;
	uint8_t *mid;

	msg->lchan = conn->lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_LOC_UPD_ACCEPT;

	lai = (struct gsm48_loc_area_id *) msgb_put(msg, sizeof(*lai));
	gsm48_generate_lai2(lai, bts_lai(conn->bts));

	if (conn->subscr->tmsi == GSM_RESERVED_TMSI) {
		uint8_t mi[10];
		int len;
		len = gsm48_generate_mid_from_imsi(mi, conn->subscr->imsi);
		mid = msgb_put(msg, len);
		memcpy(mid, mi, len);
	} else {
		mid = msgb_put(msg, GSM48_MID_TMSI_LEN);
		gsm48_generate_mid_from_tmsi(mid, conn->subscr->tmsi);
	}

	DEBUGP(DMM, "-> LOCATION UPDATE ACCEPT\n");

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

/* Transmit Chapter 9.2.10 Identity Request */
static int mm_tx_identity_req(struct gsm_subscriber_connection *conn, uint8_t id_type)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 ID REQ");
	struct gsm48_hdr *gh;

	msg->lchan = conn->lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh) + 1);
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_ID_REQ;
	gh->data[0] = id_type;

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

static struct gsm_subscriber *subscr_create(const struct gsm_network *net,
					    const char *imsi)
{
	if (!net->auto_create_subscr)
		return NULL;

	if (!subscr_regexp_check(net, imsi))
		return NULL;

	return subscr_create_subscriber(net->subscr_group, imsi);
}

/* Parse Chapter 9.2.11 Identity Response */
static int mm_rx_id_resp(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm_network *net = conn->network;
	uint8_t mi_type = gh->data[1] & GSM_MI_TYPE_MASK;
	char mi_string[GSM48_MI_SIZE];

	gsm48_mi_to_string(mi_string, sizeof(mi_string), &gh->data[1], gh->data[0]);
	DEBUGP(DMM, "IDENTITY RESPONSE: MI(%s)=%s\n",
		gsm48_mi_type_name(mi_type), mi_string);

	osmo_signal_dispatch(SS_SUBSCR, S_SUBSCR_IDENTITY, gh->data);

	switch (mi_type) {
	case GSM_MI_TYPE_IMSI:
		/* look up subscriber based on IMSI, create if not found */
		if (!conn->subscr) {
			conn->subscr = subscr_get_by_imsi(net->subscr_group,
							  mi_string);
			if (!conn->subscr)
				conn->subscr = subscr_create(net, mi_string);
		}
		if (!conn->subscr && conn->loc_operation) {
			gsm0408_loc_upd_rej(conn, net->reject_cause);
			loc_updating_failure(conn, 1);
			return 0;
		}
		if (conn->loc_operation)
			conn->loc_operation->waiting_for_imsi = 0;
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		/* update subscribe <-> IMEI mapping */
		if (conn->subscr) {
			db_subscriber_assoc_imei(conn->subscr, mi_string);
			db_sync_equipment(&conn->subscr->equipment);
		}
		if (conn->loc_operation)
			conn->loc_operation->waiting_for_imei = 0;
		break;
	}

	/* Check if we can let the mobile station enter */
	return gsm0408_authorize(conn, msg);
}


static void loc_upd_rej_cb(void *data)
{
	struct gsm_subscriber_connection *conn = data;

	LOGP(DMM, LOGL_DEBUG, "Location Updating Request procedure timedout.\n");
	gsm0408_loc_upd_rej(conn, conn->network->reject_cause);
	loc_updating_failure(conn, 1);
}

static void schedule_reject(struct gsm_subscriber_connection *conn)
{
	osmo_timer_setup(&conn->loc_operation->updating_timer, loc_upd_rej_cb,
			 conn);
	osmo_timer_schedule(&conn->loc_operation->updating_timer, 5, 0);
}

static const struct value_string lupd_names[] = {
	{ GSM48_LUPD_NORMAL, "NORMAL" },
	{ GSM48_LUPD_PERIODIC, "PERIODIC" },
	{ GSM48_LUPD_IMSI_ATT, "IMSI ATTACH" },
	{ 0, NULL }
};

/* Chapter 9.2.15: Receive Location Updating Request */
static int mm_rx_loc_upd_req(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_loc_upd_req *lu;
	struct gsm_subscriber *subscr = NULL;
	uint8_t mi_type;
	char mi_string[GSM48_MI_SIZE];

 	lu = (struct gsm48_loc_upd_req *) gh->data;

	mi_type = lu->mi[0] & GSM_MI_TYPE_MASK;

	gsm48_mi_to_string(mi_string, sizeof(mi_string), lu->mi, lu->mi_len);

	DEBUGPC(DMM, "MI(%s)=%s type=%s ", gsm48_mi_type_name(mi_type),
		mi_string, get_value_string(lupd_names, lu->type));

	osmo_signal_dispatch(SS_SUBSCR, S_SUBSCR_IDENTITY, &lu->mi_len);

	switch (lu->type) {
	case GSM48_LUPD_NORMAL:
		rate_ctr_inc(&conn->network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_TYPE_NORMAL]);
		break;
	case GSM48_LUPD_IMSI_ATT:
		rate_ctr_inc(&conn->network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_TYPE_ATTACH]);
		break;
	case GSM48_LUPD_PERIODIC:
		rate_ctr_inc(&conn->network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_TYPE_PERIODIC]);
		break;
	}

	/*
	 * Pseudo Spoof detection: Just drop a second/concurrent
	 * location updating request.
	 */
	if (conn->loc_operation) {
		DEBUGPC(DMM, "ignoring request due an existing one: %p.\n",
			conn->loc_operation);
		gsm0408_loc_upd_rej(conn, GSM48_REJECT_PROTOCOL_ERROR);
		return 0;
	}

	allocate_loc_updating_req(conn);

	conn->loc_operation->key_seq = lu->key_seq;

	switch (mi_type) {
	case GSM_MI_TYPE_IMSI:
		DEBUGPC(DMM, "\n");
		/* we always want the IMEI, too */
		mm_tx_identity_req(conn, GSM_MI_TYPE_IMEI);
		conn->loc_operation->waiting_for_imei = 1;

		/* look up subscriber based on IMSI, create if not found */
		subscr = subscr_get_by_imsi(conn->network->subscr_group, mi_string);
		if (!subscr)
			subscr = subscr_create(conn->network, mi_string);
		if (!subscr) {
			gsm0408_loc_upd_rej(conn, conn->network->reject_cause);
			loc_updating_failure(conn, 0); /* FIXME: set release == true? */
			return 0;
		}
		break;
	case GSM_MI_TYPE_TMSI:
		DEBUGPC(DMM, "\n");
		/* look up the subscriber based on TMSI, request IMSI if it fails */
		subscr = subscr_get_by_tmsi(conn->network->subscr_group,
					    tmsi_from_string(mi_string));
		if (!subscr) {
			/* send IDENTITY REQUEST message to get IMSI */
			mm_tx_identity_req(conn, GSM_MI_TYPE_IMSI);
			conn->loc_operation->waiting_for_imsi = 1;
		}
		/* we always want the IMEI, too */
		mm_tx_identity_req(conn, GSM_MI_TYPE_IMEI);
		conn->loc_operation->waiting_for_imei = 1;
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		/* no sim card... FIXME: what to do ? */
		DEBUGPC(DMM, "unimplemented mobile identity type\n");
		break;
	default:
		DEBUGPC(DMM, "unknown mobile identity type\n");
		break;
	}

	/* schedule the reject timer */
	schedule_reject(conn);

	if (!subscr) {
		DEBUGPC(DRR, "<- Can't find any subscriber for this ID\n");
		/* FIXME: request id? close channel? */
		return -EINVAL;
	}

	conn->subscr = subscr;
	conn->subscr->equipment.classmark1 = lu->classmark1;

	/* check if we can let the subscriber into our network immediately
	 * or if we need to wait for identity responses. */
	return gsm0408_authorize(conn, msg);
}

/* Turn int into semi-octet representation: 98 => 0x89 */
static uint8_t bcdify(uint8_t value)
{
        uint8_t ret;

        ret = value / 10;
        ret |= (value % 10) << 4;

        return ret;
}


/* Section 9.2.15a */
int gsm48_tx_mm_info(struct gsm_subscriber_connection *conn)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 MM INF");
	struct gsm48_hdr *gh;
	struct gsm_network *net = conn->network;
	uint8_t *ptr8;
	int name_len, name_pad;

	time_t cur_t;
	struct tm* gmt_time;
	struct tm* local_time;
	int tzunits;
	int dst = 0;

	msg->lchan = conn->lchan;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_INFO;

	if (net->name_long) {
#if 0
		name_len = strlen(net->name_long);
		/* 10.5.3.5a */
		ptr8 = msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_LONG;
		ptr8[1] = name_len*2 +1;
		ptr8[2] = 0x90; /* UCS2, no spare bits, no CI */

		ptr16 = (uint16_t *) msgb_put(msg, name_len*2);
		for (i = 0; i < name_len; i++)
			ptr16[i] = htons(net->name_long[i]);

		/* FIXME: Use Cell Broadcast, not UCS-2, since
		 * UCS-2 is only supported by later revisions of the spec */
#endif
		name_len = (strlen(net->name_long)*7)/8;
		name_pad = (8 - strlen(net->name_long)*7)%8;
		if (name_pad > 0)
			name_len++;
		/* 10.5.3.5a */
		ptr8 = msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_LONG;
		ptr8[1] = name_len +1;
		ptr8[2] = 0x80 | name_pad; /* Cell Broadcast DCS, no CI */

		ptr8 = msgb_put(msg, name_len);
		gsm_7bit_encode_n(ptr8, name_len, net->name_long, NULL);

	}

	if (net->name_short) {
#if 0
		name_len = strlen(net->name_short);
		/* 10.5.3.5a */
		ptr8 = (uint8_t *) msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_SHORT;
		ptr8[1] = name_len*2 + 1;
		ptr8[2] = 0x90; /* UCS2, no spare bits, no CI */

		ptr16 = (uint16_t *) msgb_put(msg, name_len*2);
		for (i = 0; i < name_len; i++)
			ptr16[i] = htons(net->name_short[i]);
#endif
		name_len = (strlen(net->name_short)*7)/8;
		name_pad = (8 - strlen(net->name_short)*7)%8;
		if (name_pad > 0)
			name_len++;
		/* 10.5.3.5a */
		ptr8 = (uint8_t *) msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NAME_SHORT;
		ptr8[1] = name_len +1;
		ptr8[2] = 0x80 | name_pad; /* Cell Broadcast DCS, no CI */

		ptr8 = msgb_put(msg, name_len);
		gsm_7bit_encode_n(ptr8, name_len, net->name_short, NULL);

	}

	/* Section 10.5.3.9 */
	cur_t = time(NULL);
	gmt_time = gmtime(&cur_t);

	ptr8 = msgb_put(msg, 8);
	ptr8[0] = GSM48_IE_NET_TIME_TZ;
	ptr8[1] = bcdify(gmt_time->tm_year % 100);
	ptr8[2] = bcdify(gmt_time->tm_mon + 1);
	ptr8[3] = bcdify(gmt_time->tm_mday);
	ptr8[4] = bcdify(gmt_time->tm_hour);
	ptr8[5] = bcdify(gmt_time->tm_min);
	ptr8[6] = bcdify(gmt_time->tm_sec);

	if (net->tz.override) {
		/* Convert tz.hr and tz.mn to units */
		if (net->tz.hr < 0) {
			tzunits = ((net->tz.hr/-1)*4);
			tzunits = tzunits + (net->tz.mn/15);
			ptr8[7] = bcdify(tzunits);
			/* Set negative time */
			ptr8[7] |= 0x08;
		}
		else {
			tzunits = net->tz.hr*4;
			tzunits = tzunits + (net->tz.mn/15);
			ptr8[7] = bcdify(tzunits);
		}
		/* Convert DST value */
		if (net->tz.dst >= 0 && net->tz.dst <= 2)
			dst = net->tz.dst;
	}
	else {
		/* Need to get GSM offset and convert into 15 min units */
		/* This probably breaks if gmtoff returns a value not evenly divisible by 15? */
#ifdef HAVE_TM_GMTOFF_IN_TM
		local_time = localtime(&cur_t);
		tzunits = (local_time->tm_gmtoff/60)/15;
#else
		/* find timezone offset */
		time_t utc;
		double offsetFromUTC;
		utc = mktime(gmt_time);
		local_time = localtime(&cur_t);
		offsetFromUTC = difftime(cur_t, utc);
		if (local_time->tm_isdst)
			offsetFromUTC += 3600.0;
		tzunits = ((int)offsetFromUTC) / 60 / 15;
#endif
		if (tzunits < 0) {
			tzunits = tzunits/-1;
			ptr8[7] = bcdify(tzunits);
			/* Flip it to negative */
			ptr8[7] |= 0x08;
		}
		else
			ptr8[7] = bcdify(tzunits);

		/* Does not support DST +2 */
		if (local_time->tm_isdst)
			dst = 1;
	}

	if (dst) {
		ptr8 = msgb_put(msg, 3);
		ptr8[0] = GSM48_IE_NET_DST;
		ptr8[1] = 1;
		ptr8[2] = dst;
	}

	DEBUGP(DMM, "-> MM INFO\n");

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

/*! Send an Authentication Request to MS on the given subscriber connection
 * according to 3GPP/ETSI TS 24.008, Section 9.2.2.
 * \param[in] conn  Subscriber connection to send on.
 * \param[in] rand  Random challenge token to send, must be 16 bytes long.
 * \param[in] autn  r99: In case of UMTS mutual authentication, AUTN token to
 * 	send; must be 16 bytes long, or pass NULL for plain GSM auth.
 * \param[in] key_seq  auth tuple's sequence number.
 */
int gsm48_tx_mm_auth_req(struct gsm_subscriber_connection *conn, uint8_t *rand,
			 uint8_t *autn, int key_seq)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 AUTH REQ");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	struct gsm48_auth_req *ar = (struct gsm48_auth_req *) msgb_put(msg, sizeof(*ar));
        char *rand2=catch_rand();
        const unsigned char *test=hex2ascii(rand2);
        rand=test;
	DEBUGP(DMM, "-> AUTH REQ (rand = %s)\n", osmo_hexdump(test, 16));
	if (autn)
		DEBUGP(DMM, "   AUTH REQ (autn = %s)\n", osmo_hexdump(autn, 16));

	msg->lchan = conn->lchan;
	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_AUTH_REQ;

	ar->key_seq = 0;

	/* 16 bytes RAND parameters */
	osmo_static_assert(sizeof(ar->rand) == 16, sizeof_auth_req_r99_rand);
	if (rand)
		memcpy(ar->rand, rand, 16);


	/* 16 bytes AUTN */
	if (autn)
		msgb_tlv_put(msg, GSM48_IE_AUTN, 16, autn);

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

/* Section 9.2.1 */
int gsm48_tx_mm_auth_rej(struct gsm_subscriber_connection *conn)
{
	DEBUGP(DMM, "-> AUTH REJECT\n");
	return gsm48_tx_simple(conn, GSM48_PDISC_MM, GSM48_MT_MM_AUTH_REJ);
}

/*
 * At the 30C3 phones miss their periodic update
 * interval a lot and then remain unreachable. In case
 * we still know the TMSI we can just attach it again.
 */
static void implit_attach(struct gsm_subscriber_connection *conn)
{
	if (conn->subscr->lac != GSM_LAC_RESERVED_DETACHED)
		return;

	subscr_update(conn->subscr, conn->bts,
		      GSM_SUBSCRIBER_UPDATE_ATTACHED);
}


static int _gsm48_rx_mm_serv_req_sec_cb(
	unsigned int hooknum, unsigned int event,
	struct msgb *msg, void *data, void *param)
{
	struct gsm_subscriber_connection *conn = data;
	int rc = 0;

	/* auth failed or succeeded, the timer was stopped */
	conn->expire_timer_stopped = 1;

	switch (event) {
		case GSM_SECURITY_AUTH_FAILED:
			/* Nothing to do */
			break;

		case GSM_SECURITY_NOAVAIL:
		case GSM_SECURITY_ALREADY:
			rc = gsm48_tx_mm_serv_ack(conn);
			implit_attach(conn);
			break;

		case GSM_SECURITY_SUCCEEDED:
			/* nothing to do. CIPHER MODE COMMAND is
			 * implicit CM SERV ACK */
			implit_attach(conn);
			break;

		default:
			rc = -EINVAL;
	};

	return rc;
}

/*
 * Handle CM Service Requests
 * a) Verify that the packet is long enough to contain the information
 *    we require otherwsie reject with INCORRECT_MESSAGE
 * b) Try to parse the TMSI. If we do not have one reject
 * c) Check that we know the subscriber with the TMSI otherwise reject
 *    with a HLR cause
 * d) Set the subscriber on the gsm_lchan and accept
 */
static int gsm48_rx_mm_serv_req(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	uint8_t mi_type;
	char mi_string[GSM48_MI_SIZE];

	struct gsm_network *network = conn->network;
	struct gsm_subscriber *subscr;
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_service_request *req =
			(struct gsm48_service_request *)gh->data;
	/* unfortunately in Phase1 the classmark2 length is variable */
	uint8_t classmark2_len = gh->data[1];
	uint8_t *classmark2 = gh->data+2;
	uint8_t mi_len = *(classmark2 + classmark2_len);
	uint8_t *mi = (classmark2 + classmark2_len + 1);

	DEBUGP(DMM, "<- CM SERVICE REQUEST ");
	if (msg->data_len < sizeof(struct gsm48_service_request*)) {
		DEBUGPC(DMM, "wrong sized message\n");
		return gsm48_tx_mm_serv_rej(conn,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	if (msg->data_len < req->mi_len + 6) {
		DEBUGPC(DMM, "does not fit in packet\n");
		return gsm48_tx_mm_serv_rej(conn,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	gsm48_mi_to_string(mi_string, sizeof(mi_string), mi, mi_len);
	mi_type = mi[0] & GSM_MI_TYPE_MASK;

	if (mi_type == GSM_MI_TYPE_IMSI) {
		DEBUGPC(DMM, "serv_type=0x%02x MI(%s)=%s\n",
			req->cm_service_type, gsm48_mi_type_name(mi_type),
			mi_string);
		subscr = subscr_get_by_imsi(network->subscr_group,
					    mi_string);
	} else if (mi_type == GSM_MI_TYPE_TMSI) {
		DEBUGPC(DMM, "serv_type=0x%02x MI(%s)=%s\n",
			req->cm_service_type, gsm48_mi_type_name(mi_type),
			mi_string);
		subscr = subscr_get_by_tmsi(network->subscr_group,
				tmsi_from_string(mi_string));
	} else {
		DEBUGPC(DMM, "mi_type is not expected: %d\n", mi_type);
		return gsm48_tx_mm_serv_rej(conn,
					    GSM48_REJECT_INCORRECT_MESSAGE);
	}

	osmo_signal_dispatch(SS_SUBSCR, S_SUBSCR_IDENTITY, (classmark2 + classmark2_len));

	if (is_siemens_bts(conn->bts))
		send_siemens_mrpci(msg->lchan, classmark2-1);


	/* FIXME: if we don't know the TMSI, inquire abit IMSI and allocate new TMSI */
	if (!subscr)
		return gsm48_tx_mm_serv_rej(conn,
					    GSM48_REJECT_IMSI_UNKNOWN_IN_VLR);

	if (!conn->subscr)
		conn->subscr = subscr;
	else if (conn->subscr == subscr)
		subscr_put(subscr); /* lchan already has a ref, don't need another one */
	else {
		DEBUGP(DMM, "<- CM Channel already owned by someone else?\n");
		subscr_put(subscr);
	}

	subscr->equipment.classmark2_len = classmark2_len;
	memcpy(subscr->equipment.classmark2, classmark2, classmark2_len);
	db_sync_equipment(&subscr->equipment);

	/* we will send a MM message soon */
	conn->expire_timer_stopped = 1;

	return gsm48_secure_channel(conn, req->cipher_key_seq,
			_gsm48_rx_mm_serv_req_sec_cb, NULL);
}

static int gsm48_rx_mm_imsi_detach_ind(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm_network *network = conn->network;
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_imsi_detach_ind *idi =
				(struct gsm48_imsi_detach_ind *) gh->data;
	uint8_t mi_type = idi->mi[0] & GSM_MI_TYPE_MASK;
	char mi_string[GSM48_MI_SIZE];
	struct gsm_subscriber *subscr = NULL;

	gsm48_mi_to_string(mi_string, sizeof(mi_string), idi->mi, idi->mi_len);
	DEBUGP(DMM, "IMSI DETACH INDICATION: MI(%s)=%s",
		gsm48_mi_type_name(mi_type), mi_string);

	rate_ctr_inc(&network->msc_ctrs->ctr[MSC_CTR_LOC_UPDATE_TYPE_DETACH]);

	switch (mi_type) {
	case GSM_MI_TYPE_TMSI:
		DEBUGPC(DMM, "\n");
		subscr = subscr_get_by_tmsi(network->subscr_group,
					    tmsi_from_string(mi_string));
		break;
	case GSM_MI_TYPE_IMSI:
		DEBUGPC(DMM, "\n");
		subscr = subscr_get_by_imsi(network->subscr_group,
					    mi_string);
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		/* no sim card... FIXME: what to do ? */
		DEBUGPC(DMM, ": unimplemented mobile identity type\n");
		break;
	default:
		DEBUGPC(DMM, ": unknown mobile identity type\n");
		break;
	}

	if (subscr) {
		subscr_update(subscr, conn->bts,
			      GSM_SUBSCRIBER_UPDATE_DETACHED);
		DEBUGP(DMM, "Subscriber: %s\n", subscr_name(subscr));

		subscr->equipment.classmark1 = idi->classmark1;
		db_sync_equipment(&subscr->equipment);

		subscr_put(subscr);
	} else
		DEBUGP(DMM, "Unknown Subscriber ?!?\n");

	/* FIXME: iterate over all transactions and release them,
	 * imagine an IMSI DETACH happening during an active call! */

	release_anchor(conn);
	return 0;
}

static int gsm48_rx_mm_status(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);

	DEBUGP(DMM, "MM STATUS (reject cause 0x%02x)\n", gh->data[0]);

	return 0;
}

static int parse_gsm_auth_resp(uint8_t *res, uint8_t *res_len,
			       struct gsm_subscriber_connection *conn,
			       struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_auth_resp *ar = (struct gsm48_auth_resp*) gh->data;

	if (msgb_l3len(msg) < sizeof(*gh) + sizeof(*ar)) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM AUTHENTICATION RESPONSE:"
		     " l3 length invalid: %u\n",
		     subscr_name(conn->subscr), msgb_l3len(msg));
		return -EINVAL;
	}
        *res_len = sizeof(ar->sres);
        memcpy(res, ar->sres, sizeof(ar->sres));
	client(osmo_hexdump(ar->sres,4));
        return 0;
}

static int parse_umts_auth_resp(uint8_t *res, uint8_t *res_len,
				struct gsm_subscriber_connection *conn,
				struct msgb *msg)
{
	struct gsm48_hdr *gh;
	uint8_t *data;
	uint8_t iei;
	uint8_t ie_len;
	unsigned int data_len;

	/* First parse the GSM part */
	if (parse_gsm_auth_resp(res, res_len, conn, msg))
		return -EINVAL;
	OSMO_ASSERT(*res_len == 4);

	/* Then add the extended res part */
	gh = msgb_l3(msg);
	data = gh->data + sizeof(struct gsm48_auth_resp);
	data_len = msgb_l3len(msg) - (data - (uint8_t*)msgb_l3(msg));

	if (data_len < 3) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM AUTHENTICATION RESPONSE:"
		     " l3 length invalid: %u\n",
		     subscr_name(conn->subscr), msgb_l3len(msg));
		return -EINVAL;
	}

	iei = data[0];
	ie_len = data[1];
	if (iei != GSM48_IE_AUTH_RES_EXT) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM R99 AUTHENTICATION RESPONSE:"
		     " expected IEI 0x%02x, got 0x%02x\n",
		     subscr_name(conn->subscr),
		     GSM48_IE_AUTH_RES_EXT, iei);
		return -EINVAL;
	}

	if (ie_len > 12) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM R99 AUTHENTICATION RESPONSE:"
		     " extended Auth Resp IE 0x%02x is too large: %u bytes\n",
		     subscr_name(conn->subscr), GSM48_IE_AUTH_RES_EXT, ie_len);
		return -EINVAL;
	}

	*res_len += ie_len;
	memcpy(res + 4, &data[2], ie_len);
	return 0;
}

/* Chapter 9.2.3: Authentication Response */
static int gsm48_rx_mm_auth_resp(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm_network *net = conn->network;
	uint8_t res[16];
	uint8_t res_len = 0;
	int rc;
	bool is_r99;

	if (!conn->subscr) {
		LOGP(DMM, LOGL_ERROR,
		     "MM AUTHENTICATION RESPONSE: invalid: no subscriber\n");
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	if (msgb_l3len(msg) >
	    sizeof(struct gsm48_hdr) + sizeof(struct gsm48_auth_resp)) {
		rc = parse_umts_auth_resp(res, &res_len, conn, msg);
		is_r99 = true;
	} else {
		rc = parse_gsm_auth_resp(res, &res_len, conn, msg);
		is_r99 = false;
	}

	if (rc) {
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	DEBUGP(DMM, "%s: MM %s AUTHENTICATION RESPONSE (%s = %s)\n",
	       subscr_name(conn->subscr),
	       is_r99 ? "R99" : "GSM", is_r99 ? "res" : "sres",
	       osmo_hexdump_nospc(res, res_len));
        
	/* Future: vlr_sub_rx_auth_resp(conn->vsub, is_r99,
	 *				conn->via_ran == RAN_UTRAN_IU,
	 *				res, res_len);
	 */

	if (res_len != 4) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM AUTHENTICATION RESPONSE:"
		     " UMTS authentication not supported\n",
		     subscr_name(conn->subscr));
	}

	/* Safety check */
	if (!conn->sec_operation) {
		DEBUGP(DMM, "No authentication/cipher operation in progress !!!\n");
		return -EIO;
	}

	/* Validate SRES */
	if (memcmp(conn->sec_operation->atuple.vec.sres, res, 4)) {
		int rc;
		gsm_cbfn *cb = conn->sec_operation->cb;

		DEBUGPC(DMM, "Invalid (expected %s)\n",
			osmo_hexdump(conn->sec_operation->atuple.vec.sres, 4));

		if (cb)
			cb(GSM_HOOK_RR_SECURITY, GSM_SECURITY_AUTH_FAILED,
			   NULL, conn, conn->sec_operation->cb_data);

		rc = gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return rc;
	}

	DEBUGPC(DMM, "OK\n");

	/* Start ciphering */
	return gsm0808_cipher_mode(conn, net->a5_encryption,
	                           conn->sec_operation->atuple.vec.kc, 8, 0);
}

static int gsm48_rx_mm_auth_fail(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t cause;
	uint8_t auts_tag;
	uint8_t auts_len;
	uint8_t *auts;
	int rc;

	if (!conn->sec_operation) {
		DEBUGP(DMM, "%s: MM R99 AUTHENTICATION FAILURE:"
		       " No authentication/cipher operation in progress\n",
		       subscr_name(conn->subscr));
		return -EINVAL;
	}

	if (!conn->subscr) {
		LOGP(DMM, LOGL_ERROR,
		     "MM R99 AUTHENTICATION FAILURE: invalid: no subscriber\n");
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	if (msgb_l3len(msg) < sizeof(*gh) + 1) {
		LOGP(DMM, LOGL_ERROR,
		     "%s: MM R99 AUTHENTICATION FAILURE:"
		     " l3 length invalid: %u\n",
		     subscr_name(conn->subscr), msgb_l3len(msg));
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	cause = gh->data[0];

	if (cause != GSM48_REJECT_SYNCH_FAILURE) {
		LOGP(DMM, LOGL_INFO,
		     "%s: MM R99 AUTHENTICATION FAILURE: cause 0x%0x\n",
		     subscr_name(conn->subscr), cause);
		rc = gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return rc;
	}

	/* This is a Synch Failure procedure, which should pass an AUTS to
	 * resynchronize the sequence nr with the HLR. Expecting exactly one
	 * TLV with 14 bytes of AUTS. */

	if (msgb_l3len(msg) < sizeof(*gh) + 1 + 2) {
		LOGP(DMM, LOGL_INFO,
		     "%s: MM R99 AUTHENTICATION FAILURE:"
		     " invalid Synch Failure: missing AUTS IE\n",
		     subscr_name(conn->subscr));
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	auts_tag = gh->data[1];
	auts_len = gh->data[2];
	auts = &gh->data[3];

	if (auts_tag != GSM48_IE_AUTS
	    || auts_len != 14) {
		LOGP(DMM, LOGL_INFO,
		     "%s: MM R99 AUTHENTICATION FAILURE:"
		     " invalid Synch Failure:"
		     " expected AUTS IE 0x%02x of 14 bytes,"
		     " got IE 0x%02x of %u bytes\n",
		     subscr_name(conn->subscr),
		     GSM48_IE_AUTS, auts_tag, auts_len);
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	if (msgb_l3len(msg) < sizeof(*gh) + 1 + 2 + auts_len) {
		LOGP(DMM, LOGL_INFO,
		     "%s: MM R99 AUTHENTICATION FAILURE:"
		     " invalid Synch Failure msg: message truncated (%u)\n",
		     subscr_name(conn->subscr), msgb_l3len(msg));
		gsm48_tx_mm_auth_rej(conn);
		release_security_operation(conn);
		return -EINVAL;
	}

	/* We have an AUTS IE with exactly 14 bytes of AUTS and the msgb is
	 * large enough. */

	DEBUGP(DMM, "%s: MM R99 AUTHENTICATION SYNCH (AUTS = %s)\n",
	       subscr_name(conn->subscr), osmo_hexdump_nospc(auts, 14));

	/* Future: vlr_sub_rx_auth_fail(conn->vsub, auts); */

	LOGP(DMM, LOGL_ERROR, "%s: MM R99 AUTHENTICATION not supported\n",
	     subscr_name(conn->subscr));
	rc = gsm48_tx_mm_auth_rej(conn);
	release_security_operation(conn);
	return rc;
}

/* Receive a GSM 04.08 Mobility Management (MM) message */
static int gsm0408_rcv_mm(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	int rc = 0;

	switch (gsm48_hdr_msg_type(gh)) {
	case GSM48_MT_MM_LOC_UPD_REQUEST:
		DEBUGP(DMM, "LOCATION UPDATING REQUEST: ");
		rc = mm_rx_loc_upd_req(conn, msg);
		break;
	case GSM48_MT_MM_ID_RESP:
		rc = mm_rx_id_resp(conn, msg);
		break;
	case GSM48_MT_MM_CM_SERV_REQ:
		rc = gsm48_rx_mm_serv_req(conn, msg);
		break;
	case GSM48_MT_MM_STATUS:
		rc = gsm48_rx_mm_status(msg);
		break;
	case GSM48_MT_MM_TMSI_REALL_COMPL:
		DEBUGP(DMM, "TMSI Reallocation Completed. Subscriber: %s\n",
		       subscr_name(conn->subscr));
		loc_updating_success(conn, 1);
		break;
	case GSM48_MT_MM_IMSI_DETACH_IND:
		rc = gsm48_rx_mm_imsi_detach_ind(conn, msg);
		break;
	case GSM48_MT_MM_CM_REEST_REQ:
		DEBUGP(DMM, "CM REESTABLISH REQUEST: Not implemented\n");
		break;
	case GSM48_MT_MM_AUTH_RESP:
		rc = gsm48_rx_mm_auth_resp(conn, msg);
		break;
	case GSM48_MT_MM_AUTH_FAIL:
		rc = gsm48_rx_mm_auth_fail(conn, msg);
		break;
	default:
		LOGP(DMM, LOGL_NOTICE, "Unknown GSM 04.08 MM msg type 0x%02x\n",
			gh->msg_type);
		break;
	}

	return rc;
}

/* Receive a PAGING RESPONSE message from the MS */
static int gsm48_rx_rr_pag_resp(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	struct gsm48_pag_resp *resp;
	uint8_t *classmark2_lv = gh->data + 1;
	uint8_t mi_type;
	char mi_string[GSM48_MI_SIZE];
	struct gsm_subscriber *subscr = NULL;
	struct bsc_subscr *bsub;
	uint32_t tmsi;
	int rc = 0;

	resp = (struct gsm48_pag_resp *) &gh->data[0];
	gsm48_paging_extract_mi(resp, msgb_l3len(msg) - sizeof(*gh),
				mi_string, &mi_type);
	DEBUGP(DRR, "PAGING RESPONSE: MI(%s)=%s\n",
		gsm48_mi_type_name(mi_type), mi_string);

	switch (mi_type) {
	case GSM_MI_TYPE_TMSI:
		tmsi = tmsi_from_string(mi_string);
		subscr = subscr_get_by_tmsi(conn->network->subscr_group, tmsi);
		break;
	case GSM_MI_TYPE_IMSI:
		subscr = subscr_get_by_imsi(conn->network->subscr_group,
					    mi_string);
		break;
	}

	if (!subscr) {
		DEBUGP(DRR, "<- Can't find any subscriber for this ID\n");
		/* FIXME: request id? close channel? */
		return -EINVAL;
	}

	if (!conn->subscr) {
		conn->subscr = subscr;
	} else if (conn->subscr != subscr) {
		LOGP(DRR, LOGL_ERROR, "<- Channel already owned by someone else?\n");
		subscr_put(subscr);
		return -EINVAL;
	} else {
		DEBUGP(DRR, "<- Channel already owned by us\n");
		subscr_put(subscr);
		subscr = conn->subscr;
	}

	log_set_context(LOG_CTX_VLR_SUBSCR, subscr);
	DEBUGP(DRR, "<- Channel was requested by %s\n",
		subscr->name && strlen(subscr->name) ? subscr->name : subscr->imsi);

	subscr->equipment.classmark2_len = *classmark2_lv;
	memcpy(subscr->equipment.classmark2, classmark2_lv+1, *classmark2_lv);
	db_sync_equipment(&subscr->equipment);

	/* TODO MSC split -- creating a BSC subscriber directly from MSC data
	 * structures in RAM. At some point the MSC will send a message to the
	 * BSC instead. */
	bsub = bsc_subscr_find_or_create_by_imsi(conn->network->bsc_subscribers,
						 subscr->imsi);
	bsub->tmsi = subscr->tmsi;
	bsub->lac = subscr->lac;

	/* We received a paging */
	conn->expire_timer_stopped = 1;

	rc = gsm48_handle_paging_resp(conn, msg, bsub);
	return rc;
}

static int gsm48_rx_rr_app_info(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t apdu_id_flags;
	uint8_t apdu_len;
	uint8_t *apdu_data;

	apdu_id_flags = gh->data[0];
	apdu_len = gh->data[1];
	apdu_data = gh->data+2;

	DEBUGP(DRR, "RX APPLICATION INFO id/flags=0x%02x apdu_len=%u apdu=%s\n",
		apdu_id_flags, apdu_len, osmo_hexdump(apdu_data, apdu_len));

	return db_apdu_blob_store(conn->subscr, apdu_id_flags, apdu_len, apdu_data);
}

/* Receive a GSM 04.08 Radio Resource (RR) message */
static int gsm0408_rcv_rr(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	int rc = 0;

	switch (gh->msg_type) {
	case GSM48_MT_RR_PAG_RESP:
		rc = gsm48_rx_rr_pag_resp(conn, msg);
		break;
	case GSM48_MT_RR_APP_INFO:
		rc = gsm48_rx_rr_app_info(conn, msg);
		break;
	default:
		LOGP(DRR, LOGL_NOTICE, "MSC: Unimplemented %s GSM 04.08 RR "
		     "message\n", gsm48_rr_msg_name(gh->msg_type));
		break;
	}

	return rc;
}

int gsm48_send_rr_app_info(struct gsm_subscriber_connection *conn, uint8_t apdu_id,
			   uint8_t apdu_len, const uint8_t *apdu)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 APP INF");
	struct gsm48_hdr *gh;

	msg->lchan = conn->lchan;

	DEBUGP(DRR, "TX APPLICATION INFO id=0x%02x, len=%u\n",
		apdu_id, apdu_len);

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh) + 2 + apdu_len);
	gh->proto_discr = GSM48_PDISC_RR;
	gh->msg_type = GSM48_MT_RR_APP_INFO;
	gh->data[0] = apdu_id;
	gh->data[1] = apdu_len;
	memcpy(gh->data+2, apdu, apdu_len);

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

/* FIXME: this count_statistics is a state machine behaviour. we should convert
 * the complete call control into a state machine. Afterwards we can move this
 * code into state transitions.
 */
static void count_statistics(struct gsm_trans *trans, int new_state)
{
	int old_state = trans->cc.state;
	struct rate_ctr_group *msc = trans->net->msc_ctrs;

	if (old_state == new_state)
		return;

	/* state incoming */
	switch (new_state) {
	case GSM_CSTATE_ACTIVE:
		osmo_counter_inc(trans->net->active_calls);
		rate_ctr_inc(&msc->ctr[MSC_CTR_CALL_ACTIVE]);
		break;
	}

	/* state outgoing */
	switch (old_state) {
	case GSM_CSTATE_ACTIVE:
		osmo_counter_dec(trans->net->active_calls);
		if (new_state == GSM_CSTATE_DISCONNECT_REQ ||
				new_state == GSM_CSTATE_DISCONNECT_IND)
			rate_ctr_inc(&msc->ctr[MSC_CTR_CALL_COMPLETE]);
		else
			rate_ctr_inc(&msc->ctr[MSC_CTR_CALL_INCOMPLETE]);
		break;
	}
}

/* Call Control */

/* The entire call control code is written in accordance with Figure 7.10c
 * for 'very early assignment', i.e. we allocate a TCH/F during IMMEDIATE
 * ASSIGN, then first use that TCH/F for signalling and later MODE MODIFY
 * it for voice */

static void new_cc_state(struct gsm_trans *trans, int state)
{
	if (state > 31 || state < 0)
		return;

	DEBUGP(DCC, "new state %s -> %s\n",
		gsm48_cc_state_name(trans->cc.state),
		gsm48_cc_state_name(state));

	count_statistics(trans, state);
	trans->cc.state = state;
}

static int gsm48_cc_tx_status(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC STATUS");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	uint8_t *cause, *call_state;

	gh->msg_type = GSM48_MT_CC_STATUS;

	cause = msgb_put(msg, 3);
	cause[0] = 2;
	cause[1] = GSM48_CAUSE_CS_GSM | GSM48_CAUSE_LOC_USER;
	cause[2] = 0x80 | 30;	/* response to status inquiry */

	call_state = msgb_put(msg, 1);
	call_state[0] = 0xc0 | 0x00;

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_tx_simple(struct gsm_subscriber_connection *conn,
			   uint8_t pdisc, uint8_t msg_type)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 TX SIMPLE");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	msg->lchan = conn->lchan;

	gh->proto_discr = pdisc;
	gh->msg_type = msg_type;

	return gsm48_conn_sendmsg(msg, conn, NULL);
}

static void gsm48_stop_cc_timer(struct gsm_trans *trans)
{
	if (osmo_timer_pending(&trans->cc.timer)) {
		DEBUGP(DCC, "stopping pending timer T%x\n", trans->cc.Tcurrent);
		osmo_timer_del(&trans->cc.timer);
		trans->cc.Tcurrent = 0;
	}
}

static int mncc_recvmsg(struct gsm_network *net, struct gsm_trans *trans,
			int msg_type, struct gsm_mncc *mncc)
{
	struct msgb *msg;
	unsigned char *data;

	if (trans)
		if (trans->conn && trans->conn->lchan)
			DEBUGP(DCC, "(bts %d trx %d ts %d ti %x sub %s) "
				"Sending '%s' to MNCC.\n",
				trans->conn->lchan->ts->trx->bts->nr,
				trans->conn->lchan->ts->trx->nr,
				trans->conn->lchan->ts->nr, trans->transaction_id,
				(trans->subscr)?(trans->subscr->extension):"-",
				get_mncc_name(msg_type));
		else
			DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
				"Sending '%s' to MNCC.\n",
				(trans->subscr)?(trans->subscr->extension):"-",
				get_mncc_name(msg_type));
	else
		DEBUGP(DCC, "(bts - trx - ts - ti -- sub -) "
			"Sending '%s' to MNCC.\n", get_mncc_name(msg_type));

	mncc->msg_type = msg_type;

	msg = msgb_alloc(sizeof(struct gsm_mncc), "MNCC");
	if (!msg)
		return -ENOMEM;

	data = msgb_put(msg, sizeof(struct gsm_mncc));
	memcpy(data, mncc, sizeof(struct gsm_mncc));

	cc_tx_to_mncc(net, msg);

	return 0;
}

int mncc_release_ind(struct gsm_network *net, struct gsm_trans *trans,
		     uint32_t callref, int location, int value)
{
	struct gsm_mncc rel;

	memset(&rel, 0, sizeof(rel));
	rel.callref = callref;
	mncc_set_cause(&rel, location, value);
	if (trans && trans->cc.state == GSM_CSTATE_RELEASE_REQ)
		return mncc_recvmsg(net, trans, MNCC_REL_CNF, &rel);
	return mncc_recvmsg(net, trans, MNCC_REL_IND, &rel);
}

/* Call Control Specific transaction release.
 * gets called by trans_free, DO NOT CALL YOURSELF! */
void _gsm48_cc_trans_free(struct gsm_trans *trans)
{
	gsm48_stop_cc_timer(trans);

	/* send release to L4, if callref still exists */
	if (trans->callref) {
		/* Ressource unavailable */
		mncc_release_ind(trans->net, trans, trans->callref,
				 GSM48_CAUSE_LOC_PRN_S_LU,
				 GSM48_CC_CAUSE_RESOURCE_UNAVAIL);
	}
	if (trans->cc.state != GSM_CSTATE_NULL)
		new_cc_state(trans, GSM_CSTATE_NULL);
	if (trans->conn)
		trau_mux_unmap(&trans->conn->lchan->ts->e1_link, trans->callref);
}

static int gsm48_cc_tx_setup(struct gsm_trans *trans, void *arg);

/* call-back from paging the B-end of the connection */
static int setup_trig_pag_evt(unsigned int hooknum, unsigned int event,
			      struct msgb *msg, void *_conn, void *_transt)
{
	struct gsm_subscriber_connection *conn = _conn;
	struct gsm_trans *transt = _transt;

	OSMO_ASSERT(!transt->conn);

	/* check all tranactions (without lchan) for subscriber */
	switch (event) {
	case GSM_PAGING_SUCCEEDED:
		DEBUGP(DCC, "Paging subscr %s succeeded!\n", transt->subscr->extension);
		OSMO_ASSERT(conn);
		/* Assign lchan */
		transt->conn = conn;
		/* send SETUP request to called party */
		gsm48_cc_tx_setup(transt, &transt->cc.msg);
		break;
	case GSM_PAGING_EXPIRED:
	case GSM_PAGING_BUSY:
		DEBUGP(DCC, "Paging subscr %s expired!\n",
			transt->subscr->extension);
		/* Temporarily out of order */
		mncc_release_ind(transt->net, transt,
				 transt->callref,
				 GSM48_CAUSE_LOC_PRN_S_LU,
				 GSM48_CC_CAUSE_DEST_OOO);
		transt->callref = 0;
		transt->paging_request = NULL;
		trans_free(transt);
		break;
	default:
		LOGP(DCC, LOGL_ERROR, "Unknown paging event %d\n", event);
		break;
	}

	transt->paging_request = NULL;
	return 0;
}

static int tch_recv_mncc(struct gsm_network *net, uint32_t callref, int enable);

/* handle audio path for handover */
static int switch_for_handover(struct gsm_lchan *old_lchan,
			struct gsm_lchan *new_lchan)
{
	struct rtp_socket *old_rs, *new_rs, *other_rs;

	/* Ask the new socket to send to the already known port. */
	if (new_lchan->conn->mncc_rtp_bridge) {
		LOGP(DHO, LOGL_DEBUG, "Forwarding RTP\n");
		rsl_ipacc_mdcx(new_lchan,
					old_lchan->abis_ip.connect_ip,
					old_lchan->abis_ip.connect_port, 0);
		return 0;
	}

	if (ipacc_rtp_direct) {
		LOGP(DHO, LOGL_ERROR, "unable to handover in direct RTP mode\n");
		return 0;
	}

	/* RTP Proxy mode */
	new_rs = new_lchan->abis_ip.rtp_socket;
	old_rs = old_lchan->abis_ip.rtp_socket;

	if (!new_rs) {
		LOGP(DHO, LOGL_ERROR, "no RTP socket for new_lchan\n");
		return -EIO;
	}

	rsl_ipacc_mdcx_to_rtpsock(new_lchan);

	if (!old_rs) {
		LOGP(DHO, LOGL_ERROR, "no RTP socket for old_lchan\n");
		return -EIO;
	}

	/* copy rx_action and reference to other sock */
	new_rs->rx_action = old_rs->rx_action;
	new_rs->tx_action = old_rs->tx_action;
	new_rs->transmit = old_rs->transmit;

	switch (old_lchan->abis_ip.rtp_socket->rx_action) {
	case RTP_PROXY:
		other_rs = old_rs->proxy.other_sock;
		rtp_socket_proxy(new_rs, other_rs);
		/* delete reference to other end socket to prevent
		 * rtp_socket_free() from removing the inverse reference */
		old_rs->proxy.other_sock = NULL;
		break;
	case RTP_RECV_UPSTREAM:
		new_rs->receive = old_rs->receive;
		break;
	case RTP_NONE:
		break;
	}

	return 0;
}

static void maybe_switch_for_handover(struct gsm_lchan *lchan)
{
	struct gsm_lchan *old_lchan;
	old_lchan = bsc_handover_pending(lchan);
	if (old_lchan)
		switch_for_handover(old_lchan, lchan);
}

/* some other part of the code sends us a signal */
static int handle_abisip_signal(unsigned int subsys, unsigned int signal,
				 void *handler_data, void *signal_data)
{
	struct gsm_lchan *lchan = signal_data;
	int rc;
	struct gsm_network *net;
	struct gsm_trans *trans;

	if (subsys != SS_ABISIP)
		return 0;

	/* RTP bridge handling */
	if (lchan->conn && lchan->conn->mncc_rtp_bridge)
		return tch_rtp_signal(lchan, signal);

	/* in case we use direct BTS-to-BTS RTP */
	if (ipacc_rtp_direct)
		return 0;

	switch (signal) {
	case S_ABISIP_CRCX_ACK:
		/* in case we don't use direct BTS-to-BTS RTP */
		/* the BTS has successfully bound a TCH to a local ip/port,
		 * which means we can connect our UDP socket to it */
		if (lchan->abis_ip.rtp_socket) {
			rtp_socket_free(lchan->abis_ip.rtp_socket);
			lchan->abis_ip.rtp_socket = NULL;
		}

		lchan->abis_ip.rtp_socket = rtp_socket_create();
		if (!lchan->abis_ip.rtp_socket)
			return -EIO;

		rc = rtp_socket_connect(lchan->abis_ip.rtp_socket,
				   lchan->abis_ip.bound_ip,
				   lchan->abis_ip.bound_port);
		if (rc < 0)
			return -EIO;

		/* check if any transactions on this lchan still have
		 * a tch_recv_mncc request pending */
		net = lchan->ts->trx->bts->network;
		llist_for_each_entry(trans, &net->trans_list, entry) {
			if (trans->conn && trans->conn->lchan == lchan && trans->tch_recv) {
				DEBUGP(DCC, "pending tch_recv_mncc request\n");
				tch_recv_mncc(net, trans->callref, 1);
			}
		}

		/*
		 * TODO: this appears to be too early? Why not until after
		 * the handover detect or the handover complete?
		 *
		 * Do we have a handover pending for this new lchan? In that
		 * case re-route the audio from the old channel to the new one.
		 */
		maybe_switch_for_handover(lchan);
		break;
	case S_ABISIP_DLCX_IND:
		/* the BTS tells us a RTP stream has been disconnected */
		if (lchan->abis_ip.rtp_socket) {
			rtp_socket_free(lchan->abis_ip.rtp_socket);
			lchan->abis_ip.rtp_socket = NULL;
		}

		break;
	}

	return 0;
}

/* map two ipaccess RTP streams onto each other */
static int tch_map(struct gsm_lchan *lchan, struct gsm_lchan *remote_lchan)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	struct gsm_bts *remote_bts = remote_lchan->ts->trx->bts;
	enum gsm_chan_t lt = lchan->type, rt = remote_lchan->type;
	enum gsm48_chan_mode lm = lchan->tch_mode, rm = remote_lchan->tch_mode;
	int rc;

	DEBUGP(DCC, "Setting up TCH map between (bts=%u,trx=%u,ts=%u,%s) and "
	       "(bts=%u,trx=%u,ts=%u,%s)\n",
	       bts->nr, lchan->ts->trx->nr, lchan->ts->nr,
	       get_value_string(gsm_chan_t_names, lt),
	       remote_bts->nr, remote_lchan->ts->trx->nr, remote_lchan->ts->nr,
	       get_value_string(gsm_chan_t_names, rt));

	if (bts->type != remote_bts->type) {
		LOGP(DCC, LOGL_ERROR, "Cannot switch calls between different BTS types yet\n");
		return -EINVAL;
	}

	if (lt != rt) {
		LOGP(DCC, LOGL_ERROR, "Cannot patch through call with different"
		     " channel types: local = %s, remote = %s\n",
		     get_value_string(gsm_chan_t_names, lt),
		     get_value_string(gsm_chan_t_names, rt));
		return -EBADSLT;
	}

	if (lm != rm) {
		LOGP(DCC, LOGL_ERROR, "Cannot patch through call with different"
		     " channel modes: local = %s, remote = %s\n",
		     get_value_string(gsm48_chan_mode_names, lm),
		     get_value_string(gsm48_chan_mode_names, rm));
		return -EMEDIUMTYPE;
	}

	// todo: map between different bts types
	switch (bts->type) {
	case GSM_BTS_TYPE_NANOBTS:
	case GSM_BTS_TYPE_OSMOBTS:
		if (!ipacc_rtp_direct) {
			if (!lchan->abis_ip.rtp_socket) {
				LOGP(DHO, LOGL_ERROR, "no RTP socket for "
					"lchan\n");
				return -EIO;
			}
			if (!remote_lchan->abis_ip.rtp_socket) {
				LOGP(DHO, LOGL_ERROR, "no RTP socket for "
					"remote_lchan\n");
				return -EIO;
			}

			/* connect the TCH's to our RTP proxy */
			rc = rsl_ipacc_mdcx_to_rtpsock(lchan);
			if (rc < 0)
				return rc;
			rc = rsl_ipacc_mdcx_to_rtpsock(remote_lchan);
			if (rc < 0)
				return rc;
			/* connect them with each other */
			rtp_socket_proxy(lchan->abis_ip.rtp_socket,
					 remote_lchan->abis_ip.rtp_socket);
		} else {
			/* directly connect TCH RTP streams to each other */
			rc = rsl_ipacc_mdcx(lchan, remote_lchan->abis_ip.bound_ip,
						remote_lchan->abis_ip.bound_port,
						remote_lchan->abis_ip.rtp_payload2);
			if (rc < 0)
				return rc;
			rc = rsl_ipacc_mdcx(remote_lchan, lchan->abis_ip.bound_ip,
						lchan->abis_ip.bound_port,
						lchan->abis_ip.rtp_payload2);
		}
		break;
	case GSM_BTS_TYPE_BS11:
	case GSM_BTS_TYPE_RBS2000:
	case GSM_BTS_TYPE_NOKIA_SITE:
		trau_mux_map_lchan(lchan, remote_lchan);
		break;
	default:
		LOGP(DCC, LOGL_ERROR, "Unknown BTS type %u\n", bts->type);
		return -EINVAL;
	}

	return 0;
}

/* bridge channels of two transactions */
static int tch_bridge(struct gsm_network *net, struct gsm_mncc_bridge *bridge)
{
	struct gsm_trans *trans1 = trans_find_by_callref(net, bridge->callref[0]);
	struct gsm_trans *trans2 = trans_find_by_callref(net, bridge->callref[1]);

	if (!trans1 || !trans2)
		return -EIO;

	if (!trans1->conn || !trans2->conn)
		return -EIO;

	/* Which subscriber do we want to track trans1 or trans2? */
	log_set_context(LOG_CTX_VLR_SUBSCR, trans1->subscr);

	/* through-connect channel */
	return tch_map(trans1->conn->lchan, trans2->conn->lchan);
}

/* enable receive of channels to MNCC upqueue */
static int tch_recv_mncc(struct gsm_network *net, uint32_t callref, int enable)
{
	struct gsm_trans *trans;
	struct gsm_lchan *lchan;
	struct gsm_bts *bts;
	int rc;

	/* Find callref */
	trans = trans_find_by_callref(net, callref);
	if (!trans)
		return -EIO;
	if (!trans->conn)
		return 0;

	log_set_context(LOG_CTX_VLR_SUBSCR, trans->subscr);
	lchan = trans->conn->lchan;
	bts = lchan->ts->trx->bts;

	/* store receive state */
	trans->tch_recv = enable;

	switch (bts->type) {
	case GSM_BTS_TYPE_NANOBTS:
	case GSM_BTS_TYPE_OSMOBTS:
		if (ipacc_rtp_direct) {
			LOGP(DCC, LOGL_ERROR, "Error: RTP proxy is disabled\n");
			return -EINVAL;
		}
		/* In case, we don't have a RTP socket to the BTS yet, the BTS
		 * will not be connected to our RTP proxy and the socket will
		 * not be assigned to the application interface. This method
		 * will be called again, once the audio socket is created and
		 * connected. */
		if (!lchan->abis_ip.rtp_socket) {
			DEBUGP(DCC, "queue tch_recv_mncc request (%d)\n", enable);
			return 0;
		}
		if (enable) {
			/* connect the TCH's to our RTP proxy */
			rc = rsl_ipacc_mdcx_to_rtpsock(lchan);
			if (rc < 0)
				return rc;
			/* assign socket to application interface */
			rtp_socket_upstream(lchan->abis_ip.rtp_socket,
				net, callref);
		} else
			rtp_socket_upstream(lchan->abis_ip.rtp_socket,
				net, 0);
		break;
	case GSM_BTS_TYPE_BS11:
	case GSM_BTS_TYPE_RBS2000:
	case GSM_BTS_TYPE_NOKIA_SITE:
		/* In case we don't have a TCH with correct mode, the TRAU muxer
		 * will not be asigned to the application interface. This is
		 * performed by switch_trau_mux() after successful handover or
		 * assignment. */
		if (lchan->tch_mode == GSM48_CMODE_SIGN) {
			DEBUGP(DCC, "queue tch_recv_mncc request (%d)\n", enable);
			return 0;
		}
		if (enable)
			return trau_recv_lchan(lchan, callref);
		return trau_mux_unmap(NULL, callref);
		break;
	default:
		LOGP(DCC, LOGL_ERROR, "Unknown BTS type %u\n", bts->type);
		return -EINVAL;
	}

	return 0;
}

static int gsm48_cc_rx_status_enq(struct gsm_trans *trans, struct msgb *msg)
{
	DEBUGP(DCC, "-> STATUS ENQ\n");
	return gsm48_cc_tx_status(trans, msg);
}

static int gsm48_cc_tx_release(struct gsm_trans *trans, void *arg);
static int gsm48_cc_tx_disconnect(struct gsm_trans *trans, void *arg);

static void gsm48_cc_timeout(void *arg)
{
	struct gsm_trans *trans = arg;
	int disconnect = 0, release = 0;
	int mo_cause = GSM48_CC_CAUSE_RECOVERY_TIMER;
	int mo_location = GSM48_CAUSE_LOC_USER;
	int l4_cause = GSM48_CC_CAUSE_NORMAL_UNSPEC;
	int l4_location = GSM48_CAUSE_LOC_PRN_S_LU;
	struct gsm_mncc mo_rel, l4_rel;

	memset(&mo_rel, 0, sizeof(struct gsm_mncc));
	mo_rel.callref = trans->callref;
	memset(&l4_rel, 0, sizeof(struct gsm_mncc));
	l4_rel.callref = trans->callref;

	switch(trans->cc.Tcurrent) {
	case 0x303:
		release = 1;
		l4_cause = GSM48_CC_CAUSE_USER_NOTRESPOND;
		break;
	case 0x310:
		disconnect = 1;
		l4_cause = GSM48_CC_CAUSE_USER_NOTRESPOND;
		break;
	case 0x313:
		disconnect = 1;
		/* unknown, did not find it in the specs */
		break;
	case 0x301:
		disconnect = 1;
		l4_cause = GSM48_CC_CAUSE_USER_NOTRESPOND;
		break;
	case 0x308:
		if (!trans->cc.T308_second) {
			/* restart T308 a second time */
			gsm48_cc_tx_release(trans, &trans->cc.msg);
			trans->cc.T308_second = 1;
			break; /* stay in release state */
		}
		trans_free(trans);
		return;
//		release = 1;
//		l4_cause = 14;
//		break;
	case 0x306:
		release = 1;
		mo_cause = trans->cc.msg.cause.value;
		mo_location = trans->cc.msg.cause.location;
		break;
	case 0x323:
		disconnect = 1;
		break;
	default:
		release = 1;
	}

	if (release && trans->callref) {
		/* process release towards layer 4 */
		mncc_release_ind(trans->net, trans, trans->callref,
				 l4_location, l4_cause);
		trans->callref = 0;
	}

	if (disconnect && trans->callref) {
		/* process disconnect towards layer 4 */
		mncc_set_cause(&l4_rel, l4_location, l4_cause);
		mncc_recvmsg(trans->net, trans, MNCC_DISC_IND, &l4_rel);
	}

	/* process disconnect towards mobile station */
	if (disconnect || release) {
		mncc_set_cause(&mo_rel, mo_location, mo_cause);
		mo_rel.cause.diag[0] = ((trans->cc.Tcurrent & 0xf00) >> 8) + '0';
		mo_rel.cause.diag[1] = ((trans->cc.Tcurrent & 0x0f0) >> 4) + '0';
		mo_rel.cause.diag[2] = (trans->cc.Tcurrent & 0x00f) + '0';
		mo_rel.cause.diag_len = 3;

		if (disconnect)
			gsm48_cc_tx_disconnect(trans, &mo_rel);
		if (release)
			gsm48_cc_tx_release(trans, &mo_rel);
	}

}

/* disconnect both calls from the bridge */
static inline void disconnect_bridge(struct gsm_network *net,
				     struct gsm_mncc_bridge *bridge, int err)
{
	struct gsm_trans *trans0 = trans_find_by_callref(net, bridge->callref[0]);
	struct gsm_trans *trans1 = trans_find_by_callref(net, bridge->callref[1]);
	struct gsm_mncc mx_rel;
	if (!trans0 || !trans1)
		return;

	DEBUGP(DCC, "Failed to bridge TCH for calls %x <-> %x :: %s \n",
	       trans0->callref, trans1->callref, strerror(err));

	memset(&mx_rel, 0, sizeof(struct gsm_mncc));
	mncc_set_cause(&mx_rel, GSM48_CAUSE_LOC_INN_NET,
		       GSM48_CC_CAUSE_CHAN_UNACCEPT);

	mx_rel.callref = trans0->callref;
	gsm48_cc_tx_disconnect(trans0, &mx_rel);

	mx_rel.callref = trans1->callref;
	gsm48_cc_tx_disconnect(trans1, &mx_rel);
}

static void gsm48_start_cc_timer(struct gsm_trans *trans, int current,
				 int sec, int micro)
{
	DEBUGP(DCC, "starting timer T%x with %d seconds\n", current, sec);
	osmo_timer_setup(&trans->cc.timer, gsm48_cc_timeout, trans);
	osmo_timer_schedule(&trans->cc.timer, sec, micro);
	trans->cc.Tcurrent = current;
}

static int gsm48_cc_rx_setup(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t msg_type = gsm48_hdr_msg_type(gh);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc setup;

	memset(&setup, 0, sizeof(struct gsm_mncc));
	setup.callref = trans->callref;
	setup.lchan_type = trans->conn->lchan->type;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* emergency setup is identified by msg_type */
	if (msg_type == GSM48_MT_CC_EMERG_SETUP)
		setup.emergency = 1;

	/* use subscriber as calling party number */
	setup.fields |= MNCC_F_CALLING;
	osmo_strlcpy(setup.calling.number, trans->subscr->extension,
		     sizeof(setup.calling.number));
	osmo_strlcpy(setup.imsi, trans->subscr->imsi, sizeof(setup.imsi));

	/* bearer capability */
	if (TLVP_PRESENT(&tp, GSM48_IE_BEARER_CAP)) {
		setup.fields |= MNCC_F_BEARER_CAP;
		gsm48_decode_bearer_cap(&setup.bearer_cap,
				  TLVP_VAL(&tp, GSM48_IE_BEARER_CAP)-1);
		apply_codec_restrictions(trans->conn->bts, &setup.bearer_cap);
	}
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		setup.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&setup.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* called party bcd number */
	if (TLVP_PRESENT(&tp, GSM48_IE_CALLED_BCD)) {
		setup.fields |= MNCC_F_CALLED;
		gsm48_decode_called(&setup.called,
			      TLVP_VAL(&tp, GSM48_IE_CALLED_BCD)-1);
	}
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		setup.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&setup.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		setup.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&setup.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}
	/* CLIR suppression */
	if (TLVP_PRESENT(&tp, GSM48_IE_CLIR_SUPP))
		setup.clir.sup = 1;
	/* CLIR invocation */
	if (TLVP_PRESENT(&tp, GSM48_IE_CLIR_INVOC))
		setup.clir.inv = 1;
	/* cc cap */
	if (TLVP_PRESENT(&tp, GSM48_IE_CC_CAP)) {
		setup.fields |= MNCC_F_CCCAP;
		gsm48_decode_cccap(&setup.cccap,
			     TLVP_VAL(&tp, GSM48_IE_CC_CAP)-1);
	}

	new_cc_state(trans, GSM_CSTATE_INITIATED);

	LOGP(DCC, LOGL_INFO, "Subscriber %s (%s) sends SETUP to %s\n",
	     subscr_name(trans->subscr), trans->subscr->extension,
	     setup.called.number);

	rate_ctr_inc(&trans->net->msc_ctrs->ctr[MSC_CTR_CALL_MO_SETUP]);

	/* indicate setup to MNCC */
	mncc_recvmsg(trans->net, trans, MNCC_SETUP_IND, &setup);

	/* MNCC code will modify the channel asynchronously, we should
	 * ipaccess-bind only after the modification has been made to the
	 * lchan->tch_mode */
	return 0;
}

static int gsm48_cc_tx_setup(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC STUP");
	struct gsm48_hdr *gh;
	struct gsm_mncc *setup = arg;
	int rc, trans_id;

	gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	/* transaction id must not be assigned */
	if (trans->transaction_id != 0xff) { /* unasssigned */
		DEBUGP(DCC, "TX Setup with assigned transaction. "
			"This is not allowed!\n");
		/* Temporarily out of order */
		rc = mncc_release_ind(trans->net, trans, trans->callref,
				      GSM48_CAUSE_LOC_PRN_S_LU,
				      GSM48_CC_CAUSE_RESOURCE_UNAVAIL);
		trans->callref = 0;
		trans_free(trans);
		return rc;
	}

	/* Get free transaction_id */
	trans_id = trans_assign_trans_id(trans->net, trans->subscr,
					 GSM48_PDISC_CC, 0);
	if (trans_id < 0) {
		/* no free transaction ID */
		rc = mncc_release_ind(trans->net, trans, trans->callref,
				      GSM48_CAUSE_LOC_PRN_S_LU,
				      GSM48_CC_CAUSE_RESOURCE_UNAVAIL);
		trans->callref = 0;
		trans_free(trans);
		return rc;
	}
	trans->transaction_id = trans_id;

	gh->msg_type = GSM48_MT_CC_SETUP;

	gsm48_start_cc_timer(trans, 0x303, GSM48_T303);

	/* bearer capability */
	if (setup->fields & MNCC_F_BEARER_CAP)
		gsm48_encode_bearer_cap(msg, 0, &setup->bearer_cap);
	/* facility */
	if (setup->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &setup->facility);
	/* progress */
	if (setup->fields & MNCC_F_PROGRESS)
		gsm48_encode_progress(msg, 0, &setup->progress);
	/* calling party BCD number */
	if (setup->fields & MNCC_F_CALLING)
		gsm48_encode_calling(msg, &setup->calling);
	/* called party BCD number */
	if (setup->fields & MNCC_F_CALLED)
		gsm48_encode_called(msg, &setup->called);
	/* user-user */
	if (setup->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &setup->useruser);
	/* redirecting party BCD number */
	if (setup->fields & MNCC_F_REDIRECTING)
		gsm48_encode_redirecting(msg, &setup->redirecting);
	/* signal */
	if (setup->fields & MNCC_F_SIGNAL)
		gsm48_encode_signal(msg, setup->signal);

	new_cc_state(trans, GSM_CSTATE_CALL_PRESENT);

	rate_ctr_inc(&trans->net->msc_ctrs->ctr[MSC_CTR_CALL_MT_SETUP]);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_call_conf(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc call_conf;

	gsm48_stop_cc_timer(trans);
	gsm48_start_cc_timer(trans, 0x310, GSM48_T310);

	memset(&call_conf, 0, sizeof(struct gsm_mncc));
	call_conf.callref = trans->callref;
	call_conf.lchan_type = trans->conn->lchan->type;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
#if 0
	/* repeat */
	if (TLVP_PRESENT(&tp, GSM48_IE_REPEAT_CIR))
		call_conf.repeat = 1;
	if (TLVP_PRESENT(&tp, GSM48_IE_REPEAT_SEQ))
		call_conf.repeat = 2;
#endif
	/* bearer capability */
	if (TLVP_PRESENT(&tp, GSM48_IE_BEARER_CAP)) {
		call_conf.fields |= MNCC_F_BEARER_CAP;
		gsm48_decode_bearer_cap(&call_conf.bearer_cap,
				  TLVP_VAL(&tp, GSM48_IE_BEARER_CAP)-1);
		apply_codec_restrictions(trans->conn->bts, &call_conf.bearer_cap);
	}
	/* cause */
	if (TLVP_PRESENT(&tp, GSM48_IE_CAUSE)) {
		call_conf.fields |= MNCC_F_CAUSE;
		gsm48_decode_cause(&call_conf.cause,
			     TLVP_VAL(&tp, GSM48_IE_CAUSE)-1);
	}
	/* cc cap */
	if (TLVP_PRESENT(&tp, GSM48_IE_CC_CAP)) {
		call_conf.fields |= MNCC_F_CCCAP;
		gsm48_decode_cccap(&call_conf.cccap,
			     TLVP_VAL(&tp, GSM48_IE_CC_CAP)-1);
	}

	/* IMSI of called subscriber */
	osmo_strlcpy(call_conf.imsi, trans->subscr->imsi,
		     sizeof(call_conf.imsi));

	new_cc_state(trans, GSM_CSTATE_MO_TERM_CALL_CONF);

	return mncc_recvmsg(trans->net, trans, MNCC_CALL_CONF_IND,
			    &call_conf);
}

static int gsm48_cc_tx_call_proc(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *proceeding = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC PROC");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_CALL_PROC;

	new_cc_state(trans, GSM_CSTATE_MO_CALL_PROC);

	/* bearer capability */
	if (proceeding->fields & MNCC_F_BEARER_CAP)
		gsm48_encode_bearer_cap(msg, 0, &proceeding->bearer_cap);
	/* facility */
	if (proceeding->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &proceeding->facility);
	/* progress */
	if (proceeding->fields & MNCC_F_PROGRESS)
		gsm48_encode_progress(msg, 0, &proceeding->progress);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_alerting(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc alerting;

	gsm48_stop_cc_timer(trans);
	gsm48_start_cc_timer(trans, 0x301, GSM48_T301);

	memset(&alerting, 0, sizeof(struct gsm_mncc));
	alerting.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		alerting.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&alerting.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}

	/* progress */
	if (TLVP_PRESENT(&tp, GSM48_IE_PROGR_IND)) {
		alerting.fields |= MNCC_F_PROGRESS;
		gsm48_decode_progress(&alerting.progress,
				TLVP_VAL(&tp, GSM48_IE_PROGR_IND)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		alerting.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&alerting.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	new_cc_state(trans, GSM_CSTATE_CALL_RECEIVED);

	return mncc_recvmsg(trans->net, trans, MNCC_ALERT_IND,
			    &alerting);
}

static int gsm48_cc_tx_alerting(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *alerting = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC ALERT");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_ALERTING;

	/* facility */
	if (alerting->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &alerting->facility);
	/* progress */
	if (alerting->fields & MNCC_F_PROGRESS)
		gsm48_encode_progress(msg, 0, &alerting->progress);
	/* user-user */
	if (alerting->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &alerting->useruser);

	new_cc_state(trans, GSM_CSTATE_CALL_DELIVERED);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_progress(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *progress = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC PROGRESS");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_PROGRESS;

	/* progress */
	gsm48_encode_progress(msg, 1, &progress->progress);
	/* user-user */
	if (progress->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &progress->useruser);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_connect(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *connect = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSN 04.08 CC CON");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_CONNECT;

	gsm48_stop_cc_timer(trans);
	gsm48_start_cc_timer(trans, 0x313, GSM48_T313);

	/* facility */
	if (connect->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &connect->facility);
	/* progress */
	if (connect->fields & MNCC_F_PROGRESS)
		gsm48_encode_progress(msg, 0, &connect->progress);
	/* connected number */
	if (connect->fields & MNCC_F_CONNECTED)
		gsm48_encode_connected(msg, &connect->connected);
	/* user-user */
	if (connect->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &connect->useruser);

	new_cc_state(trans, GSM_CSTATE_CONNECT_IND);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_connect(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc connect;

	gsm48_stop_cc_timer(trans);

	memset(&connect, 0, sizeof(struct gsm_mncc));
	connect.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* use subscriber as connected party number */
	connect.fields |= MNCC_F_CONNECTED;
	osmo_strlcpy(connect.connected.number, trans->subscr->extension,
		     sizeof(connect.connected.number));
	osmo_strlcpy(connect.imsi, trans->subscr->imsi, sizeof(connect.imsi));

	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		connect.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&connect.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		connect.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&connect.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		connect.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&connect.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	new_cc_state(trans, GSM_CSTATE_CONNECT_REQUEST);
	rate_ctr_inc(&trans->net->msc_ctrs->ctr[MSC_CTR_CALL_MT_CONNECT]);

	return mncc_recvmsg(trans->net, trans, MNCC_SETUP_CNF, &connect);
}


static int gsm48_cc_rx_connect_ack(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm_mncc connect_ack;

	gsm48_stop_cc_timer(trans);

	new_cc_state(trans, GSM_CSTATE_ACTIVE);
	rate_ctr_inc(&trans->net->msc_ctrs->ctr[MSC_CTR_CALL_MO_CONNECT_ACK]);

	memset(&connect_ack, 0, sizeof(struct gsm_mncc));
	connect_ack.callref = trans->callref;

	return mncc_recvmsg(trans->net, trans, MNCC_SETUP_COMPL_IND,
			    &connect_ack);
}

static int gsm48_cc_tx_connect_ack(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC CON ACK");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_CONNECT_ACK;

	new_cc_state(trans, GSM_CSTATE_ACTIVE);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_disconnect(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc disc;

	gsm48_stop_cc_timer(trans);

	new_cc_state(trans, GSM_CSTATE_DISCONNECT_REQ);

	memset(&disc, 0, sizeof(struct gsm_mncc));
	disc.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_CAUSE, 0);
	/* cause */
	if (TLVP_PRESENT(&tp, GSM48_IE_CAUSE)) {
		disc.fields |= MNCC_F_CAUSE;
		gsm48_decode_cause(&disc.cause,
			     TLVP_VAL(&tp, GSM48_IE_CAUSE)-1);
	}
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		disc.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&disc.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		disc.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&disc.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		disc.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&disc.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	return mncc_recvmsg(trans->net, trans, MNCC_DISC_IND, &disc);

}

static struct gsm_mncc_cause default_cause = {
	.location	= GSM48_CAUSE_LOC_PRN_S_LU,
	.coding		= 0,
	.rec		= 0,
	.rec_val	= 0,
	.value		= GSM48_CC_CAUSE_NORMAL_UNSPEC,
	.diag_len	= 0,
	.diag		= { 0 },
};

static int gsm48_cc_tx_disconnect(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *disc = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC DISC");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_DISCONNECT;

	gsm48_stop_cc_timer(trans);
	gsm48_start_cc_timer(trans, 0x306, GSM48_T306);

	/* cause */
	if (disc->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 1, &disc->cause);
	else
		gsm48_encode_cause(msg, 1, &default_cause);

	/* facility */
	if (disc->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &disc->facility);
	/* progress */
	if (disc->fields & MNCC_F_PROGRESS)
		gsm48_encode_progress(msg, 0, &disc->progress);
	/* user-user */
	if (disc->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &disc->useruser);

	/* store disconnect cause for T306 expiry */
	memcpy(&trans->cc.msg, disc, sizeof(struct gsm_mncc));

	new_cc_state(trans, GSM_CSTATE_DISCONNECT_IND);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_release(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc rel;
	int rc;

	gsm48_stop_cc_timer(trans);

	memset(&rel, 0, sizeof(struct gsm_mncc));
	rel.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* cause */
	if (TLVP_PRESENT(&tp, GSM48_IE_CAUSE)) {
		rel.fields |= MNCC_F_CAUSE;
		gsm48_decode_cause(&rel.cause,
			     TLVP_VAL(&tp, GSM48_IE_CAUSE)-1);
	}
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		rel.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&rel.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		rel.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&rel.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		rel.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&rel.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	if (trans->cc.state == GSM_CSTATE_RELEASE_REQ) {
		/* release collision 5.4.5 */
		rc = mncc_recvmsg(trans->net, trans, MNCC_REL_CNF, &rel);
	} else {
		rc = gsm48_tx_simple(trans->conn,
				     GSM48_PDISC_CC | (trans->transaction_id << 4),
				     GSM48_MT_CC_RELEASE_COMPL);
		rc = mncc_recvmsg(trans->net, trans, MNCC_REL_IND, &rel);
	}

	new_cc_state(trans, GSM_CSTATE_NULL);

	trans->callref = 0;
	trans_free(trans);

	return rc;
}

static int gsm48_cc_tx_release(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *rel = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC REL");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_RELEASE;

	gsm48_stop_cc_timer(trans);
	gsm48_start_cc_timer(trans, 0x308, GSM48_T308);

	/* cause */
	if (rel->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 0, &rel->cause);
	/* facility */
	if (rel->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &rel->facility);
	/* user-user */
	if (rel->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &rel->useruser);

	trans->cc.T308_second = 0;
	memcpy(&trans->cc.msg, rel, sizeof(struct gsm_mncc));

	if (trans->cc.state != GSM_CSTATE_RELEASE_REQ)
		new_cc_state(trans, GSM_CSTATE_RELEASE_REQ);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_release_compl(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc rel;
	int rc = 0;

	gsm48_stop_cc_timer(trans);

	memset(&rel, 0, sizeof(struct gsm_mncc));
	rel.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* cause */
	if (TLVP_PRESENT(&tp, GSM48_IE_CAUSE)) {
		rel.fields |= MNCC_F_CAUSE;
		gsm48_decode_cause(&rel.cause,
			     TLVP_VAL(&tp, GSM48_IE_CAUSE)-1);
	}
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		rel.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&rel.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		rel.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&rel.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		rel.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&rel.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	if (trans->callref) {
		switch (trans->cc.state) {
		case GSM_CSTATE_CALL_PRESENT:
			rc = mncc_recvmsg(trans->net, trans,
					  MNCC_REJ_IND, &rel);
			break;
		case GSM_CSTATE_RELEASE_REQ:
			rc = mncc_recvmsg(trans->net, trans,
					  MNCC_REL_CNF, &rel);
			break;
		default:
			rc = mncc_recvmsg(trans->net, trans,
					  MNCC_REL_IND, &rel);
		}
	}

	trans->callref = 0;
	trans_free(trans);

	return rc;
}

static int gsm48_cc_tx_release_compl(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *rel = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC REL COMPL");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));
	int ret;

	gh->msg_type = GSM48_MT_CC_RELEASE_COMPL;

	trans->callref = 0;

	gsm48_stop_cc_timer(trans);

	/* cause */
	if (rel->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 0, &rel->cause);
	/* facility */
	if (rel->fields & MNCC_F_FACILITY)
		gsm48_encode_facility(msg, 0, &rel->facility);
	/* user-user */
	if (rel->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 0, &rel->useruser);

	ret =  gsm48_conn_sendmsg(msg, trans->conn, trans);

	trans_free(trans);

	return ret;
}

static int gsm48_cc_rx_facility(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc fac;

	memset(&fac, 0, sizeof(struct gsm_mncc));
	fac.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_FACILITY, 0);
	/* facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_FACILITY)) {
		fac.fields |= MNCC_F_FACILITY;
		gsm48_decode_facility(&fac.facility,
				TLVP_VAL(&tp, GSM48_IE_FACILITY)-1);
	}
	/* ss-version */
	if (TLVP_PRESENT(&tp, GSM48_IE_SS_VERS)) {
		fac.fields |= MNCC_F_SSVERSION;
		gsm48_decode_ssversion(&fac.ssversion,
				 TLVP_VAL(&tp, GSM48_IE_SS_VERS)-1);
	}

	return mncc_recvmsg(trans->net, trans, MNCC_FACILITY_IND, &fac);
}

static int gsm48_cc_tx_facility(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *fac = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC FAC");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_FACILITY;

	/* facility */
	gsm48_encode_facility(msg, 1, &fac->facility);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_hold(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm_mncc hold;

	memset(&hold, 0, sizeof(struct gsm_mncc));
	hold.callref = trans->callref;
	return mncc_recvmsg(trans->net, trans, MNCC_HOLD_IND, &hold);
}

static int gsm48_cc_tx_hold_ack(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC HLD ACK");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_HOLD_ACK;

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_hold_rej(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *hold_rej = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC HLD REJ");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_HOLD_REJ;

	/* cause */
	if (hold_rej->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 1, &hold_rej->cause);
	else
		gsm48_encode_cause(msg, 1, &default_cause);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_retrieve(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm_mncc retrieve;

	memset(&retrieve, 0, sizeof(struct gsm_mncc));
	retrieve.callref = trans->callref;
	return mncc_recvmsg(trans->net, trans, MNCC_RETRIEVE_IND,
			    &retrieve);
}

static int gsm48_cc_tx_retrieve_ack(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC RETR ACK");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_RETR_ACK;

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_retrieve_rej(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *retrieve_rej = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC RETR REJ");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_RETR_REJ;

	/* cause */
	if (retrieve_rej->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 1, &retrieve_rej->cause);
	else
		gsm48_encode_cause(msg, 1, &default_cause);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_start_dtmf(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc dtmf;

	memset(&dtmf, 0, sizeof(struct gsm_mncc));
	dtmf.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, 0, 0);
	/* keypad facility */
	if (TLVP_PRESENT(&tp, GSM48_IE_KPD_FACILITY)) {
		dtmf.fields |= MNCC_F_KEYPAD;
		gsm48_decode_keypad(&dtmf.keypad,
			      TLVP_VAL(&tp, GSM48_IE_KPD_FACILITY)-1);
	}

	return mncc_recvmsg(trans->net, trans, MNCC_START_DTMF_IND, &dtmf);
}

static int gsm48_cc_tx_start_dtmf_ack(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *dtmf = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 DTMF ACK");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_START_DTMF_ACK;

	/* keypad */
	if (dtmf->fields & MNCC_F_KEYPAD)
		gsm48_encode_keypad(msg, dtmf->keypad);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_start_dtmf_rej(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *dtmf = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 DTMF REJ");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_START_DTMF_REJ;

	/* cause */
	if (dtmf->fields & MNCC_F_CAUSE)
		gsm48_encode_cause(msg, 1, &dtmf->cause);
	else
		gsm48_encode_cause(msg, 1, &default_cause);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_stop_dtmf_ack(struct gsm_trans *trans, void *arg)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 DTMF STP ACK");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_STOP_DTMF_ACK;

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_stop_dtmf(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm_mncc dtmf;

	memset(&dtmf, 0, sizeof(struct gsm_mncc));
	dtmf.callref = trans->callref;

	return mncc_recvmsg(trans->net, trans, MNCC_STOP_DTMF_IND, &dtmf);
}

static int gsm48_cc_rx_modify(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc modify;

	memset(&modify, 0, sizeof(struct gsm_mncc));
	modify.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_BEARER_CAP, 0);
	/* bearer capability */
	if (TLVP_PRESENT(&tp, GSM48_IE_BEARER_CAP)) {
		modify.fields |= MNCC_F_BEARER_CAP;
		gsm48_decode_bearer_cap(&modify.bearer_cap,
				  TLVP_VAL(&tp, GSM48_IE_BEARER_CAP)-1);
		apply_codec_restrictions(trans->conn->bts, &modify.bearer_cap);
	}

	new_cc_state(trans, GSM_CSTATE_MO_ORIG_MODIFY);

	return mncc_recvmsg(trans->net, trans, MNCC_MODIFY_IND, &modify);
}

static int gsm48_cc_tx_modify(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *modify = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC MOD");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_MODIFY;

	gsm48_start_cc_timer(trans, 0x323, GSM48_T323);

	/* bearer capability */
	gsm48_encode_bearer_cap(msg, 1, &modify->bearer_cap);

	new_cc_state(trans, GSM_CSTATE_MO_TERM_MODIFY);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_modify_complete(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc modify;

	gsm48_stop_cc_timer(trans);

	memset(&modify, 0, sizeof(struct gsm_mncc));
	modify.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_BEARER_CAP, 0);
	/* bearer capability */
	if (TLVP_PRESENT(&tp, GSM48_IE_BEARER_CAP)) {
		modify.fields |= MNCC_F_BEARER_CAP;
		gsm48_decode_bearer_cap(&modify.bearer_cap,
				  TLVP_VAL(&tp, GSM48_IE_BEARER_CAP)-1);
		apply_codec_restrictions(trans->conn->bts, &modify.bearer_cap);
	}

	new_cc_state(trans, GSM_CSTATE_ACTIVE);

	return mncc_recvmsg(trans->net, trans, MNCC_MODIFY_CNF, &modify);
}

static int gsm48_cc_tx_modify_complete(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *modify = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC MOD COMPL");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_MODIFY_COMPL;

	/* bearer capability */
	gsm48_encode_bearer_cap(msg, 1, &modify->bearer_cap);

	new_cc_state(trans, GSM_CSTATE_ACTIVE);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_modify_reject(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc modify;

	gsm48_stop_cc_timer(trans);

	memset(&modify, 0, sizeof(struct gsm_mncc));
	modify.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_BEARER_CAP, GSM48_IE_CAUSE);
	/* bearer capability */
	if (TLVP_PRESENT(&tp, GSM48_IE_BEARER_CAP)) {
		modify.fields |= GSM48_IE_BEARER_CAP;
		gsm48_decode_bearer_cap(&modify.bearer_cap,
				  TLVP_VAL(&tp, GSM48_IE_BEARER_CAP)-1);
		apply_codec_restrictions(trans->conn->bts, &modify.bearer_cap);
	}
	/* cause */
	if (TLVP_PRESENT(&tp, GSM48_IE_CAUSE)) {
		modify.fields |= MNCC_F_CAUSE;
		gsm48_decode_cause(&modify.cause,
			     TLVP_VAL(&tp, GSM48_IE_CAUSE)-1);
	}

	new_cc_state(trans, GSM_CSTATE_ACTIVE);

	return mncc_recvmsg(trans->net, trans, MNCC_MODIFY_REJ, &modify);
}

static int gsm48_cc_tx_modify_reject(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *modify = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC MOD REJ");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_MODIFY_REJECT;

	/* bearer capability */
	gsm48_encode_bearer_cap(msg, 1, &modify->bearer_cap);
	/* cause */
	gsm48_encode_cause(msg, 1, &modify->cause);

	new_cc_state(trans, GSM_CSTATE_ACTIVE);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_tx_notify(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *notify = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 CC NOT");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_NOTIFY;

	/* notify */
	gsm48_encode_notify(msg, notify->notify);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_notify(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
//	struct tlv_parsed tp;
	struct gsm_mncc notify;

	memset(&notify, 0, sizeof(struct gsm_mncc));
	notify.callref = trans->callref;
//	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len);
	if (payload_len >= 1)
		gsm48_decode_notify(&notify.notify, gh->data);

	return mncc_recvmsg(trans->net, trans, MNCC_NOTIFY_IND, &notify);
}

static int gsm48_cc_tx_userinfo(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *user = arg;
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 USR INFO");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->msg_type = GSM48_MT_CC_USER_INFO;

	/* user-user */
	if (user->fields & MNCC_F_USERUSER)
		gsm48_encode_useruser(msg, 1, &user->useruser);
	/* more data */
	if (user->more)
		gsm48_encode_more(msg);

	return gsm48_conn_sendmsg(msg, trans->conn, trans);
}

static int gsm48_cc_rx_userinfo(struct gsm_trans *trans, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	unsigned int payload_len = msgb_l3len(msg) - sizeof(*gh);
	struct tlv_parsed tp;
	struct gsm_mncc user;

	memset(&user, 0, sizeof(struct gsm_mncc));
	user.callref = trans->callref;
	tlv_parse(&tp, &gsm48_att_tlvdef, gh->data, payload_len, GSM48_IE_USER_USER, 0);
	/* user-user */
	if (TLVP_PRESENT(&tp, GSM48_IE_USER_USER)) {
		user.fields |= MNCC_F_USERUSER;
		gsm48_decode_useruser(&user.useruser,
				TLVP_VAL(&tp, GSM48_IE_USER_USER)-1);
	}
	/* more data */
	if (TLVP_PRESENT(&tp, GSM48_IE_MORE_DATA))
		user.more = 1;

	return mncc_recvmsg(trans->net, trans, MNCC_USERINFO_IND, &user);
}

static int _gsm48_lchan_modify(struct gsm_trans *trans, void *arg)
{
	struct gsm_mncc *mode = arg;
	struct gsm_lchan *lchan = trans->conn->lchan;

	/*
	 * We were forced to make an assignment a lot earlier and
	 * we should avoid sending another assignment that might
	 * even lead to a different kind of lchan (TCH/F vs. TCH/H).
	 * In case of rtp-bridge it is too late to change things
	 * here.
	 */
	if (trans->conn->mncc_rtp_bridge && lchan->tch_mode != GSM48_CMODE_SIGN)
		return 0;

	return gsm0808_assign_req(trans->conn, mode->lchan_mode,
		trans->conn->lchan->type != GSM_LCHAN_TCH_H);
}

static void mncc_recv_rtp(struct gsm_network *net, uint32_t callref,
		int cmd, uint32_t addr, uint16_t port, uint32_t payload_type,
		uint32_t payload_msg_type)
{
	uint8_t data[sizeof(struct gsm_mncc)];
	struct gsm_mncc_rtp *rtp;

	memset(&data, 0, sizeof(data));
	rtp = (struct gsm_mncc_rtp *) &data[0];

	rtp->callref = callref;
	rtp->msg_type = cmd;
	rtp->ip = addr;
	rtp->port = port;
	rtp->payload_type = payload_type;
	rtp->payload_msg_type = payload_msg_type;
	mncc_recvmsg(net, NULL, cmd, (struct gsm_mncc *)data);
}

static void mncc_recv_rtp_sock(struct gsm_network *net, struct gsm_trans *trans, int cmd)
{
	struct gsm_lchan *lchan;
	int msg_type;

	lchan = trans->conn->lchan;
	switch (lchan->abis_ip.rtp_payload) {
	case RTP_PT_GSM_FULL:
		msg_type = GSM_TCHF_FRAME;
		break;
	case RTP_PT_GSM_EFR:
		msg_type = GSM_TCHF_FRAME_EFR;
		break;
	case RTP_PT_GSM_HALF:
		msg_type = GSM_TCHH_FRAME;
		break;
	case RTP_PT_AMR:
		msg_type = GSM_TCH_FRAME_AMR;
		break;
	default:
		LOGP(DMNCC, LOGL_ERROR, "%s unknown payload type %d\n",
			gsm_lchan_name(lchan), lchan->abis_ip.rtp_payload);
		msg_type = 0;
		break;
	}

	return mncc_recv_rtp(net, trans->callref, cmd,
			lchan->abis_ip.bound_ip,
			lchan->abis_ip.bound_port,
			lchan->abis_ip.rtp_payload,
			msg_type);
}

static void mncc_recv_rtp_err(struct gsm_network *net, uint32_t callref, int cmd)
{
	return mncc_recv_rtp(net, callref, cmd, 0, 0, 0, 0);
}

static int tch_rtp_create(struct gsm_network *net, uint32_t callref)
{
	struct gsm_bts *bts;
	struct gsm_lchan *lchan;
	struct gsm_trans *trans;
	enum gsm48_chan_mode m;

	/* Find callref */
	trans = trans_find_by_callref(net, callref);
	if (!trans) {
		LOGP(DMNCC, LOGL_ERROR, "RTP create for non-existing trans\n");
		mncc_recv_rtp_err(net, callref, MNCC_RTP_CREATE);
		return -EIO;
	}
	log_set_context(LOG_CTX_VLR_SUBSCR, trans->subscr);
	if (!trans->conn) {
		LOGP(DMNCC, LOGL_NOTICE, "RTP create for trans without conn\n");
		mncc_recv_rtp_err(net, callref, MNCC_RTP_CREATE);
		return 0;
	}

	lchan = trans->conn->lchan;
	bts = lchan->ts->trx->bts;
	if (!is_ipaccess_bts(bts)) {
		/*
		 * I want this to be straight forward and have no audio flow
		 * through the nitb/osmo-mss system. This currently means that
		 * this will not work with BS11/Nokia type BTS. We would need
		 * to have a trau<->rtp bridge for these but still preferable
		 * in another process.
		 */
		LOGP(DMNCC, LOGL_ERROR, "RTP create only works with IP systems\n");
		mncc_recv_rtp_err(net, callref, MNCC_RTP_CREATE);
		return -EINVAL;
	}

	trans->conn->mncc_rtp_bridge = 1;
	/*
	 * *sigh* we need to pick a codec now. Pick the most generic one
	 * right now and hope we could fix that later on. This is very
	 * similiar to the routine above.
	 * Fallback to the internal MNCC mode to select a route.
	 */
	if (lchan->tch_mode == GSM48_CMODE_SIGN) {
		trans->conn->mncc_rtp_create_pending = 1;
		m = mncc_codec_for_mode(lchan->type);
		LOGP(DMNCC, LOGL_DEBUG, "RTP create: codec=%s, chan_type=%s\n",
		     get_value_string(gsm48_chan_mode_names, m),
		     get_value_string(gsm_chan_t_names, lchan->type));
		return gsm0808_assign_req(trans->conn, m,
				lchan->type != GSM_LCHAN_TCH_H);
	}

	mncc_recv_rtp_sock(trans->net, trans, MNCC_RTP_CREATE);
	return 0;
}

static int tch_rtp_connect(struct gsm_network *net, void *arg)
{
	struct gsm_lchan *lchan;
	struct gsm_trans *trans;
	struct gsm_mncc_rtp *rtp = arg;

	/* Find callref */
	trans = trans_find_by_callref(net, rtp->callref);
	if (!trans) {
		LOGP(DMNCC, LOGL_ERROR, "RTP connect for non-existing trans\n");
		mncc_recv_rtp_err(net, rtp->callref, MNCC_RTP_CONNECT);
		return -EIO;
	}
	log_set_context(LOG_CTX_VLR_SUBSCR, trans->subscr);
	if (!trans->conn) {
		LOGP(DMNCC, LOGL_ERROR, "RTP connect for trans without conn\n");
		mncc_recv_rtp_err(net, rtp->callref, MNCC_RTP_CONNECT);
		return 0;
	}

	lchan = trans->conn->lchan;
	LOGP(DMNCC, LOGL_DEBUG, "RTP connect: codec=%s, chan_type=%s\n",
		     get_value_string(gsm48_chan_mode_names,
				      mncc_codec_for_mode(lchan->type)),
		     get_value_string(gsm_chan_t_names, lchan->type));

	/* TODO: Check if payload_msg_type is compatible with what we have */
	if (rtp->payload_type != lchan->abis_ip.rtp_payload) {
		LOGP(DMNCC, LOGL_ERROR, "RTP connect with different RTP payload\n");
		mncc_recv_rtp_err(net, rtp->callref, MNCC_RTP_CONNECT);
	}

	/*
	 * FIXME: payload2 can't be sent with MDCX as the osmo-bts code
	 * complains about both rtp and rtp payload2 being present in the
	 * same package!
	 */
	trans->conn->mncc_rtp_connect_pending = 1;
	return rsl_ipacc_mdcx(lchan, rtp->ip, rtp->port, 0);
}

static int tch_rtp_signal(struct gsm_lchan *lchan, int signal)
{
	struct gsm_network *net;
	struct gsm_trans *tmp, *trans = NULL;

	net = lchan->ts->trx->bts->network;
	llist_for_each_entry(tmp, &net->trans_list, entry) {
		if (!tmp->conn)
			continue;
		if (tmp->conn->lchan != lchan && tmp->conn->ho_lchan != lchan)
			continue;
		trans = tmp;
		break;
	}

	if (!trans) {
		LOGP(DMNCC, LOGL_ERROR, "%s IPA abis signal but no transaction.\n",
			gsm_lchan_name(lchan));
		return 0;
	}

	switch (signal) {
	case S_ABISIP_CRCX_ACK:
		if (lchan->conn->mncc_rtp_create_pending) {
			lchan->conn->mncc_rtp_create_pending = 0;
			LOGP(DMNCC, LOGL_NOTICE, "%s sending pending RTP create ind.\n",
				gsm_lchan_name(lchan));
			mncc_recv_rtp_sock(net, trans, MNCC_RTP_CREATE);
		}
		/*
		 * TODO: this appears to be too early? Why not until after
		 * the handover detect or the handover complete?
		 */
		maybe_switch_for_handover(lchan);
		break;
	case S_ABISIP_MDCX_ACK:
		if (lchan->conn->mncc_rtp_connect_pending) {
			lchan->conn->mncc_rtp_connect_pending = 0;
			LOGP(DMNCC, LOGL_NOTICE, "%s sending pending RTP connect ind.\n",
				gsm_lchan_name(lchan));
			mncc_recv_rtp_sock(net, trans, MNCC_RTP_CONNECT);
		}
		break;
	}

	return 0;
}


static struct downstate {
	uint32_t	states;
	int		type;
	int		(*rout) (struct gsm_trans *trans, void *arg);
} downstatelist[] = {
	/* mobile originating call establishment */
	{SBIT(GSM_CSTATE_INITIATED), /* 5.2.1.2 */
	 MNCC_CALL_PROC_REQ, gsm48_cc_tx_call_proc},
	{SBIT(GSM_CSTATE_INITIATED) | SBIT(GSM_CSTATE_MO_CALL_PROC), /* 5.2.1.2 | 5.2.1.5 */
	 MNCC_ALERT_REQ, gsm48_cc_tx_alerting},
	{SBIT(GSM_CSTATE_INITIATED) | SBIT(GSM_CSTATE_MO_CALL_PROC) | SBIT(GSM_CSTATE_CALL_DELIVERED), /* 5.2.1.2 | 5.2.1.6 | 5.2.1.6 */
	 MNCC_SETUP_RSP, gsm48_cc_tx_connect},
	{SBIT(GSM_CSTATE_MO_CALL_PROC), /* 5.2.1.4.2 */
	 MNCC_PROGRESS_REQ, gsm48_cc_tx_progress},
	/* mobile terminating call establishment */
	{SBIT(GSM_CSTATE_NULL), /* 5.2.2.1 */
	 MNCC_SETUP_REQ, gsm48_cc_tx_setup},
	{SBIT(GSM_CSTATE_CONNECT_REQUEST),
	 MNCC_SETUP_COMPL_REQ, gsm48_cc_tx_connect_ack},
	 /* signalling during call */
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_NOTIFY_REQ, gsm48_cc_tx_notify},
	{ALL_STATES - SBIT(GSM_CSTATE_NULL) - SBIT(GSM_CSTATE_RELEASE_REQ),
	 MNCC_FACILITY_REQ, gsm48_cc_tx_facility},
	{ALL_STATES,
	 MNCC_START_DTMF_RSP, gsm48_cc_tx_start_dtmf_ack},
	{ALL_STATES,
	 MNCC_START_DTMF_REJ, gsm48_cc_tx_start_dtmf_rej},
	{ALL_STATES,
	 MNCC_STOP_DTMF_RSP, gsm48_cc_tx_stop_dtmf_ack},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_HOLD_CNF, gsm48_cc_tx_hold_ack},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_HOLD_REJ, gsm48_cc_tx_hold_rej},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_RETRIEVE_CNF, gsm48_cc_tx_retrieve_ack},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_RETRIEVE_REJ, gsm48_cc_tx_retrieve_rej},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_MODIFY_REQ, gsm48_cc_tx_modify},
	{SBIT(GSM_CSTATE_MO_ORIG_MODIFY),
	 MNCC_MODIFY_RSP, gsm48_cc_tx_modify_complete},
	{SBIT(GSM_CSTATE_MO_ORIG_MODIFY),
	 MNCC_MODIFY_REJ, gsm48_cc_tx_modify_reject},
	{SBIT(GSM_CSTATE_ACTIVE),
	 MNCC_USERINFO_REQ, gsm48_cc_tx_userinfo},
	/* clearing */
	{SBIT(GSM_CSTATE_INITIATED),
	 MNCC_REJ_REQ, gsm48_cc_tx_release_compl},
	{ALL_STATES - SBIT(GSM_CSTATE_NULL) - SBIT(GSM_CSTATE_DISCONNECT_IND) - SBIT(GSM_CSTATE_RELEASE_REQ) - SBIT(GSM_CSTATE_DISCONNECT_REQ), /* 5.4.4 */
	 MNCC_DISC_REQ, gsm48_cc_tx_disconnect},
	{ALL_STATES - SBIT(GSM_CSTATE_NULL) - SBIT(GSM_CSTATE_RELEASE_REQ), /* 5.4.3.2 */
	 MNCC_REL_REQ, gsm48_cc_tx_release},
	/* special */
	{ALL_STATES,
	 MNCC_LCHAN_MODIFY, _gsm48_lchan_modify},
};

#define DOWNSLLEN \
	(sizeof(downstatelist) / sizeof(struct downstate))


int mncc_tx_to_cc(struct gsm_network *net, int msg_type, void *arg)
{
	int i, rc = 0;
	struct gsm_trans *trans = NULL, *transt;
	struct gsm_subscriber_connection *conn = NULL;
	struct gsm_bts *bts = NULL;
	struct gsm_mncc *data = arg, rel;

	DEBUGP(DMNCC, "receive message %s\n", get_mncc_name(msg_type));

	/* handle special messages */
	switch(msg_type) {
	case MNCC_BRIDGE:
		rc = tch_bridge(net, arg);
		if (rc < 0)
			disconnect_bridge(net, arg, -rc);
		return rc;
	case MNCC_FRAME_DROP:
		return tch_recv_mncc(net, data->callref, 0);
	case MNCC_FRAME_RECV:
		return tch_recv_mncc(net, data->callref, 1);
	case MNCC_RTP_CREATE:
		return tch_rtp_create(net, data->callref);
	case MNCC_RTP_CONNECT:
		return tch_rtp_connect(net, arg);
	case MNCC_RTP_FREE:
		/* unused right now */
		return -EIO;
	case GSM_TCHF_FRAME:
	case GSM_TCHF_FRAME_EFR:
	case GSM_TCHH_FRAME:
	case GSM_TCH_FRAME_AMR:
		/* Find callref */
		trans = trans_find_by_callref(net, data->callref);
		if (!trans) {
			LOGP(DMNCC, LOGL_ERROR, "TCH frame for non-existing trans\n");
			return -EIO;
		}
		log_set_context(LOG_CTX_VLR_SUBSCR, trans->subscr);
		if (!trans->conn) {
			LOGP(DMNCC, LOGL_NOTICE, "TCH frame for trans without conn\n");
			return 0;
		}
		if (!trans->conn->lchan) {
			LOGP(DMNCC, LOGL_NOTICE, "TCH frame for trans without lchan\n");
			return 0;
		}
		if (trans->conn->lchan->type != GSM_LCHAN_TCH_F
		 && trans->conn->lchan->type != GSM_LCHAN_TCH_H) {
			/* This should be LOGL_ERROR or NOTICE, but
			 * unfortuantely it happens for a couple of frames at
			 * the beginning of every RTP connection */
			LOGP(DMNCC, LOGL_DEBUG, "TCH frame for lchan != TCH_F/TCH_H\n");
			return 0;
		}
		bts = trans->conn->lchan->ts->trx->bts;
		switch (bts->type) {
		case GSM_BTS_TYPE_NANOBTS:
		case GSM_BTS_TYPE_OSMOBTS:
			if (!trans->conn->lchan->abis_ip.rtp_socket) {
				DEBUGP(DMNCC, "TCH frame to lchan without RTP connection\n");
				return 0;
			}
			return rtp_send_frame(trans->conn->lchan->abis_ip.rtp_socket, arg);
		case GSM_BTS_TYPE_BS11:
		case GSM_BTS_TYPE_RBS2000:
		case GSM_BTS_TYPE_NOKIA_SITE:
			return trau_send_frame(trans->conn->lchan, arg);
		default:
			LOGP(DCC, LOGL_ERROR, "Unknown BTS type %u\n", bts->type);
		}
		return -EINVAL;
	}

	memset(&rel, 0, sizeof(struct gsm_mncc));
	rel.callref = data->callref;

	/* Find callref */
	trans = trans_find_by_callref(net, data->callref);

	/* Callref unknown */
	if (!trans) {
		struct gsm_subscriber *subscr;

		if (msg_type != MNCC_SETUP_REQ) {
			DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
				"Received '%s' from MNCC with "
				"unknown callref %d\n", data->called.number,
				get_mncc_name(msg_type), data->callref);
			/* Invalid call reference */
			return mncc_release_ind(net, NULL, data->callref,
						GSM48_CAUSE_LOC_PRN_S_LU,
						GSM48_CC_CAUSE_INVAL_TRANS_ID);
		}
		if (!data->called.number[0] && !data->imsi[0]) {
			DEBUGP(DCC, "(bts - trx - ts - ti) "
				"Received '%s' from MNCC with "
				"no number or IMSI\n", get_mncc_name(msg_type));
			/* Invalid number */
			return mncc_release_ind(net, NULL, data->callref,
						GSM48_CAUSE_LOC_PRN_S_LU,
						GSM48_CC_CAUSE_INV_NR_FORMAT);
		}
		/* New transaction due to setup, find subscriber */
		if (data->called.number[0])
			subscr = subscr_get_by_extension(net->subscr_group,
							data->called.number);
		else
			subscr = subscr_get_by_imsi(net->subscr_group,
						    data->imsi);

		/* update the subscriber we deal with */
		log_set_context(LOG_CTX_VLR_SUBSCR, subscr);

		/* If subscriber is not found */
		if (!subscr) {
			DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
				"Received '%s' from MNCC with "
				"unknown subscriber %s\n", data->called.number,
				get_mncc_name(msg_type), data->called.number);
			/* Unknown subscriber */
			return mncc_release_ind(net, NULL, data->callref,
						GSM48_CAUSE_LOC_PRN_S_LU,
						GSM48_CC_CAUSE_UNASSIGNED_NR);
		}
		/* If subscriber is not "attached" */
		if (!subscr->lac) {
			DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
				"Received '%s' from MNCC with "
				"detached subscriber %s\n", data->called.number,
				get_mncc_name(msg_type), data->called.number);
			subscr_put(subscr);
			/* Temporarily out of order */
			return mncc_release_ind(net, NULL, data->callref,
						GSM48_CAUSE_LOC_PRN_S_LU,
						GSM48_CC_CAUSE_DEST_OOO);
		}
		/* Create transaction */
		trans = trans_alloc(net, subscr, GSM48_PDISC_CC, 0xff, data->callref);
		if (!trans) {
			DEBUGP(DCC, "No memory for trans.\n");
			subscr_put(subscr);
			/* Ressource unavailable */
			mncc_release_ind(net, NULL, data->callref,
					 GSM48_CAUSE_LOC_PRN_S_LU,
					 GSM48_CC_CAUSE_RESOURCE_UNAVAIL);
			return -ENOMEM;
		}
		/* Find lchan */
		conn = connection_for_subscr(subscr);

		/* If subscriber has no lchan */
		if (!conn) {
			/* find transaction with this subscriber already paging */
			llist_for_each_entry(transt, &net->trans_list, entry) {
				/* Transaction of our lchan? */
				if (transt == trans ||
				    transt->subscr != subscr)
					continue;
				DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
					"Received '%s' from MNCC with "
					"unallocated channel, paging already "
					"started for lac %d.\n",
					data->called.number,
					get_mncc_name(msg_type), subscr->lac);
				subscr_put(subscr);
				trans_free(trans);
				return 0;
			}
			/* store setup informations until paging was successfull */
			memcpy(&trans->cc.msg, data, sizeof(struct gsm_mncc));

			/* Request a channel */
			trans->paging_request = subscr_request_channel(subscr,
							RSL_CHANNEED_TCH_F, setup_trig_pag_evt,
							trans);
			if (!trans->paging_request) {
				LOGP(DCC, LOGL_ERROR, "Failed to allocate paging token.\n");
				subscr_put(subscr);
				trans_free(trans);
				return 0;
			}
			subscr_put(subscr);
			return 0;
		}
		/* Assign lchan */
		trans->conn = conn;
		subscr_put(subscr);
	} else {
		/* update the subscriber we deal with */
		log_set_context(LOG_CTX_VLR_SUBSCR, trans->subscr);
	}

	if (trans->conn)
		conn = trans->conn;

	/* if paging did not respond yet */
	if (!conn) {
		DEBUGP(DCC, "(bts - trx - ts - ti -- sub %s) "
			"Received '%s' from MNCC in paging state\n",
			(trans->subscr)?(trans->subscr->extension):"-",
			get_mncc_name(msg_type));
		mncc_set_cause(&rel, GSM48_CAUSE_LOC_PRN_S_LU,
				GSM48_CC_CAUSE_NORM_CALL_CLEAR);
		if (msg_type == MNCC_REL_REQ)
			rc = mncc_recvmsg(net, trans, MNCC_REL_CNF, &rel);
		else
			rc = mncc_recvmsg(net, trans, MNCC_REL_IND, &rel);
		trans->callref = 0;
		trans_free(trans);
		return rc;
	}

	if (msg_type == MNCC_REL_REQ && conn->mncc_rtp_create_pending)
	        conn->mncc_rtp_create_pending = 0;

	DEBUGP(DCC, "(bts %d trx %d ts %d ti %02x sub %s) "
		"Received '%s' from MNCC in state %d (%s)\n",
		conn->bts->nr, conn->lchan->ts->trx->nr, conn->lchan->ts->nr,
		trans->transaction_id,
		(trans->conn->subscr)?(trans->conn->subscr->extension):"-",
		get_mncc_name(msg_type), trans->cc.state,
		gsm48_cc_state_name(trans->cc.state));

	/* Find function for current state and message */
	for (i = 0; i < DOWNSLLEN; i++)
		if ((msg_type == downstatelist[i].type)
		 && ((1 << trans->cc.state) & downstatelist[i].states))
			break;
	if (i == DOWNSLLEN) {
		DEBUGP(DCC, "Message unhandled at this state.\n");
		return 0;
	}

	rc = downstatelist[i].rout(trans, arg);

	return rc;
}


static struct datastate {
	uint32_t	states;
	int		type;
	int		(*rout) (struct gsm_trans *trans, struct msgb *msg);
} datastatelist[] = {
	/* mobile originating call establishment */
	{SBIT(GSM_CSTATE_NULL), /* 5.2.1.2 */
	 GSM48_MT_CC_SETUP, gsm48_cc_rx_setup},
	{SBIT(GSM_CSTATE_NULL), /* 5.2.1.2 */
	 GSM48_MT_CC_EMERG_SETUP, gsm48_cc_rx_setup},
	{SBIT(GSM_CSTATE_CONNECT_IND), /* 5.2.1.2 */
	 GSM48_MT_CC_CONNECT_ACK, gsm48_cc_rx_connect_ack},
	/* mobile terminating call establishment */
	{SBIT(GSM_CSTATE_CALL_PRESENT), /* 5.2.2.3.2 */
	 GSM48_MT_CC_CALL_CONF, gsm48_cc_rx_call_conf},
	{SBIT(GSM_CSTATE_CALL_PRESENT) | SBIT(GSM_CSTATE_MO_TERM_CALL_CONF), /* ???? | 5.2.2.3.2 */
	 GSM48_MT_CC_ALERTING, gsm48_cc_rx_alerting},
	{SBIT(GSM_CSTATE_CALL_PRESENT) | SBIT(GSM_CSTATE_MO_TERM_CALL_CONF) | SBIT(GSM_CSTATE_CALL_RECEIVED), /* (5.2.2.6) | 5.2.2.6 | 5.2.2.6 */
	 GSM48_MT_CC_CONNECT, gsm48_cc_rx_connect},
	 /* signalling during call */
	{ALL_STATES - SBIT(GSM_CSTATE_NULL),
	 GSM48_MT_CC_FACILITY, gsm48_cc_rx_facility},
	{SBIT(GSM_CSTATE_ACTIVE),
	 GSM48_MT_CC_NOTIFY, gsm48_cc_rx_notify},
	{ALL_STATES,
	 GSM48_MT_CC_START_DTMF, gsm48_cc_rx_start_dtmf},
	{ALL_STATES,
	 GSM48_MT_CC_STOP_DTMF, gsm48_cc_rx_stop_dtmf},
	{ALL_STATES,
	 GSM48_MT_CC_STATUS_ENQ, gsm48_cc_rx_status_enq},
	{SBIT(GSM_CSTATE_ACTIVE),
	 GSM48_MT_CC_HOLD, gsm48_cc_rx_hold},
	{SBIT(GSM_CSTATE_ACTIVE),
	 GSM48_MT_CC_RETR, gsm48_cc_rx_retrieve},
	{SBIT(GSM_CSTATE_ACTIVE),
	 GSM48_MT_CC_MODIFY, gsm48_cc_rx_modify},
	{SBIT(GSM_CSTATE_MO_TERM_MODIFY),
	 GSM48_MT_CC_MODIFY_COMPL, gsm48_cc_rx_modify_complete},
	{SBIT(GSM_CSTATE_MO_TERM_MODIFY),
	 GSM48_MT_CC_MODIFY_REJECT, gsm48_cc_rx_modify_reject},
	{SBIT(GSM_CSTATE_ACTIVE),
	 GSM48_MT_CC_USER_INFO, gsm48_cc_rx_userinfo},
	/* clearing */
	{ALL_STATES - SBIT(GSM_CSTATE_NULL) - SBIT(GSM_CSTATE_RELEASE_REQ), /* 5.4.3.2 */
	 GSM48_MT_CC_DISCONNECT, gsm48_cc_rx_disconnect},
	{ALL_STATES - SBIT(GSM_CSTATE_NULL), /* 5.4.4.1.2.2 */
	 GSM48_MT_CC_RELEASE, gsm48_cc_rx_release},
	{ALL_STATES, /* 5.4.3.4 */
	 GSM48_MT_CC_RELEASE_COMPL, gsm48_cc_rx_release_compl},
};

#define DATASLLEN \
	(sizeof(datastatelist) / sizeof(struct datastate))

static int gsm0408_rcv_cc(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t msg_type = gsm48_hdr_msg_type(gh);
	uint8_t transaction_id = gsm48_hdr_trans_id_flip_ti(gh);
	struct gsm_trans *trans = NULL;
	int i, rc = 0;

	if (msg_type & 0x80) {
		DEBUGP(DCC, "MSG 0x%2x not defined for PD error\n", msg_type);
		return -EINVAL;
	}

	if (!conn->subscr) {
		LOGP(DCC, LOGL_ERROR, "Invalid conn, no subscriber\n");
		return -EINVAL;
	}

	/* Find transaction */
	trans = trans_find_by_id(conn, GSM48_PDISC_CC, transaction_id);

	DEBUGP(DCC, "(bts %d trx %d ts %d ti %x sub %s) "
		"Received '%s' from MS in state %d (%s)\n",
		conn->bts->nr, conn->lchan->ts->trx->nr, conn->lchan->ts->nr,
		transaction_id, (conn->subscr)?(conn->subscr->extension):"-",
		gsm48_cc_msg_name(msg_type), trans?(trans->cc.state):0,
		gsm48_cc_state_name(trans?(trans->cc.state):0));

	/* Create transaction */
	if (!trans) {
		DEBUGP(DCC, "Unknown transaction ID %x, "
			"creating new trans.\n", transaction_id);
		/* Create transaction */
		trans = trans_alloc(conn->network, conn->subscr,
				    GSM48_PDISC_CC,
				    transaction_id, new_callref++);
		if (!trans) {
			DEBUGP(DCC, "No memory for trans.\n");
			rc = gsm48_tx_simple(conn,
					     GSM48_PDISC_CC | (transaction_id << 4),
					     GSM48_MT_CC_RELEASE_COMPL);
			return -ENOMEM;
		}
		/* Assign transaction */
		trans->conn = conn;
	}

	/* find function for current state and message */
	for (i = 0; i < DATASLLEN; i++)
		if ((msg_type == datastatelist[i].type)
		 && ((1 << trans->cc.state) & datastatelist[i].states))
			break;
	if (i == DATASLLEN) {
		DEBUGP(DCC, "Message unhandled at this state.\n");
		return 0;
	}

	assert(trans->subscr);

	rc = datastatelist[i].rout(trans, msg);

	return rc;
}

/* Create a dummy to wait five seconds */
static void release_anchor(struct gsm_subscriber_connection *conn)
{
	if (!conn->anch_operation)
		return;

	osmo_timer_del(&conn->anch_operation->timeout);
	talloc_free(conn->anch_operation);
	conn->anch_operation = NULL;
}

static void anchor_timeout(void *_data)
{
	struct gsm_subscriber_connection *con = _data;

	release_anchor(con);
	msc_release_connection(con);
}

int gsm0408_new_conn(struct gsm_subscriber_connection *conn)
{
	conn->anch_operation = talloc_zero(conn, struct gsm_anchor_operation);
	if (!conn->anch_operation)
		return -1;

	osmo_timer_setup(&conn->anch_operation->timeout, anchor_timeout, conn);
	osmo_timer_schedule(&conn->anch_operation->timeout, 5, 0);
	return 0;
}

struct gsm_subscriber_connection *msc_subscr_con_allocate(struct gsm_network *network)
{
	struct gsm_subscriber_connection *conn;

	conn = talloc_zero(network, struct gsm_subscriber_connection);
	if (!conn)
		return NULL;

	conn->network = network;
	llist_add_tail(&conn->entry, &network->subscr_conns);
	return conn;
}

void msc_subscr_con_free(struct gsm_subscriber_connection *conn)
{
	if (!conn)
		return;

	if (conn->subscr) {
		subscr_put(conn->subscr);
		conn->subscr = NULL;
	}

	llist_del(&conn->entry);
	talloc_free(conn);
}

/* Main entry point for GSM 04.08/44.008 Layer 3 data (e.g. from the BSC). */
int gsm0408_dispatch(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t pdisc = gsm48_hdr_pdisc(gh);
	int rc = 0;

	OSMO_ASSERT(conn);
	OSMO_ASSERT(msg);

	LOGP(DRLL, LOGL_DEBUG, "Dispatching 04.08 message, pdisc=%d\n", pdisc);
#if 0
	if (silent_call_reroute(conn, msg))
		return silent_call_rx(conn, msg);
#endif

	switch (pdisc) {
	case GSM48_PDISC_CC:
		release_anchor(conn);
		rc = gsm0408_rcv_cc(conn, msg);
		break;
	case GSM48_PDISC_MM:
		rc = gsm0408_rcv_mm(conn, msg);
		break;
	case GSM48_PDISC_RR:
		rc = gsm0408_rcv_rr(conn, msg);
		break;
	case GSM48_PDISC_SMS:
		release_anchor(conn);
		rc = gsm0411_rcv_sms(conn, msg);
		break;
	case GSM48_PDISC_MM_GPRS:
	case GSM48_PDISC_SM_GPRS:
		LOGP(DRLL, LOGL_NOTICE, "Unimplemented "
			"GSM 04.08 discriminator 0x%02x\n", pdisc);
		rc = -ENOTSUP;
		break;
	case GSM48_PDISC_NC_SS:
		release_anchor(conn);
		rc = handle_rcv_ussd(conn, msg);
		break;
	case GSM48_PDISC_TEST:
		rc = gsm0414_rcv_test(conn, msg);
		break;
	default:
		LOGP(DRLL, LOGL_NOTICE, "Unknown "
			"GSM 04.08 discriminator 0x%02x\n", pdisc);
		rc = -EINVAL;
		break;
	}

	return rc;
}

/*
 * This will be run by the linker when loading the DSO. We use it to
 * do system initialization, e.g. registration of signal handlers.
 */
static __attribute__((constructor)) void on_dso_load_0408(void)
{
	osmo_signal_register_handler(SS_ABISIP, handle_abisip_signal, NULL);
}
