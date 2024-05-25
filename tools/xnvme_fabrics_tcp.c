#include <errno.h>
#include <libxnvme.h>

static int
sub_connect(struct xnvme_cli *cli)
{
	struct xnvme_fabrics_connection_parameters params = {
		.port = XNVME_FABRICS_TCP_PORT,
		.ddgst_enable = 1,
		.hdgst_enable = 1,
	};
	struct xnvme_fabrics_connection *con;

	params.addr = cli->args.sys_uri;

	con = xnvme_fabrics_connect(&params);
	if (!con) {
		perror("failed connecting to server");
		return errno;
	}

	printf("Connected to %s, %d\n", params.addr, XNVME_FABRICS_TCP_PORT);

	xnvme_fabrics_disconnect(con);

	return 0;
}

static struct xnvme_cli_sub g_subs[] = {
	{
		"connect",
		"Connect to the given TCP address, port 4420 is assumed",
		"Connect to the given TCP address, port 4420 is assumed",
		sub_connect,
		{
			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_SYS_URI, XNVME_CLI_LREQ},

			XNVME_CLI_CORE_OPTS,
		},
	},
};

static struct xnvme_cli g_cli = {
	.title = "xNVMe Fabrics (NVMe/TCP) Initiator",
	.descr_short = "",
	.descr_long = "",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};

int
main(int argc, char **argv)
{
	return xnvme_cli_run(&g_cli, argc, argv, XNVME_CLI_INIT_NONE);
}
