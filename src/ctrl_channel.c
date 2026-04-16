#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doca_error.h>

#include "ctrl_channel.h"
#include "comch_ctrl.h"

/* ------------------------------------------------------------------ */
/* Internal vtable                                                      */
/* ------------------------------------------------------------------ */

struct ctrl_channel_ops {
	void (*destroy)(struct ctrl_channel *);
	doca_error_t (*wait_for_connection)(struct ctrl_channel *);
	doca_error_t (*send)(struct ctrl_channel *, const void *, uint32_t);
	doca_error_t (*wait_for_message)(struct ctrl_channel *, void *, uint32_t, uint32_t *);
	uint32_t (*get_max_msg_size)(const struct ctrl_channel *);
	doca_error_t (*progress)(struct ctrl_channel *);
};

struct ctrl_channel {
	const struct ctrl_channel_ops *ops;
};

/* ------------------------------------------------------------------ */
/* COMCH backend                                                        */
/* ------------------------------------------------------------------ */

struct comch_channel {
	struct ctrl_channel base; /* must be first */
	struct comch_ctrl *ctrl;
	struct doca_comch_connection *connection;
};

static void comch_destroy(struct ctrl_channel *ch)
{
	struct comch_channel *cc = (struct comch_channel *)ch;

	comch_ctrl_destroy(cc->ctrl);
	free(cc);
}

static doca_error_t comch_wait_for_connection(struct ctrl_channel *ch)
{
	struct comch_channel *cc = (struct comch_channel *)ch;
	doca_error_t result;

	result = comch_ctrl_wait_for_connection(cc->ctrl);
	if (result == DOCA_SUCCESS)
		cc->connection = comch_ctrl_get_connection(cc->ctrl);
	return result;
}

static doca_error_t comch_send(struct ctrl_channel *ch, const void *msg, uint32_t len)
{
	struct comch_channel *cc = (struct comch_channel *)ch;

	return comch_ctrl_send(cc->ctrl, cc->connection, msg, len);
}

static doca_error_t comch_wait_for_message(struct ctrl_channel *ch,
					    void *buf, uint32_t buf_len, uint32_t *msg_len)
{
	struct comch_channel *cc = (struct comch_channel *)ch;

	return comch_ctrl_wait_for_message(cc->ctrl, buf, buf_len, msg_len);
}

static uint32_t comch_get_max_msg_size(const struct ctrl_channel *ch)
{
	const struct comch_channel *cc = (const struct comch_channel *)ch;

	return comch_ctrl_get_max_msg_size(cc->ctrl);
}

static doca_error_t comch_progress(struct ctrl_channel *ch)
{
	struct comch_channel *cc = (struct comch_channel *)ch;

	return comch_ctrl_progress(cc->ctrl);
}

static const struct ctrl_channel_ops comch_ops = {
	.destroy             = comch_destroy,
	.wait_for_connection = comch_wait_for_connection,
	.send                = comch_send,
	.wait_for_message    = comch_wait_for_message,
	.get_max_msg_size    = comch_get_max_msg_size,
	.progress            = comch_progress,
};

static doca_error_t comch_channel_create(struct comch_ctrl *ctrl, struct ctrl_channel **out)
{
	struct comch_channel *cc = calloc(1, sizeof(*cc));

	if (cc == NULL) {
		comch_ctrl_destroy(ctrl);
		return DOCA_ERROR_NO_MEMORY;
	}
	cc->base.ops = &comch_ops;
	cc->ctrl = ctrl;
	*out = &cc->base;
	return DOCA_SUCCESS;
}

doca_error_t ctrl_channel_comch_server_create(const char *service_name,
					       const char *dev_pci_addr,
					       const char *rep_pci_addr,
					       struct ctrl_channel **out)
{
	struct comch_ctrl *ctrl = NULL;
	doca_error_t result;

	result = comch_ctrl_server_create(service_name, dev_pci_addr, rep_pci_addr, &ctrl);
	if (result != DOCA_SUCCESS)
		return result;
	return comch_channel_create(ctrl, out);
}

doca_error_t ctrl_channel_comch_client_create(const char *service_name,
					       const char *dev_pci_addr,
					       struct ctrl_channel **out)
{
	struct comch_ctrl *ctrl = NULL;
	doca_error_t result;

	result = comch_ctrl_client_create(service_name, dev_pci_addr, &ctrl);
	if (result != DOCA_SUCCESS)
		return result;
	return comch_channel_create(ctrl, out);
}

/* ------------------------------------------------------------------ */
/* TCP backend                                                          */
/* ------------------------------------------------------------------ */

/*
 * Wire framing: each message is preceded by a 4-byte native-endian length.
 *
 *   [ uint32_t len ][ uint8_t data[len] ]
 *
 * Both server and client must be compiled from the same codebase, so
 * native byte order is fine.
 */

struct tcp_channel {
	struct ctrl_channel base; /* must be first */
	int listen_fd;            /* server only; -1 for client */
	int client_fd;            /* connected fd */
	uint16_t port;
	int is_client;
};

static doca_error_t tcp_send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n <= 0)
			return DOCA_ERROR_IO_FAILED;
		p   += n;
		len -= (size_t)n;
	}
	return DOCA_SUCCESS;
}

static doca_error_t tcp_recv_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = recv(fd, p, len, MSG_WAITALL);

		if (n <= 0)
			return DOCA_ERROR_IO_FAILED;
		p   += n;
		len -= (size_t)n;
	}
	return DOCA_SUCCESS;
}

static void tcp_destroy(struct ctrl_channel *ch)
{
	struct tcp_channel *tc = (struct tcp_channel *)ch;

	if (tc->client_fd >= 0)
		close(tc->client_fd);
	if (tc->listen_fd >= 0)
		close(tc->listen_fd);
	free(tc);
}

static doca_error_t tcp_wait_for_connection(struct ctrl_channel *ch)
{
	struct tcp_channel *tc = (struct tcp_channel *)ch;
	int fd;
	int one = 1;

	if (tc->is_client)
		return DOCA_SUCCESS; /* already connected in create */

	fd = accept(tc->listen_fd, NULL, NULL);
	if (fd < 0) {
		perror("[TCP] accept");
		return DOCA_ERROR_IO_FAILED;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	tc->client_fd = fd;
	return DOCA_SUCCESS;
}

static doca_error_t tcp_send(struct ctrl_channel *ch, const void *msg, uint32_t len)
{
	struct tcp_channel *tc = (struct tcp_channel *)ch;
	doca_error_t result;

	result = tcp_send_all(tc->client_fd, &len, sizeof(len));
	if (result != DOCA_SUCCESS)
		return result;
	return tcp_send_all(tc->client_fd, msg, len);
}

static doca_error_t tcp_wait_for_message(struct ctrl_channel *ch,
					  void *buf, uint32_t buf_len, uint32_t *msg_len)
{
	struct tcp_channel *tc = (struct tcp_channel *)ch;
	doca_error_t result;
	uint32_t wire_len;

	result = tcp_recv_all(tc->client_fd, &wire_len, sizeof(wire_len));
	if (result != DOCA_SUCCESS)
		return result;

	if (wire_len > buf_len) {
		fprintf(stderr, "[TCP] incoming message %u bytes > buffer %u bytes\n",
			wire_len, buf_len);
		return DOCA_ERROR_NO_MEMORY;
	}

	result = tcp_recv_all(tc->client_fd, buf, wire_len);
	if (result == DOCA_SUCCESS)
		*msg_len = wire_len;
	return result;
}

static uint32_t tcp_get_max_msg_size(const struct ctrl_channel *ch)
{
	(void)ch;
	return (uint32_t)64 * 1024;
}

static doca_error_t tcp_progress(struct ctrl_channel *ch)
{
	(void)ch;
	return DOCA_SUCCESS;
}

static const struct ctrl_channel_ops tcp_ops = {
	.destroy             = tcp_destroy,
	.wait_for_connection = tcp_wait_for_connection,
	.send                = tcp_send,
	.wait_for_message    = tcp_wait_for_message,
	.get_max_msg_size    = tcp_get_max_msg_size,
	.progress            = tcp_progress,
};

doca_error_t ctrl_channel_tcp_client_create(const char *dpu_host, uint16_t port,
					     struct ctrl_channel **out)
{
	struct tcp_channel *tc = calloc(1, sizeof(*tc));
	struct sockaddr_in addr;
	int fd;
	int one = 1;

	if (tc == NULL)
		return DOCA_ERROR_NO_MEMORY;

	tc->base.ops = &tcp_ops;
	tc->listen_fd = -1;
	tc->client_fd = -1;
	tc->port = port;
	tc->is_client = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("[TCP] socket");
		free(tc);
		return DOCA_ERROR_IO_FAILED;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
	if (inet_pton(AF_INET, dpu_host, &addr.sin_addr) != 1) {
		fprintf(stderr, "[TCP] invalid DPU address: %s\n", dpu_host);
		close(fd);
		free(tc);
		return DOCA_ERROR_INVALID_VALUE;
	}

	fprintf(stderr, "[TCP] Connecting to %s:%u ...\n", dpu_host, port);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("[TCP] connect");
		close(fd);
		free(tc);
		return DOCA_ERROR_CONNECTION_ABORTED;
	}
	fprintf(stderr, "[TCP] Connected\n");

	tc->client_fd = fd;
	*out = &tc->base;
	return DOCA_SUCCESS;
}

doca_error_t ctrl_channel_tcp_server_create(uint16_t port, struct ctrl_channel **out)
{
	struct tcp_channel *tc = calloc(1, sizeof(*tc));
	struct sockaddr_in addr;
	int fd;
	int opt = 1;

	if (tc == NULL)
		return DOCA_ERROR_NO_MEMORY;

	tc->base.ops = &tcp_ops;
	tc->listen_fd = -1;
	tc->client_fd = -1;
	tc->port = port;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("[TCP] socket");
		free(tc);
		return DOCA_ERROR_IO_FAILED;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(port);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("[TCP] bind");
		close(fd);
		free(tc);
		return DOCA_ERROR_IO_FAILED;
	}

	if (listen(fd, 1) < 0) {
		perror("[TCP] listen");
		close(fd);
		free(tc);
		return DOCA_ERROR_IO_FAILED;
	}

	tc->listen_fd = fd;
	*out = &tc->base;
	fprintf(stderr, "[TCP] Listening on 0.0.0.0:%u\n", port);
	return DOCA_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

void ctrl_channel_destroy(struct ctrl_channel *ch)
{
	if (ch != NULL)
		ch->ops->destroy(ch);
}

doca_error_t ctrl_channel_wait_for_connection(struct ctrl_channel *ch)
{
	return ch->ops->wait_for_connection(ch);
}

doca_error_t ctrl_channel_send(struct ctrl_channel *ch, const void *msg, uint32_t len)
{
	return ch->ops->send(ch, msg, len);
}

doca_error_t ctrl_channel_wait_for_message(struct ctrl_channel *ch,
					    void *buf, uint32_t buf_len, uint32_t *msg_len)
{
	return ch->ops->wait_for_message(ch, buf, buf_len, msg_len);
}

uint32_t ctrl_channel_get_max_msg_size(const struct ctrl_channel *ch)
{
	return ch->ops->get_max_msg_size(ch);
}

doca_error_t ctrl_channel_progress(struct ctrl_channel *ch)
{
	return ch->ops->progress(ch);
}
