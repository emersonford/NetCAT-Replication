/* vim: set noet: */
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <arpa/inet.h>

#include <infiniband/verbs.h>

#include "resources.h"
#include "sockets.h"

/******************************************************************************
 * *	Function: post_receive
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 *  *	Output
 *  *	none
 *  *
 *  *	Returns
 *  *	0 on success, error code on failure
 *  *
 *  *	Description
 *  *
 *  ******************************************************************************/

static int post_receive(struct resources *res)
{
	struct ibv_recv_wr	rr;
	struct ibv_sge		sge;
	struct ibv_recv_wr	*bad_wr;
	int			rc;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = config.msg_size;
	sge.lkey = res->mr->lkey;

	/* prepare the receive work request */
	memset(&rr, 0, sizeof(rr));
	rr.next = NULL;
	rr.wr_id = 0;
	rr.sg_list = &sge;
	rr.num_sge = 1;

	/* post the Receive Request to the RQ */
	rc = ibv_post_recv(res->qp, &rr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post RR\n");
	else
		debug_print("Receive Request was posted\n");

	return rc;
}


/**
 * Pin all current and future memory pages in memory so that the OS does not
 * swap them to disk.
 *
 * Note that future mapping operations (e.g. mmap, stack expansion, etc)
 * may fail if their memory cannot be pinned due to resource limits. Thus the
 * check below may not capture all possible failures up front. It's probably
 * best to call this at the end of initialisation (after most large allocations
 * have been made).
 */
static void pin_all_memory() {
	int r = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (r != 0) {
		fprintf(stderr, "Could not lock all memory pages (%s)\n", strerror(errno));
	}
}


void resources_init(struct resources *res)
{
	memset(res, 0, sizeof *res);
	res->sock = -1;
}


int resources_create(struct resources *res)
{
	struct ibv_device	 **dev_list = NULL;
	struct ibv_qp_init_attr  qp_init_attr;
	struct ibv_device	 *ib_dev = NULL;
	size_t			 size;
	int		 	 i, j;
	int			 mr_flags = 0;
	int			 cq_size = 0;
	int			 num_devices;
	int			 rc = 0;
	char			 curr_num = 0;


	if (config.server_name)	{
		/* Client side */
		res->sock = sock_connect(config.server_name, config.tcp_port);
		if (res->sock < 0) {
			fprintf(stderr, "[Client only] failed to establish TCP connection to server %s, port %d\n", config.server_name, config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}
	} else {
		/* server side */
		debug_print("[Server only] waiting on port %d for TCP connection\n", config.tcp_port);
		res->sock = sock_connect(NULL, config.tcp_port);
		if (res->sock < 0) {
			fprintf(stderr, "[Server only] failed to establish TCP connection with client on port %d\n", config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}
	}

	debug_print("TCP connection was established\n");
	debug_print("searching for IB devices in host\n");

	/* get device names in the system */
	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		fprintf(stderr, "failed to get IB devices list\n");
		rc = 1;
		goto resources_create_exit;
	}

	/* if there isn't any IB device in host */
	if (!num_devices) {
		fprintf(stderr, "found %d device(s)\n", num_devices);
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("found %d device(s)\n", num_devices);

	/* search for the specific device we want to work with */
	for (i = 0; i < num_devices; i ++) {
		if (!config.dev_name) {
			config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
			debug_print("device not specified, using first one found: %s\n", config.dev_name);
		}
		if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
			ib_dev = dev_list[i];
			break;
		}
	}

	/* if the device wasn't found in host */
	if (!ib_dev) {
		fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}

	/* get device handle */
	res->ib_ctx = ibv_open_device(ib_dev);
	if (!res->ib_ctx) {
		fprintf(stderr, "failed to open device %s\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}

	/* We are now done with device list, free it */
	ibv_free_device_list(dev_list);
	dev_list = NULL;
	ib_dev = NULL;

	/* query port properties */
	if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
		fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
		rc = 1;
		goto resources_create_exit;
	}

	/* allocate Protection Domain */
	res->pd = ibv_alloc_pd(res->ib_ctx);
	if (!res->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		rc = 1;
		goto resources_create_exit;
	}

	/* each side will send only one WR, so Completion Queue with 1 entry is enough */
	cq_size = 1;
	res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
	if (!res->cq) {
		fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
		rc = 1;
		goto resources_create_exit;
	}

	/* allocate the memory buffer that will hold the data */
	if (!config.server_name)
		size = config.row_count * (config.column_count * config.msg_size);
	else
		size = config.msg_size;

	res->buf = (char *) malloc(size);
	pin_all_memory();

	if (!res->buf) {
		fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
		rc = 1;
		goto resources_create_exit;
	}

	if (!config.server_name) {
		for (i = 0; i < config.row_count; i++) {
			for (j = 0; j < config.column_count; j++) {
				res->buf[j * (config.column_count * config.msg_size) + i * (config.msg_size)] = curr_num;
				curr_num++;
			}
		}
	}
	else
		memset(res->buf, 0 , size);

	/* register the memory buffer */
	mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
	if (!res->mr) {
		fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

	/* create the Queue Pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.sq_sig_all = 0;
	qp_init_attr.send_cq = res->cq;
	qp_init_attr.recv_cq = res->cq;
	qp_init_attr.cap.max_send_wr  = 1;
	qp_init_attr.cap.max_recv_wr  = 1;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;

	res->qp = ibv_create_qp(res->pd, &qp_init_attr);
	if (!res->qp) {
		fprintf(stderr, "failed to create QP\n");
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("QP was created, QP number=0x%x\n", res->qp->qp_num);

resources_create_exit:
	if (rc) {
		/* Error encountered, cleanup */

		if (res->qp) {
			ibv_destroy_qp(res->qp);
			res->qp = NULL;
		}

		if (res->mr) {
			ibv_dereg_mr(res->mr);
			res->mr = NULL;
		}

		if (res->buf) {
			free(res->buf);
			res->buf = NULL;
		}

		if (res->cq) {
			ibv_destroy_cq(res->cq);
			res->cq = NULL;
		}

		if (res->pd) {
			ibv_dealloc_pd(res->pd);
			res->pd = NULL;
		}

		if (res->ib_ctx) {
			ibv_close_device(res->ib_ctx);
			res->ib_ctx = NULL;
		}

		if (dev_list) {
			ibv_free_device_list(dev_list);
			dev_list = NULL;
		}

		if (res->sock >= 0) {
			if (close(res->sock))
				fprintf(stderr, "failed to close socket\n");
			res->sock = -1;
		}
	}

	return rc;
}


int modify_qp_to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = config.ib_port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to INIT (%s)\n", strerror(errno));

	return rc;
}


int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, const uint8_t *dgid)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_2048;
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dlid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config.ib_port;
	if (config.gid_idx >= 0) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.port_num = 1;
		memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.sgid_index = config.gid_idx;
		attr.ah_attr.grh.traffic_class = 0;
	}

	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTR (%s)\n", strerror(errno));

	return rc;
}


int modify_qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state	= IBV_QPS_RTS;
	attr.timeout	= 0x12;
	attr.retry_cnt	= 6;
	attr.rnr_retry	= 0;
	attr.sq_psn	= 0;
	attr.max_rd_atomic = 1;

	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTS (%s)\n", strerror(errno));

	return rc;
}


int connect_qp(struct resources *res)
{
	struct cm_con_data_t	local_con_data;
	struct cm_con_data_t	remote_con_data;
	struct cm_con_data_t	tmp_con_data;
	int			rc = 0;
	char 			temp_char;
	union ibv_gid		my_gid;


	if (config.gid_idx >= 0) {
		rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
		if (rc) {
			fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
			return rc;
		}
	} else
		memset(&my_gid, 0, sizeof my_gid);

	/* exchange using TCP sockets info required to connect QPs */
	local_con_data.addr = htonll((uintptr_t)res->buf);
	local_con_data.rkey = htonl(res->mr->rkey);
	local_con_data.qp_num = htonl(res->qp->qp_num);
	local_con_data.lid = htons(res->port_attr.lid);
	memcpy(local_con_data.gid, &my_gid, sizeof(my_gid));

	debug_print("\nLocal LID	= 0x%x\n", res->port_attr.lid);
	if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0) {
		fprintf(stderr, "failed to exchange connection data between sides\n");
		return 1;
	}

	remote_con_data.addr = ntohll(tmp_con_data.addr);
	remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	remote_con_data.lid = ntohs(tmp_con_data.lid);
	memcpy(remote_con_data.gid, tmp_con_data.gid, sizeof(my_gid));

	/* save the remote side attributes, we will need it for the post SR */
	res->remote_props = remote_con_data;

	debug_print("Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
	debug_print("Remote rkey = 0x%x\n", remote_con_data.rkey);
	debug_print("Remote QP number = 0x%x\n", remote_con_data.qp_num);
	debug_print("Remote LID = 0x%x\n", remote_con_data.lid);
	if (config.gid_idx >= 0) {
		const uint8_t *p = remote_con_data.gid;

		debug_print("Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	/* modify the QP to init */
	rc = modify_qp_to_init(res->qp);
	if (rc) {
		fprintf(stderr, "change QP state to INIT failed\n");
		return rc;
	}

	/* let the client post RR to be prepared for incoming messages */
	if (config.server_name) {
		rc = post_receive(res);
		if (rc) {
			fprintf(stderr, "failed to post RR\n");
			return rc;
		}
	}


	/* modify the QP to RTR */
	rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTR (%s)\n", strerror(errno));
		return rc;
	}

	rc = modify_qp_to_rts(res->qp);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTS (%s)\n", strerror(errno));
		return rc;
	}

	debug_print("QP state was change to RTS\n");

	/* sync to make sure that both sides are in states that they can connect to prevent packet loose */
	if (sock_sync_data(res->sock, 1, "Q", &temp_char)) { /* just send a dummy char back and forth */
		fprintf(stderr, "sync error after QPs were moved to RTS\n");
		return rc;
	}

	return 0;
}


int resources_destroy(struct resources *res)
{
	int rc = 0;

	if (res->qp)
		if (ibv_destroy_qp(res->qp)) {
			fprintf(stderr, "failed to destroy QP\n");
			rc = 1;
		}

	if (res->mr)
		if (ibv_dereg_mr(res->mr)) {
			fprintf(stderr, "failed to deregister MR\n");
			rc = 1;
		}

	if (res->buf)
		free(res->buf);

	if (res->cq)
		if (ibv_destroy_cq(res->cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			rc = 1;
		}

	if (res->pd)
		if (ibv_dealloc_pd(res->pd)) {
			fprintf(stderr, "failed to deallocate PD\n");
			rc = 1;
		}

	if (res->ib_ctx)
		if (ibv_close_device(res->ib_ctx)) {
			fprintf(stderr, "failed to close device context\n");
			rc = 1;
		}

	if (res->sock >= 0)
		if (close(res->sock)) {
			fprintf(stderr, "failed to close socket\n");
			rc = 1;
		}

	return rc;
}
