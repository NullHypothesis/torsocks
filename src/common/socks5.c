/*
 * Copyright (C) 2013 - David Goulet <dgoulet@ev0ke.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 only, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

#include <lib/torsocks.h>

#include "log.h"
#include "socks5.h"

/*
 * Receive data on a given file descriptor using recv(2). This handles partial
 * send and EINTR.
 *
 * Return the number of bytes received or a negative errno error.
 */
static ssize_t recv_data(int fd, void *buf, size_t len)
{
	ssize_t ret, read_len, read_left, index;

	assert(buf);
	assert(fd >= 0);

	read_left = len;
	index = 0;
	do {
		read_len = recv(fd, buf + index, read_left, 0);
		if (read_len < 0) {
			ret = -errno;
			if (errno == EINTR) {
				/* Try again after interruption. */
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (index) {
					/* Return the number of bytes received up to this point. */
					ret = index;
				}
				continue;
			} else {
				PERROR("recv socks5 data");
				goto error;
			}
		}
		read_left -= read_len;
		index += read_len;
	} while (read_left > 0);

	/* Everything was received. */
	ret = index;

error:
	return ret;
}

/*
 * Send data to a given file descriptor using send(2). This handles partial
 * send and EINTR.
 *
 * Return the number of bytes sent or a negative errno error.
 */
static ssize_t send_data(int fd, const void *buf, size_t len)
{
	ssize_t ret, sent_len, sent_left, index;

	assert(buf);
	assert(fd >= 0);

	sent_left = len;
	index = 0;
	do {
		sent_len = send(fd, buf + index, sent_left, 0);
		if (sent_len < 0) {
			ret = -errno;
			if (errno == EINTR) {
				/* Send again after interruption. */
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (index) {
					/* Return the number of bytes sent up to this point. */
					ret = index;
				}
				continue;
			} else {
				PERROR("send socks5 data");
				goto error;
			}
		}
		sent_left -= sent_len;
		index += sent_len;
	} while (sent_left > 0);

	/* Everything was sent. */
	ret = index;

error:
	return ret;
}

/*
 * Connect to socks5 server address from the global configuration.
 *
 * Return 0 on success or else a negative value.
 */
ATTR_HIDDEN
int socks5_connect(struct connection *conn)
{
	int ret;
	struct sockaddr *socks5_addr = NULL;

	assert(conn);
	assert(conn->fd >= 0);

	switch (tsocks_config.socks5_addr.domain) {
	case CONNECTION_DOMAIN_INET:
		socks5_addr = (struct sockaddr *) &tsocks_config.socks5_addr.u.sin;
		break;
	case CONNECTION_DOMAIN_INET6:
		socks5_addr = (struct sockaddr *) &tsocks_config.socks5_addr.u.sin6;
		break;
	default:
		ERR("Socks5 connect domain unknown %d",
				tsocks_config.socks5_addr.domain);
		assert(0);
		ret = -EBADF;
		goto error;
	}

	do {
		/* Use the original libc connect() to the Tor. */
		ret = tsocks_libc_connect(conn->fd, socks5_addr, sizeof(*socks5_addr));
	} while (ret < 0 &&
			(errno == EINTR || errno == EINPROGRESS || errno == EALREADY));
	if (ret < 0) {
		/* The non blocking socket is now connected. */
		if (errno == EISCONN) {
			ret = 0;
			goto error;
		}
		ret = -errno;
		PERROR("socks5 libc connect");
	}

error:
	return ret;
}

/*
 * Send socks5 method packet to server.
 *
 * Return 0 on success or else a negative errno value.
 */
ATTR_HIDDEN
int socks5_send_method(struct connection *conn)
{
	int ret = 0;
	ssize_t ret_send;
	struct socks5_method_req msg;

	assert(conn);
	assert(conn->fd >= 0);

	msg.ver = SOCKS5_VERSION;
	msg.nmethods = 0x01;
	msg.methods = SOCKS5_NO_AUTH_METHOD;

	DBG("Socks5 sending method ver: %d, nmethods 0x%02x, methods 0x%02x",
			msg.ver, msg.nmethods, msg.methods);

	ret_send = send_data(conn->fd, &msg, sizeof(msg));
	if (ret_send < 0) {
		ret = ret_send;
		goto error;
	}

error:
	return ret;
}

/*
 * Receive socks5 method response packet from server.
 *
 * Return 0 on success or else a negative errno value.
 */
ATTR_HIDDEN
int socks5_recv_method(struct connection *conn)
{
	int ret;
	ssize_t ret_recv;
	struct socks5_method_res msg;

	assert(conn);
	assert(conn->fd >= 0);

	ret_recv = recv_data(conn->fd, &msg, sizeof(msg));
	if (ret_recv < 0) {
		ret = ret_recv;
		goto error;
	}

	DBG("Socks5 received method ver: %d, method 0x%02x", msg.ver, msg.method);

	if (msg.ver != SOCKS5_VERSION ||
			msg.method == SOCKS5_NO_ACCPT_METHOD) {
		ret = -ECONNABORTED;
		goto error;
	}

	/* Successfully received. */
	ret = 0;

error:
	return ret;
}

/*
 * Send a connect request to the SOCKS5 server using the given connection and
 * the destination address in it pointing to the destination that needs to be
 * reached through Tor.
 *
 * Return 0 on success or else a negative value.
 */
ATTR_HIDDEN
int socks5_send_connect_request(struct connection *conn)
{
	int ret;
	/* Buffer to send won't go over a full TCP size. */
	char buffer[1500];
	ssize_t buf_len, ret_send;
	struct socks5_request msg;

	assert(conn);
	assert(conn->fd >= 0);

	memset(buffer, 0, sizeof(buffer));
	buf_len = sizeof(msg);

	msg.ver = SOCKS5_VERSION;
	msg.cmd = SOCKS5_CMD_CONNECT;
	/* Always zeroed. */
	msg.rsv = 0;

	switch (conn->dest_addr.domain) {
	case CONNECTION_DOMAIN_INET:
	{
		struct socks5_request_ipv4 req_ipv4;

		msg.atyp = SOCKS5_ATYP_IPV4;
		/* Copy the first part of the request. */
		memcpy(buffer, &msg, buf_len);

		/* Prepare the ipv4 payload to be copied in the send buffer. */
		memcpy(req_ipv4.addr, &conn->dest_addr.u.sin.sin_addr,
				sizeof(req_ipv4.addr));
		req_ipv4.port = conn->dest_addr.u.sin.sin_port;

		/* Copy ipv4 request portion in the buffer. */
		memcpy(buffer + buf_len, &req_ipv4, sizeof(req_ipv4));
		buf_len += sizeof(req_ipv4);
		break;
	}
	case CONNECTION_DOMAIN_INET6:
	{
		struct socks5_request_ipv6 req_ipv6;

		msg.atyp = SOCKS5_ATYP_IPV6;
		/* Copy the first part of the request. */
		memcpy(buffer, &msg, buf_len);

		/* Prepare the ipv6 payload to be copied in the send buffer. */
		memcpy(req_ipv6.addr, &conn->dest_addr.u.sin6.sin6_addr,
				sizeof(req_ipv6.addr));
		req_ipv6.port = conn->dest_addr.u.sin6.sin6_port;

		/* Copy ipv6 request portion in the buffer. */
		memcpy(buffer + buf_len, &req_ipv6, sizeof(req_ipv6));
		buf_len += sizeof(req_ipv6);
		break;
	}
	case CONNECTION_DOMAIN_NAME:
	{
		struct socks5_request_domain req_name;

		msg.atyp = SOCKS5_ATYP_DOMAIN;
		/* Copy the first part of the request. */
		memcpy(buffer, &msg, buf_len);

		/* Setup domain name request buffer. */
		req_name.len = strlen(conn->dest_addr.hostname.addr);
		memcpy(req_name.name, conn->dest_addr.hostname.addr, req_name.len);
		req_name.port = conn->dest_addr.hostname.port;

		/* Copy ipv6 request portion in the buffer. */
		memcpy(buffer + buf_len, &req_name.len, sizeof(req_name.len));
		buf_len += sizeof(req_name.len);
		memcpy(buffer + buf_len, req_name.name, req_name.len);
		buf_len += req_name.len;
		memcpy(buffer + buf_len, &req_name.port, sizeof(req_name.port));
		buf_len += sizeof(req_name.port);
		break;
	}
	default:
		ERR("Socks5 connection domain unknown %d", conn->dest_addr.domain);
		ret = -EINVAL;
		goto error;
	}

	DBG("Socks5 sending connect request to fd %d", conn->fd);

	ret_send = send_data(conn->fd, &buffer, buf_len);
	if (ret_send < 0) {
		ret = ret_send;
		goto error;
	}

	/* Data was sent successfully. */
	ret = 0;

error:
	return ret;
}

/*
 * Receive on the given connection the SOCKS5 connect reply.
 *
 * Return 0 on success or else a negative value.
 */
ATTR_HIDDEN
int socks5_recv_connect_reply(struct connection *conn)
{
	int ret;
	ssize_t ret_recv;
	char buffer[22];	/* Maximum size possible (with IPv6). */
	struct socks5_reply msg;
	size_t recv_len;

	assert(conn);
	assert(conn >= 0);

	/* Beginning of the payload we are receiving. */
	recv_len = sizeof(msg);
	/* Len of BND.PORT */
	recv_len += sizeof(uint16_t);

	switch (conn->dest_addr.domain) {
	case CONNECTION_DOMAIN_NAME:
		/*
		 * Tor returns and IPv4 upon resolution. Same for .onion address.
		 */
	case CONNECTION_DOMAIN_INET:
		recv_len+= 4;
		break;
	case CONNECTION_DOMAIN_INET6:
		recv_len += 16;
		break;
	}

	ret_recv = recv_data(conn->fd, buffer, recv_len);
	if (ret_recv < 0) {
		ret = ret_recv;
		goto error;
	}

	/* Copy the beginning of the reply so we can parse it easily. */
	memcpy(&msg, buffer, sizeof(msg));

	DBG("Socks5 received connect reply - ver: %d, rep: 0x%02x, atype: 0x%02x",
			msg.ver, msg.rep, msg.atyp);

	switch (msg.rep) {
	case SOCKS5_REPLY_SUCCESS:
		DBG("Socks5 connection is successful.");
		ret = 0;
		break;
	case SOCKS5_REPLY_FAIL:
		ERR("General SOCKS server failure");
		ret = -ECONNREFUSED;
		break;
	case SOCKS5_REPLY_DENY_RULE:
		ERR("Connection not allowed by ruleset");
		ret = -ECONNABORTED;
		break;
	case SOCKS5_REPLY_NO_NET:
		ERR("Network unreachable");
		ret = -ENETUNREACH;
		break;
	case SOCKS5_REPLY_NO_HOST:
		ERR("Host unreachable");
		ret = -EHOSTUNREACH;
		break;
	case SOCKS5_REPLY_REFUSED:
		ERR("Connection refused to Tor SOCKS");
		ret = -ECONNREFUSED;
		break;
	case SOCKS5_REPLY_TTL_EXP:
		ERR("Connection timed out");
		ret = -ETIMEDOUT;
		break;
	case SOCKS5_REPLY_CMD_NOTSUP:
		ERR("Command not supported");
		ret = -ECONNREFUSED;
		break;
	case SOCKS5_REPLY_ADR_NOTSUP:
		ERR("Address type not supported");
		ret = -ECONNREFUSED;
		break;
	default:
		ERR("Socks5 server replied an unknown code %d", msg.rep);
		ret = -ECONNABORTED;
		break;
	}

error:
	return ret;
}

/*
 * Send a SOCKS5 Tor resolve request for a given hostname using an already
 * connected connection.
 *
 * Return 0 on success or else a negative value.
 */
ATTR_HIDDEN
int socks5_send_resolve_request(const char *hostname, struct connection *conn)
{
	int ret, ret_send;
	/*
	 * Can't go bigger than that. 4 bytes for the header, 1 for the name len
	 * and 255 for the name.
	 */
	char buffer[260];
	size_t name_len, msg_len, data_len;
	struct socks5_request msg;
	struct socks5_request_resolve req;

	assert(hostname);
	assert(conn);
	assert(conn->fd >= 0);

	memset(buffer, 0, sizeof(buffer));
	msg_len = sizeof(msg);

	msg.ver = SOCKS5_VERSION;
	msg.cmd = SOCKS5_CMD_RESOLVE;
	/* Always zeroed. */
	msg.rsv = 0;
	/* By default we use IPv4 address. */
	msg.atyp = SOCKS5_ATYP_DOMAIN;

	name_len = strlen(hostname);
	if (name_len > sizeof(req.name)) {
		ret = -EINVAL;
		goto error;
	}

	/* Setup resolve request. */
	req.len = name_len;
	memcpy(req.name, hostname, name_len);

	/* Copy final buffer. */
	memcpy(buffer, &msg, msg_len);
	memcpy(buffer + msg_len, &req, sizeof(req));
	data_len = msg_len + sizeof(req);

	ret_send = send_data(conn->fd, &buffer, data_len);
	if (ret_send < 0) {
		ret = ret_send;
		goto error;
	}

	/* Data was sent successfully. */
	ret = 0;
	DBG("[socks5] Resolve for %s sent successfully", hostname);

error:
	return ret;
}

/*
 * Receive a Tor resolve reply on the given connection. The ip address pointer
 * is populated with the replied value or else untouched on error.
 *
 * Return 0 on success else a negative value.
 */
ATTR_HIDDEN
int socks5_recv_resolve_reply(struct connection *conn, void *addr,
		size_t addrlen)
{
	int ret;
	size_t recv_len;
	ssize_t ret_recv;
	struct {
		struct socks5_reply msg;
		union {
			uint8_t ipv4[4];
			uint8_t ipv6[16];
		} addr;
	} buffer;

	assert(conn);
	assert(conn >= 0);
	assert(addr);

	ret_recv = recv_data(conn->fd, &buffer, sizeof(buffer.msg));
	if (ret_recv < 0) {
		ret = ret_recv;
		goto error;
	}

	if (buffer.msg.ver != SOCKS5_VERSION) {
		ERR("Bad SOCKS5 version reply");
		ret = -ECONNABORTED;
		goto error;
	}

	if (buffer.msg.rep != SOCKS5_REPLY_SUCCESS) {
		ERR("Unable to resolve. Status reply: %d", buffer.msg.rep);
		ret = -ECONNABORTED;
		goto error;
	}

	if (buffer.msg.atyp == SOCKS5_ATYP_IPV4) {
		/* Size of a binary IPv4 in bytes. */
		recv_len = sizeof(buffer.addr.ipv4);
	} else if (buffer.msg.atyp == SOCKS5_ATYP_IPV6) {
		/* Size of a binary IPv6 in bytes. */
		recv_len = sizeof(buffer.addr.ipv6);
	} else {
		ERR("Bad SOCKS5 atyp reply %d", buffer.msg.atyp);
		ret = -EINVAL;
		goto error;
	}

	ret_recv = recv_data(conn->fd, &buffer.addr, recv_len);
	if (ret_recv < 0) {
		ret = ret_recv;
		goto error;
	}

	if (addrlen < recv_len) {
		ERR("[socks5] Resolve destination buffer too small");
		ret = -EINVAL;
		goto error;
	}

	memcpy(addr, &buffer.addr, recv_len);

	/* Everything went well and ip_addr has been populated. */
	ret = 0;
	DBG("[socks5] Resolve reply received successfully");

error:
	return ret;
}

/*
 * Send a SOCKS5 Tor resolve ptr request for a given ip address using an
 * already connected connection.
 *
 * Return 0 on success or else a negative value.
 */
ATTR_HIDDEN
int socks5_send_resolve_ptr_request(const void *ip, struct connection *conn)
{
	int ret, ret_send;
	char buffer[20];	/* Can't go higher than that (with IPv6). */
	char ip_str[INET6_ADDRSTRLEN];
	size_t msg_len, data_len;
	struct socks5_request msg;
	struct socks5_request_resolve_ptr req;

	assert(conn);
	assert(conn->fd >= 0);

	DBG("[socks5] Resolve ptr request for ip %u", ip);

	memset(buffer, 0, sizeof(buffer));
	msg_len = sizeof(msg);

	msg.ver = SOCKS5_VERSION;
	msg.cmd = SOCKS5_CMD_RESOLVE_PTR;
	/* Always zeroed. */
	msg.rsv = 0;

	if (inet_ntop(AF_INET, ip, ip_str, sizeof(ip_str))) {
		msg.atyp = SOCKS5_ATYP_IPV4;
		memcpy(req.addr.ipv4, ip, 4);
	} else if (inet_ntop(AF_INET6, ip, ip_str, sizeof(ip_str))) {
		msg.atyp = SOCKS5_ATYP_IPV6;
		memcpy(req.addr.ipv4, ip, 16);
	} else {
		ERR("Unknown address domain of %d", ip);
		ret = -EINVAL;
		goto error;
	}

	/* Copy final buffer. */
	memcpy(buffer, &msg, msg_len);
	memcpy(buffer + msg_len, &req, sizeof(req));
	data_len = msg_len + sizeof(req);

	ret_send = send_data(conn->fd, &buffer, data_len);
	if (ret_send < 0) {
		ret = ret_send;
		goto error;
	}

	/* Data was sent successfully. */
	ret = 0;
	DBG("[socks5] Resolve PTR for %u sent successfully", ip);

error:
	return ret;
}

/*
 * Receive a Tor resolve ptr reply on the given connection. The hostname value
 * is populated with the returned name from Tor. On error, it's untouched. The
 * memory is allocated so the caller needs to free the memory on success.
 *
 * Return 0 on success else a negative value.
 */
ATTR_HIDDEN
int socks5_recv_resolve_ptr_reply(struct connection *conn, char **_hostname)
{
	int ret;
	ssize_t ret_recv;
	char *hostname = NULL;
	struct {
		struct socks5_reply msg;
		uint8_t len;
	} buffer;

	assert(conn);
	assert(conn >= 0);
	assert(_hostname);

	ret_recv = recv_data(conn->fd, &buffer, sizeof(buffer));
	if (ret_recv < 0) {
		ret = ret_recv;
		goto error;
	}

	if (buffer.msg.ver != SOCKS5_VERSION) {
		ERR("Bad SOCKS5 version reply");
		ret = -ECONNABORTED;
		goto error;
	}

	if (buffer.msg.rep != SOCKS5_REPLY_SUCCESS) {
		ERR("Unable to resolve. Status reply: %d", buffer.msg.rep);
		ret = -ECONNABORTED;
		goto error;
	}

	if (buffer.msg.atyp == SOCKS5_ATYP_DOMAIN) {
		/* Allocate hostname len plus an extra for the null byte. */
		hostname = zmalloc(buffer.len + 1);
		if (!hostname) {
			ret = -ENOMEM;
			goto error;
		}
		ret_recv = recv_data(conn->fd, hostname, buffer.len);
		if (ret_recv < 0) {
			ret = ret_recv;
			goto error;
		}
		hostname[buffer.len] = '\0';
	} else {
		ERR("Bad SOCKS5 atyp reply %d", buffer.msg.atyp);
		ret = -EINVAL;
		goto error;
	}

	*_hostname = hostname;
	DBG("[socks5] Resolve reply received: %s", *_hostname);
	return 0;

error:
	free(hostname);
	return ret;
}
