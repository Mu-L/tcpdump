/*
 * Copyright (c) 1988, 1989, 1990, 1991, 1993, 1994, 1995, 1996
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/* \summary: Internet Control Message Protocol (ICMP) printer */

#include <config.h>

#include "netdissect-stdinc.h"

#include <stdio.h>
#include <string.h>

#define ND_LONGJMP_FROM_TCHECK
#include "netdissect.h"
#include "addrtoname.h"
#include "extract.h"

#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "ipproto.h"
#include "mpls.h"

/*
 * Interface Control Message Protocol Definitions.
 * Per RFC 792, September 1981.
 */

/*
 * Structure of an icmp header.
 */
struct icmp {
	nd_uint8_t  icmp_type;		/* type of message, see below */
	nd_uint8_t  icmp_code;		/* type sub code */
	nd_uint16_t icmp_cksum;		/* ones complement cksum of struct */
	union {
		nd_uint8_t ih_pptr;	/* ICMP_PARAMPROB */
		nd_ipv4 ih_gwaddr;	/* ICMP_REDIRECT */
		struct ih_idseq {
			nd_uint16_t icd_id;
			nd_uint16_t icd_seq;
		} ih_idseq;
		struct ih_idseqx {	/* RFC8335 */
			nd_uint16_t icdx_id;
			nd_uint8_t icdx_seq;
			nd_uint8_t icdx_info;
		} ih_idseqx;
		nd_uint32_t ih_void;
	} icmp_hun;
#define	icmp_pptr	icmp_hun.ih_pptr
#define	icmp_gwaddr	icmp_hun.ih_gwaddr
#define	icmp_id		icmp_hun.ih_idseq.icd_id
#define	icmp_seq	icmp_hun.ih_idseq.icd_seq
#define	icmp_xseq	icmp_hun.ih_idseqx.icdx_seq
#define	icmp_xinfo	icmp_hun.ih_idseqx.icdx_info
#define	icmp_void	icmp_hun.ih_void
	union {
		struct id_ts {
			nd_uint32_t its_otime;
			nd_uint32_t its_rtime;
			nd_uint32_t its_ttime;
		} id_ts;
		struct id_ip  {
			struct ip idi_ip;
			/* options and then 64 bits of data */
		} id_ip;
		nd_uint32_t id_mask;
		nd_byte id_data[1];
	} icmp_dun;
#define	icmp_otime	icmp_dun.id_ts.its_otime
#define	icmp_rtime	icmp_dun.id_ts.its_rtime
#define	icmp_ttime	icmp_dun.id_ts.its_ttime
#define	icmp_ip		icmp_dun.id_ip.idi_ip
#define	icmp_mask	icmp_dun.id_mask
#define	icmp_data	icmp_dun.id_data
};

/*
 * Lower bounds on packet lengths for various types.
 * For the error advice packets must first insure that the
 * packet is large enough to contain the returned ip header.
 * Only then can we do the check to see if 64 bits of packet
 * data have been returned, since we need to check the returned
 * ip header length.
 */
#define	ICMP_MINLEN	8				/* abs minimum */
#define ICMP_EXTD_MINLEN (156 - sizeof (struct ip))     /* draft-bonica-internet-icmp-08 */
#define	ICMP_TSLEN	(8 + 3 * sizeof (uint32_t))	/* timestamp */
#define	ICMP_MASKLEN	12				/* address mask */
#define	ICMP_ADVLENMIN	(8 + sizeof (struct ip) + 8)	/* min */
#define	ICMP_ADVLEN(p)	(8 + (IP_HL(&(p)->icmp_ip) << 2) + 8)
	/* N.B.: must separately check that ip_hl >= 5 */

/*
 * Definition of type and code field values.
 */
#define	ICMP_ECHOREPLY		0		/* echo reply */
#define	ICMP_UNREACH		3		/* dest unreachable, codes: */
#define		ICMP_UNREACH_NET	0		/* bad net */
#define		ICMP_UNREACH_HOST	1		/* bad host */
#define		ICMP_UNREACH_PROTOCOL	2		/* bad protocol */
#define		ICMP_UNREACH_PORT	3		/* bad port */
#define		ICMP_UNREACH_NEEDFRAG	4		/* IP_DF caused drop */
#define		ICMP_UNREACH_SRCFAIL	5		/* src route failed */
#define		ICMP_UNREACH_NET_UNKNOWN 6		/* unknown net */
#define		ICMP_UNREACH_HOST_UNKNOWN 7		/* unknown host */
#define		ICMP_UNREACH_ISOLATED	8		/* src host isolated */
#define		ICMP_UNREACH_NET_PROHIB	9		/* prohibited access */
#define		ICMP_UNREACH_HOST_PROHIB 10		/* ditto */
#define		ICMP_UNREACH_TOSNET	11		/* bad tos for net */
#define		ICMP_UNREACH_TOSHOST	12		/* bad tos for host */
#define	ICMP_SOURCEQUENCH	4		/* packet lost, slow down */
#define	ICMP_REDIRECT		5		/* shorter route, codes: */
#define		ICMP_REDIRECT_NET	0		/* for network */
#define		ICMP_REDIRECT_HOST	1		/* for host */
#define		ICMP_REDIRECT_TOSNET	2		/* for tos and net */
#define		ICMP_REDIRECT_TOSHOST	3		/* for tos and host */
#define	ICMP_ECHO		8		/* echo service */
#define	ICMP_ROUTERADVERT	9		/* router advertisement */
#define	ICMP_ROUTERSOLICIT	10		/* router solicitation */
#define	ICMP_TIMXCEED		11		/* time exceeded, code: */
#define		ICMP_TIMXCEED_INTRANS	0		/* ttl==0 in transit */
#define		ICMP_TIMXCEED_REASS	1		/* ttl==0 in reass */
#define	ICMP_PARAMPROB		12		/* ip header bad */
#define		ICMP_PARAMPROB_OPTABSENT 1		/* req. opt. absent */
#define	ICMP_TSTAMP		13		/* timestamp request */
#define	ICMP_TSTAMPREPLY	14		/* timestamp reply */
#define	ICMP_IREQ		15		/* information request */
#define	ICMP_IREQREPLY		16		/* information reply */
#define	ICMP_MASKREQ		17		/* address mask request */
#define	ICMP_MASKREPLY		18		/* address mask reply */

#define	ICMP_EXTENDED_ECHO_REQUEST	42	/* extended echo request */
#define	ICMP_EXTENDED_ECHO_REPLY	43	/* extended echo reply */
#define		ICMP_ECHO_X_MALFORMED_QUERY	1	/* malformed query */
#define		ICMP_ECHO_X_NO_SUCH_INTERFACE	2	/* no such interface */
#define		ICMP_ECHO_X_NO_SUCH_TABLE_ENTRY	3	/* no such table entry */
#define		ICMP_ECHO_X_MULTIPLE_INTERFACES	4	/* multiple interfaces satisfy query */

#define ICMP_ERRTYPE(type) \
	((type) == ICMP_UNREACH || (type) == ICMP_SOURCEQUENCH || \
	(type) == ICMP_REDIRECT || (type) == ICMP_TIMXCEED || \
	(type) == ICMP_PARAMPROB)
#define	ICMP_MULTIPART_EXT_TYPE(type) \
	((type) == ICMP_UNREACH || \
         (type) == ICMP_TIMXCEED || \
         (type) == ICMP_PARAMPROB)
#define ICMP_EXTENDED_ECHO_TYPE(type) \
        ((type) == ICMP_EXTENDED_ECHO_REQUEST || \
	 (type) == ICMP_EXTENDED_ECHO_REPLY)
/* rfc1700 */
#ifndef ICMP_UNREACH_NET_UNKNOWN
#define ICMP_UNREACH_NET_UNKNOWN	6	/* destination net unknown */
#endif
#ifndef ICMP_UNREACH_HOST_UNKNOWN
#define ICMP_UNREACH_HOST_UNKNOWN	7	/* destination host unknown */
#endif
#ifndef ICMP_UNREACH_ISOLATED
#define ICMP_UNREACH_ISOLATED		8	/* source host isolated */
#endif
#ifndef ICMP_UNREACH_NET_PROHIB
#define ICMP_UNREACH_NET_PROHIB		9	/* admin prohibited net */
#endif
#ifndef ICMP_UNREACH_HOST_PROHIB
#define ICMP_UNREACH_HOST_PROHIB	10	/* admin prohibited host */
#endif
#ifndef ICMP_UNREACH_TOSNET
#define ICMP_UNREACH_TOSNET		11	/* tos prohibited net */
#endif
#ifndef ICMP_UNREACH_TOSHOST
#define ICMP_UNREACH_TOSHOST		12	/* tos prohibited host */
#endif

/* rfc1716 */
#ifndef ICMP_UNREACH_FILTER_PROHIB
#define ICMP_UNREACH_FILTER_PROHIB	13	/* admin prohibited filter */
#endif
#ifndef ICMP_UNREACH_HOST_PRECEDENCE
#define ICMP_UNREACH_HOST_PRECEDENCE	14	/* host precedence violation */
#endif
#ifndef ICMP_UNREACH_PRECEDENCE_CUTOFF
#define ICMP_UNREACH_PRECEDENCE_CUTOFF	15	/* precedence cutoff */
#endif

/* Most of the icmp types */
static const struct tok icmp2str[] = {
	{ ICMP_ECHOREPLY,		"echo reply" },
	{ ICMP_SOURCEQUENCH,		"source quench" },
	{ ICMP_ECHO,			"echo request" },
	{ ICMP_ROUTERSOLICIT,		"router solicitation" },
	{ ICMP_TSTAMP,			"time stamp request" },
	{ ICMP_TSTAMPREPLY,		"time stamp reply" },
	{ ICMP_IREQ,			"information request" },
	{ ICMP_IREQREPLY,		"information reply" },
	{ ICMP_MASKREQ,			"address mask request" },
	{ ICMP_EXTENDED_ECHO_REQUEST,   "extended echo request" },
	{ ICMP_EXTENDED_ECHO_REPLY,     "extended echo reply" },
	{ 0,				NULL }
};

static const struct tok icmp_extended_echo_reply_code_str[] = {
	{ 0,				"No error" },
	{ ICMP_ECHO_X_MALFORMED_QUERY,  "Malformed Query" },
	{ ICMP_ECHO_X_NO_SUCH_INTERFACE, "No Such Interface" },
	{ ICMP_ECHO_X_NO_SUCH_TABLE_ENTRY, "No Such Table Entry" },
	{ ICMP_ECHO_X_MULTIPLE_INTERFACES, "Multiple Interfaces Satisfy Query" },
	{ 0,				NULL }
};

static const struct tok icmp_extended_echo_reply_state_str[] = {
	{ 0,				"Reserved" },
	{ 1,				"Incomplete" },
	{ 2,				"Reachable" },
	{ 3,				"Stale" },
	{ 4,				"Delay" },
	{ 5,				"Probe" },
	{ 6,				"Failed" },
	{ 0,				NULL }
};

/* rfc1191 */
struct mtu_discovery {
	nd_uint16_t unused;
	nd_uint16_t nexthopmtu;
};

/* rfc1256 */
struct ih_rdiscovery {
	nd_uint8_t ird_addrnum;
	nd_uint8_t ird_addrsiz;
	nd_uint16_t ird_lifetime;
};

struct id_rdiscovery {
	nd_uint32_t ird_addr;
	nd_uint32_t ird_pref;
};

/*
 * RFC 4884 - Extended ICMP to Support Multi-Part Messages
 *
 * This is a general extension mechanism, based on the mechanism
 * in draft-bonica-icmp-mpls-02 ICMP Extensions for MultiProtocol
 * Label Switching.
 *
 * The Destination Unreachable, Time Exceeded
 * and Parameter Problem messages are slightly changed as per
 * the above RFC. A new Length field gets added to give
 * the caller an idea about the length of the piggybacked
 * IP packet before the extension header starts.
 *
 * The Length field represents length of the padded "original datagram"
 * field  measured in 32-bit words.
 *
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     Type      |     Code      |          Checksum             |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |     unused    |    Length     |          unused               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |      Internet Header + leading octets of original datagram    |
 * |                                                               |
 * |                           //                                  |
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

struct icmp_ext_t {
    nd_uint8_t  icmp_type;
    nd_uint8_t  icmp_code;
    nd_uint16_t icmp_checksum;
    nd_byte     icmp_reserved;
    nd_uint8_t  icmp_length;
    nd_byte     icmp_reserved2[2];
    nd_byte     icmp_ext_legacy_header[128]; /* extension header starts 128 bytes after ICMP header */
    nd_byte     icmp_ext_version_res[2];
    nd_uint16_t icmp_ext_checksum;
    nd_byte     icmp_ext_data[1];
};

/*
 * Extract version from the first octet of icmp_ext_version_res.
 */
#define ICMP_EXT_EXTRACT_VERSION(x) (((x)&0xf0)>>4)

/*
 * Current version.
 */
#define ICMP_EXT_VERSION 2

/*
 * Extension object class numbers.
 *
 * Class 1 dates back to draft-bonica-icmp-mpls-02.
 *
 * Class 2 was used for an "Extended Payload Object Class", which
 * contained bytes of the payload beyond the first 128 bytes, in
 * draft-bonica-icmp-mpls-02; it was reassigned to an "Interface
 * Information Object" in RFC 5837.
 *
 * Class 3 is defined by RFC8335.
 */

/* rfc4950  */
#define MPLS_STACK_ENTRY_OBJECT_CLASS            1
/* rfc5837 */
#define INTERFACE_INFORMATION_OBJECT_CLASS       2
/* rfc8335 */
#define INTERFACE_IDENTIFICATION_OBJECT_CLASS    3

struct icmp_multipart_ext_object_header_t {
    nd_uint16_t length;
    nd_uint8_t  class_num;
    nd_uint8_t  ctype;
};

static const struct tok icmp_multipart_ext_obj_values[] = {
    { MPLS_STACK_ENTRY_OBJECT_CLASS,         "MPLS Stack Entry Object" },
    { INTERFACE_INFORMATION_OBJECT_CLASS,    "Interface Information Object" },
    { INTERFACE_IDENTIFICATION_OBJECT_CLASS, "Interface Identification Object" },
    { 0, NULL}
};

/* rfc5837 */
static const struct tok icmp_interface_information_role_values[] = {
    { 0, "Incoming IP Interface"},
    { 1, "Sub-IP Component of Incoming IP Interface"},
    { 2, "Outgoing IP Interface"},
    { 3, "IP Next hop"},
    { 0, NULL }
};

/*
Interface IP Address Sub-Object
0                            31
+-------+-------+-------+-------+
|      AFI      |    Reserved   |
+-------+-------+-------+-------+
|         IP Address   ....
*/
struct icmp_interface_information_ipaddr_subobject_t {
    nd_uint16_t  afi;
    nd_uint16_t  reserved;
    nd_byte      ip_addr[];
};

/*
Interface Name Sub-Object
octet    0        1                                   63
        +--------+-----------................-----------------+
        | length |   interface name octets 1-63               |
        +--------+-----------................-----------------+
*/
struct icmp_interface_information_ifname_subobject_t {
    nd_uint8_t  length;
    nd_byte     if_name[63];
};

/*
 * Interface Identification IP Address Sub-Object
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |            AFI                | Address Length|   Reserved    |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                Address   ....
 */
struct icmp_interface_identification_ipaddr_subobject_t {
    nd_uint16_t  afi;
    nd_uint8_t   addrlen;
    nd_uint8_t   reserved;
    nd_byte      ip_addr[];
};

/* prototypes */
const char *icmp_tstamp_print(u_int);

/* print the milliseconds since midnight UTC */
const char *
icmp_tstamp_print(u_int tstamp)
{
    u_int msec,sec,min,hrs;

    static char buf[64];

    msec = tstamp % 1000;
    sec = tstamp / 1000;
    min = sec / 60; sec -= min * 60;
    hrs = min / 60; min -= hrs * 60;
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",hrs,min,sec,msec);
    return buf;
}

#define CHECK_TLEN(len) \
	ND_TCHECK_LEN(obj_tptr, len); \
	if (obj_tlen < (len)) { \
	    return obj_len; \
	}

#define UPDATE_TLEN(len) \
	obj_tlen -= (len)

#define UPDATE_TPTR_AND_TLEN(len) \
	obj_tptr += (len); \
	obj_tlen -= (len)

static int
print_icmp_multipart_ext_object(netdissect_options *ndo, const uint8_t *obj_ptr)
{
	u_int obj_len, obj_class_num, obj_ctype;
	const struct icmp_multipart_ext_object_header_t *icmp_multipart_ext_object_header;
	const uint8_t *obj_tptr;
	u_int obj_tlen;

	icmp_multipart_ext_object_header = (const struct icmp_multipart_ext_object_header_t *)obj_ptr;
	obj_len = GET_BE_U_2(icmp_multipart_ext_object_header->length);
	obj_class_num = GET_U_1(icmp_multipart_ext_object_header->class_num);
	obj_ctype = GET_U_1(icmp_multipart_ext_object_header->ctype);

	ND_PRINT("\n\t  %s (%u), Class-Type: %u, length %u",
		 tok2str(icmp_multipart_ext_obj_values,"unknown",obj_class_num),
		 obj_class_num,
		 obj_ctype,
		 obj_len);

	/* infinite loop protection */
	if ((obj_class_num == 0) ||	/* XXX - why is this necessary? */
	    (obj_len < sizeof(struct icmp_multipart_ext_object_header_t))) {
	    return -1;
	}
	obj_tptr = obj_ptr + sizeof(struct icmp_multipart_ext_object_header_t);
	obj_tlen = obj_len - sizeof(struct icmp_multipart_ext_object_header_t);

	switch (obj_class_num) {
	case MPLS_STACK_ENTRY_OBJECT_CLASS:
	    switch(obj_ctype) {
	    case 1:
	      {
		uint32_t raw_label;

		CHECK_TLEN(4);
		raw_label = GET_BE_U_4(obj_tptr);
		ND_PRINT("\n\t    label %u, tc %u", MPLS_LABEL(raw_label), MPLS_TC(raw_label));
		if (MPLS_STACK(raw_label))
		    ND_PRINT(", [S]");
		ND_PRINT(", ttl %u", MPLS_TTL(raw_label));
		break;
	      }
	    default:
		print_unknown_data(ndo, obj_tptr, "\n\t    ", obj_tlen);
	    }
	    break;

	case INTERFACE_INFORMATION_OBJECT_CLASS:
	  {
	    /*
	    Ctype in a INTERFACE_INFORMATION_OBJECT_CLASS object:

	    Bit     0       1       2       3       4       5       6       7
	    +-------+-------+-------+-------+-------+-------+-------+-------+
	    | Interface Role| Rsvd1 | Rsvd2 |ifIndex| IPAddr|  name |  MTU  |
	    +-------+-------+-------+-------+-------+-------+-------+-------+
	    */
	    u_int interface_role, if_index_flag, ipaddr_flag, name_flag, mtu_flag;

	    interface_role = (obj_ctype & 0xc0) >> 6;
	    if_index_flag  = (obj_ctype & 0x8) >> 3;
	    ipaddr_flag    = (obj_ctype & 0x4) >> 2;
	    name_flag      = (obj_ctype & 0x2) >> 1;
	    mtu_flag       = (obj_ctype & 0x1);

	    ND_PRINT("\n\t    Interface Role: %s",
		     tok2str(icmp_interface_information_role_values,
		     "an unknown interface role",interface_role));

	    if (if_index_flag) {
		CHECK_TLEN(4);
		ND_PRINT("\n\t    Interface Index: %u", GET_BE_U_4(obj_tptr));
		UPDATE_TPTR_AND_TLEN(4);
	    }
	    if (ipaddr_flag) {
		uint16_t afi;
		const struct icmp_interface_information_ipaddr_subobject_t *ipaddr_subobj;

		ND_PRINT("\n\t    IP Address sub-object: ");
		ipaddr_subobj = (const struct icmp_interface_information_ipaddr_subobject_t *) obj_tptr;

		CHECK_TLEN(sizeof *ipaddr_subobj);

		/* AFI */
		afi = GET_BE_U_2(ipaddr_subobj->afi);

		/* Reserved space */
		ND_TCHECK_1(ipaddr_subobj->reserved);

		UPDATE_TPTR_AND_TLEN(sizeof *ipaddr_subobj);

		switch (afi) {
		    case 1:
			CHECK_TLEN(4);
			ND_PRINT("%s", GET_IPADDR_STRING(ipaddr_subobj->ip_addr));
			UPDATE_TPTR_AND_TLEN(4);
			break;
		    case 2:
			CHECK_TLEN(16);
			ND_PRINT("%s", GET_IP6ADDR_STRING(ipaddr_subobj->ip_addr));
			UPDATE_TPTR_AND_TLEN(16);
			break;
		    default:
			ND_PRINT("Unknown Address Family Identifier");
			return -1;
		}
	    }
	    if (name_flag) {
		uint8_t inft_name_length_field;
		const struct icmp_interface_information_ifname_subobject_t *ifname_subobj;

		ifname_subobj = (const struct icmp_interface_information_ifname_subobject_t *) obj_tptr;
		CHECK_TLEN(1);
		inft_name_length_field = GET_U_1(ifname_subobj->length);

		ND_PRINT("\n\t    Interface Name");
		if (inft_name_length_field < 1) {
		    ND_PRINT(" [length %u]", inft_name_length_field);
		    nd_print_invalid(ndo);
		    break;
		}
		CHECK_TLEN(inft_name_length_field);
		if (inft_name_length_field % 4 != 0) {
		    ND_PRINT(" [length %u != N x 4]", inft_name_length_field);
		    nd_print_invalid(ndo);
		    UPDATE_TPTR_AND_TLEN(inft_name_length_field);
		    break;
		}
		if (inft_name_length_field > 64) {
		    ND_PRINT(" [length %u > 64]", inft_name_length_field);
		    nd_print_invalid(ndo);
		    UPDATE_TPTR_AND_TLEN(inft_name_length_field);
		    break;
		}
		ND_PRINT(", length %u: ", inft_name_length_field);
		nd_printjnp(ndo, ifname_subobj->if_name,
			    inft_name_length_field - 1);
		UPDATE_TPTR_AND_TLEN(inft_name_length_field);
	    }
	    if (mtu_flag) {
		CHECK_TLEN(4);
		ND_PRINT("\n\t    MTU: %u", GET_BE_U_4(obj_tptr));
		UPDATE_TPTR_AND_TLEN(4);
	    }
	    break;
	  }

	case INTERFACE_IDENTIFICATION_OBJECT_CLASS:
	    switch (obj_ctype) {
	    case 1:
		ND_PRINT("\n\t    Interface Name, length %u: ", obj_tlen);
		nd_printjnp(ndo, obj_tptr, obj_tlen);
		break;
	    case 2:
		CHECK_TLEN(4);
		ND_PRINT("\n\t    Interface Index: %u", GET_BE_U_4(obj_tptr));
		break;
	    case 3:
	      {
		const struct icmp_interface_identification_ipaddr_subobject_t *id_ipaddr_subobj;
		uint16_t afi;
		uint8_t addrlen;

		ND_PRINT("\n\t    IP Address sub-object: ");
		id_ipaddr_subobj = (const struct icmp_interface_identification_ipaddr_subobject_t *) obj_tptr;
		CHECK_TLEN(sizeof *id_ipaddr_subobj);
		afi = GET_BE_U_2(id_ipaddr_subobj->afi);
		addrlen = GET_U_1(id_ipaddr_subobj->addrlen);
		ND_TCHECK_1(id_ipaddr_subobj->reserved);
		UPDATE_TLEN(sizeof *id_ipaddr_subobj);

		CHECK_TLEN(addrlen);
		switch (afi) {
		    case 1:
			if (addrlen != 4) {
			    ND_PRINT("[length %d != 4] ", addrlen);
			}
			ND_PRINT("%s", GET_IPADDR_STRING(id_ipaddr_subobj->ip_addr));
			break;
		    case 2:
			if (addrlen != 16) {
			    ND_PRINT("[length %d != 16] ", addrlen);
			}
			ND_PRINT("%s", GET_IP6ADDR_STRING(id_ipaddr_subobj->ip_addr));
			break;
		    default:
			ND_PRINT("Unknown Address Family Identifier");
			return -1;
		}
		break;
	      }
	    default:
		print_unknown_data(ndo, obj_tptr, "\n\t    ", obj_tlen);
		break;
	    }
	    break;

	default:
	    print_unknown_data(ndo, obj_tptr, "\n\t    ", obj_tlen);
	    break;
	}
	return obj_len;
}

void
print_icmp_rfc8335(netdissect_options *ndo, uint8_t xinfo, int isrequest, uint8_t icmp_code, const uint8_t *data) {
	struct cksum_vec vec[1];

	if (isrequest) {
		ND_PRINT("\n\t%s Interface", xinfo & 1 ? "Local" : "Remote");
		if (ICMP_EXT_EXTRACT_VERSION(GET_U_1(data)) != ICMP_EXT_VERSION) {
		    nd_print_invalid(ndo);
		} else {
		    // A single extended object.  The extended header is not
		    // located at offset 128 in this case, so we can not use
		    // icmp_ext_checksum.
		    uint16_t sum = GET_BE_U_2(data + 2);
		    uint16_t len = GET_BE_U_2(data + 4);
		    // The checksum is over the extended header and the single
		    // object
		    len += 4;
		    vec[0].ptr = data;
		    vec[0].len = len;
		    if (ND_TTEST_LEN(vec[0].ptr, vec[0].len)) {
			ND_PRINT(", checksum 0x%04x (%scorrect), length %u",
			       sum,
			       in_cksum(vec, 1) ? "in" : "",
			       len);
		    }
		    print_icmp_multipart_ext_object(ndo, data + 4);
		}
	} else {
		    int state = ( xinfo & 0xe0 ) >> 5;
		    ND_PRINT("\n\tCode %d (%s), State %d (%s), active %d ipv4 %d ipv6 %d",
			    icmp_code, tok2str(icmp_extended_echo_reply_code_str, "Unknown", icmp_code),
			    state, tok2str(icmp_extended_echo_reply_state_str, "Unknown", state),
			    xinfo & 4 ? 1 : 0,
			    xinfo & 2 ? 1 : 0,
			    xinfo & 1 ? 1 : 0);
	}
}

void
icmp_print(netdissect_options *ndo, const u_char *bp, u_int plen,
           int fragmented)
{
	const struct icmp *dp;
	uint8_t icmp_type, icmp_code;
	const struct icmp_ext_t *ext_dp;
	const char *str;
	const uint8_t *obj_tptr;
	u_int hlen;
	char buf[512];
	struct cksum_vec vec[1];

	ndo->ndo_protocol = "icmp";
	dp = (const struct icmp *)bp;
	ext_dp = (const struct icmp_ext_t *)bp;
	str = buf;

	icmp_type = GET_U_1(dp->icmp_type);
	icmp_code = GET_U_1(dp->icmp_code);
	switch (icmp_type) {

	case ICMP_ECHO:
	case ICMP_ECHOREPLY:
		(void)snprintf(buf, sizeof(buf), "echo %s, id %u, seq %u",
                               icmp_type == ICMP_ECHO ?
                               "request" : "reply",
                               GET_BE_U_2(dp->icmp_id),
                               GET_BE_U_2(dp->icmp_seq));
		break;

	case ICMP_UNREACH:
		switch (icmp_code) {

		case ICMP_UNREACH_NET:
			(void)snprintf(buf, sizeof(buf),
			    "net %s unreachable",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_HOST:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_PROTOCOL:
			(void)snprintf(buf, sizeof(buf),
			    "%s protocol %u unreachable",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_U_1(dp->icmp_ip.ip_p));
			break;

		case ICMP_UNREACH_PORT:
		    {
			const struct ip *oip;
			const struct udphdr *ouh;
			uint8_t ip_proto;
			uint16_t dport;

			oip = &dp->icmp_ip;
			hlen = IP_HL(oip) * 4;
			ouh = (const struct udphdr *)(((const u_char *)oip) + hlen);
			dport = GET_BE_U_2(ouh->uh_dport);
			ip_proto = GET_U_1(oip->ip_p);
			switch (ip_proto) {

			case IPPROTO_TCP:
				(void)snprintf(buf, sizeof(buf),
					"%s tcp port %s unreachable",
					GET_IPADDR_STRING(oip->ip_dst),
					tcpport_string(ndo, dport));
				break;

			case IPPROTO_UDP:
				(void)snprintf(buf, sizeof(buf),
					"%s udp port %s unreachable",
					GET_IPADDR_STRING(oip->ip_dst),
					udpport_string(ndo, dport));
				break;

			default:
				(void)snprintf(buf, sizeof(buf),
					"%s protocol %u port %u unreachable",
					GET_IPADDR_STRING(oip->ip_dst),
					ip_proto, dport);
				break;
			}
			break;
		    }

		case ICMP_UNREACH_NEEDFRAG:
		    {
			const struct mtu_discovery *mp;
			u_int mtu;

			mp = (const struct mtu_discovery *)(const u_char *)&dp->icmp_void;
			mtu = GET_BE_U_2(mp->nexthopmtu);
			if (mtu) {
				(void)snprintf(buf, sizeof(buf),
				    "%s unreachable - need to frag (mtu %u)",
				    GET_IPADDR_STRING(dp->icmp_ip.ip_dst), mtu);
			} else {
				(void)snprintf(buf, sizeof(buf),
				    "%s unreachable - need to frag",
				    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			}
		    }
			break;

		case ICMP_UNREACH_SRCFAIL:
			(void)snprintf(buf, sizeof(buf),
			    "%s unreachable - source route failed",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_NET_UNKNOWN:
			(void)snprintf(buf, sizeof(buf),
			    "net %s unreachable - unknown",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_HOST_UNKNOWN:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - unknown",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_ISOLATED:
			(void)snprintf(buf, sizeof(buf),
			    "%s unreachable - source host isolated",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_NET_PROHIB:
			(void)snprintf(buf, sizeof(buf),
			    "net %s unreachable - admin prohibited",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_HOST_PROHIB:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - admin prohibited",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_TOSNET:
			(void)snprintf(buf, sizeof(buf),
			    "net %s unreachable - tos prohibited",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_TOSHOST:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - tos prohibited",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_FILTER_PROHIB:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - admin prohibited filter",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_HOST_PRECEDENCE:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - host precedence violation",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		case ICMP_UNREACH_PRECEDENCE_CUTOFF:
			(void)snprintf(buf, sizeof(buf),
			    "host %s unreachable - precedence cutoff",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst));
			break;

		default:
			(void)snprintf(buf, sizeof(buf),
			    "%s unreachable - #%u",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    icmp_code);
			break;
		}
		break;

	case ICMP_REDIRECT:
		switch (icmp_code) {

		case ICMP_REDIRECT_NET:
			(void)snprintf(buf, sizeof(buf),
			    "redirect %s to net %s",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_IPADDR_STRING(dp->icmp_gwaddr));
			break;

		case ICMP_REDIRECT_HOST:
			(void)snprintf(buf, sizeof(buf),
			    "redirect %s to host %s",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_IPADDR_STRING(dp->icmp_gwaddr));
			break;

		case ICMP_REDIRECT_TOSNET:
			(void)snprintf(buf, sizeof(buf),
			    "redirect-tos %s to net %s",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_IPADDR_STRING(dp->icmp_gwaddr));
			break;

		case ICMP_REDIRECT_TOSHOST:
			(void)snprintf(buf, sizeof(buf),
			    "redirect-tos %s to host %s",
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_IPADDR_STRING(dp->icmp_gwaddr));
			break;

		default:
			(void)snprintf(buf, sizeof(buf),
			    "redirect-#%u %s to %s", icmp_code,
			    GET_IPADDR_STRING(dp->icmp_ip.ip_dst),
			    GET_IPADDR_STRING(dp->icmp_gwaddr));
			break;
		}
		break;

	case ICMP_ROUTERADVERT:
	    {
		char *cp;
		const struct ih_rdiscovery *ihp;
		const struct id_rdiscovery *idp;
		u_int lifetime, num, size;

		(void)snprintf(buf, sizeof(buf), "router advertisement");
		cp = buf + strlen(buf);

		ihp = (const struct ih_rdiscovery *)&dp->icmp_void;
		(void)strncpy(cp, " lifetime ", sizeof(buf) - (cp - buf));
		cp = buf + strlen(buf);
		lifetime = GET_BE_U_2(ihp->ird_lifetime);
		if (lifetime < 60) {
			(void)snprintf(cp, sizeof(buf) - (cp - buf), "%u",
			    lifetime);
		} else if (lifetime < 60 * 60) {
			(void)snprintf(cp, sizeof(buf) - (cp - buf), "%u:%02u",
			    lifetime / 60, lifetime % 60);
		} else {
			(void)snprintf(cp, sizeof(buf) - (cp - buf),
			    "%u:%02u:%02u",
			    lifetime / 3600,
			    (lifetime % 3600) / 60,
			    lifetime % 60);
		}
		cp = buf + strlen(buf);

		num = GET_U_1(ihp->ird_addrnum);
		(void)snprintf(cp, sizeof(buf) - (cp - buf), " %u:", num);
		cp = buf + strlen(buf);

		size = GET_U_1(ihp->ird_addrsiz);
		if (size != 2) {
			(void)snprintf(cp, sizeof(buf) - (cp - buf),
			    " [size %u]", size);
			break;
		}
		idp = (const struct id_rdiscovery *)&dp->icmp_data;
		while (num != 0) {
			(void)snprintf(cp, sizeof(buf) - (cp - buf), " {%s %u}",
			    GET_IPADDR_STRING(idp->ird_addr),
			    GET_BE_U_4(idp->ird_pref));
			cp = buf + strlen(buf);
			++idp;
			num--;
		}
	    }
		break;

	case ICMP_TIMXCEED:
		ND_TCHECK_4(dp->icmp_ip.ip_dst);
		switch (icmp_code) {

		case ICMP_TIMXCEED_INTRANS:
			str = "time exceeded in-transit";
			break;

		case ICMP_TIMXCEED_REASS:
			str = "ip reassembly time exceeded";
			break;

		default:
			(void)snprintf(buf, sizeof(buf), "time exceeded-#%u",
			    icmp_code);
			break;
		}
		break;

	case ICMP_PARAMPROB:
		if (icmp_code)
			(void)snprintf(buf, sizeof(buf),
			    "parameter problem - code %u", icmp_code);
		else {
			(void)snprintf(buf, sizeof(buf),
			    "parameter problem - octet %u",
			    GET_U_1(dp->icmp_pptr));
		}
		break;

	case ICMP_MASKREPLY:
		(void)snprintf(buf, sizeof(buf), "address mask is 0x%08x",
		    GET_BE_U_4(dp->icmp_mask));
		break;

	case ICMP_TSTAMP:
		(void)snprintf(buf, sizeof(buf),
		    "time stamp query id %u seq %u",
		    GET_BE_U_2(dp->icmp_id),
		    GET_BE_U_2(dp->icmp_seq));
		break;

	case ICMP_TSTAMPREPLY:
		(void)snprintf(buf, sizeof(buf),
		    "time stamp reply id %u seq %u: org %s",
                               GET_BE_U_2(dp->icmp_id),
                               GET_BE_U_2(dp->icmp_seq),
                               icmp_tstamp_print(GET_BE_U_4(dp->icmp_otime)));

                (void)snprintf(buf+strlen(buf),sizeof(buf)-strlen(buf),", recv %s",
                         icmp_tstamp_print(GET_BE_U_4(dp->icmp_rtime)));
                (void)snprintf(buf+strlen(buf),sizeof(buf)-strlen(buf),", xmit %s",
                         icmp_tstamp_print(GET_BE_U_4(dp->icmp_ttime)));
                break;

	case ICMP_EXTENDED_ECHO_REQUEST:
	case ICMP_EXTENDED_ECHO_REPLY:
		/* brief info here due to limited buf; more info below */
		(void)snprintf(buf, sizeof(buf), "extended echo %s, id %u, seq %u",
                               icmp_type == ICMP_EXTENDED_ECHO_REQUEST ?
                               "request" : "reply",
                               GET_BE_U_2(dp->icmp_id),
                               GET_U_1(dp->icmp_xseq));
		break;

	default:
		str = tok2str(icmp2str, "type-#%u", icmp_type);
		break;
	}
	ND_PRINT("ICMP %s, length %u", str, plen);
	if (ndo->ndo_vflag && !fragmented) { /* don't attempt checksumming if this is a frag */
		if (ND_TTEST_LEN(bp, plen)) {
			uint16_t sum;

			vec[0].ptr = (const uint8_t *)(const void *)dp;
			vec[0].len = plen;
			sum = in_cksum(vec, 1);
			if (sum != 0) {
				uint16_t icmp_sum = GET_BE_U_2(dp->icmp_cksum);
				ND_PRINT(" (wrong icmp cksum %x (->%x)!)",
					     icmp_sum,
					     in_cksum_shouldbe(icmp_sum, sum));
			}
		}
	}

        /*
         * print the remnants of the IP packet.
         * save the snaplength as this may get overridden in the IP printer.
         */
	if (ndo->ndo_vflag >= 1 && ICMP_ERRTYPE(icmp_type)) {
		const struct ip *ip;
		const u_char *snapend_save;

		bp += 8;
		ND_PRINT("\n\t");
		ip = (const struct ip *)bp;
		snapend_save = ndo->ndo_snapend;
		/*
		 * Update the snapend because extensions (MPLS, ...) may be
		 * present after the IP packet. In this case the current
		 * (outer) packet's snapend is not what ip_print() needs to
		 * decode an IP packet nested in the middle of an ICMP payload.
		 *
		 * This prevents that, in ip_print(), for the nested IP packet,
		 * the remaining length < remaining caplen.
		 */
		ndo->ndo_snapend = ND_MIN(bp + GET_BE_U_2(ip->ip_len),
					  ndo->ndo_snapend);
		ip_print(ndo, bp, GET_BE_U_2(ip->ip_len));
		ndo->ndo_snapend = snapend_save;
	}

	/* ndo_protocol reassignment after ip_print() call */
	ndo->ndo_protocol = "icmp";

        /*
         * Attempt to decode multi-part message extensions (rfc4884) only for some ICMP types.
         */
        if (ndo->ndo_vflag >= 1 && plen > ICMP_EXTD_MINLEN && ICMP_MULTIPART_EXT_TYPE(icmp_type)) {
            ND_TCHECK_SIZE(ext_dp);

            /*
             * Check first if the multi-part extension header shows a non-zero length.
             * If the length field is not set then silently verify the checksum
             * to check if an extension header is present. This is expedient,
             * however not all implementations set the length field proper.
             */
            if (GET_U_1(ext_dp->icmp_length) == 0 &&
                ND_TTEST_LEN(ext_dp->icmp_ext_version_res, plen - ICMP_EXTD_MINLEN)) {
                vec[0].ptr = (const uint8_t *)(const void *)&ext_dp->icmp_ext_version_res;
                vec[0].len = plen - ICMP_EXTD_MINLEN;
                if (in_cksum(vec, 1)) {
                    return;
                }
            }

            ND_PRINT("\n\tICMP Multi-Part extension v%u",
                   ICMP_EXT_EXTRACT_VERSION(*(ext_dp->icmp_ext_version_res)));

            /*
             * Sanity checking of the header.
             */
            if (ICMP_EXT_EXTRACT_VERSION(*(ext_dp->icmp_ext_version_res)) !=
                ICMP_EXT_VERSION) {
                ND_PRINT(" packet not supported");
                return;
            }

            hlen = plen - ICMP_EXTD_MINLEN;
            if (ND_TTEST_LEN(ext_dp->icmp_ext_version_res, hlen)) {
                vec[0].ptr = (const uint8_t *)(const void *)&ext_dp->icmp_ext_version_res;
                vec[0].len = hlen;
                ND_PRINT(", checksum 0x%04x (%scorrect), length %u",
                       GET_BE_U_2(ext_dp->icmp_ext_checksum),
                       in_cksum(vec, 1) ? "in" : "",
                       hlen);
            }

            hlen -= 4; /* subtract common header size */
            obj_tptr = (const uint8_t *)ext_dp->icmp_ext_data;

            while (hlen > sizeof(struct icmp_multipart_ext_object_header_t)) {
                int obj_tlen = print_icmp_multipart_ext_object(ndo, obj_tptr);
                if (obj_tlen < 0) {
                    /* malformed object */
                    return;
                }
                if (hlen < (u_int)obj_tlen)
                    break;
                hlen -= obj_tlen;
                obj_tptr += obj_tlen;
            }
        }

	if (ndo->ndo_vflag >= 1 && ICMP_EXTENDED_ECHO_TYPE(icmp_type)) {
	    uint8_t xinfo = GET_U_1(dp->icmp_xinfo);
	    // RFC8335 printing is shared between ICMP and ICMPv6
	    print_icmp_rfc8335( ndo, xinfo, icmp_type == ICMP_EXTENDED_ECHO_REQUEST, icmp_code, dp->icmp_data );
	}

	return;
}
