#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_pe.h>

#include "comch_ctrl.h"
#include "doca_device_utils.h"

#define COMCH_NUM_TASKS 128
#define SLEEP_NANOS 10000

struct comch_ctrl {
	struct doca_pe *pe;
	struct doca_ctx *ctx;
	struct doca_dev *dev;
	struct doca_dev_rep *dev_rep;
	struct doca_comch_server *server;
	struct doca_comch_client *client;
	struct doca_comch_connection *connection;
	uint32_t max_msg_size;
	uint8_t is_server;
	volatile uint8_t connection_ready;
	volatile uint8_t has_message;
	uint32_t last_msg_len;
	uint8_t *recv_buffer;
};

static const char *skip_pci_domain(const char *pci_addr)
{
	if (pci_addr == NULL)
		return NULL;
	if (strncmp(pci_addr, "0000:", 5) == 0)
		return pci_addr + 5;
	return pci_addr;
}

static int pci_addr_matches(const char *lhs, const char *rhs)
{
	if (lhs == NULL || rhs == NULL)
		return 0;
	if (strcmp(lhs, rhs) == 0)
		return 1;
	return strcmp(skip_pci_domain(lhs), skip_pci_domain(rhs)) == 0;
}

static doca_error_t open_dev_rep_auto(struct doca_dev *local, struct doca_dev_rep **out_rep)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_reps = 0;
	doca_error_t result;

	*out_rep = NULL;
	result = doca_devinfo_rep_create_list(local, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_reps; ++i) {
		result = doca_dev_rep_open(rep_list[i], out_rep);
		if (result == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_list);
			return DOCA_SUCCESS;
		}
	}

	doca_devinfo_rep_destroy_list(rep_list);
	return DOCA_ERROR_NOT_FOUND;
}

static doca_error_t open_dev_rep_by_pci(struct doca_dev *local,
					const char *rep_pci_addr,
					struct doca_dev_rep **out_rep)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_reps = 0;
	doca_error_t result;

	*out_rep = NULL;
	result = doca_devinfo_rep_create_list(local, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_reps; ++i) {
		char rep_pci[DOCA_DEVINFO_REP_PCI_ADDR_SIZE] = {0};

		result = doca_devinfo_rep_get_pci_addr_str(rep_list[i], rep_pci);
		if (result == DOCA_SUCCESS && pci_addr_matches(rep_pci, rep_pci_addr)) {
			result = doca_dev_rep_open(rep_list[i], out_rep);
			doca_devinfo_rep_destroy_list(rep_list);
			return result;
		}
	}

	doca_devinfo_rep_destroy_list(rep_list);
	return DOCA_ERROR_NOT_FOUND;
}

static void send_completed_cb(struct doca_comch_task_send *task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;
	doca_task_free(doca_comch_task_send_as_task(task));
}

static void send_error_cb(struct doca_comch_task_send *task,
			  union doca_data task_user_data,
			  union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;
	doca_task_free(doca_comch_task_send_as_task(task));
}

static void server_connection_cb(struct doca_comch_event_connection_status_changed *event,
				 struct doca_comch_connection *connection,
				 uint8_t change_successful)
{
	union doca_data ctx_user_data = {0};
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(connection);
	struct comch_ctrl *ctrl = NULL;

	(void)event;

	if (change_successful == 0)
		return;

	if (doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data) != DOCA_SUCCESS)
		return;
	ctrl = ctx_user_data.ptr;
	if (ctrl == NULL)
		return;

	ctrl->connection = connection;
	(void)doca_comch_connection_set_user_data(connection, ctx_user_data);
	ctrl->connection_ready = 1;
}

static void server_disconnection_cb(struct doca_comch_event_connection_status_changed *event,
				    struct doca_comch_connection *connection,
				    uint8_t change_successful)
{
	union doca_data ctx_user_data = {0};
	struct doca_comch_server *server = doca_comch_server_get_server_ctx(connection);
	struct comch_ctrl *ctrl = NULL;

	(void)event;
	(void)change_successful;

	if (doca_ctx_get_user_data(doca_comch_server_as_ctx(server), &ctx_user_data) != DOCA_SUCCESS)
		return;
	ctrl = ctx_user_data.ptr;
	if (ctrl == NULL)
		return;

	ctrl->connection = NULL;
	ctrl->connection_ready = 0;
}

static void server_recv_cb(struct doca_comch_event_msg_recv *event,
			   uint8_t *recv_buffer,
			   uint32_t msg_len,
			   struct doca_comch_connection *connection)
{
	union doca_data connection_user_data;
	struct comch_ctrl *ctrl;

	(void)event;

	connection_user_data = doca_comch_connection_get_user_data(connection);
	ctrl = connection_user_data.ptr;
	if (ctrl == NULL || ctrl->recv_buffer == NULL || msg_len > ctrl->max_msg_size)
		return;

	memcpy(ctrl->recv_buffer, recv_buffer, msg_len);
	ctrl->last_msg_len = msg_len;
	ctrl->has_message = 1;
}

static void client_recv_cb(struct doca_comch_event_msg_recv *event,
			   uint8_t *recv_buffer,
			   uint32_t msg_len,
			   struct doca_comch_connection *connection)
{
	server_recv_cb(event, recv_buffer, msg_len, connection);
}

static doca_error_t common_init(struct comch_ctrl *ctrl)
{
	doca_error_t result;
	union doca_data ctx_data = {0};
	struct timespec ts = {.tv_sec = 0, .tv_nsec = SLEEP_NANOS};

	result = doca_pe_create(&ctrl->pe);
	if (result != DOCA_SUCCESS)
		return result;

	ctx_data.ptr = ctrl;
	result = doca_ctx_set_user_data(ctrl->ctx, ctx_data);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_pe_connect_ctx(ctrl->pe, ctrl->ctx);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_ctx_start(ctrl->ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS)
		return result;

	if (result == DOCA_ERROR_IN_PROGRESS) {
		for (int i = 0; i < 64; ++i) {
			(void)doca_pe_progress(ctrl->pe);
			nanosleep(&ts, &ts);
		}
	}

	ctrl->recv_buffer = calloc(1, ctrl->max_msg_size);
	if (ctrl->recv_buffer == NULL)
		return DOCA_ERROR_NO_MEMORY;

	return DOCA_SUCCESS;
}

doca_error_t comch_ctrl_server_create(const char *service_name,
				      const char *dev_pci_addr,
				      const char *rep_pci_addr,
				      struct comch_ctrl **out_ctrl)
{
	struct comch_ctrl *ctrl = calloc(1, sizeof(*ctrl));
	doca_error_t result;

	if (ctrl == NULL)
		return DOCA_ERROR_NO_MEMORY;

	ctrl->is_server = 1;
	result = open_comch_device_by_pci(dev_pci_addr, true, &ctrl->dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] Failed to open local device %s for server: %s\n",
			dev_pci_addr,
			doca_error_get_descr(result));
		goto fail;
	}

	if (rep_pci_addr != NULL && rep_pci_addr[0] != '\0')
		result = open_dev_rep_by_pci(ctrl->dev, rep_pci_addr, &ctrl->dev_rep);
	else
		result = open_dev_rep_auto(ctrl->dev, &ctrl->dev_rep);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] Failed to open representor for server: %s\n",
			doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_cap_get_max_msg_size(doca_dev_as_devinfo(ctrl->dev), &ctrl->max_msg_size);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] Failed to query max msg size: %s\n", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_server_create(ctrl->dev, ctrl->dev_rep, service_name, &ctrl->server);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] doca_comch_server_create failed: %s\n", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_server_set_max_msg_size(ctrl->server, ctrl->max_msg_size);
	if (result != DOCA_SUCCESS)
		goto fail;

	ctrl->ctx = doca_comch_server_as_ctx(ctrl->server);
	result = doca_comch_server_task_send_set_conf(ctrl->server, send_completed_cb, send_error_cb, COMCH_NUM_TASKS);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_comch_server_event_msg_recv_register(ctrl->server, server_recv_cb);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_comch_server_event_connection_status_changed_register(ctrl->server,
									    server_connection_cb,
									    server_disconnection_cb);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = common_init(ctrl);
	if (result != DOCA_SUCCESS)
		goto fail;

	*out_ctrl = ctrl;
	return DOCA_SUCCESS;

fail:
	comch_ctrl_destroy(ctrl);
	return result;
}

doca_error_t comch_ctrl_client_create(const char *service_name,
				      const char *dev_pci_addr,
				      struct comch_ctrl **out_ctrl)
{
	struct comch_ctrl *ctrl = calloc(1, sizeof(*ctrl));
	union doca_data conn_data = {0};
	doca_error_t result;

	if (ctrl == NULL)
		return DOCA_ERROR_NO_MEMORY;

	result = open_comch_device_by_pci(dev_pci_addr, false, &ctrl->dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] Failed to open local device %s for client: %s\n",
			dev_pci_addr,
			doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_cap_get_max_msg_size(doca_dev_as_devinfo(ctrl->dev), &ctrl->max_msg_size);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] Failed to query client max msg size: %s\n", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_client_create(ctrl->dev, service_name, &ctrl->client);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "[COMCH] doca_comch_client_create failed: %s\n", doca_error_get_descr(result));
		goto fail;
	}

	result = doca_comch_client_set_max_msg_size(ctrl->client, ctrl->max_msg_size);
	if (result != DOCA_SUCCESS)
		goto fail;

	ctrl->ctx = doca_comch_client_as_ctx(ctrl->client);
	result = doca_comch_client_task_send_set_conf(ctrl->client, send_completed_cb, send_error_cb, COMCH_NUM_TASKS);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_comch_client_event_msg_recv_register(ctrl->client, client_recv_cb);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = common_init(ctrl);
	if (result != DOCA_SUCCESS)
		goto fail;

	for (;;) {
		result = doca_comch_client_get_connection(ctrl->client, &ctrl->connection);
		if (result == DOCA_SUCCESS && ctrl->connection != NULL)
			break;
		if (result != DOCA_ERROR_NOT_CONNECTED &&
		    result != DOCA_ERROR_AGAIN &&
		    result != DOCA_ERROR_IN_PROGRESS)
			goto fail;
		(void)comch_ctrl_progress(ctrl);
	}

	conn_data.ptr = ctrl;
	(void)doca_comch_connection_set_user_data(ctrl->connection, conn_data);
	ctrl->connection_ready = 1;
	*out_ctrl = ctrl;
	return DOCA_SUCCESS;

fail:
	comch_ctrl_destroy(ctrl);
	return result;
}

void comch_ctrl_destroy(struct comch_ctrl *ctrl)
{
	if (ctrl == NULL)
		return;

	free(ctrl->recv_buffer);
	if (ctrl->ctx != NULL)
		(void)doca_ctx_stop(ctrl->ctx);
	if (ctrl->server != NULL)
		(void)doca_comch_server_destroy(ctrl->server);
	if (ctrl->client != NULL)
		(void)doca_comch_client_destroy(ctrl->client);
	if (ctrl->dev_rep != NULL)
		(void)doca_dev_rep_close(ctrl->dev_rep);
	if (ctrl->pe != NULL)
		(void)doca_pe_destroy(ctrl->pe);
	if (ctrl->dev != NULL)
		(void)doca_dev_close(ctrl->dev);
	free(ctrl);
}

struct doca_comch_connection *comch_ctrl_get_connection(struct comch_ctrl *ctrl)
{
	return ctrl == NULL ? NULL : ctrl->connection;
}

doca_error_t comch_ctrl_progress(struct comch_ctrl *ctrl)
{
	if (ctrl == NULL || ctrl->pe == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	(void)doca_pe_progress(ctrl->pe);
	return DOCA_SUCCESS;
}

doca_error_t comch_ctrl_send(struct comch_ctrl *ctrl,
			     struct doca_comch_connection *connection,
			     const void *msg,
			     uint32_t len)
{
	struct doca_comch_task_send *task = NULL;
	doca_error_t result;
	struct timespec ts = {.tv_sec = 0, .tv_nsec = SLEEP_NANOS};

	if (ctrl == NULL || connection == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (len > ctrl->max_msg_size)
		return DOCA_ERROR_INVALID_VALUE;

	if (ctrl->is_server)
		result = doca_comch_server_task_send_alloc_init(ctrl->server, connection, msg, len, &task);
	else
		result = doca_comch_client_task_send_alloc_init(ctrl->client, connection, msg, len, &task);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_task_submit(doca_comch_task_send_as_task(task));
	if (result != DOCA_SUCCESS) {
		doca_task_free(doca_comch_task_send_as_task(task));
		return result;
	}

	for (int i = 0; i < 8; ++i) {
		(void)doca_pe_progress(ctrl->pe);
		nanosleep(&ts, &ts);
	}

	return DOCA_SUCCESS;
}

uint32_t comch_ctrl_get_max_msg_size(const struct comch_ctrl *ctrl)
{
	return ctrl == NULL ? 0 : ctrl->max_msg_size;
}

doca_error_t comch_ctrl_wait_for_connection(struct comch_ctrl *ctrl)
{
	struct timespec ts = {.tv_sec = 0, .tv_nsec = SLEEP_NANOS};

	while (ctrl->connection_ready == 0) {
		(void)comch_ctrl_progress(ctrl);
		nanosleep(&ts, &ts);
	}

	return DOCA_SUCCESS;
}

doca_error_t comch_ctrl_wait_for_message(struct comch_ctrl *ctrl,
					 void *buffer,
					 uint32_t buffer_len,
					 uint32_t *msg_len)
{
	struct timespec ts = {.tv_sec = 0, .tv_nsec = SLEEP_NANOS};

	if (ctrl == NULL || buffer == NULL || msg_len == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	while (ctrl->has_message == 0) {
		(void)comch_ctrl_progress(ctrl);
		nanosleep(&ts, &ts);
	}

	if (ctrl->last_msg_len > buffer_len)
		return DOCA_ERROR_NO_MEMORY;

	memcpy(buffer, ctrl->recv_buffer, ctrl->last_msg_len);
	*msg_len = ctrl->last_msg_len;
	ctrl->has_message = 0;
	return DOCA_SUCCESS;
}
