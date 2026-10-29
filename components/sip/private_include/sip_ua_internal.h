#pragma once

/* Introspection hooks for host tests only. */

#include "sip_ua.h"

/* Number of live transactions (to check that everything gets cleaned up). */
int sip_ua_debug_txn_count(const sip_ua_t *ua);
/* Number of occupied call slots (visible or still cleaning up). */
int sip_ua_debug_call_slots(const sip_ua_t *ua);
/* Current Contact address (after NAT fix-up). */
sip_addr_t sip_ua_debug_contact(const sip_ua_t *ua);
/* Number of occupied subscription slots (incl. ones being un-SUBSCRIBEd). */
int sip_ua_debug_sub_slots(const sip_ua_t *ua);
