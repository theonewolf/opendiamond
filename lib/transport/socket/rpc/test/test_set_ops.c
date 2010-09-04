/*
 * miniRPC - TCP RPC library with asynchronous operations
 *
 * Copyright (C) 2007-2010 Carnegie Mellon University
 *
 * This code is distributed "AS IS" without warranty of any kind under the
 * terms of the GNU Lesser General Public License version 2.1, as shown in
 * the file COPYING.
 */

#define ITERS 25

#include "common.h"

const struct proto_server_operations ops_ok;
const struct proto_server_operations ops_fail;

static mrpc_status_t do_ping_ok(void *conn_data)
{
	expect(proto_server_set_operations(conn_data, &ops_fail), 0);
	return MINIRPC_OK;
}

static mrpc_status_t do_ping_fail(void *conn_data)
{
	expect(proto_server_set_operations(conn_data, &ops_ok), 0);
	return 1;
}

const struct proto_server_operations ops_ok = {
	.ping = do_ping_ok
};

const struct proto_server_operations ops_fail = {
	.ping = do_ping_fail
};

int main(int argc, char **argv)
{
	struct mrpc_conn_set *sset;
	struct mrpc_conn_set *cset;
	struct mrpc_connection *sconn;
	struct mrpc_connection *conn;
	int ret;
	int i;

	if (mrpc_conn_set_create(&sset, proto_server, NULL))
		die("Couldn't allocate conn set");
	mrpc_set_disconnect_func(sset, disconnect_normal);
	start_monitored_dispatcher(sset);
	if (mrpc_conn_set_create(&cset, proto_client, NULL))
		die("Couldn't allocate conn set");
	mrpc_set_disconnect_func(cset, disconnect_user);
	start_monitored_dispatcher(cset);

	ret=mrpc_conn_create(&sconn, sset, NULL);
	if (ret)
		die("%s", strerror(ret));
	expect(proto_server_set_operations(sconn, &ops_ok), 0);
	ret=mrpc_conn_create(&conn, cset, NULL);
	if (ret)
		die("%s", strerror(ret));
	bind_conn_pair(sconn, conn);

	for (i=0; i<ITERS; i++) {
		expect(proto_ping(conn), 0);
		expect(proto_ping(conn), 1);
	}

	mrpc_conn_close(conn);
	mrpc_conn_unref(conn);
	mrpc_conn_set_unref(cset);
	mrpc_conn_set_unref(sset);
	expect_disconnects(1, 1, 0);
	return 0;
}
