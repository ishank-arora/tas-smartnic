#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <utils.h>
#include <config.h>

enum cfg_params {
    CP_SHM_LEN,
    CP_IP_ADDR,
    CP_IP_ROUTE,
    CP_PORT,
    CP_FP_NO_HUGEPAGES,
};

static struct option opts[] = {
    {
        .name = "shm-len",
        .has_arg = required_argument,
        .val = CP_SHM_LEN,
    },
    {   
        .name = "ip-addr",
        .has_arg = required_argument,
        .val = CP_IP_ADDR,
    },
    {
        .name = "port",
        .has_arg = required_argument,
        .val = CP_PORT,
    },
    {
        .name = "fp-no-hugepages",
        .has_arg = no_argument,
        .val = CP_FP_NO_HUGEPAGES,
    },
    {
        .name = NULL,
    },
};

static int config_defaults(struct configuration *c, char *progname);
static void print_usage(struct configuration *c, char *progname);

int config_parse(struct configuration *c, int argc, char *argv[]) {
    int ret, done = 0;
    if (config_defaults(c, argv[0]) != 0) {
        fprintf(stderr, "config_parse: config defaults failed\n");
        goto failed;
    }

    while (!done) {
        ret = getopt_long(argc, argv, "", opts, NULL);
        switch (ret) {
            case CP_SHM_LEN:
                if (parse_int64(optarg, &c->shm_len) != 0) {
                    fprintf(stderr, "shm len parsing failed\n");
                    goto failed;
                }
                break;
            case CP_IP_ADDR:
                if (util_parse_ipv4(optarg, &c->ip) != 0) {
                    fprintf(stderr, "Parsing host-exposed IP failed\n");
                    goto failed;
                }
                break;
            case CP_PORT:
                if (parse_int16(optarg, &c->port) != 0) {
                    fprintf(stderr, "Parsing host-exposed port failed\n");
                    goto failed;
                }
                break;
            case CP_FP_NO_HUGEPAGES:
                c->fp_hugepages = 0;
                break;
            case -1:
                done = 1;
                break;
            default:
                abort();
        }
    }
    if (optind != argc) {
        goto failed;
    }

    if(c->ip == 0) {
        fprintf(stderr, "ip-addr is a required argument!\n");
        return -1;
    }

    return 0;

failed:
    config_defaults(c, argv[0]);
    print_usage(c, argv[0]);
    return -1;
}

static int config_defaults(struct configuration *c, char *progname)
{
    c->ip = 0;
    c->port = 0;
    c->shm_len = 1024 * 1024 * 1024;
    c->fp_hugepages = 1;
    return 0;
}

static void print_usage(struct configuration *c, char *progname)
{
    fprintf(stderr, "Usage: %s [OPTION]... --ip-addr=IP[/PREFIXLEN]\n"
      "\n"
      "Memory Sizes:\n"
      "  --shm-len=LEN                     Shared memory len "
          "[default: %"PRIu64"]\n"
      "\n"
      "IP protocol parameters:\n"
      "  --ip-addr=ADDR[/PREFIXLEN]        Set local IP address\n"
      "  --port=PORT                       Set port\n"
      "Fast path:\n"
      "  --fp-no-hugepages           Disable hugepages for SHM "
          "[default: enabled]\n"
    ,progname, c->shm_len);

}