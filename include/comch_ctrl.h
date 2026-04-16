#ifndef COMCH_CTRL_H
#define COMCH_CTRL_H

#include <stdint.h>

#include <doca_comch.h>
#include <doca_dev.h>
#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comch_ctrl;

doca_error_t comch_ctrl_server_create(const char *service_name,
				      const char *dev_pci_addr,
				      const char *rep_pci_addr,
				      struct comch_ctrl **out_ctrl);

doca_error_t comch_ctrl_client_create(const char *service_name,
				      const char *dev_pci_addr,
				      struct comch_ctrl **out_ctrl);

void comch_ctrl_destroy(struct comch_ctrl *ctrl);

struct doca_comch_connection *comch_ctrl_get_connection(struct comch_ctrl *ctrl);

doca_error_t comch_ctrl_progress(struct comch_ctrl *ctrl);

doca_error_t comch_ctrl_send(struct comch_ctrl *ctrl,
			     struct doca_comch_connection *connection,
			     const void *msg,
			     uint32_t len);

uint32_t comch_ctrl_get_max_msg_size(const struct comch_ctrl *ctrl);

doca_error_t comch_ctrl_wait_for_connection(struct comch_ctrl *ctrl);

doca_error_t comch_ctrl_wait_for_message(struct comch_ctrl *ctrl,
					 void *buffer,
					 uint32_t buffer_len,
					 uint32_t *msg_len);

#ifdef __cplusplus
}
#endif

#endif
