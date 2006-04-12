/******************************************************************************
 check_ntp.c: utility to check ntp servers independant of any commandline
              programs or external libraries.
 original author: sean finney <seanius@seanius.net>
 ******************************************************************************
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

 $Id$
 
*****************************************************************************/

const char *progname = "check_ntp";
const char *revision = "$Revision$";
const char *copyright = "2006";
const char *email = "nagiosplug-devel@lists.sourceforge.net";

#include "common.h"
#include "netutils.h"
#include "utils.h"

static char *server_address=NULL;
static int verbose=0;
static int zero_offset_bad=0;
static double owarn=0;
static double ocrit=0;
static short do_jitter=0;
static double jwarn=0;
static double jcrit=0;

int process_arguments (int, char **);
void print_help (void);
void print_usage (void);

/* number of times to perform each request to get a good average. */
#define AVG_NUM 4

/* max size of control message data */
#define MAX_CM_SIZE 468

/* this structure holds everything in an ntp request/response as per rfc1305 */
typedef struct {
	uint8_t flags;       /* byte with leapindicator,vers,mode. see macros */
	uint8_t stratum;     /* clock stratum */
	int8_t poll;         /* polling interval */
	int8_t precision;    /* precision of the local clock */
	int32_t rtdelay;     /* total rt delay, as a fixed point num. see macros */
	uint32_t rtdisp;     /* like above, but for max err to primary src */
	uint32_t refid;      /* ref clock identifier */
	uint64_t refts;      /* reference timestamp.  local time local clock */
	uint64_t origts;     /* time at which request departed client */
	uint64_t rxts;       /* time at which request arrived at server */
	uint64_t txts;       /* time at which request departed server */
} ntp_message;

/* this structure holds everything in an ntp control message as per rfc1305 */
typedef struct {
	uint8_t flags;       /* byte with leapindicator,vers,mode. see macros */
	uint8_t op;          /* R,E,M bits and Opcode */
	uint16_t seq;        /* Packet sequence */
	uint16_t status;     /* Clock status */
	uint16_t assoc;      /* Association */
	uint16_t offset;     /* Similar to TCP sequence # */
	uint16_t count;      /* # bytes of data */
	char data[MAX_CM_SIZE]; /* ASCII data of the request */
	                        /* NB: not necessarily NULL terminated! */
} ntp_control_message;

/* this is an association/status-word pair found in control packet reponses */
typedef struct {
	uint16_t assoc;
	uint16_t status;
} ntp_assoc_status_pair;

/* bits 1,2 are the leap indicator */
#define LI_MASK 0xc0
#define LI(x) ((x&LI_MASK)>>6)
#define LI_SET(x,y) do{ x |= ((y<<6)&LI_MASK); }while(0)
/* and these are the values of the leap indicator */
#define LI_NOWARNING 0x00
#define LI_EXTRASEC 0x01
#define LI_MISSINGSEC 0x02
#define LI_ALARM 0x03
/* bits 3,4,5 are the ntp version */
#define VN_MASK 0x38
#define VN(x)	((x&VN_MASK)>>3)
#define VN_SET(x,y)	do{ x |= ((y<<3)&VN_MASK); }while(0)
#define VN_RESERVED 0x02
/* bits 6,7,8 are the ntp mode */
#define MODE_MASK 0x07
#define MODE(x) (x&MODE_MASK)
#define MODE_SET(x,y)	do{ x |= (y&MODE_MASK); }while(0)
/* here are some values */
#define MODE_CLIENT 0x03
#define MODE_CONTROLMSG 0x06
/* In control message, bits 8-10 are R,E,M bits */
#define REM_MASK 0xe0
#define REM_RESP 0x80
#define REM_ERROR 0x40
#define REM_MORE 0x20
/* In control message, bits 11 - 15 are opcode */
#define OP_MASK 0x1f
#define OP_SET(x,y)   do{ x |= (y&OP_MASK); }while(0)
#define OP_READSTAT 0x01
#define OP_READVAR  0x02
/* In peer status bytes, bytes 6,7,8 determine clock selection status */
#define PEER_SEL(x) (x&0x07)
#define PEER_INCLUDED 0x04
#define PEER_SYNCSOURCE 0x06

/**
 ** a note about the 32-bit "fixed point" numbers:
 **
 they are divided into halves, each being a 16-bit int in network byte order:
 - the first 16 bits are an int on the left side of a decimal point.
 - the second 16 bits represent a fraction n/(2^16)
 likewise for the 64-bit "fixed point" numbers with everything doubled :) 
 **/

/* macros to access the left/right 16 bits of a 32-bit ntp "fixed point"
   number.  note that these can be used as lvalues too */
#define L16(x) (((uint16_t*)&x)[0])
#define R16(x) (((uint16_t*)&x)[1])
/* macros to access the left/right 32 bits of a 64-bit ntp "fixed point"
   number.  these too can be used as lvalues */
#define L32(x) (((uint32_t*)&x)[0])
#define R32(x) (((uint32_t*)&x)[1])

/* ntp wants seconds since 1/1/00, epoch is 1/1/70.  this is the difference */
#define EPOCHDIFF 0x83aa7e80UL

/* extract a 32-bit ntp fixed point number into a double */
#define NTP32asDOUBLE(x) (ntohs(L16(x)) + (double)ntohs(R16(x))/65536.0)

/* likewise for a 64-bit ntp fp number */
#define NTP64asDOUBLE(n) (double)(((uint64_t)n)?\
                         (ntohl(L32(n))-EPOCHDIFF) + \
                         (.00000001*(0.5+(double)(ntohl(R32(n))/42.94967296))):\
                         0)

/* convert a struct timeval to a double */
#define TVasDOUBLE(x) (double)(x.tv_sec+(0.000001*x.tv_usec))

/* convert an ntp 64-bit fp number to a struct timeval */
#define NTP64toTV(n,t) \
	do{ if(!n) t.tv_sec = t.tv_usec = 0; \
	    else { \
			t.tv_sec=ntohl(L32(n))-EPOCHDIFF; \
			t.tv_usec=(int)(0.5+(double)(ntohl(R32(n))/4294.967296)); \
		} \
	}while(0)

/* convert a struct timeval to an ntp 64-bit fp number */
#define TVtoNTP64(t,n) \
	do{ if(!t.tv_usec && !t.tv_sec) n=0x0UL; \
		else { \
			L32(n)=htonl(t.tv_sec + EPOCHDIFF); \
			R32(n)=htonl((4294.967296*t.tv_usec)+.5); \
		} \
	} while(0)

/* NTP control message header is 12 bytes, plus any data in the data
 * field, plus null padding to the nearest 32-bit boundary per rfc.
 */
#define SIZEOF_NTPCM(m) (12+ntohs(m.count)+((m.count)?4-(ntohs(m.count)%4):0))

/* finally, a little helper or two for debugging: */
#define DBG(x) do{if(verbose>1){ x; }}while(0);
#define PRINTSOCKADDR(x) \
	do{ \
		printf("%u.%u.%u.%u", (x>>24)&0xff, (x>>16)&0xff, (x>>8)&0xff, x&0xff);\
	}while(0);

/* calculate the offset of the local clock */
static inline double calc_offset(const ntp_message *m, const struct timeval *t){
	double client_tx, peer_rx, peer_tx, client_rx, rtdelay;
	client_tx = NTP64asDOUBLE(m->origts);
	peer_rx = NTP64asDOUBLE(m->rxts);
	peer_tx = NTP64asDOUBLE(m->txts);
	client_rx=TVasDOUBLE((*t));
	rtdelay=NTP32asDOUBLE(m->rtdelay);
	return (.5*((peer_tx-client_rx)+(peer_rx-client_tx)))-rtdelay;
}

/* print out a ntp packet in human readable/debuggable format */
void print_ntp_message(const ntp_message *p){
	struct timeval ref, orig, rx, tx;

	NTP64toTV(p->refts,ref);
	NTP64toTV(p->origts,orig);
	NTP64toTV(p->rxts,rx);
	NTP64toTV(p->txts,tx);

	printf("packet contents:\n");
	printf("\tflags: 0x%.2x\n", p->flags);
	printf("\t  li=%d (0x%.2x)\n", LI(p->flags), p->flags&LI_MASK);
	printf("\t  vn=%d (0x%.2x)\n", VN(p->flags), p->flags&VN_MASK);
	printf("\t  mode=%d (0x%.2x)\n", MODE(p->flags), p->flags&MODE_MASK);
	printf("\tstratum = %d\n", p->stratum);
	printf("\tpoll = %g\n", pow(2, p->poll));
	printf("\tprecision = %g\n", pow(2, p->precision));
	printf("\trtdelay = %-.16g\n", NTP32asDOUBLE(p->rtdelay));
	printf("\trtdisp = %-.16g\n", NTP32asDOUBLE(p->rtdisp));
	printf("\trefid = %x\n", p->refid);
	printf("\trefts = %-.16g\n", NTP64asDOUBLE(p->refts));
	printf("\torigts = %-.16g\n", NTP64asDOUBLE(p->origts));
	printf("\trxts = %-.16g\n", NTP64asDOUBLE(p->rxts));
	printf("\ttxts = %-.16g\n", NTP64asDOUBLE(p->txts));
}

void print_ntp_control_message(const ntp_control_message *p){
	int i=0, numpeers=0;
	const ntp_assoc_status_pair *peer=NULL;

	printf("control packet contents:\n");
	printf("\tflags: 0x%.2x , 0x%.2x\n", p->flags, p->op);
	printf("\t  li=%d (0x%.2x)\n", LI(p->flags), p->flags&LI_MASK);
	printf("\t  vn=%d (0x%.2x)\n", VN(p->flags), p->flags&VN_MASK);
	printf("\t  mode=%d (0x%.2x)\n", MODE(p->flags), p->flags&MODE_MASK);
	printf("\t  response=%d (0x%.2x)\n", (p->op&REM_RESP)>0, p->op&REM_RESP);
	printf("\t  more=%d (0x%.2x)\n", (p->op&REM_MORE)>0, p->op&REM_MORE);
	printf("\t  error=%d (0x%.2x)\n", (p->op&REM_ERROR)>0, p->op&REM_ERROR);
	printf("\t  op=%d (0x%.2x)\n", p->op&OP_MASK, p->op&OP_MASK);
	printf("\tsequence: %d (0x%.2x)\n", ntohs(p->seq), ntohs(p->seq));
	printf("\tstatus: %d (0x%.2x)\n", ntohs(p->status), ntohs(p->status));
	printf("\tassoc: %d (0x%.2x)\n", ntohs(p->assoc), ntohs(p->assoc));
	printf("\toffset: %d (0x%.2x)\n", ntohs(p->offset), ntohs(p->offset));
	printf("\tcount: %d (0x%.2x)\n", ntohs(p->count), ntohs(p->count));
	numpeers=ntohs(p->count)/(sizeof(ntp_assoc_status_pair));
	if(p->op&REM_RESP && p->op&OP_READSTAT){
		peer=(ntp_assoc_status_pair*)p->data;
		for(i=0;i<numpeers;i++){
			printf("\tpeer id %.2x status %.2x", 
			       ntohs(peer[i].assoc), ntohs(peer[i].status));
			if (PEER_SEL(peer[i].status) >= PEER_INCLUDED){
				if(PEER_SEL(peer[i].status) >= PEER_SYNCSOURCE){
					printf(" <-- current sync source");
				} else {
					printf(" <-- current sync candidate");
				}
			}
			printf("\n");
		}
	}
}

void setup_request(ntp_message *p){
	struct timeval t;

	memset(p, 0, sizeof(ntp_message));
	LI_SET(p->flags, LI_ALARM);
	VN_SET(p->flags, 4);
	MODE_SET(p->flags, MODE_CLIENT);
	p->poll=4;
	p->precision=0xfa;
	L16(p->rtdelay)=htons(1);
	L16(p->rtdisp)=htons(1);

	gettimeofday(&t, NULL);
	TVtoNTP64(t,p->txts);
}

double offset_request(const char *host){
	int i=0, conn=-1;
	ntp_message req;
	double next_offset=0., avg_offset=0.;
	struct timeval recv_time;

	for(i=0; i<AVG_NUM; i++){
		if(verbose) printf("offset run: %d/%d\n", i+1, AVG_NUM);
		setup_request(&req);
		my_udp_connect(server_address, 123, &conn);
		write(conn, &req, sizeof(ntp_message));
		read(conn, &req, sizeof(ntp_message));
		gettimeofday(&recv_time, NULL);
		/* if(verbose) print_packet(&req); */
		close(conn);
		next_offset=calc_offset(&req, &recv_time);
		if(verbose) printf("offset: %g\n", next_offset);
		avg_offset+=next_offset;
	}
	avg_offset/=AVG_NUM;
	if(verbose) printf("average offset: %g\n", avg_offset);
	return avg_offset;
}


/* this should behave more like ntpdate, but needs optomisations... */
double offset_request_ntpdate(const char *host){
	int i=0, j=0, ga_result=0, num_hosts=0, *socklist=NULL;
	ntp_message req;
	double offset=0., avg_offset=0.;
	struct timeval recv_time;
	struct addrinfo *ai=NULL, *ai_tmp=NULL, hints;

	/* setup hints to only return results from getaddrinfo that we'd like */
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = address_family;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_socktype = SOCK_DGRAM;

	/* XXX better error handling here... */
	ga_result = getaddrinfo(host, "123", &hints, &ai);
	if(ga_result!=0){
		fprintf(stderr, "error getting address for %s: %s\n",
				host, gai_strerror(ga_result));
		return -1.0;
	}

	/* count te number of returned hosts, and allocate an array of sockets */
	ai_tmp=ai;
	while(ai_tmp){
		ai_tmp = ai_tmp->ai_next;
		num_hosts++;
	}
	socklist=(int*)malloc(sizeof(int)*num_hosts);
	if(socklist==NULL) die(STATE_UNKNOWN, "can not allocate socket array");

	/* setup each socket for writing */
	ai_tmp=ai;
	for(i=0;ai_tmp;i++){
		socklist[i]=socket(ai_tmp->ai_family, SOCK_DGRAM, IPPROTO_UDP);
		if(socklist[i] == -1) {
			perror(NULL);
			die(STATE_UNKNOWN, "can not create new socket");
		}
		if(connect(socklist[i], ai_tmp->ai_addr, ai_tmp->ai_addrlen)){
			die(STATE_UNKNOWN, "can't create socket connection");
		}
		ai_tmp = ai_tmp->ai_next;
	}

	/* now do AVG_NUM checks to each host. this needs to be optimized
	 * two ways:
	 *  - use some parellization w/poll for much faster results.  currently
	 *    we do send/recv, send/recv, etc, whereas we could use poll(), to
	 *    determine when to read and just do a bunch of writing when we
	 *    have free time.
	 *  - behave like ntpdate and only take the 5 best responses.
	 */
	for(i=0; i<AVG_NUM; i++){
		if(verbose) printf("offset calculation run %d/%d\n", i+1, AVG_NUM);
		for(j=0; j<num_hosts; j++){
			if(verbose) printf("peer %d: ", j);
			setup_request(&req);
			write(socklist[j], &req, sizeof(ntp_message));
			read(socklist[j], &req, sizeof(ntp_message));
			gettimeofday(&recv_time, NULL);
			offset=calc_offset(&req, &recv_time);
			if(verbose) printf("offset: %g\n", offset);
			avg_offset+=offset;
		}
		avg_offset/=num_hosts;
	}
	avg_offset/=AVG_NUM;
	if(verbose) printf("overall average offset: %g\n", avg_offset);

	for(j=0; j<num_hosts; j++){ close(socklist[j]); }
	freeaddrinfo(ai);
	return avg_offset;
}

void
setup_control_request(ntp_control_message *p, uint8_t opcode, uint16_t seq){
	memset(p, 0, sizeof(ntp_control_message));
	LI_SET(p->flags, LI_NOWARNING);
	VN_SET(p->flags, VN_RESERVED);
	MODE_SET(p->flags, MODE_CONTROLMSG);
	OP_SET(p->op, opcode);
	p->seq = htons(seq);
	/* Remaining fields are zero for requests */
}

/* XXX handle responses with the error bit set */
double jitter_request(const char *host){
	int conn=-1, i, npeers=0, num_candidates=0, syncsource_found=0;
	int run=0, min_peer_sel=PEER_INCLUDED, num_selected=0, num_valid=0;
	ntp_assoc_status_pair *peers;
	ntp_control_message req;
	double rval = 0.0, jitter = -1.0;
	char *startofvalue=NULL, *nptr=NULL;

	/* Long-winded explanation:
	 * Getting the jitter requires a number of steps:
	 * 1) Send a READSTAT request.
	 * 2) Interpret the READSTAT reply
	 *  a) The data section contains a list of peer identifiers (16 bits)
	 *     and associated status words (16 bits)
	 *  b) We want the value of 0x06 in the SEL (peer selection) value,
	 *     which means "current synchronizatin source".  If that's missing,
	 *     we take anything better than 0x04 (see the rfc for details) but
	 *     set a minimum of warning.
	 * 3) Send a READVAR request for information on each peer identified
	 *    in 2b greater than the minimum selection value.
	 * 4) Extract the jitter value from the data[] (it's ASCII)
	 */
	my_udp_connect(server_address, 123, &conn);
	setup_control_request(&req, OP_READSTAT, 1);

	DBG(printf("sending READSTAT request"));
	write(conn, &req, SIZEOF_NTPCM(req));
	DBG(print_ntp_control_message(&req));
	/* Attempt to read the largest size packet possible
	 * Is it possible for an NTP server to have more than 117 synchronization
	 * sources?  If so, we will receive a second datagram with additional
	 * peers listed, since 117 is the maximum number that can fit in a
	 * single NTP control datagram.  This code doesn't handle that case */
	/* XXX check the REM_MORE bit */
	req.count=htons(MAX_CM_SIZE);
	DBG(printf("recieving READSTAT response"))
	read(conn, &req, SIZEOF_NTPCM(req));
	DBG(print_ntp_control_message(&req));
	/* Each peer identifier is 4 bytes in the data section, which
	 * we represent as a ntp_assoc_status_pair datatype.
	 */
	npeers=ntohs(req.count)/sizeof(ntp_assoc_status_pair);
	peers=(ntp_assoc_status_pair*)malloc(sizeof(ntp_assoc_status_pair)*npeers);
	memcpy((void*)peers, (void*)req.data, sizeof(ntp_assoc_status_pair)*npeers);
	/* first, let's find out if we have a sync source, or if there are
	 * at least some candidates.  in the case of the latter we'll issue
	 * a warning but go ahead with the check on them. */
	for (i = 0; i < npeers; i++){
		if (PEER_SEL(peers[i].status) >= PEER_INCLUDED){
			num_candidates++;
			if(PEER_SEL(peers[i].status) >= PEER_SYNCSOURCE){
				syncsource_found=1;
				min_peer_sel=PEER_SYNCSOURCE;
			}
		}
	}
	if(verbose) printf("%d candiate peers available\n", num_candidates);
	if(verbose && syncsource_found) printf("synchronization source found\n");
	/* XXX if ! syncsource_found set status to warning */

	for (run=0; run<AVG_NUM; run++){
		if(verbose) printf("jitter run %d of %d\n", run+1, AVG_NUM);
		for (i = 0; i < npeers; i++){
			/* Only query this server if it is the current sync source */
			if (PEER_SEL(peers[i].status) >= min_peer_sel){
				setup_control_request(&req, OP_READVAR, 2);
				req.assoc = peers[i].assoc;
				/* By spec, putting the variable name "jitter"  in the request
				 * should cause the server to provide _only_ the jitter value.
				 * thus reducing net traffic, guaranteeing us only a single
				 * datagram in reply, and making intepretation much simpler
				 */
				strncpy(req.data, "jitter", 6);
				req.count = htons(6);
				DBG(printf("sending READVAR request...\n"));
				write(conn, &req, SIZEOF_NTPCM(req));
				DBG(print_ntp_control_message(&req));

				req.count = htons(MAX_CM_SIZE);
				DBG(printf("recieving READVAR response...\n"));
				read(conn, &req, SIZEOF_NTPCM(req));
				DBG(print_ntp_control_message(&req));

				/* get to the float value */
				if(verbose) {
					printf("parsing jitter from peer %.2x: ", peers[i].assoc);
				}
				startofvalue = strchr(req.data, '=') + 1;
				jitter = strtod(startofvalue, &nptr);
				num_selected++;
				if(jitter == 0 && startofvalue==nptr){
					printf("warning: unable to parse server response.\n");
					/* XXX errors value ... */
				} else {
					if(verbose) printf("%g\n", jitter);
					num_valid++;
					rval += jitter;
				}
			}
		}
		if(verbose){
			printf("jitter parsed from %d/%d peers\n", num_selected, num_valid);
		}
	}

	rval /= num_valid;

	close(conn);
	free(peers);
	/* If we return -1.0, it means no synchronization source was found */
	return rval;
}

int process_arguments(int argc, char **argv){
	int c;
	int option=0;
	static struct option longopts[] = {
		{"version", no_argument, 0, 'V'},
		{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'},
		{"use-ipv4", no_argument, 0, '4'},
		{"use-ipv6", no_argument, 0, '6'},
		{"warning", required_argument, 0, 'w'},
		{"critical", required_argument, 0, 'c'},
		{"zero-offset", no_argument, 0, 'O'},
		{"jwarn", required_argument, 0, 'j'},
		{"jcrit", required_argument, 0, 'k'},
		{"timeout", required_argument, 0, 't'},
		{"hostname", required_argument, 0, 'H'},
		{0, 0, 0, 0}
	};

	
	if (argc < 2)
		usage ("\n");

	while (1) {
		c = getopt_long (argc, argv, "Vhv46w:c:Oj:k:t:H:", longopts, &option);
		if (c == -1 || c == EOF || c == 1)
			break;

		switch (c) {
		case 'h':
			print_help();
			exit(STATE_OK);
			break;
		case 'V':
			print_revision(progname, revision);
			exit(STATE_OK);
			break;
		case 'v':
			verbose++;
			break;
		case 'w':
			owarn = atof(optarg);
			break;
		case 'c':
			ocrit = atof(optarg);
			break;
		case 'j':
			do_jitter=1;
			jwarn = atof(optarg);
			break;
		case 'k':
			do_jitter=1;
			jcrit = atof(optarg);
			break;
		case 'H':
			if(is_host(optarg) == FALSE)
				usage2(_("Invalid hostname/address"), optarg);
			server_address = strdup(optarg);
			break;
		case 't':
			socket_timeout=atoi(optarg);
			break;
		case 'O':
			zero_offset_bad=1;
			break;
		case '4':
			address_family = AF_INET;
			break;
		case '6':
#ifdef USE_IPV6
			address_family = AF_INET6;
#else
			usage4 (_("IPv6 support not available"));
#endif
			break;
		case '?':
			/* print short usage statement if args not parsable */
			usage2 (_("Unknown argument"), optarg);
			break;
		}
	}

	if (ocrit < owarn){
		usage4(_("Critical offset should be larger than warning offset"));
	}

	if (ocrit < owarn){
		usage4(_("Critical jitter should be larger than warning jitter"));
	}

	if(server_address == NULL){
		usage4(_("Hostname was not supplied"));
	}

	return 0;
}

int main(int argc, char *argv[]){
	int result = STATE_UNKNOWN;
	double offset=0, jitter=0;

	if (process_arguments (argc, argv) == ERROR)
		usage4 (_("Could not parse arguments"));

	/* initialize alarm signal handling */
	signal (SIGALRM, socket_timeout_alarm_handler);

	/* set socket timeout */
	alarm (socket_timeout);

	offset = offset_request(server_address);
	if(offset > ocrit){
		result = STATE_CRITICAL;
	} else if(offset > owarn) {
		result = STATE_WARNING;
	} else {
		result = STATE_OK;
	}

	/* If not told to check the jitter, we don't even send packets.
	 * jitter is checked using NTP control packets, which not all
	 * servers recognize.  Trying to check the jitter on OpenNTPD
	 * (for example) will result in an error
	 */
	if(do_jitter){
		jitter=jitter_request(server_address);
		if(jitter > jcrit){
			result = max_state(result, STATE_CRITICAL);
		} else if(jitter > jwarn) {
			result = max_state(result, STATE_WARNING);
		} else if(jitter == -1.0 && result == STATE_OK){
			/* -1 indicates that we couldn't calculate the jitter
			 * Only overrides STATE_OK from the offset */
			result = STATE_UNKNOWN;
		}
	}

	switch (result) {
		case STATE_CRITICAL :
			printf("NTP CRITICAL: ");
			break;
		case STATE_WARNING :
			printf("NTP WARNING: ");
			break;
		case STATE_OK :
			printf("NTP OK: ");
			break;
		default :
			printf("NTP UNKNOWN: ");
			break;
	}

	printf("Offset %g secs|offset=%g", offset, offset);
	if (do_jitter) printf("|jitter=%f", jitter);
	printf("\n");

	if(server_address!=NULL) free(server_address);
	return result;
}


void print_usage(void){
	printf("\
Usage: %s -H <host> [-O] [-w <warn>] [-c <crit>] [-j <warn>] [-k <crit>] [-v verbose]\
\n", progname);
}

void print_help(void){
	print_revision(progname, revision);

	printf ("Copyright (c) 1999 Ethan Galstad\n");
	printf (COPYRIGHT, copyright, email);

	print_usage();
	printf (_(UT_HELP_VRSN));
	printf (_(UT_HOST_PORT), 'p', "123");
	printf (_(UT_WARN_CRIT));
	printf (_(UT_TIMEOUT), DEFAULT_SOCKET_TIMEOUT);
	printf (_(UT_VERBOSE));
	printf(_(UT_SUPPORT));
}
