/*
 *
 * (C) 2005-10 - Luca Deri <deri@ntop.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * VLAN support courtesy of Vincent Magnin <vincent.magnin@ci.unil.ch>
 *
 */
//aaa
#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/poll.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <net/ethernet.h>     /* the L2 protocols */
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "pfring.h"

#define ALARM_SLEEP             1
#define DEFAULT_SNAPLEN       128
#define MAX_NUM_THREADS        64

int verbose = 0, num_channels = 1;
pfring_stat pfringStats;

static struct timeval startTime;
pfring  *ring[MAX_NUM_THREADS] = { NULL };
unsigned long long numPkts[MAX_NUM_THREADS] = { 0 }, numBytes[MAX_NUM_THREADS] = { 0 };
unsigned long long numPkts_IP[MAX_NUM_THREADS] = { 0 }, numBytes_IP[MAX_NUM_THREADS] = { 0 };
u_int8_t wait_for_packet = 1,  do_shutdown = 0;
pthread_t pd_thread[MAX_NUM_THREADS];

#define DEFAULT_DEVICE     "eth0"

/**************************************
 * tommy libs
 */
#include "../tommyds-1.0/tommyhashdyn.h"
#include "../tommyds-1.0/tommylist.h"
#include "assert.h"
typedef tommy_hashdyn_node map_sIPdIP_counters_node;
typedef tommy_hashdyn map_ports_connection_status;
tommy_hashdyn map[MAX_NUM_THREADS];
tommy_list counter_list[MAX_NUM_THREADS];
tommy_list all_connection_status_list[MAX_NUM_THREADS];
typedef enum{SYN,SYNACK,ACK} SYNC_STATE;
struct counters{
	unsigned long long tcp_counter,udp_counter,icmp_counter,others_counter;
	unsigned long long tcp_bytes,udp_bytes,icmp_bytes,others_bytes;
};
struct nodo{
	tommy_node node; // map's interface
	tommy_node list_node; // list_interface
	struct counters counters;
	map_ports_connection_status ports_conections_status; // TODO a little bit expensive. In the end,
	                                                     // we count ports.
	u_int8_t rx_direction; /* 1=RX: packet received by the NIC, 0=TX: packet transmitted by the NIC */
	struct nodo * reverse_node; // node with reverse sIP,dIP tuple.
};
struct sync_status_node{
	tommy_node node; // map's interface
	tommy_node list_node;
	SYNC_STATE state;
};
struct memory_block{
	void * mem;
	size_t count,size;
};
struct memory_block_list{
	struct memory_block memory_block;
	struct memory_block_list * next;
};
struct memory_block_list * counters_pool[MAX_NUM_THREADS];
struct memory_block_list * syncs_states_pool[MAX_NUM_THREADS];

#define INITIAL_RECORDS_PER_THREAD 1024
#define INITIAL_PORTS_POOL_SIZE 1

inline tommy_hashdyn_node* find_value(tommy_hashdyn * const hashdyn,const tommy_uint64_t hash){
	tommy_hashdyn_node * i = tommy_hashdyn_bucket(hashdyn, hash);
	while(i){
		// we first check if the hash matches, as in the same bucket we may have multiples hash values
		if (i->key == hash)
			break;
		i=i->next;
	}
	return i;
}

inline void grow_memory_block_list(struct memory_block_list ** list,const size_t element_size){
	struct memory_block_list * memory_block_list_node = malloc(sizeof(struct memory_block_list));
	size_t new_size = (*list)->memory_block.size*2;
	// printf("Array %p is full. Creating a new array of size %lu",*list,new_size);
	// puts(  "//////////////////////////////////////////////////");
	int i;
	for(i=0;i<num_channels;++i){
		if(*list==counters_pool[i])
			printf("Counter pool threadId=%d growing",i);
		else if(*list==syncs_states_pool[i])
			printf("connections status threadId=%d growing",i);
	}
	memory_block_list_node->memory_block.mem = malloc(new_size*element_size);
	memory_block_list_node->memory_block.size = new_size;
	memory_block_list_node->memory_block.count = 0;

	memory_block_list_node->next = *list;
	*list = memory_block_list_node;
}

/* *************************************** */
/*
 * The time difference in millisecond
 */
double delta_time (struct timeval * now,
		   struct timeval * before) {
  time_t delta_seconds;
  time_t delta_microseconds;

  /*
   * compute delta in second, 1/10's and 1/1000's second units
   */
  delta_seconds      = now -> tv_sec  - before -> tv_sec;
  delta_microseconds = now -> tv_usec - before -> tv_usec;

  if(delta_microseconds < 0) {
    /* manually carry a one from the seconds field */
    delta_microseconds += 1000000;  /* 1e6 */
    -- delta_seconds;
  }
  return((double)(delta_seconds * 1000) + (double)delta_microseconds/1000);
}

/* ******************************** */

void print_stats() {
  pfring_stat pfringStat;
  struct timeval endTime;
  double deltaMillisec;
  static u_int64_t lastPkts[MAX_NUM_THREADS] = { 0 };
  u_int64_t diff;
  static struct timeval lastTime;
  int i;
  unsigned long long nBytes = 0, nPkts = 0, pkt_dropped = 0;
  unsigned long long nPktsLast = 0;
	unsigned long long incomingPkts=0,outgoingPkts=0;
	unsigned long long owcPkts=0;
  double pkt_thpt = 0, delta;

	struct counters counters; memset(&counters,0,sizeof(struct counters));
	int nPkts_IP=0,nBytes_IP=0;

  if(startTime.tv_sec == 0) {
    gettimeofday(&startTime, NULL);
    return;
  }

  gettimeofday(&endTime, NULL);
  deltaMillisec = delta_time(&endTime, &startTime);

  delta = delta_time(&endTime, &lastTime);

  for(i=0; i < num_channels; i++) {
    nBytes += numBytes[i], nPkts += numPkts[i];
		nBytes_IP += numBytes_IP[i], nPkts_IP += numPkts_IP[i];

		tommy_node * iterator = tommy_list_head(&counter_list[i]);
		while(iterator){
			struct nodo * nodo = (struct nodo *)iterator->data;
			counters.tcp_counter    += nodo->counters.tcp_counter;
			counters.tcp_bytes      += nodo->counters.tcp_bytes;
			counters.udp_counter    += nodo->counters.udp_counter;
			counters.udp_bytes      += nodo->counters.udp_bytes;
			counters.icmp_counter   += nodo->counters.icmp_counter;
			counters.icmp_bytes     += nodo->counters.icmp_bytes;
			counters.others_counter += nodo->counters.others_counter;
			counters.others_bytes   += nodo->counters.others_bytes;
			(*(nodo->rx_direction?&incomingPkts:&outgoingPkts))
				+=nodo->counters.icmp_counter+nodo->counters.udp_counter+nodo->counters.tcp_counter
				+nodo->counters.others_counter;
			iterator=iterator->next;
		}
		
		iterator = tommy_list_head(&all_connection_status_list[i]);
		while(iterator){
			struct sync_status_node * node = (struct sync_status_node *) iterator->data;
			switch(node->state){
				case SYN:
				case SYNACK:
					owcPkts++;
				default:
					break;
			};
			iterator=iterator->next;
		}
  
    if(pfring_stats(ring[i], &pfringStat) >= 0) {
      double thpt = ((double)8*numBytes[i])/(deltaMillisec*1000);

      fprintf(stderr, "=========================\n"
	      "Absolute Stats: [channel=%d][%u pkts rcvd][%u pkts dropped]\n"
	      "Total Pkts=%u/Dropped=%.1f %%\n",
	      i, (unsigned int)numPkts[i], (unsigned int)pfringStat.drop,
	      (unsigned int)(numPkts[i]+pfringStat.drop),
	      numPkts[i] == 0 ? 0 : (double)(pfringStat.drop*100)/(double)(numPkts[i]+pfringStat.drop));
      fprintf(stderr, "%llu pkts - %llu bytes", numPkts[i], numBytes[i]);
      fprintf(stderr, " [%.1f pkt/sec - %.2f Mbit/sec]\n", (double)(numPkts[i]*1000)/deltaMillisec, thpt);
      pkt_dropped += pfringStat.drop;

      if(lastTime.tv_sec > 0) {
	double pps;
	
	diff = numPkts[i]-lastPkts[i];
	nPktsLast += diff;
	pps = ((double)diff/(double)(delta/1000));
	fprintf(stderr, "=========================\n"
		"Actual Stats: [channel=%d][%llu pkts][%.1f ms][%.1f pkt/sec]\n",
		i, (long long unsigned int)diff, delta, pps);
	pkt_thpt += pps;
      }
      lastPkts[i] = numPkts[i];

      fprintf(stderr,"Num packets: [TCP] %llu \t [UDP] %llu \t [ICMP] %llu \t [others] %llu [total] %llu\n",
				counters.tcp_counter, counters.udp_counter, counters.icmp_counter, counters.others_counter,
				counters.tcp_counter+ counters.udp_counter+ counters.icmp_counter+ counters.others_counter);
			fprintf(stderr,"Num bytes: [TCP] %llu \t [UDP] %llu \t [ICMP] %llu \t [others] %llu [total] %llu \n",
				counters.tcp_bytes, counters.udp_bytes, counters.icmp_bytes, counters.others_bytes,
				counters.tcp_bytes+ counters.udp_bytes+ counters.icmp_bytes+ counters.others_bytes);			
			double nPkts_dbl = nPkts_IP;
			if(nPkts>0) fprintf(stderr,"Ratio: [TCP] %f \t [UDP] %f \t [ICMP] %f \t others %f \n",
				counters.tcp_counter/nPkts_dbl, counters.udp_counter/nPkts_dbl, counters.icmp_counter/nPkts_dbl, 
				counters.others_counter/nPkts_dbl);
			
			fprintf(stderr,"non-IP packets: %llu\n",nPkts-nPkts_IP);
			fprintf(stderr,"Incoming: %llu \tOutgoing : %llu \tratio: %f\n",incomingPkts,
					outgoingPkts,((double)incomingPkts)/outgoingPkts);
			fprintf(stderr,"OWCR: [TCP] %f\n",owcPkts/nPkts_dbl);
    }
  }

  lastTime.tv_sec = endTime.tv_sec, lastTime.tv_usec = endTime.tv_usec;

  fprintf(stderr, "=========================\n");
  fprintf(stderr, "Aggregate stats (all channels): [%.1f pkt/sec][%llu pkts dropped]\n", 
	  (double)(nPktsLast*1000)/(double)delta, pkt_dropped);
  fprintf(stderr, "=========================\n\n");
	
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;
  int i;

  fprintf(stderr, "Leaving...\n");
  if(called) return; else called = 1;
  do_shutdown = 1;
  print_stats();

  for(i=0; i<num_channels; i++)
    pfring_shutdown(ring[i]);

  for(i=0; i<num_channels; i++) {
    pthread_join(pd_thread[i], NULL);
    pfring_close(ring[i]);
  }

  exit(0);
}

/* ******************************** */

void my_sigalarm(int sig) {
  if (do_shutdown)
    return;
  print_stats();
  alarm(ALARM_SLEEP);
  signal(SIGALRM, my_sigalarm);
}

/* *************************************** */

void printHelp(void) {
  printf("pfcount_multichannel\n(C) 2005-12 Deri Luca <deri@ntop.org>\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name (No device@channel), and dnaX for DNA\n");

  printf("-e <direction>  0=RX+TX, 1=RX only, 2=TX only\n");
  printf("-l <len>        Capture length\n");
  printf("-w <watermark>  Watermark\n");
  printf("-p <poll wait>  Poll wait (msec)\n");
  printf("-b <cpu %%>      CPU pergentage priority (0-99)\n");
  printf("-a              Active packet wait\n");
  printf("-r              Rehash RSS packets\n");
  printf("-v              Verbose\n");
}

/* ****************************************************** */

static char hex[] = "0123456789ABCDEF";

char* etheraddr_string(const u_char *ep, char *buf) {
  u_int i, j;
  char *cp;

  cp = buf;
  if ((j = *ep >> 4) != 0)
    *cp++ = hex[j];
  else
    *cp++ = '0';

  *cp++ = hex[*ep++ & 0xf];

  for(i = 5; (int)--i >= 0;) {
    *cp++ = ':';
    if ((j = *ep >> 4) != 0)
      *cp++ = hex[j];
    else
      *cp++ = '0';

    *cp++ = hex[*ep++ & 0xf];
  }

  *cp = '\0';
  return (buf);
}

/* ****************************************************** */

/*
 * A faster replacement for inet_ntoa().
 */
char* _intoa(unsigned int addr, char* buf, u_short bufLen) {
  char *cp, *retStr;
  u_int byte;
  int n;

  cp = &buf[bufLen];
  *--cp = '\0';

  n = 4;
  do {
    byte = addr & 0xff;
    *--cp = byte % 10 + '0';
    byte /= 10;
    if (byte > 0) {
      *--cp = byte % 10 + '0';
      byte /= 10;
      if (byte > 0)
	*--cp = byte + '0';
    }
    *--cp = '.';
    addr >>= 8;
  } while (--n > 0);

  /* Convert the string to lowercase */
  retStr = (char*)(cp+1);

  return(retStr);
}

/* ************************************ */

char* intoa(unsigned int addr) {
  static char buf[sizeof "ff:ff:ff:ff:ff:ff:255.255.255.255"];

  return(_intoa(addr, buf, sizeof(buf)));
}

/* ************************************ */

inline char* in6toa(struct in6_addr addr6) {
  static char buf[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"];

  snprintf(buf, sizeof(buf),
	   "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
	   addr6.s6_addr[0], addr6.s6_addr[1], addr6.s6_addr[2],
	   addr6.s6_addr[3], addr6.s6_addr[4], addr6.s6_addr[5], addr6.s6_addr[6],
	   addr6.s6_addr[7], addr6.s6_addr[8], addr6.s6_addr[9], addr6.s6_addr[10],
	   addr6.s6_addr[11], addr6.s6_addr[12], addr6.s6_addr[13], addr6.s6_addr[14],
	   addr6.s6_addr[15]);

  return(buf);
}

/* ****************************************************** */

char* proto2str(u_short proto) {
  static char protoName[8];

  switch(proto) {
  case IPPROTO_TCP:  return("TCP");
  case IPPROTO_UDP:  return("UDP");
  case IPPROTO_ICMP: return("ICMP");
  default:
    snprintf(protoName, sizeof(protoName), "%d", proto);
    return(protoName);
  }
}

/* ****************************************************** */

static int32_t thiszone;

void dummyProcesssPacket(const struct pfring_pkthdr *h, const u_char *p, const u_char *user_bytes) {
  long threadId = (long)user_bytes;
	struct ether_header ehdr;
   u_short eth_type;
	 struct ip ip;

	 memcpy(&ehdr, p+h->extended_hdr.parsed_header_len, sizeof(struct ether_header));
    eth_type = ntohs(ehdr.ether_type);

  if(verbose) {
    u_short vlan_id;
    char buf1[32], buf2[32];
    int s;
    uint nsec;

    if(h->ts.tv_sec == 0)
      gettimeofday((struct timeval*)&h->ts, NULL);

    s = (h->ts.tv_sec + thiszone) % 86400;
    nsec = h->extended_hdr.timestamp_ns % 1000;
    
    printf("%02d:%02d:%02d.%06u%03u ",
	   s / 3600, (s % 3600) / 60, s % 60,
	   (unsigned)h->ts.tv_usec, nsec);

#if 0
    for(i=0; i<32; i++) printf("%02X ", p[i]);
    printf("\n");
#endif

    if(h->extended_hdr.parsed_header_len > 0) {
      printf("[eth_type=0x%04X]", h->extended_hdr.parsed_pkt.eth_type);
      printf("[l3_proto=%u]", (unsigned int)h->extended_hdr.parsed_pkt.l3_proto);

      printf("[%s:%d -> ", (h->extended_hdr.parsed_pkt.eth_type == 0x86DD) ?
	     in6toa(h->extended_hdr.parsed_pkt.ipv6_src) : intoa(h->extended_hdr.parsed_pkt.ipv4_src),
	     h->extended_hdr.parsed_pkt.l4_src_port);
      printf("%s:%d] ", (h->extended_hdr.parsed_pkt.eth_type == 0x86DD) ?
	     in6toa(h->extended_hdr.parsed_pkt.ipv6_dst) : intoa(h->extended_hdr.parsed_pkt.ipv4_dst),
	     h->extended_hdr.parsed_pkt.l4_dst_port);

      printf("[%s -> %s] ",
	     etheraddr_string(h->extended_hdr.parsed_pkt.smac, buf1),
	     etheraddr_string(h->extended_hdr.parsed_pkt.dmac, buf2));
    }

    printf("[%s -> %s][eth_type=0x%04X] ",
	   etheraddr_string(ehdr.ether_shost, buf1),
	   etheraddr_string(ehdr.ether_dhost, buf2), eth_type);


    if(eth_type == 0x8100) {
      vlan_id = (p[14] & 15)*256 + p[15];
      eth_type = (p[16])*256 + p[17];
      printf("[vlan %u] ", vlan_id);
      p+=4;
    }

    if(eth_type == 0x0800) {
      memcpy(&ip, p+h->extended_hdr.parsed_header_len+sizeof(ehdr), sizeof(struct ip));
      printf("[%s]", proto2str(ip.ip_p));
      printf("[%s:%d ", intoa(ntohl(ip.ip_src.s_addr)), h->extended_hdr.parsed_pkt.l4_src_port);
      printf("-> %s:%d] ", intoa(ntohl(ip.ip_dst.s_addr)), h->extended_hdr.parsed_pkt.l4_dst_port);

      printf("[tos=%d][tcp_seq_num=%u][caplen=%d][len=%d][parsed_header_len=%d]"
	     "[eth_offset=%d][l3_offset=%d][l4_offset=%d][payload_offset=%d]\n",
	     h->extended_hdr.parsed_pkt.ipv4_tos, h->extended_hdr.parsed_pkt.tcp.seq_num,
	     h->caplen, h->len, h->extended_hdr.parsed_header_len,
	     h->extended_hdr.parsed_pkt.offset.eth_offset,
	     h->extended_hdr.parsed_pkt.offset.l3_offset,
	     h->extended_hdr.parsed_pkt.offset.l4_offset,
	     h->extended_hdr.parsed_pkt.offset.payload_offset);

    } else {
      if(eth_type == 0x0806)
	printf("[ARP]");
      else
	printf("[eth_type=0x%04X]", eth_type);

      printf("[caplen=%d][len=%d][parsed_header_len=%d]"
	     "[eth_offset=%d][l3_offset=%d][l4_offset=%d][payload_offset=%d]\n",
	     h->caplen, h->len, h->extended_hdr.parsed_header_len,
	     h->extended_hdr.parsed_pkt.offset.eth_offset,
	     h->extended_hdr.parsed_pkt.offset.l3_offset,
	     h->extended_hdr.parsed_pkt.offset.l4_offset,
	     h->extended_hdr.parsed_pkt.offset.payload_offset);
    }
  }

  numPkts[threadId]++, numBytes[threadId] += h->len;

	
	if(eth_type == 0x0800) { /* IP */
		numPkts_IP[threadId]++, numBytes_IP[threadId] += h->len;
		memcpy(&ip, p+h->extended_hdr.parsed_header_len+sizeof(ehdr), sizeof(struct ip));
		const uint64_t pre_hash = ((uint64_t)ntohl(ip.ip_src.s_addr)<<32)+ntohl(ip.ip_dst.s_addr);
		const uint8_t proto = ip.ip_p;

// 		printf("[%x]", ip.ip_p);
// 		printf("[%x ", ntohl(ip.ip_src.s_addr));
// 		printf("-> %x] ", ntohl(ip.ip_dst.s_addr));
// 		printf("Hash: %lx\n",hash);
//
// 		printf("[%s]", proto2str(ip.ip_p));
// 		printf("[%s ", intoa(ntohl(ip.ip_src.s_addr)));
// 		printf("-> %s] ", intoa(ntohl(ip.ip_dst.s_addr)));
// 		printf("size of map: %u\n\n",tommy_hashtable_count(&map[threadId]));

		map_sIPdIP_counters_node * i = find_value(&map[threadId],tommy_inthash_u64(pre_hash));
		
// 		printf("Hash%sgrown",i!=NULL?" ":" not ");

		if(i==NULL){ // hash not found
			//printf("packet pool memsegment count/size: ");
			//printf("%lu/%lu\n",counters_pool[threadId]->memory_block.count,
			//                   counters_pool[threadId]->memory_block.size);
			if(counters_pool[threadId]->memory_block.count ==counters_pool[threadId]->memory_block.size )
				grow_memory_block_list(&counters_pool[threadId],sizeof(struct nodo));
			struct nodo * nodo 
				=&(((struct nodo *) counters_pool[threadId]->memory_block.mem)
					[counters_pool[threadId]->memory_block.count++]);
			memset(&nodo->counters,0,sizeof(struct counters));
			tommy_hashdyn_init(&nodo->ports_conections_status);
// 			printf("ehd len: %d\n",h->extended_hdr.parsed_header_len);
			nodo->rx_direction = h->extended_hdr.rx_direction;
			tommy_hashdyn_node * reverse_node_node 
				= find_value(&map[threadId],tommy_inthash_u64(pre_hash<<32|pre_hash>>32));
			if(reverse_node_node){
				nodo->reverse_node = reverse_node_node->data;
				nodo->reverse_node->reverse_node = nodo;
			}else{
				nodo->reverse_node=NULL;
			}
			tommy_hashdyn_insert(&map[threadId],&nodo->node,nodo,tommy_inthash_u64(pre_hash));
			tommy_list_insert_tail(&counter_list[threadId],&nodo->list_node,nodo);
			
			i=&nodo->node;
		}
		
		struct counters * act_counters = &((struct nodo *) i->data)->counters;
		switch(proto){
			case 0x06:
				act_counters->tcp_counter++;
				act_counters->tcp_bytes += h->len;
				break;
			case 0x11:
				act_counters->udp_counter++;
				act_counters->udp_bytes += h->len;
				break;
			case 0x01:
				act_counters->icmp_counter++;
				act_counters->icmp_bytes += h->len;
				break;
			default:
				act_counters->others_counter++;
				act_counters->others_bytes += h->len;
				break;
		}
		
		const u_int8_t interesting_flags = h->extended_hdr.parsed_pkt.tcp.flags&(0x10|0x02);
		if(proto==0x06 && interesting_flags){ // SYN or ACK flag activated
			const u_int16_t src_port = h->extended_hdr.parsed_pkt.l4_src_port,
			                dst_port = h->extended_hdr.parsed_pkt.l4_dst_port;
// 			puts("ACK or SYN");
// 			printf("Interesting flags: %x\n",interesting_flags);
			// if(interesting_flags==0x02)
			//	puts("SYN!!");
			// printf("src->dest: %d->%d, %x->%x\n",src_port,dst_port,src_port,dst_port);
			
			tommy_uint32_t porthash;
			map_ports_connection_status * connections_status_list;
			if(interesting_flags!=(0x10|0x02)){
				connections_status_list = &((struct nodo*)i->data)->ports_conections_status;
				porthash = tommy_inthash_u32((src_port<<16)|dst_port);
			}else{ // SYN+ACK, response to a SYN
				connections_status_list = &((struct nodo*)i->data)->reverse_node->ports_conections_status;
				porthash = tommy_inthash_u32((dst_port<<16)|src_port);
			}				
			
//  			printf("hash seed: %x; hash: %x\n",
//  						 interesting_flags!=(0x10|0x02)?((src_port<<16)+dst_port):((dst_port<<16)+src_port),
//  						 porthash);
			// printf("connections_status_list: %p\n",connections_status_list);
			tommy_hashdyn_node * map_ports_iterator = find_value(connections_status_list,porthash);
			struct sync_status_node * sync_status_node=map_ports_iterator?map_ports_iterator->data:NULL;
			switch(interesting_flags){
				case 0x02: // only SYN
					if(map_ports_iterator){
						sync_status_node=map_ports_iterator->data;
					}else{
						struct memory_block * memory_block = &syncs_states_pool[threadId]->memory_block;
// 						printf("count/size of syncs_states_pool:%lu/%lu",memory_block->count,
// 						                                                 memory_block->size);
						if(memory_block->count==memory_block->size){
							grow_memory_block_list(&syncs_states_pool[threadId],sizeof(struct sync_status_node));
							memory_block = &syncs_states_pool[threadId]->memory_block;
						}
						
						sync_status_node
							= &(((struct sync_status_node *)memory_block->mem)[memory_block->count++]);
						tommy_hashdyn_insert(connections_status_list,
																&sync_status_node->node,sync_status_node,porthash);
						tommy_list_insert_tail(&all_connection_status_list[threadId],
						                     &sync_status_node->list_node,sync_status_node);
					}
					
					sync_status_node->state=SYN;
					break;
					
				case 0x10|0x02: // SYN+ACK
					if(sync_status_node!=NULL && sync_status_node->state==SYN)
						sync_status_node->state=SYNACK;
					break;
					
				case 0x10:
					if(sync_status_node!=NULL && sync_status_node->state==SYNACK)
						sync_status_node->state=ACK;
					break;
					
				default:
					printf("This should not be executed... flags: %x\n",interesting_flags);
					exit(-1);
			};
		}
	}
}

/* *************************************** */

int32_t gmt2local(time_t t) {
  int dt, dir;
  struct tm *gmt, *loc;
  struct tm sgmt;

  if (t == 0)
    t = time(NULL);
  gmt = &sgmt;
  *gmt = *gmtime(&t);
  loc = localtime(&t);
  dt = (loc->tm_hour - gmt->tm_hour) * 60 * 60 +
    (loc->tm_min - gmt->tm_min) * 60;

  /*
   * If the year or julian day is different, we span 00:00 GMT
   * and must add or subtract a day. Check the year first to
   * avoid problems when the julian day wraps.
   */
  dir = loc->tm_year - gmt->tm_year;
  if (dir == 0)
    dir = loc->tm_yday - gmt->tm_yday;
  dt += dir * 24 * 60 * 60;

  return (dt);
}

/* *************************************** */

void* packet_consumer_thread(void* _id) {
  int s;
  long thread_id = (long)_id; 
  u_int numCPU = sysconf( _SC_NPROCESSORS_ONLN );
  u_long core_id = thread_id % numCPU;
 
  if(numCPU > 1) {
    /* Bind this thread to a specific core */
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if((s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset)) != 0)
      fprintf(stderr, "Error while binding thread %ld to core %ld: errno=%i\n", 
	     thread_id, core_id, s);
    else {
      printf("Set thread %lu on core %lu/%u\n", thread_id, core_id, numCPU);
    }
  }
  
  pfring_loop(ring[thread_id],dummyProcesssPacket,(u_char *)thread_id,wait_for_packet);

//   while(1) {
//     u_char *buffer = NULL;
//     struct pfring_pkthdr hdr;
// 
//     if(do_shutdown) break;
// 		
// 
//     if(pfring_recv(ring[thread_id], &buffer, 0, &hdr, wait_for_packet) > 0) {
//       if(do_shutdown) break;
//       dummyProcesssPacket(&hdr, buffer, (u_char*)thread_id);
//     } else {
//       if(wait_for_packet == 0) sched_yield();
//       //usleep(1);
//     }
//   }

  return(NULL);
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c;
  int snaplen = DEFAULT_SNAPLEN, rc, watermark = 0, rehash_rss = 0;
  packet_direction direction = rx_and_tx_direction;
  long i;
  u_int16_t cpu_percentage = 0, poll_duration = 0;
  u_int32_t version;

  startTime.tv_sec = 0;
  thiszone = gmt2local(0);

  while((c = getopt(argc,argv,"hi:l:vae:w:b:rp:" /* "f:" */)) != -1) {
    switch(c) {
    case 'h':
      printHelp();
      return(0);
      break;
    case 'a':
      wait_for_packet = 0;
      break;
    case 'e':
      switch(atoi(optarg)) {
      case rx_and_tx_direction:
      case rx_only_direction:
      case tx_only_direction:
	direction = atoi(optarg);
	break;
      }
      break;
    case 'l':
      snaplen = atoi(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'v':
      verbose = 1;
      break;
    case 'w':
      watermark = atoi(optarg);
      break;
    case 'b':
      cpu_percentage = atoi(optarg);
      break;
    case 'r':
      rehash_rss = 1;
      break;
    case 'p':
      poll_duration = atoi(optarg);
      break;
    }
  }

  if(verbose) watermark = 1;
  if(device == NULL) device = DEFAULT_DEVICE;

  printf("Capturing from %s\n", device);

  /* hardcode: promisc=1, to_ms=500 */
  num_channels = pfring_open_multichannel(device, snaplen, PF_RING_PROMISC|  PF_RING_LONG_HEADER,
										  ring);
  
  if(num_channels <= 0) {
    fprintf(stderr, "pfring_open_multichannel() returned %d [%s]\n", num_channels, strerror(errno));
    return(-1);
  }

  if (num_channels > MAX_NUM_THREADS) {
    printf("Too many channels (%d), using %d channels\n", num_channels, MAX_NUM_THREADS);
    num_channels = MAX_NUM_THREADS;
  } else 
    printf("Found %d channels\n", num_channels);

  pfring_version(ring[0], &version);  
  printf("Using PF_RING v.%d.%d.%d\n",
	 (version & 0xFFFF0000) >> 16,
	 (version & 0x0000FF00) >> 8,
	 version & 0x000000FF);
  
  for(i=0; i<num_channels; i++) {
    char buf[32];
    
    snprintf(buf, sizeof(buf), "pfcount_multichannel-thread %ld", i);
    pfring_set_application_name(ring[i], buf);

    if((rc = pfring_set_direction(ring[i], direction)) != 0)
	fprintf(stderr, "pfring_set_direction returned %d [direction=%d] (you can't capture TX with DNA)\n", rc, direction);
    
    if((rc = pfring_set_socket_mode(ring[i], recv_only_mode)) != 0)
	fprintf(stderr, "pfring_set_socket_mode returned [rc=%d]\n", rc);

    if(watermark > 0) {
      if((rc = pfring_set_poll_watermark(ring[i], watermark)) != 0)
	fprintf(stderr, "pfring_set_poll_watermark returned [rc=%d][watermark=%d]\n", rc, watermark);
    }
    
    if(rehash_rss)
      pfring_enable_rss_rehash(ring[i]);
    
    if(poll_duration > 0)
      pfring_set_poll_duration(ring[i], poll_duration);

    pfring_enable_ring(ring[i]);

		tommy_hashdyn_init(&map[i]);
		counters_pool[i] = malloc(sizeof(struct memory_block_list));
		counters_pool[i]->memory_block.count = 0;
		counters_pool[i]->memory_block.mem  = malloc(INITIAL_RECORDS_PER_THREAD*sizeof(struct nodo));
		counters_pool[i]->memory_block.size = INITIAL_RECORDS_PER_THREAD;
		syncs_states_pool[i] = malloc(sizeof(struct memory_block_list));
		syncs_states_pool[i]->memory_block.count = 0;
		syncs_states_pool[i]->memory_block.mem  = malloc(INITIAL_PORTS_POOL_SIZE*sizeof(struct sync_status_node));
		syncs_states_pool[i]->memory_block.size = INITIAL_PORTS_POOL_SIZE;
    pthread_create(&pd_thread[i], NULL, packet_consumer_thread, (void*)i);
  }

  if(cpu_percentage > 0) {
    if(cpu_percentage > 99) cpu_percentage = 99;
    pfring_config(cpu_percentage);
  }

  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT, sigproc);

  if(!verbose) {
    signal(SIGALRM, my_sigalarm);
    alarm(ALARM_SLEEP);
  }
  
  for(i=0; i<num_channels; i++)
    pthread_join(pd_thread[i], NULL);

  return(0);
}
