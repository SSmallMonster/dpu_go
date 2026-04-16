#ifndef CTRL_CHANNEL_H
#define CTRL_CHANNEL_H

#include <stdint.h>

#include <doca_error.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ctrl_channel;

/* ---- Server-side constructors (DPU) ---- */

/*
 * Create a COMCH-backed control channel server (DPU side).
 */
doca_error_t ctrl_channel_comch_server_create(const char *service_name,
					       const char *dev_pci_addr,
					       const char *rep_pci_addr,
					       struct ctrl_channel **out);

/*
 * Create a TCP-backed control channel server.
 * Listens on 0.0.0.0:port and accepts one client connection.
 */
doca_error_t ctrl_channel_tcp_server_create(uint16_t port,
					     struct ctrl_channel **out);

/* ---- Client-side constructors (Host) ---- */

/*
 * Create a COMCH-backed control channel client (Host side).
 */
doca_error_t ctrl_channel_comch_client_create(const char *service_name,
					       const char *dev_pci_addr,
					       struct ctrl_channel **out);

/*
 * Create a TCP-backed control channel client.
 * Connects immediately to dpu_host:port on creation.
 */
doca_error_t ctrl_channel_tcp_client_create(const char *dpu_host,
					     uint16_t port,
					     struct ctrl_channel **out);

void ctrl_channel_destroy(struct ctrl_channel *ch);

/* Block until a client connects. */
doca_error_t ctrl_channel_wait_for_connection(struct ctrl_channel *ch);

/* Send a message to the connected client. */
doca_error_t ctrl_channel_send(struct ctrl_channel *ch, const void *msg, uint32_t len);

/* Block until a message is received.  *msg_len is set to the actual length. */
doca_error_t ctrl_channel_wait_for_message(struct ctrl_channel *ch,
					    void *buf,
					    uint32_t buf_len,
					    uint32_t *msg_len);

uint32_t ctrl_channel_get_max_msg_size(const struct ctrl_channel *ch);

/* Drive the underlying event engine (no-op for TCP). */
doca_error_t ctrl_channel_progress(struct ctrl_channel *ch);

#ifdef __cplusplus
}
#endif

#endif /* CTRL_CHANNEL_H */
