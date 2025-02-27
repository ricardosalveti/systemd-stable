/* SPDX-License-Identifier: LGPL-2.1+ */

#include "sd-messages.h"

#include "alloc-util.h"
#include "resolved-dns-server.h"
#include "resolved-dns-stub.h"
#include "resolved-resolv-conf.h"
#include "siphash24.h"
#include "string-table.h"
#include "string-util.h"

/* The amount of time to wait before retrying with a full feature set */
#define DNS_SERVER_FEATURE_GRACE_PERIOD_MAX_USEC (6 * USEC_PER_HOUR)
#define DNS_SERVER_FEATURE_GRACE_PERIOD_MIN_USEC (5 * USEC_PER_MINUTE)

/* The number of times we will attempt a certain feature set before degrading */
#define DNS_SERVER_FEATURE_RETRY_ATTEMPTS 3

int dns_server_new(
                Manager *m,
                DnsServer **ret,
                DnsServerType type,
                Link *l,
                int family,
                const union in_addr_union *in_addr,
                int ifindex) {

        DnsServer *s;

        assert(m);
        assert((type == DNS_SERVER_LINK) == !!l);
        assert(in_addr);

        if (!IN_SET(family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        if (l) {
                if (l->n_dns_servers >= LINK_DNS_SERVERS_MAX)
                        return -E2BIG;
        } else {
                if (m->n_dns_servers >= MANAGER_DNS_SERVERS_MAX)
                        return -E2BIG;
        }

        s = new(DnsServer, 1);
        if (!s)
                return -ENOMEM;

        *s = (DnsServer) {
                .n_ref = 1,
                .manager = m,
                .type = type,
                .family = family,
                .address = *in_addr,
                .ifindex = ifindex,
        };

        dns_server_reset_features(s);

        switch (type) {

        case DNS_SERVER_LINK:
                s->link = l;
                LIST_APPEND(servers, l->dns_servers, s);
                l->n_dns_servers++;
                break;

        case DNS_SERVER_SYSTEM:
                LIST_APPEND(servers, m->dns_servers, s);
                m->n_dns_servers++;
                break;

        case DNS_SERVER_FALLBACK:
                LIST_APPEND(servers, m->fallback_dns_servers, s);
                m->n_dns_servers++;
                break;

        default:
                assert_not_reached("Unknown server type");
        }

        s->linked = true;

#if ENABLE_DNS_OVER_TLS
        dnstls_server_init(s);
#endif

        /* A new DNS server that isn't fallback is added and the one
         * we used so far was a fallback one? Then let's try to pick
         * the new one */
        if (type != DNS_SERVER_FALLBACK &&
            m->current_dns_server &&
            m->current_dns_server->type == DNS_SERVER_FALLBACK)
                manager_set_dns_server(m, NULL);

        if (ret)
                *ret = s;

        return 0;
}

static DnsServer* dns_server_free(DnsServer *s)  {
        assert(s);

        dns_server_unref_stream(s);

#if ENABLE_DNS_OVER_TLS
        dnstls_server_free(s);
#endif

        free(s->server_string);
        return mfree(s);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(DnsServer, dns_server, dns_server_free);

void dns_server_unlink(DnsServer *s) {
        assert(s);
        assert(s->manager);

        /* This removes the specified server from the linked list of
         * servers, but any server might still stay around if it has
         * refs, for example from an ongoing transaction. */

        if (!s->linked)
                return;

        switch (s->type) {

        case DNS_SERVER_LINK:
                assert(s->link);
                assert(s->link->n_dns_servers > 0);
                LIST_REMOVE(servers, s->link->dns_servers, s);
                s->link->n_dns_servers--;
                break;

        case DNS_SERVER_SYSTEM:
                assert(s->manager->n_dns_servers > 0);
                LIST_REMOVE(servers, s->manager->dns_servers, s);
                s->manager->n_dns_servers--;
                break;

        case DNS_SERVER_FALLBACK:
                assert(s->manager->n_dns_servers > 0);
                LIST_REMOVE(servers, s->manager->fallback_dns_servers, s);
                s->manager->n_dns_servers--;
                break;
        default:
                assert_not_reached("Unknown server type");
        }

        s->linked = false;

        if (s->link && s->link->current_dns_server == s)
                link_set_dns_server(s->link, NULL);

        if (s->manager->current_dns_server == s)
                manager_set_dns_server(s->manager, NULL);

        /* No need to keep a default stream around anymore */
        dns_server_unref_stream(s);

        dns_server_unref(s);
}

void dns_server_move_back_and_unmark(DnsServer *s) {
        DnsServer *tail;

        assert(s);

        if (!s->marked)
                return;

        s->marked = false;

        if (!s->linked || !s->servers_next)
                return;

        /* Move us to the end of the list, so that the order is
         * strictly kept, if we are not at the end anyway. */

        switch (s->type) {

        case DNS_SERVER_LINK:
                assert(s->link);
                LIST_FIND_TAIL(servers, s, tail);
                LIST_REMOVE(servers, s->link->dns_servers, s);
                LIST_INSERT_AFTER(servers, s->link->dns_servers, tail, s);
                break;

        case DNS_SERVER_SYSTEM:
                LIST_FIND_TAIL(servers, s, tail);
                LIST_REMOVE(servers, s->manager->dns_servers, s);
                LIST_INSERT_AFTER(servers, s->manager->dns_servers, tail, s);
                break;

        case DNS_SERVER_FALLBACK:
                LIST_FIND_TAIL(servers, s, tail);
                LIST_REMOVE(servers, s->manager->fallback_dns_servers, s);
                LIST_INSERT_AFTER(servers, s->manager->fallback_dns_servers, tail, s);
                break;

        default:
                assert_not_reached("Unknown server type");
        }
}

static void dns_server_verified(DnsServer *s, DnsServerFeatureLevel level) {
        assert(s);

        if (s->verified_feature_level > level)
                return;

        if (s->verified_feature_level != level) {
                log_debug("Verified we get a response at feature level %s from DNS server %s.",
                          dns_server_feature_level_to_string(level),
                          dns_server_string(s));
                s->verified_feature_level = level;
        }

        assert_se(sd_event_now(s->manager->event, clock_boottime_or_monotonic(), &s->verified_usec) >= 0);
}

static void dns_server_reset_counters(DnsServer *s) {
        assert(s);

        s->n_failed_udp = 0;
        s->n_failed_tcp = 0;
        s->n_failed_tls = 0;
        s->packet_truncated = false;
        s->verified_usec = 0;

        /* Note that we do not reset s->packet_bad_opt and s->packet_rrsig_missing here. We reset them only when the
         * grace period ends, but not when lowering the possible feature level, as a lower level feature level should
         * not make RRSIGs appear or OPT appear, but rather make them disappear. If the reappear anyway, then that's
         * indication for a differently broken OPT/RRSIG implementation, and we really don't want to support that
         * either.
         *
         * This is particularly important to deal with certain Belkin routers which break OPT for certain lookups (A),
         * but pass traffic through for others (AAAA). If we detect the broken behaviour on one lookup we should not
         * re-enable it for another, because we cannot validate things anyway, given that the RRSIG/OPT data will be
         * incomplete. */
}

void dns_server_packet_received(DnsServer *s, int protocol, DnsServerFeatureLevel level, size_t size) {
        assert(s);

        if (protocol == IPPROTO_UDP) {
                if (s->possible_feature_level == level)
                        s->n_failed_udp = 0;
        } else if (protocol == IPPROTO_TCP) {
                if (DNS_SERVER_FEATURE_LEVEL_IS_TLS(level)) {
                        if (s->possible_feature_level == level)
                                s->n_failed_tls = 0;
                } else {
                        if (s->possible_feature_level == level)
                                s->n_failed_tcp = 0;

                        /* Successful TCP connections are only useful to verify the TCP feature level. */
                        level = DNS_SERVER_FEATURE_LEVEL_TCP;
                }
        }

        /* If the RRSIG data is missing, then we can only validate EDNS0 at max */
        if (s->packet_rrsig_missing && level >= DNS_SERVER_FEATURE_LEVEL_DO)
                level = DNS_SERVER_FEATURE_LEVEL_IS_TLS(level) ? DNS_SERVER_FEATURE_LEVEL_TLS_PLAIN : DNS_SERVER_FEATURE_LEVEL_EDNS0;

        /* If the OPT RR got lost, then we can only validate UDP at max */
        if (s->packet_bad_opt && level >= DNS_SERVER_FEATURE_LEVEL_EDNS0)
                level = DNS_SERVER_FEATURE_LEVEL_EDNS0 - 1;

        /* Even if we successfully receive a reply to a request announcing support for large packets,
                that does not mean we can necessarily receive large packets. */
        if (level == DNS_SERVER_FEATURE_LEVEL_LARGE)
                level = DNS_SERVER_FEATURE_LEVEL_LARGE - 1;

        dns_server_verified(s, level);

        /* Remember the size of the largest UDP packet we received from a server,
           we know that we can always announce support for packets with at least
           this size. */
        if (protocol == IPPROTO_UDP && s->received_udp_packet_max < size)
                s->received_udp_packet_max = size;
}

void dns_server_packet_lost(DnsServer *s, int protocol, DnsServerFeatureLevel level) {
        assert(s);
        assert(s->manager);

        if (s->possible_feature_level == level) {
                if (protocol == IPPROTO_UDP)
                        s->n_failed_udp++;
                else if (protocol == IPPROTO_TCP) {
                        if (DNS_SERVER_FEATURE_LEVEL_IS_TLS(level))
                                s->n_failed_tls++;
                        else
                                s->n_failed_tcp++;
                }
        }
}

void dns_server_packet_truncated(DnsServer *s, DnsServerFeatureLevel level) {
        assert(s);

        /* Invoked whenever we get a packet with TC bit set. */

        if (s->possible_feature_level != level)
                return;

        s->packet_truncated = true;
}

void dns_server_packet_rrsig_missing(DnsServer *s, DnsServerFeatureLevel level) {
        assert(s);

        if (level < DNS_SERVER_FEATURE_LEVEL_DO)
                return;

        /* If the RRSIG RRs are missing, we have to downgrade what we previously verified */
        if (s->verified_feature_level >= DNS_SERVER_FEATURE_LEVEL_DO)
                s->verified_feature_level = DNS_SERVER_FEATURE_LEVEL_IS_TLS(level) ? DNS_SERVER_FEATURE_LEVEL_TLS_PLAIN : DNS_SERVER_FEATURE_LEVEL_EDNS0;

        s->packet_rrsig_missing = true;
}

void dns_server_packet_bad_opt(DnsServer *s, DnsServerFeatureLevel level) {
        assert(s);

        if (level < DNS_SERVER_FEATURE_LEVEL_EDNS0)
                return;

        /* If the OPT RR got lost, we have to downgrade what we previously verified */
        if (s->verified_feature_level >= DNS_SERVER_FEATURE_LEVEL_EDNS0)
                s->verified_feature_level = DNS_SERVER_FEATURE_LEVEL_EDNS0-1;

        s->packet_bad_opt = true;
}

void dns_server_packet_rcode_downgrade(DnsServer *s, DnsServerFeatureLevel level) {
        assert(s);

        /* Invoked whenever we got a FORMERR, SERVFAIL or NOTIMP rcode from a server and downgrading the feature level
         * for the transaction made it go away. In this case we immediately downgrade to the feature level that made
         * things work. */

        if (s->verified_feature_level > level)
                s->verified_feature_level = level;

        if (s->possible_feature_level > level) {
                s->possible_feature_level = level;
                dns_server_reset_counters(s);
        }

        log_debug("Downgrading transaction feature level fixed an RCODE error, downgrading server %s too.", dns_server_string(s));
}

static bool dns_server_grace_period_expired(DnsServer *s) {
        usec_t ts;

        assert(s);
        assert(s->manager);

        if (s->verified_usec == 0)
                return false;

        assert_se(sd_event_now(s->manager->event, clock_boottime_or_monotonic(), &ts) >= 0);

        if (s->verified_usec + s->features_grace_period_usec > ts)
                return false;

        s->features_grace_period_usec = MIN(s->features_grace_period_usec * 2, DNS_SERVER_FEATURE_GRACE_PERIOD_MAX_USEC);

        return true;
}

DnsServerFeatureLevel dns_server_possible_feature_level(DnsServer *s) {
        DnsServerFeatureLevel best;

        assert(s);

        /* Determine the best feature level we care about. If DNSSEC mode is off there's no point in using anything
         * better than EDNS0, hence don't even try. */
        if (dns_server_get_dnssec_mode(s) != DNSSEC_NO)
                best = dns_server_get_dns_over_tls_mode(s) == DNS_OVER_TLS_NO ?
                        DNS_SERVER_FEATURE_LEVEL_LARGE :
                        DNS_SERVER_FEATURE_LEVEL_TLS_DO;
        else
                best = dns_server_get_dns_over_tls_mode(s) == DNS_OVER_TLS_NO ?
                        DNS_SERVER_FEATURE_LEVEL_EDNS0 :
                        DNS_SERVER_FEATURE_LEVEL_TLS_PLAIN;

        /* Clamp the feature level the highest level we care about. The DNSSEC mode might have changed since the last
         * time, hence let's downgrade if we are still at a higher level. */
        if (s->possible_feature_level > best)
                s->possible_feature_level = best;

        if (s->possible_feature_level < best && dns_server_grace_period_expired(s)) {

                s->possible_feature_level = best;

                dns_server_reset_counters(s);

                s->packet_bad_opt = false;
                s->packet_rrsig_missing = false;

                log_info("Grace period over, resuming full feature set (%s) for DNS server %s.",
                         dns_server_feature_level_to_string(s->possible_feature_level),
                         dns_server_string(s));

                dns_server_flush_cache(s);

        } else if (s->possible_feature_level <= s->verified_feature_level)
                s->possible_feature_level = s->verified_feature_level;
        else {
                DnsServerFeatureLevel p = s->possible_feature_level;

                if (s->n_failed_tcp >= DNS_SERVER_FEATURE_RETRY_ATTEMPTS &&
                    s->possible_feature_level == DNS_SERVER_FEATURE_LEVEL_TCP) {

                        /* We are at the TCP (lowest) level, and we tried a couple of TCP connections, and it didn't
                         * work. Upgrade back to UDP again. */
                        log_debug("Reached maximum number of failed TCP connection attempts, trying UDP again...");
                        s->possible_feature_level = DNS_SERVER_FEATURE_LEVEL_UDP;
                } else if (s->n_failed_tls > 0 &&
                           DNS_SERVER_FEATURE_LEVEL_IS_TLS(s->possible_feature_level)) {

                        /* We tried to connect using DNS-over-TLS, and it didn't work. Downgrade to plaintext UDP
                         * if we don't require DNS-over-TLS */

                        log_debug("Server doesn't support DNS-over-TLS, downgrading protocol...");
                        s->possible_feature_level--;
                } else if (s->packet_bad_opt &&
                           s->possible_feature_level >= DNS_SERVER_FEATURE_LEVEL_EDNS0) {

                        /* A reply to one of our EDNS0 queries didn't carry a valid OPT RR, then downgrade to below
                         * EDNS0 levels. After all, some records generate different responses with and without OPT RR
                         * in the request. Example:
                         * https://open.nlnetlabs.nl/pipermail/dnssec-trigger/2014-November/000376.html */

                        log_debug("Server doesn't support EDNS(0) properly, downgrading feature level...");
                        s->possible_feature_level = DNS_SERVER_FEATURE_LEVEL_UDP;

                } else if (s->packet_rrsig_missing &&
                           s->possible_feature_level >= DNS_SERVER_FEATURE_LEVEL_DO) {

                        /* RRSIG data was missing on a EDNS0 packet with DO bit set. This means the server doesn't
                         * augment responses with DNSSEC RRs. If so, let's better not ask the server for it anymore,
                         * after all some servers generate different replies depending if an OPT RR is in the query or
                         * not. */

                        log_debug("Detected server responses lack RRSIG records, downgrading feature level...");
                        s->possible_feature_level = DNS_SERVER_FEATURE_LEVEL_IS_TLS(s->possible_feature_level) ? DNS_SERVER_FEATURE_LEVEL_TLS_PLAIN : DNS_SERVER_FEATURE_LEVEL_EDNS0;

                } else if (s->n_failed_udp >= DNS_SERVER_FEATURE_RETRY_ATTEMPTS &&
                           s->possible_feature_level >= (dns_server_get_dnssec_mode(s) == DNSSEC_YES ? DNS_SERVER_FEATURE_LEVEL_LARGE : DNS_SERVER_FEATURE_LEVEL_UDP)) {

                        /* We lost too many UDP packets in a row, and are on a feature level of UDP or higher. If the
                         * packets are lost, maybe the server cannot parse them, hence downgrading sounds like a good
                         * idea. We might downgrade all the way down to TCP this way.
                         *
                         * If strict DNSSEC mode is used we won't downgrade below DO level however, as packet loss
                         * might have many reasons, a broken DNSSEC implementation being only one reason. And if the
                         * user is strict on DNSSEC, then let's assume that DNSSEC is not the fault here. */

                        log_debug("Lost too many UDP packets, downgrading feature level...");
                        s->possible_feature_level--;

                } else if (s->n_failed_tcp >= DNS_SERVER_FEATURE_RETRY_ATTEMPTS &&
                           s->packet_truncated &&
                           s->possible_feature_level > (dns_server_get_dnssec_mode(s) == DNSSEC_YES ? DNS_SERVER_FEATURE_LEVEL_LARGE : DNS_SERVER_FEATURE_LEVEL_UDP)) {

                         /* We got too many TCP connection failures in a row, we had at least one truncated packet, and
                          * are on a feature level above UDP. By downgrading things and getting rid of DNSSEC or EDNS0
                          * data we hope to make the packet smaller, so that it still works via UDP given that TCP
                          * appears not to be a fallback. Note that if we are already at the lowest UDP level, we don't
                          * go further down, since that's TCP, and TCP failed too often after all. */

                        log_debug("Got too many failed TCP connection failures and truncated UDP packets, downgrading feature level...");
                        s->possible_feature_level--;
                }

                if (p != s->possible_feature_level) {

                        /* We changed the feature level, reset the counting */
                        dns_server_reset_counters(s);

                        log_warning("Using degraded feature set (%s) for DNS server %s.",
                                    dns_server_feature_level_to_string(s->possible_feature_level),
                                    dns_server_string(s));
                }
        }

        return s->possible_feature_level;
}

int dns_server_adjust_opt(DnsServer *server, DnsPacket *packet, DnsServerFeatureLevel level) {
        size_t packet_size;
        bool edns_do;
        int r;

        assert(server);
        assert(packet);
        assert(packet->protocol == DNS_PROTOCOL_DNS);

        /* Fix the OPT field in the packet to match our current feature level. */

        r = dns_packet_truncate_opt(packet);
        if (r < 0)
                return r;

        if (level < DNS_SERVER_FEATURE_LEVEL_EDNS0)
                return 0;

        edns_do = level >= DNS_SERVER_FEATURE_LEVEL_DO;

        if (level >= DNS_SERVER_FEATURE_LEVEL_LARGE)
                packet_size = DNS_PACKET_UNICAST_SIZE_LARGE_MAX;
        else
                packet_size = server->received_udp_packet_max;

        return dns_packet_append_opt(packet, packet_size, edns_do, 0, NULL);
}

int dns_server_ifindex(const DnsServer *s) {
        assert(s);

        /* The link ifindex always takes precedence */
        if (s->link)
                return s->link->ifindex;

        if (s->ifindex > 0)
                return s->ifindex;

        return 0;
}

const char *dns_server_string(DnsServer *server) {
        assert(server);

        if (!server->server_string)
                (void) in_addr_ifindex_to_string(server->family, &server->address, dns_server_ifindex(server), &server->server_string);

        return strna(server->server_string);
}

bool dns_server_dnssec_supported(DnsServer *server) {
        assert(server);

        /* Returns whether the server supports DNSSEC according to what we know about it */

        if (server->possible_feature_level < DNS_SERVER_FEATURE_LEVEL_DO)
                return false;

        if (server->packet_bad_opt)
                return false;

        if (server->packet_rrsig_missing)
                return false;

        /* DNSSEC servers need to support TCP properly (see RFC5966), if they don't, we assume DNSSEC is borked too */
        if (server->n_failed_tcp >= DNS_SERVER_FEATURE_RETRY_ATTEMPTS)
                return false;

        return true;
}

void dns_server_warn_downgrade(DnsServer *server) {
        assert(server);

        if (server->warned_downgrade)
                return;

        log_struct(LOG_NOTICE,
                   "MESSAGE_ID=" SD_MESSAGE_DNSSEC_DOWNGRADE_STR,
                   LOG_MESSAGE("Server %s does not support DNSSEC, downgrading to non-DNSSEC mode.", dns_server_string(server)),
                   "DNS_SERVER=%s", dns_server_string(server),
                   "DNS_SERVER_FEATURE_LEVEL=%s", dns_server_feature_level_to_string(server->possible_feature_level));

        server->warned_downgrade = true;
}

static void dns_server_hash_func(const DnsServer *s, struct siphash *state) {
        assert(s);

        siphash24_compress(&s->family, sizeof(s->family), state);
        siphash24_compress(&s->address, FAMILY_ADDRESS_SIZE(s->family), state);
        siphash24_compress(&s->ifindex, sizeof(s->ifindex), state);
}

static int dns_server_compare_func(const DnsServer *x, const DnsServer *y) {
        int r;

        r = CMP(x->family, y->family);
        if (r != 0)
                return r;

        r = memcmp(&x->address, &y->address, FAMILY_ADDRESS_SIZE(x->family));
        if (r != 0)
                return r;

        r = CMP(x->ifindex, y->ifindex);
        if (r != 0)
                return r;

        return 0;
}

DEFINE_HASH_OPS(dns_server_hash_ops, DnsServer, dns_server_hash_func, dns_server_compare_func);

void dns_server_unlink_all(DnsServer *first) {
        DnsServer *next;

        if (!first)
                return;

        next = first->servers_next;
        dns_server_unlink(first);

        dns_server_unlink_all(next);
}

void dns_server_unlink_marked(DnsServer *first) {
        DnsServer *next;

        if (!first)
                return;

        next = first->servers_next;

        if (first->marked)
                dns_server_unlink(first);

        dns_server_unlink_marked(next);
}

void dns_server_mark_all(DnsServer *first) {
        if (!first)
                return;

        first->marked = true;
        dns_server_mark_all(first->servers_next);
}

DnsServer *dns_server_find(DnsServer *first, int family, const union in_addr_union *in_addr, int ifindex) {
        DnsServer *s;

        LIST_FOREACH(servers, s, first)
                if (s->family == family && in_addr_equal(family, &s->address, in_addr) > 0 && s->ifindex == ifindex)
                        return s;

        return NULL;
}

DnsServer *manager_get_first_dns_server(Manager *m, DnsServerType t) {
        assert(m);

        switch (t) {

        case DNS_SERVER_SYSTEM:
                return m->dns_servers;

        case DNS_SERVER_FALLBACK:
                return m->fallback_dns_servers;

        default:
                return NULL;
        }
}

DnsServer *manager_set_dns_server(Manager *m, DnsServer *s) {
        assert(m);

        if (m->current_dns_server == s)
                return s;

        if (s)
                log_debug("Switching to %s DNS server %s.",
                          dns_server_type_to_string(s->type),
                          dns_server_string(s));

        dns_server_unref(m->current_dns_server);
        m->current_dns_server = dns_server_ref(s);

        if (m->unicast_scope)
                dns_cache_flush(&m->unicast_scope->cache);

        return s;
}

DnsServer *manager_get_dns_server(Manager *m) {
        Link *l;
        assert(m);

        /* Try to read updates resolv.conf */
        manager_read_resolv_conf(m);

        /* If no DNS server was chosen so far, pick the first one */
        if (!m->current_dns_server)
                manager_set_dns_server(m, m->dns_servers);

        if (!m->current_dns_server) {
                bool found = false;
                Iterator i;

                /* No DNS servers configured, let's see if there are
                 * any on any links. If not, we use the fallback
                 * servers */

                HASHMAP_FOREACH(l, m->links, i)
                        if (l->dns_servers) {
                                found = true;
                                break;
                        }

                if (!found)
                        manager_set_dns_server(m, m->fallback_dns_servers);
        }

        return m->current_dns_server;
}

void manager_next_dns_server(Manager *m) {
        assert(m);

        /* If there's currently no DNS server set, then the next
         * manager_get_dns_server() will find one */
        if (!m->current_dns_server)
                return;

        /* Change to the next one, but make sure to follow the linked
         * list only if the server is still linked. */
        if (m->current_dns_server->linked && m->current_dns_server->servers_next) {
                manager_set_dns_server(m, m->current_dns_server->servers_next);
                return;
        }

        /* If there was no next one, then start from the beginning of
         * the list */
        if (m->current_dns_server->type == DNS_SERVER_FALLBACK)
                manager_set_dns_server(m, m->fallback_dns_servers);
        else
                manager_set_dns_server(m, m->dns_servers);
}

bool dns_server_address_valid(int family, const union in_addr_union *sa) {

        /* Refuses the 0 IP addresses as well as 127.0.0.53 (which is our own DNS stub) */

        if (in_addr_is_null(family, sa))
                return false;

        if (family == AF_INET && sa->in.s_addr == htobe32(INADDR_DNS_STUB))
                return false;

        return true;
}

DnssecMode dns_server_get_dnssec_mode(DnsServer *s) {
        assert(s);

        if (s->link)
                return link_get_dnssec_mode(s->link);

        return manager_get_dnssec_mode(s->manager);
}

DnsOverTlsMode dns_server_get_dns_over_tls_mode(DnsServer *s) {
        assert(s);

        if (s->link)
                return link_get_dns_over_tls_mode(s->link);

        return manager_get_dns_over_tls_mode(s->manager);
}

void dns_server_flush_cache(DnsServer *s) {
        DnsServer *current;
        DnsScope *scope;

        assert(s);

        /* Flush the cache of the scope this server belongs to */

        current = s->link ? s->link->current_dns_server : s->manager->current_dns_server;
        if (current != s)
                return;

        scope = s->link ? s->link->unicast_scope : s->manager->unicast_scope;
        if (!scope)
                return;

        dns_cache_flush(&scope->cache);
}

void dns_server_reset_features(DnsServer *s) {
        assert(s);

        s->verified_feature_level = _DNS_SERVER_FEATURE_LEVEL_INVALID;
        s->possible_feature_level = DNS_SERVER_FEATURE_LEVEL_BEST;

        s->received_udp_packet_max = DNS_PACKET_UNICAST_SIZE_MAX;

        s->packet_bad_opt = false;
        s->packet_rrsig_missing = false;

        s->features_grace_period_usec = DNS_SERVER_FEATURE_GRACE_PERIOD_MIN_USEC;

        s->warned_downgrade = false;

        dns_server_reset_counters(s);

        /* Let's close the default stream, so that we reprobe with the new features */
        dns_server_unref_stream(s);
}

void dns_server_reset_features_all(DnsServer *s) {
        DnsServer *i;

        LIST_FOREACH(servers, i, s)
                dns_server_reset_features(i);
}

void dns_server_dump(DnsServer *s, FILE *f) {
        assert(s);

        if (!f)
                f = stdout;

        fputs("[Server ", f);
        fputs(dns_server_string(s), f);
        fputs(" type=", f);
        fputs(dns_server_type_to_string(s->type), f);

        if (s->type == DNS_SERVER_LINK) {
                assert(s->link);

                fputs(" interface=", f);
                fputs(s->link->name, f);
        }

        fputs("]\n", f);

        fputs("\tVerified feature level: ", f);
        fputs(strna(dns_server_feature_level_to_string(s->verified_feature_level)), f);
        fputc('\n', f);

        fputs("\tPossible feature level: ", f);
        fputs(strna(dns_server_feature_level_to_string(s->possible_feature_level)), f);
        fputc('\n', f);

        fputs("\tDNSSEC Mode: ", f);
        fputs(strna(dnssec_mode_to_string(dns_server_get_dnssec_mode(s))), f);
        fputc('\n', f);

        fputs("\tCan do DNSSEC: ", f);
        fputs(yes_no(dns_server_dnssec_supported(s)), f);
        fputc('\n', f);

        fprintf(f,
                "\tMaximum UDP packet size received: %zu\n"
                "\tFailed UDP attempts: %u\n"
                "\tFailed TCP attempts: %u\n"
                "\tSeen truncated packet: %s\n"
                "\tSeen OPT RR getting lost: %s\n"
                "\tSeen RRSIG RR missing: %s\n",
                s->received_udp_packet_max,
                s->n_failed_udp,
                s->n_failed_tcp,
                yes_no(s->packet_truncated),
                yes_no(s->packet_bad_opt),
                yes_no(s->packet_rrsig_missing));
}

void dns_server_unref_stream(DnsServer *s) {
        DnsStream *ref;

        assert(s);

        /* Detaches the default stream of this server. Some special care needs to be taken here, as that stream and
         * this server reference each other. First, take the stream out of the server. It's destructor will check if it
         * is registered with us, hence let's invalidate this separately, so that it is already unregistered. */
        ref = TAKE_PTR(s->stream);

        /* And then, unref it */
        dns_stream_unref(ref);
}

DnsScope *dns_server_scope(DnsServer *s) {
        assert(s);
        assert((s->type == DNS_SERVER_LINK) == !!s->link);

        if (s->link)
                return s->link->unicast_scope;

        return s->manager->unicast_scope;
}

static const char* const dns_server_type_table[_DNS_SERVER_TYPE_MAX] = {
        [DNS_SERVER_SYSTEM] = "system",
        [DNS_SERVER_FALLBACK] = "fallback",
        [DNS_SERVER_LINK] = "link",
};
DEFINE_STRING_TABLE_LOOKUP(dns_server_type, DnsServerType);

static const char* const dns_server_feature_level_table[_DNS_SERVER_FEATURE_LEVEL_MAX] = {
        [DNS_SERVER_FEATURE_LEVEL_TCP] = "TCP",
        [DNS_SERVER_FEATURE_LEVEL_UDP] = "UDP",
        [DNS_SERVER_FEATURE_LEVEL_EDNS0] = "UDP+EDNS0",
        [DNS_SERVER_FEATURE_LEVEL_TLS_PLAIN] = "TLS+EDNS0",
        [DNS_SERVER_FEATURE_LEVEL_DO] = "UDP+EDNS0+DO",
        [DNS_SERVER_FEATURE_LEVEL_LARGE] = "UDP+EDNS0+DO+LARGE",
        [DNS_SERVER_FEATURE_LEVEL_TLS_DO] = "TLS+EDNS0+D0",
};
DEFINE_STRING_TABLE_LOOKUP(dns_server_feature_level, DnsServerFeatureLevel);
