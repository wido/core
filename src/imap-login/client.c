/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "hash.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "process-title.h"
#include "safe-memset.h"
#include "str.h"
#include "strescape.h"
#include "imap-parser.h"
#include "client.h"
#include "client-authenticate.h"
#include "auth-client.h"
#include "ssl-proxy.h"
#include "imap-proxy.h"

/* max. size of one parameter in line, or max reply length in SASL
   authentication */
#define MAX_INBUF_SIZE 4096

/* max. size of output buffer. if it gets full, the client is disconnected.
   SASL authentication gives the largest output. */
#define MAX_OUTBUF_SIZE 4096

/* maximum length for IMAP command line. */
#define MAX_IMAP_LINE 8192

/* Disconnect client after idling this many seconds */
#define CLIENT_LOGIN_IDLE_TIMEOUT 60

/* Disconnect client when it sends too many bad commands */
#define CLIENT_MAX_BAD_COMMANDS 10

/* When max. number of simultaneous connections is reached, few of the
   oldest connections are disconnected. Since we have to go through the whole
   client hash, it's faster if we disconnect multiple clients. */
#define CLIENT_DESTROY_OLDEST_COUNT 16

#if CLIENT_LOGIN_IDLE_TIMEOUT >= AUTH_REQUEST_TIMEOUT
#  error client idle timeout must be smaller than authentication timeout
#endif

static struct hash_table *clients;
static struct timeout *to_idle;

static void client_set_title(struct imap_client *client)
{
	const char *addr;

	if (!verbose_proctitle || !process_per_connection)
		return;

	addr = net_ip2addr(&client->common.ip);
	if (addr == NULL)
		addr = "??";

	process_title_set(t_strdup_printf(client->common.tls ?
					  "[%s TLS]" : "[%s]", addr));
}

static void client_open_streams(struct imap_client *client, int fd)
{
	client->input = i_stream_create_file(fd, default_pool,
					     MAX_INBUF_SIZE, FALSE);
	client->output = o_stream_create_file(fd, default_pool, MAX_OUTBUF_SIZE,
					      FALSE);
	client->parser = imap_parser_create(client->input, client->output,
					    MAX_IMAP_LINE);
}

/* Skip incoming data until newline is found,
   returns TRUE if newline was found. */
static int client_skip_line(struct imap_client *client)
{
	const unsigned char *data;
	size_t i, data_size;

	data = i_stream_get_data(client->input, &data_size);

	for (i = 0; i < data_size; i++) {
		if (data[i] == '\n') {
			i_stream_skip(client->input, i+1);
			return TRUE;
		}
	}

	return FALSE;
}

static const char *get_capability(struct imap_client *client)
{
	const char *auths;

	auths = client_authenticate_get_capabilities(client->common.secured);
	return t_strconcat(CAPABILITY_STRING,
			   (ssl_initialized && !client->common.tls) ?
			   " STARTTLS" : "",
			   disable_plaintext_auth && !client->common.secured ?
			   " LOGINDISABLED" : "", auths, NULL);
}

static int cmd_capability(struct imap_client *client)
{
	client_send_line(client, t_strconcat("* CAPABILITY ",
					     get_capability(client), NULL));
	client_send_tagline(client, "OK Capability completed.");
	return TRUE;
}

static void client_start_tls(struct imap_client *client)
{
	int fd_ssl;

	fd_ssl = ssl_proxy_new(client->common.fd, &client->common.ip,
			       &client->common.proxy);
	if (fd_ssl == -1) {
		client_send_line(client, "* BYE TLS initialization failed.");
		client_destroy(client, "TLS initialization failed.");
		return;
	}

	client->common.tls = TRUE;
	client->common.secured = TRUE;
	client_set_title(client);

	client->common.fd = fd_ssl;
	i_stream_unref(client->input);
	o_stream_unref(client->output);
	imap_parser_destroy(client->parser);

	/* CRLF is lost from buffer when streams are reopened. */
	client->skip_line = FALSE;

	client_open_streams(client, fd_ssl);
	client->io = io_add(client->common.fd, IO_READ, client_input, client);
}

static void client_output_starttls(void *context)
{
	struct imap_client *client = context;
	int ret;

	if ((ret = o_stream_flush(client->output)) < 0) {
		client_destroy(client, "Disconnected");
		return;
	}

	if (ret > 0)
		client_start_tls(client);
}

static int cmd_starttls(struct imap_client *client)
{
	if (client->common.tls) {
		client_send_tagline(client, "BAD TLS is already active.");
		return TRUE;
	}

	if (!ssl_initialized) {
		client_send_tagline(client, "BAD TLS support isn't enabled.");
		return TRUE;
	}

	/* remove input handler, SSL proxy gives us a new fd. we also have to
	   remove it in case we have to wait for buffer to be flushed */
	if (client->io != NULL) {
		io_remove(client->io);
		client->io = NULL;
	}

	client_send_tagline(client, "OK Begin TLS negotiation now.");
	if (o_stream_get_buffer_used_size(client->output) != 0) {
		/* the buffer has to be flushed */
		o_stream_set_flush_callback(client->output,
					    client_output_starttls, client);
	} else {
		client_start_tls(client);
	}
	return TRUE;
}

static int cmd_noop(struct imap_client *client)
{
	client_send_tagline(client, "OK NOOP completed.");
	return TRUE;
}

static int cmd_logout(struct imap_client *client)
{
	client_send_line(client, "* BYE Logging out");
	client_send_tagline(client, "OK Logout completed.");
	client_destroy(client, "Aborted login");
	return TRUE;
}

static int client_command_execute(struct imap_client *client, const char *cmd,
				  struct imap_arg *args)
{
	cmd = t_str_ucase(cmd);
	if (strcmp(cmd, "LOGIN") == 0)
		return cmd_login(client, args);
	if (strcmp(cmd, "AUTHENTICATE") == 0)
		return cmd_authenticate(client, args);
	if (strcmp(cmd, "CAPABILITY") == 0)
		return cmd_capability(client);
	if (strcmp(cmd, "STARTTLS") == 0)
		return cmd_starttls(client);
	if (strcmp(cmd, "NOOP") == 0)
		return cmd_noop(client);
	if (strcmp(cmd, "LOGOUT") == 0)
		return cmd_logout(client);

	return -1;
}

static int client_handle_input(struct imap_client *client)
{
	struct imap_arg *args;
	const char *msg;
	int ret, fatal;

	i_assert(!client->common.authenticating);

	if (client->cmd_finished) {
		/* clear the previous command from memory. don't do this
		   immediately after handling command since we need the
		   cmd_tag to stay some time after authentication commands. */
		client->cmd_tag = NULL;
		client->cmd_name = NULL;
		imap_parser_reset(client->parser);

		/* remove \r\n */
		if (client->skip_line) {
			if (!client_skip_line(client))
				return FALSE;
                        client->skip_line = FALSE;
		}

		client->cmd_finished = FALSE;
	}

	if (client->cmd_tag == NULL) {
                client->cmd_tag = imap_parser_read_word(client->parser);
		if (client->cmd_tag == NULL)
			return FALSE; /* need more data */
	}

	if (client->cmd_name == NULL) {
                client->cmd_name = imap_parser_read_word(client->parser);
		if (client->cmd_name == NULL)
			return FALSE; /* need more data */
	}

	switch (imap_parser_read_args(client->parser, 0, 0, &args)) {
	case -1:
		/* error */
		msg = imap_parser_get_error(client->parser, &fatal);
		if (fatal) {
			client_send_line(client, t_strconcat("* BYE ",
							     msg, NULL));
			client_destroy(client, t_strconcat("Disconnected: ",
							   msg, NULL));
			return FALSE;
		}

		client_send_tagline(client, t_strconcat("BAD ", msg, NULL));
		client->cmd_finished = TRUE;
		client->skip_line = TRUE;
		return TRUE;
	case -2:
		/* not enough data */
		return FALSE;
	}
	client->skip_line = TRUE;

	if (*client->cmd_tag == '\0')
		ret = -1;
	else
		ret = client_command_execute(client, client->cmd_name, args);

	client->cmd_finished = TRUE;
	if (ret < 0) {
		if (*client->cmd_tag == '\0')
			client->cmd_tag = "*";
		if (++client->bad_counter >= CLIENT_MAX_BAD_COMMANDS) {
			client_send_line(client,
				"* BYE Too many invalid IMAP commands.");
			client_destroy(client, "Disconnected: "
				       "Too many invalid commands");
			return FALSE;
		}  
		client_send_tagline(client,
			"BAD Error in IMAP command received by server.");
	}

	return ret != 0;
}

int client_read(struct imap_client *client)
{
	switch (i_stream_read(client->input)) {
	case -2:
		/* buffer full */
		client_send_line(client, "* BYE Input buffer full, aborting");
		client_destroy(client, "Disconnected: Input buffer full");
		return FALSE;
	case -1:
		/* disconnected */
		client_destroy(client, "Disconnected");
		return FALSE;
	default:
		/* something was read */
		return TRUE;
	}
}

void client_input(void *context)
{
	struct imap_client *client = context;

	client->last_input = ioloop_time;

	if (!client_read(client))
		return;

	client_ref(client);

	if (!auth_client_is_connected(auth_client)) {
		/* we're not yet connected to auth process -
		   don't allow any commands */
		client_send_line(client,
			"* OK Waiting for authentication process to respond..");
		client->input_blocked = TRUE;
	} else {
		o_stream_cork(client->output);
		while (client_handle_input(client)) ;
		o_stream_uncork(client->output);
	}

	client_unref(client);
}

static void client_destroy_oldest(void)
{
	struct hash_iterate_context *iter;
	void *key, *value;
	struct imap_client *destroy_buf[CLIENT_DESTROY_OLDEST_COUNT];
	int i;

	/* find the oldest clients and put them to destroy-buffer */
	memset(destroy_buf, 0, sizeof(destroy_buf));

	iter = hash_iterate_init(clients);
	while (hash_iterate(iter, &key, &value)) {
		struct imap_client *client = key;

		for (i = 0; i < CLIENT_DESTROY_OLDEST_COUNT; i++) {
			if (destroy_buf[i] == NULL ||
			    destroy_buf[i]->created > client->created) {
				/* @UNSAFE */
				memmove(destroy_buf+i+1, destroy_buf+i,
					sizeof(destroy_buf) -
					(i+1) * sizeof(struct imap_client *));
				destroy_buf[i] = client;
				break;
			}
		}
	}
	hash_iterate_deinit(iter);

	/* then kill them */
	for (i = 0; i < CLIENT_DESTROY_OLDEST_COUNT; i++) {
		if (destroy_buf[i] == NULL)
			break;

		client_destroy(destroy_buf[i],
			       "Disconnected: Connection queue full");
	}
}

struct client *client_create(int fd, int ssl, const struct ip_addr *local_ip,
			     const struct ip_addr *ip)
{
	struct imap_client *client;
	const char *addr;
	string_t *greet;

	if (max_logging_users > CLIENT_DESTROY_OLDEST_COUNT &&
	    hash_size(clients) >= max_logging_users) {
		/* reached max. users count, kill few of the
		   oldest connections */
		client_destroy_oldest();
	}

	/* always use nonblocking I/O */
	net_set_nonblock(fd, TRUE);

	client = i_new(struct imap_client, 1);
	client->created = ioloop_time;
	client->refcount = 1;
	client->common.tls = ssl;

        addr = net_ip2addr(ip);
	client->common.secured = ssl ||
		(IPADDR_IS_V4(ip) && strncmp(addr, "127.", 4) == 0) ||
		(IPADDR_IS_V6(ip) && strcmp(addr, "::1") == 0);

	client->common.local_ip = *local_ip;
	client->common.ip = *ip;
	client->common.fd = fd;

	client_open_streams(client, fd);
	client->io = io_add(fd, IO_READ, client_input, client);

	client->last_input = ioloop_time;
	hash_insert(clients, client, client);

	main_ref();

	greet = t_str_new(128);
	str_append(greet, "* OK ");
	if (greeting_capability)
		str_printfa(greet, "[CAPABILITY %s] ", get_capability(client));
	str_append(greet, greeting);

	client_send_line(client, str_c(greet));
	client_set_title(client);
	return &client->common;
}

void client_destroy(struct imap_client *client, const char *reason)
{
	if (client->destroyed)
		return;
	client->destroyed = TRUE;

	if (reason != NULL)
		client_syslog(&client->common, "%s", reason);

	hash_remove(clients, client);

	i_stream_close(client->input);
	if (client->output != NULL)
		o_stream_close(client->output);

	if (client->common.auth_request != NULL) {
		auth_client_request_abort(client->common.auth_request);
                client->common.auth_request = NULL;
	}

	if (client->common.master_tag != 0)
		master_request_abort(&client->common);

	if (client->io != NULL) {
		io_remove(client->io);
		client->io = NULL;
	}

	if (client->common.fd != -1) {
		net_disconnect(client->common.fd);
		client->common.fd = -1;
	}

	if (client->common.proxy != NULL)
		ssl_proxy_free(client->common.proxy);
	client_unref(client);
}

void client_destroy_internal_failure(struct imap_client *client)
{
	client_send_line(client, "* BYE Internal login failure. "
			 "Refer to server log for more information.");
	client_destroy(client, t_strconcat("Internal login failure: ",
					   client->common.virtual_user, NULL));
}

void client_ref(struct imap_client *client)
{
	client->refcount++;
}

int client_unref(struct imap_client *client)
{
	if (--client->refcount > 0)
		return TRUE;

	imap_parser_destroy(client->parser);

	i_stream_unref(client->input);
	if (client->output != NULL)
		o_stream_unref(client->output);

	i_free(client->common.virtual_user);
	i_free(client->common.auth_mech_name);
	i_free(client);

	main_unref();
	return FALSE;
}

void client_send_line(struct imap_client *client, const char *line)
{
	struct const_iovec iov[2];
	ssize_t ret;

	iov[0].iov_base = line;
	iov[0].iov_len = strlen(line);
	iov[1].iov_base = "\r\n";
	iov[1].iov_len = 2;

	if ((ret = o_stream_sendv(client->output, iov, 2)) < 0)
		client_destroy(client, "Disconnected");
	else if ((size_t)ret != iov[0].iov_len + iov[1].iov_len)
		client_destroy(client, "Transmit buffer full");
}

void client_send_tagline(struct imap_client *client, const char *line)
{
	client_send_line(client, t_strconcat(client->cmd_tag, " ", line, NULL));
}

static void client_check_idle(struct imap_client *client)
{
	if (ioloop_time - client->last_input >= CLIENT_LOGIN_IDLE_TIMEOUT) {
		client_send_line(client, "* BYE Disconnected for inactivity.");
		client_destroy(client, "Disconnected: Inactivity");
	}
}

static void idle_timeout(void *context __attr_unused__)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(clients);
	while (hash_iterate(iter, &key, &value)) {
		struct imap_client *client = key;

		client_check_idle(client);
	}
	hash_iterate_deinit(iter);
}

unsigned int clients_get_count(void)
{
	return hash_size(clients);
}

void clients_notify_auth_connected(void)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(clients);
	while (hash_iterate(iter, &key, &value)) {
		struct imap_client *client = key;

		if (client->input_blocked) {
			client->input_blocked = FALSE;
			client_input(client);
		}
	}
	hash_iterate_deinit(iter);
}

void clients_destroy_all(void)
{
	struct hash_iterate_context *iter;
	void *key, *value;

	iter = hash_iterate_init(clients);
	while (hash_iterate(iter, &key, &value)) {
		struct imap_client *client = key;

		client_destroy(client, NULL);
	}
	hash_iterate_deinit(iter);
}

void clients_init(void)
{
	clients = hash_create(default_pool, default_pool, 128, NULL, NULL);
	to_idle = timeout_add(1000, idle_timeout, NULL);
}

void clients_deinit(void)
{
	clients_destroy_all();
	hash_destroy(clients);

	timeout_remove(to_idle);
}
