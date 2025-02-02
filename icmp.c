/*
 * Copyright (c) 2013-2015 Erik Ekman <yarrick@kryo.se>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright notice
 * and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>

#include "icmp.h"

struct icmp_rule {
	int request_type;
	int reply_type;
	int use_checksum;
	int strip_iphdr;
};

static const struct icmp_rule icmpv4 = {
	.request_type = ICMP_ECHO,
	.reply_type = ICMP_ECHOREPLY,
	.use_checksum = 1,
	.strip_iphdr = 1,
};
static const struct icmp_rule icmpv6 = {
	.request_type = ICMP6_ECHO_REQUEST,
	.reply_type = ICMP6_ECHO_REPLY,
	.use_checksum = 0,
	.strip_iphdr = 0,
};

#define GET_RULE(pkt) ((ICMP_ADDRFAMILY(pkt) == AF_INET) ? &icmpv4 : &icmpv6 )

static uint16_t checksum(uint8_t *data, uint32_t len)
{
	uint32_t csum = 0;
	uint32_t i;
	for (i = 0; i < len; i += 2) {
		uint16_t c = data[i] << 8;
		if (i + 1 < len) c |= data[i + 1];
		csum += c;
	}
	csum = (csum >> 16) + (csum & 0xffff);
	csum += (csum >> 16);
	return (uint16_t)(~csum);
}

static uint16_t read16(uint8_t *data)
{
	return (data[0] << 8) | data[1];
}

static void write16(uint8_t *data, uint16_t s)
{
	data[0] = s >> 8;
	data[1] = s & 0xFF;
}

static uint8_t *icmp_encode(struct icmp_packet *pkt, int *len)
{
	struct icmp_rule const *rule = GET_RULE(pkt);
	uint8_t *data;
	int pktlen;

	pktlen = ICMP_HDRLEN + pkt->payload_len;
	data = calloc(1, pktlen);
	if (!data)
		return NULL;

	if (pkt->type == ICMP_REQUEST) {
		data[0] = rule->request_type;
	} else {
		data[0] = rule->reply_type;
	}

	write16(&data[4], pkt->id);
	write16(&data[6], pkt->seqno);
	if (pkt->payload_len)
		memcpy(&data[8], pkt->payload, pkt->payload_len);

	if (rule->use_checksum) {
		write16(&data[2], checksum(data, pktlen));
	}

	*len = pktlen;
	return data;
}

int icmp_send(int socket, struct icmp_packet *pkt)
{
	int len;
	uint8_t *icmpdata;

	icmpdata = icmp_encode(pkt, &len);
	if (!icmpdata)
		return 0;

	len = sendto(socket, icmpdata, len, 0, (struct sockaddr *) &pkt->peer, pkt->peer_len);

	free(icmpdata);
	return len;
}

int icmp_parse(struct icmp_packet *pkt, uint8_t *data, int len)
{
	struct icmp_rule const *rule = GET_RULE(pkt);
	if (rule->strip_iphdr) {
		int hdrlen;
		if (len == 0) return -3;
		hdrlen = (data[0] & 0x0f) << 2;
		if (len < hdrlen) return -4;
		data += hdrlen;
		len -= hdrlen;
	}
	if (len < ICMP_HDRLEN) return -1;
	if (rule->use_checksum) {
		if (checksum(data, len) != 0) return -2;
	}
	if (rule->request_type == data[0]) {
		pkt->type = ICMP_REQUEST;
	} else if (rule->reply_type == data[0]) {
		pkt->type = ICMP_REPLY;
	} else {
		return -5;
	}
	pkt->id = read16(&data[4]);
	pkt->seqno = read16(&data[6]);
	pkt->payload_len = len - ICMP_HDRLEN;
	if (pkt->payload_len) {
		pkt->payload = malloc(pkt->payload_len);
		memcpy(pkt->payload, &data[ICMP_HDRLEN], pkt->payload_len);
	} else {
		pkt->payload = NULL;
	}
	return 0;
}

static void *get_in_addr(struct sockaddr_storage *ss)
{
	if (ss->ss_family == AF_INET) {
		return &(((struct sockaddr_in*)ss)->sin_addr);
	} else {
		return &(((struct sockaddr_in6*)ss)->sin6_addr);
	}
}

static char *icmp_type_str(struct icmp_packet *pkt)
{
	if (pkt->type == ICMP_REPLY) return "Reply from";
	if (pkt->type == ICMP_REQUEST) return "Request to";
	return "Other";
}

void icmp_dump(struct icmp_packet *pkt)
{
	char ipaddr[64];
	bzero(ipaddr, sizeof(ipaddr));
	inet_ntop(pkt->peer.ss_family, get_in_addr(&pkt->peer), ipaddr, sizeof(ipaddr));

	printf("%s %s, id %04X, seqno %04X, payload %d bytes\n",
		icmp_type_str(pkt), ipaddr, pkt->id, pkt->seqno, pkt->payload_len);
}
