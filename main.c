/* vim: set noet: */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <endian.h>
#include <getopt.h>
#include <sys/time.h>
#include <errno.h>
#include <sched.h>
#include <emmintrin.h>

#include <infiniband/verbs.h>

#include "get_clock.h"
#include "sockets.h"
#include "resources.h"
#include "print.h"

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 2000

#define CACHE_SIZE 64
#define CACHE_LINES (8192 * 1024 / CACHE_SIZE)
#define BM_BITS_PER_WORD (sizeof(uint64_t) * CHAR_BIT)
#define BM_WORDS (CACHE_LINES / BM_BITS_PER_WORD)

uint64_t bm[BM_WORDS] = {0};
#define WORD_OFFSET(b) ((b) / BM_BITS_PER_WORD)
#define BIT_OFFSET(b)  ((b) % BM_BITS_PER_WORD)

void bm_set(unsigned int addr) {
	bm[WORD_OFFSET(addr)] |= 1ull << BIT_OFFSET(addr);
}
void bm_clear(unsigned int addr) {
	bm[WORD_OFFSET(addr)] &= ~(1ull << BIT_OFFSET(addr));
}

bool bm_read(unsigned int addr) {
	return (bm[WORD_OFFSET(addr)] & (1ull << BIT_OFFSET(addr))) != 0;
}

unsigned int rand_line() {
	unsigned int r;
	while (bm_read(r = rand() % CACHE_LINES));
	bm_set(r);
	return r;
}

/* default config */
struct config_t config = {
	NULL,	/* dev_name */
	NULL,	/* server_name */
	19875,	/* tcp_port */
	1,	/* ib_port */
	-1,	/* gid_idx */
	1000, /* iters */
	0, /* mode */
	8, /* msg_size */
	1024, /* column count, resulting size of row is two pages */
	8192 /* row count, resulting size of array should be ~67 MB
			 well exceeding Intel's 20 MB LLC and producing 8M data points */
};

/* poll_completion */
/******************************************************************************
 * *	Function: poll_completion
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, 1 on failure
 * *
 * *	Description
 * *	Poll the completion queue for a single event. This function will continue to
 * *	poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
 * *
 * ******************************************************************************/

static int poll_completion(struct resources *res)
{
	struct ibv_wc	wc;
	unsigned long	start_time_msec;
	unsigned long	cur_time_msec;
	struct timeval	cur_time;
	int		poll_result;
	int		rc = 0;

	/* poll the completion for a while before giving up of doing it .. */
	gettimeofday(&cur_time, NULL);
	start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);

	do {
		poll_result = ibv_poll_cq(res->cq, 1, &wc);
		gettimeofday(&cur_time, NULL);
		cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	} while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));

	if (poll_result < 0) {
		/* poll CQ failed */
		fprintf(stderr, "poll CQ failed retval = %d, errno: %s\n", poll_result, strerror(errno));
		rc = 1;
	} else if (poll_result == 0) {
		/* the CQ is empty */
		fprintf(stderr, "completion wasn't found in the CQ after timeout. errno: %s\n", strerror(errno));
		rc = 1;
	} else {
		/* CQE found */
		debug_print("completion was found in CQ with status 0x%x\n", wc.status);

		/* check the completion status (here we don't care about the completion opcode */
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status, wc.vendor_err);
			rc = 1;
		}
	}

	return rc;
}



/******************************************************************************
 * *	Function: post_send
 * *
 * *	Input
 * *	res	 pointer to resources structure
 * *	opcode   IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, error code on failure
 * *
 * *	Description
 * *	This function will create and post a send work request
 * ******************************************************************************/

static int post_send(struct resources *res, int opcode)
{
	struct ibv_send_wr	sr;
	struct ibv_sge		sge;
	struct ibv_send_wr	*bad_wr = NULL;
	int			rc;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = CLIENT_MSG_SIZE;
	sge.lkey = res->mr->lkey;

	/* prepare the send work request */
	memset(&sr, 0, sizeof(sr));
	sr.next = NULL;
	sr.wr_id = 0;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.opcode = opcode;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(opcode != IBV_WR_SEND) {
		sr.wr.rdma.remote_addr = res->remote_props.addr;
		sr.wr.rdma.rkey = res->remote_props.rkey;
	}

	/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
	rc = ibv_post_send(res->qp, &sr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post SR\n");

	return rc;
}


/* Time the difference between an post_send and a poll_cq */
static int post_send_poll_complete(struct resources *res, int opcode, uint64_t* cycle_count)
{
	// From post_send
	struct ibv_send_wr	sr;
	struct ibv_sge		sge;
	struct ibv_send_wr	*bad_wr = NULL;
	int			rc;

	// From poll_complete
	struct ibv_wc	wc;
	int		poll_result;

	// Timing variables
	uint64_t start_cycle_count;
	uint64_t end_cycle_count;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = CLIENT_MSG_SIZE;
	sge.lkey = res->mr->lkey;

	/* prepare the send work request */
	memset(&sr, 0, sizeof(sr));
	sr.next = NULL;
	sr.wr_id = 0;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.opcode = opcode;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(opcode != IBV_WR_SEND) {
		sr.wr.rdma.remote_addr = res->remote_props.addr;
		sr.wr.rdma.rkey = res->remote_props.rkey;
	}

	/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
	start_cycle_count = start_tsc();

	rc = ibv_post_send(res->qp, &sr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post SR\n");
	do {
		poll_result = ibv_poll_cq(res->cq, 1, &wc);
	} while (poll_result == 0);

	end_cycle_count = stop_tsc();

	if (poll_result < 0) {
		/* poll CQ failed */
		fprintf(stderr, "poll CQ failed retval = %d, errno: %s\n", poll_result, strerror(errno));
		rc = 1;
	} else if (poll_result == 0) {
		/* the CQ is empty */
		fprintf(stderr, "completion wasn't found in the CQ after timeout. errno: %s\n", strerror(errno));
		rc = 1;
	} else {
		/* CQE found */
		*cycle_count = end_cycle_count - start_cycle_count;

		/* check the completion status (here we don't care about the completion opcode */
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status, wc.vendor_err);
			rc = 1;
		}
	}

	return rc;
}


/******************************************************************************
 * *	Function: print_config
 * *
 * *	Description
 * *	Print out config information
 *  ******************************************************************************/
static void print_config(void)
{
	debug_print(" ------------------------------------------------\n");
	debug_print(" Device name	: \"%s\"\n", config.dev_name);
	debug_print(" IB port	: %u\n", config.ib_port);
	if (config.server_name)
		debug_print("[client only] IP	: %s\n", config.server_name);
	debug_print(" TCP port	: %u\n", config.tcp_port);
	if (config.gid_idx >= 0)
		debug_print(" GID index	: %u\n", config.gid_idx);
	debug_print(" ------------------------------------------------\n\n");
}


/******************************************************************************
 * *	Function: usage
 * *
 * *	Input
 * *	argv0	command line arguments
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	none
 * *
 * *	Description
 * *	print a description of command line syntax
 * ******************************************************************************/

static void usage(const char *argv0)
{
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, " %s start a server and wait for connection\n", argv0);
	fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
	fprintf(stdout, "\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
	fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
	fprintf(stdout, " -i, --ib-port <port>   use port <port> of IB device (default 1)\n");
	fprintf(stdout, " -g, --gid_idx <gid index>   gid index to be used in GRH (default not used)\n");
	fprintf(stdout, " -n, --iterations <iterations>  "
			"Number of iterations to perform in the test "
			"(default 1000)\n");
	fprintf(stdout, " -m, --mode <mode>  set to 0 for seq or 1 for rand or 2 for clflush (default 0)\n");
	fprintf(stdout, " -s, --msg-size <bytes>  size of client buffer (default 8)\n");
	fprintf(stdout, " -c, --column-count <num>  number of columns (default 1024)\n");
	fprintf(stdout, " -r, --row-count <num>  number of rows (default 8192)\n");
}

static int read_write_read(struct resources *res, uint64_t target_addr, double cycles_to_usec) {
	uint64_t write_cyclces, orig_addr, read1_cycles, read2_cycles;
	int64_t delta;

	/* Store the original addr so we can change back to it after we're done. */
	orig_addr = res->remote_props.addr;
	res->remote_props.addr = target_addr;

	/* First read the contents of the server's buffer.
	 * This should be a cache miss. */
	if (post_send_poll_complete(res, IBV_WR_RDMA_READ, &read1_cycles)) {
		fprintf(stderr, "failed to post SR 2\n");
		return 1;
	}
	debug_print("[READ]  Contents of server's buffer: '%hhu', it took %lu cycles\n", res->buf[0], read1_cycles);

	/* Now we replace what's in the client's buffer to write to the server's buffer.
	 * This should pull this target_addr memory into cache. */
	res->buf[0] = res->buf[0] + 2;
	debug_print("[WRITE] Now replacing it with: '%hhu',", res->buf[0]);
	if (post_send_poll_complete(res, IBV_WR_RDMA_WRITE, &write_cyclces)) {
		fprintf(stderr, "failed to post SR 3\n");
		return 1;
	}
	debug_print("it took %lu cycles\n", write_cyclces);

	/* Then we read contents of server's buffer again.
	 * This should be a cache hit. */
	if (post_send_poll_complete(res, IBV_WR_RDMA_READ, &read2_cycles)) {
		fprintf(stderr, "failed to post SR 2\n");
		return 1;
	}
	delta = read1_cycles - read2_cycles;

	data_print("%lu,%lu,%f,%f\n", read1_cycles, read2_cycles, (read1_cycles * 1000) / cycles_to_usec, (read2_cycles * 1000) / cycles_to_usec);
	debug_print("[READ]  Contents of server's buffer: '%hhu', it took %lu cycles\n", res->buf[0], read2_cycles);
	debug_print("[DIFF]  %5ld cycles = %06.1f nsec\n", delta, delta / cycles_to_usec);

	/* Restore the original addr */
	res->remote_props.addr = orig_addr;

	return 0;
}

/******************************************************************************
 * *	Function: main
 *  *
 *  *	Input
 *  *	argc   number of items in argv
 *  *	argv   command line parameters
 *  *
 *  *	Output
 *  *	none
 *  *
 *  *	Returns
 *  *	0 on success, 1 on failure
 *  *
 *  *	Description
 *  *	Main program code
 *  ******************************************************************************/

int main(int argc, char *argv[])
{
	struct resources	res;
	int			rc = 1;
	char		temp_char;
	unsigned int		i, j;
	uint64_t start_addr, target_addr;

	/* parse the command line parameters */
	while (1) {
		int c;

		static struct option long_options[] = {
			{.name = "port",	.has_arg = 1,  .val = 'p' },
			{.name = "ib-dev",	.has_arg = 1,  .val = 'd' },
			{.name = "ib-port",     .has_arg = 1,  .val = 'i' },
			{.name = "gid-idx",     .has_arg = 1,  .val = 'g' },
			{.name = "iterations",  .has_arg = 1,  .val = 'n' },
			{.name = "mode",		.has_arg = 1,  .val = 'm'},
			{.name = "msg-size",	.has_arg = 1,  .val = 's'},
			{.name = "column-count",	.has_arg = 1,	.val = 'c'},
			{.name = "row-count",		.has_arg = 1,	.val = 'r'},
			{.name = NULL,		.has_arg = 0,  .val = '\0'}
		};

		c = getopt_long(argc, argv, "p:d:i:g:n:m:s:c:r:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
			case 'p':
				config.tcp_port = strtoul(optarg, NULL, 0);
				break;

			case 'd':
				config.dev_name = strdup(optarg);
				break;

			case 'i':
				config.ib_port = strtoul(optarg, NULL, 0);
				if (config.ib_port < 0) {
					usage(argv[0]);
					return 1;
				}
				break;

			case 'g':
				config.gid_idx = strtoul(optarg, NULL, 0);
				if (config.gid_idx < 0) {
					usage(argv[0]);
					return 1;
				}
				break;

			case 'n':
				config.iters = strtoul(optarg, NULL, 0);
				break;

			case 'm':
				config.mode = strtoul(optarg, NULL, 0);
				break;

			case 's':
				config.msg_size = strtoul(optarg, NULL, 0);
				break;

			case 'c':
				config.column_count = strtoul(optarg, NULL, 0);
				break;

			case 'r':
				config.row_count = strtoul(optarg, NULL, 0);
				break;

			default:
				usage(argv[0]);
				return 1;
		}
	}

	/* parse the last parameter (if exists) as the server name */
	if (optind == argc - 1)
		config.server_name = argv[optind];
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	/* set cpu affinity for client */
	if (config.server_name) {
		cpu_set_t s;
		CPU_ZERO(&s);
		CPU_SET(sched_getcpu(), &s);
		sched_setaffinity(0, sizeof(cpu_set_t), &s);
	}

	/* print the used parameters for info */
	print_config();

	/* init all of the resources, so cleanup will be easy */
	resources_init(&res);

	/* create resources before using them */
	if (resources_create(&res)) {
		fprintf(stderr, "failed to create resources\n");
		goto main_exit;
	}

	/* connect the QPs */
	if (connect_qp(&res)) {
		fprintf(stderr, "failed to connect QPs\n");
		goto main_exit;
	}

	/* let the server post the sr */
	if (!config.server_name)
		if (post_send(&res, IBV_WR_SEND)) {
			fprintf(stderr, "failed to post sr\n");
			goto main_exit;
		}

	/* in both sides we expect to get a completion */
	if (poll_completion(&res)) {
		fprintf(stderr, "poll completion failed\n");
		goto main_exit;
	}

	/* after polling the completion we have the message in the client buffer too */
	if (config.server_name)
		debug_print("[Client only] Message is: '%hhu'\n", res.buf[0]);

	/* Sync so we are sure server side has data ready before client tries to read it */
	if (sock_sync_data(res.sock, 1, "R", &temp_char)) {  /* just send a dummy char back and forth */
		fprintf(stderr, "sync error before RDMA ops\n");
		rc = 1;
		goto main_exit;
	}

	if (config.server_name)
		debug_print("Beginning tests...\n----------------------------\n\n");

	double cycles_to_usec = get_cpu_mhz(false);

	/*  Now the client performs an RDMA read and then write on server.
	 *  Note that the server has no idea these events have occured */
	if (config.server_name) {
		start_addr = res.remote_props.addr;

		switch (config.mode) {
			case 0: /* seq */
				for (i = 0; i < config.column_count; ++i) {
					for (j = 0; j < config.row_count; ++j) {
						/* index into the row we want */
						target_addr = start_addr + j * (config.column_count * config.msg_size);
						/* index into the column we want */
						target_addr += i * config.msg_size;

						if (read_write_read(&res, target_addr, cycles_to_usec)) {
							rc = 1;
							goto main_exit;
						}
					}
				}
				break;

			case 1: /* rand */
				for (i = 0; i < config.iters; ++i) {
					if (read_write_read(&res, start_addr + rand_line(), cycles_to_usec)) {
						rc = 1;
						goto main_exit;
					}

					if (i == CACHE_LINES) {
						memset(bm, 0, sizeof(bm));
					}
				}
				break;

			case 2: /* single byte */
				for (i = 0; i < config.iters; ++i) {
					if (read_write_read(&res, start_addr, cycles_to_usec)) {
						rc = 1;
						goto main_exit;
					}

					if (sock_sync_data(res.sock, 1, "A", &temp_char)) {  /* just send a dummy char back and forth */
						fprintf(stderr, "sync error after RDMA ops\n");
						rc = 1;
						goto main_exit;
					}

					if (sock_sync_data(res.sock, 1, "B", &temp_char)) {  /* just send a dummy char back and forth */
						fprintf(stderr, "sync error after RDMA ops\n");
						rc = 1;
						goto main_exit;
					}
				}
				break;
		}
	}
	else if (config.mode == 2) {
		for (i = 0; i < config.iters; ++i) {
			if (sock_sync_data(res.sock, 1, "A", &temp_char)) {  /* just send a dummy char back and forth */
				fprintf(stderr, "sync error after RDMA ops\n");
				rc = 1;
				goto main_exit;
			}

			_mm_clflush(res.buf);
			_mm_lfence();

			if (sock_sync_data(res.sock, 1, "B", &temp_char)) {  /* just send a dummy char back and forth */
				fprintf(stderr, "sync error after RDMA ops\n");
				rc = 1;
				goto main_exit;
			}
		}
	}

	/* Sync so server will know that client is done mucking with its memory */
	if (sock_sync_data(res.sock, 1, "W", &temp_char)) {  /* just send a dummy char back and forth */
		fprintf(stderr, "sync error after RDMA ops\n");
		rc = 1;
		goto main_exit;
	}

	rc = 0;

main_exit:
	if (resources_destroy(&res)) {
		fprintf(stderr, "failed to destroy resources\n");
		rc = 1;
	}

	if(config.dev_name)
		free((char *) config.dev_name);

	debug_print("\ntest result is %d\n", rc);

	return rc;
}
