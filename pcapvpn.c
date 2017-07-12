/*
 * pcapvpn.c
 * Creates a layer-2 VPN via a tap device and pcap
 * By J. Stuart McMurray
 * Created 20170709
 * Last Modified 20170711
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif /* #ifdef __linux__ */

#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pcap.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROMISC   1
#define BUFLEN    UINT16_MAX
#define MACFILTER "ether host %s"
#define IPFILTER  "host %s"

void usage(void);
int do_tap(char *dev);
int do_pcap(char *dev, char *addr);
void * tap_rx(void *fd);
int tap_tx(int fd);
void read_full(int fd, void *buf, size_t nbytes);
void *pcap_tx(void *arg);
void pcap_rx(u_char *, const struct pcap_pkthdr *hdr, const u_char *pkt);
int is_mac(char *addr);
int is_ip(char *addr);

int
main(int argc, char **argv)
{
        int ch;
        int use_tap;

        /* Work out whether to use the tap device or pcap */
        use_tap = 0;
        while (-1 != (ch = getopt(argc, argv, "t")))
                switch (ch) {
                        case 't':
                                use_tap = 1;
                                break;
                        default:
                                usage();
                                break;
                }
        argc -= optind;
        argv += optind;

        /* Get tap device name or MAC address */

        if (use_tap) {
                if (1 > argc)
                        usage();
                return do_tap(argv[0]);
        } else {
                if (2 > argc)
                        usage();
                return do_pcap(argv[0], argv[1]);
        }
}

/* do_tap proxies between the tap device and stdio */
int
do_tap(char *dev) 
{
        int fd;
        pthread_t rx;

        /* Open the tap device */
        if (-1 == (fd = open(dev, O_RDWR)))
                err(1, "open");
        
        /* Start receiving frames */
        if (0 != pthread_create(&rx, NULL, tap_rx, &fd))
                err(2, "pthread_create");

        return tap_tx(fd);
}

/* tap_rx receives packets from the tap device and sends them to stdout */
void *
tap_rx(void *fd)
{
        ssize_t nr;
        uint16_t nw;
        int rfd;
        uint8_t buf[BUFLEN];

        /* Get the file descriptor */
        rfd = *((int* )fd);

        for (;;) {
                /* Read a frame from the tap device */
                if (-1 == (nr = read(rfd, buf, sizeof(buf))))
                        err(3, "read");

                /* Should never happen */
                if (UINT16_MAX < nr)
                        errx(4, "Got too much (%ld) from tap device", nr);

                /* Write message size to stdout */
                nw = htons((uint16_t)nr);
                if (-1 == write(STDOUT_FILENO, &nw, sizeof(nw)))
                        err(5, "write");
                if (-1 == write(STDOUT_FILENO, buf, nr))
                        err(6, "write");
        }
        return NULL;
}

/* tap_tx sends frames from stdin to the tap device */
int
tap_tx(int fd)
{
        uint16_t nr;
        uint8_t buf[BUFLEN];


        for (;;) {
                /* Read the size and frame */
                read_full(STDIN_FILENO, &nr, sizeof(nr));
                nr = ntohs(nr);
                read_full(STDIN_FILENO, buf, (size_t)nr);

                /* Send to the tap device */
                if (-1 == write(fd, buf, (size_t)nr))
                        err(9, "write");
        }

        return 10;
}

/* read_full reads nbytes bytes from fd into buf. */
void
read_full(int fd, void *buf, size_t nbytes)
{
        ssize_t tot, nr;

        tot = nr = 0;

        while (tot < nbytes) {
                if (-1 == (nr = read(fd, ((char *)buf)+tot, nbytes-tot)))
                        err(8, "read");
                tot += nr;
        }
}

/* do_pcap proxies between pcap sniffing, stdio, and pcap injection */
int
do_pcap(char *dev, char *addr)
{
        pcap_t *p;
        struct bpf_program prog;
        bpf_u_int32 net, mask;
        char *filt;
        char errbuf[PCAP_ERRBUF_SIZE];
        pthread_t tid;

        /* Work out whether we have a mac address or IP adderss and make the
         * BPF filter text for it */
        if (is_mac(addr)) { /* MAC Address */
                if (0 > asprintf(&filt, MACFILTER, addr))
                        err(12, "asprintf");
        } else if (is_ip(addr)) { /* IP Address */
                if (0 > asprintf(&filt, IPFILTER, addr))
                        err(20, "asprintf");
        } else { /* Literal filter */
                if (0 > asprintf(&filt, "%s", addr))
                        err(21, "asprintf");
        }

        /* Uncomment the below to print the BPF filter */
        /* fprintf(stderr, "BPF filter: %s\n", filt); fflush(stderr); *//* DEBUG */ 
        if (NULL == filt) {
                errx(1, "NULL filter");
        }
        
        /* Attach to device with pcap */
        if (NULL == (p = pcap_open_live(dev, BUFLEN, PROMISC, 10, errbuf)))
                errx(11, "pcap_open_live: %s", errbuf);


        /* Get device netmask */
        if (-1 == pcap_lookupnet(dev, &net, &mask, errbuf))
                errx(12, "pcap_lookupnet: %s", errbuf);

        /* Set filter */
        if (-1 == pcap_compile(p, &prog, filt, 1, mask))
                err(13, "pcap_compile");
        if (-1 == pcap_setfilter(p, &prog))
                err(14, "pcap_setfilter");
        pcap_freecode(&prog);
        free(filt); filt = NULL;

        /* Read from stdin, inject to the network */
        if (0 != pthread_create(&tid, NULL, pcap_tx, p))
                err(15, "pthread_create");

        /* Sniff from the network, send to stdout */
        if (0 != pcap_loop(p, -1, pcap_rx, NULL))
                err(16, "pcap_loop");

        return -1;
}

/* pcap_tx reads from stdin and injects frames to the network */
void *
pcap_tx(void *arg)
{
        pcap_t *p;
        uint16_t nr;
        uint8_t buf[BUFLEN];

        p = (pcap_t *)arg;

        for (;;) {
                /* Size of frame to read */
                read_full(STDIN_FILENO, &nr, sizeof(nr));
                nr = ntohs(nr);
                /* Read frame */
                read_full(STDIN_FILENO, buf, (size_t)nr);

                /* Inject it */
                if (-1 == pcap_inject(p, buf, (size_t)nr))
                        err(17, "pcap_inject");
        }

        return NULL;
}

/* pcap_rx sniffs a packet from the network and writes it to stdout */
void
pcap_rx(u_char *user, const struct pcap_pkthdr *hdr, const u_char *pkt)
{
        uint16_t nr;
        ssize_t nw;

        /* Make sure frame wasn't truncated */
        if (hdr->len > hdr->caplen) {
                fprintf(
                        stderr,
                        "Truncated packet.  Got %"PRIu32". Was %"PRIu32".",
                        hdr->caplen,
                        hdr->len
                );
                return;
        }

        /* Make sure we didn't get too much packet */
        if (UINT16_MAX < hdr->caplen) {
                fprintf(stderr, "Huge packet.  Got %"PRIu32".", hdr->caplen);
                return;
        }

        /* Write size to stdout */
        nr = htons((uint16_t)hdr->caplen);
        if (-1 == write(STDOUT_FILENO, &nr, sizeof(nr)))
                err(18, "write");

        /* Write packet to stdout */
        if (-1 == (nw = write(STDOUT_FILENO, pkt, (size_t)hdr->caplen)))
                err(19, "write");
}

void
usage(void)
{
        extern char *__progname;
        fprintf(stderr, "Usage: %s -t tap_file\n", __progname);
        fprintf(stderr, "       %s device mac_address\n", __progname);
        exit(-1);
}

/* is_mac returns non-zero if the passed-in string is a MAC address */
int
is_mac(char *addr)
{
        int i;

        /* Make sure it's the right length */
        if (17 != strnlen(addr, 18))
                return 0;

        /* Check that each character is either a hex digit or a colon */
        for (i = 0; i < 17; ++i) {
                /* Every third character should be a colon */
                if (2 == i%3) {
                        if (':' != addr[i])
                                return 2;
                        else
                                continue;
                }

                /* Every other character should be a hex digit */
                if (!isxdigit((int)addr[i]))
                        return 3;
        }

        return 1;
}

/* is_ip returns non-zero if the passed-in string is an IP address */
int
is_ip(char *addr)
{
        size_t len;
        char rem[16];
        unsigned int b[4];
        int n, i;

        /* IP addresses must be between 7 and 15 characters */
        if ((15 < (len = strnlen(addr, 16))) || 7 > len)
                return 0;

        /* Scan the string into numbers and a remaining string */
        rem[0] = '\0';
        n = sscanf(addr, "%3u.%3u.%3u.%3u%s", &b[0], &b[1], &b[2], &b[3], rem);

        /* If we got a remaining string or didn't get all four numbers, it's
         * not a valid address. */
        if (('\0' != rem[0]) || 4 != n)
                return 0;

        /* Make sure the read bytes are within range */
        for (i = 0; i < 4; ++i)
                if (255 < b[i])
                        return 0;

        return 1;
}
