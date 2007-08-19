/* packet-snmp.c
 * Routines for SNMP (simple network management protocol)
 * Copyright (C) 1998 Didier Jorand
 *
 * See RFC 1157 for SNMPv1.
 *
 * See RFCs 1901, 1905, and 1906 for SNMPv2c.
 *
 * See RFCs 1905, 1906, 1909, and 1910 for SNMPv2u [historic].
 *
 * See RFCs 2570-2576 for SNMPv3
 * Updated to use the asn2wrs compiler made by Tomas Kukosa
 * Copyright (C) 2005 - 2006 Anders Broman [AT] ericsson.com
 *
 * See RFC 3414 for User-based Security Model for SNMPv3
 * See RFC 3826 for  (AES) Cipher Algorithm in the SNMP USM
 * Copyright (C) 2007 Luis E. Garcia Ontanon <luis.ontanon@gmail.com>
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Some stuff from:
 *
 * GXSNMP -- An snmp mangament application
 * Copyright (C) 1998 Gregory McLean & Jochen Friedrich
 * Beholder RMON ethernet network monitor,Copyright (C) 1993 DNPAP group
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <glib.h>

#include <epan/packet.h>
#include <epan/strutil.h>
#include <epan/conversation.h>
#include "etypes.h"
#include <epan/prefs.h>
#include <epan/sminmpec.h>
#include <epan/emem.h>
#include <epan/next_tvb.h>
#include <epan/uat.h>
#include <epan/asn1.h>
#include "packet-ipx.h"
#include "packet-hpext.h"


#include "packet-ber.h"

#include "packet-snmp.h"
#include "format-oid.h"

#include <epan/crypt/crypt-sha1.h>
#include <epan/crypt/crypt-md5.h>
#include <epan/expert.h>
#include <epan/report_err.h>
#include <epan/oids.h>

#ifdef _WIN32
#include <winposixtype.h>
#endif /* _WIN32 */

#ifdef HAVE_LIBGCRYPT
#include <gcrypt.h>
#endif

/* Take a pointer that may be null and return a pointer that's not null
   by turning null pointers into pointers to the above null string,
   and, if the argument pointer wasn't null, make sure we handle
   non-printable characters in the string by escaping them. */
#define	SAFE_STRING(s, l)	(((s) != NULL) ? format_text((s), (l)) : "")

#define PNAME  "Simple Network Management Protocol"
#define PSNAME "SNMP"
#define PFNAME "snmp"

#define UDP_PORT_SNMP		161
#define UDP_PORT_SNMP_TRAP	162
#define TCP_PORT_SNMP		161
#define TCP_PORT_SNMP_TRAP	162
#define TCP_PORT_SMUX		199

/* Initialize the protocol and registered fields */
static int proto_snmp = -1;
static int proto_smux = -1;

/* Default MIB modules to load */
/*
 * XXX - According to Wes Hardaker, we shouldn't do this:
 *       http://www.ethereal.com/lists/ethereal-dev/200412/msg00222.html
 */
#ifdef _WIN32
# define DEF_MIB_MODULES "IP-MIB;IF-MIB;TCP-MIB;UDP-MIB;SNMPv2-MIB;RFC1213-MIB;UCD-SNMP-MIB"
# define IMPORT_SEPARATOR ":"
#else
# define DEF_MIB_MODULES "IP-MIB:IF-MIB:TCP-MIB:UDP-MIB:SNMPv2-MIB:RFC1213-MIB:UCD-SNMP-MIB"
# define IMPORT_SEPARATOR ";"
#endif /* _WIN32 */

static gboolean display_oid = TRUE;

static gboolean snmp_usm_auth_md5(snmp_usm_params_t* p, guint8**, guint*, gchar const**);
static gboolean snmp_usm_auth_sha1(snmp_usm_params_t* p, guint8**, guint*, gchar const**);

static tvbuff_t* snmp_usm_priv_des(snmp_usm_params_t*, tvbuff_t*, gchar const**);
static tvbuff_t* snmp_usm_priv_aes(snmp_usm_params_t*, tvbuff_t*, gchar const**);


static void snmp_usm_password_to_key_md5(const guint8 *password, guint passwordlen, const guint8 *engineID, guint engineLength, guint8 *key);
static void snmp_usm_password_to_key_sha1(const guint8 *password, guint passwordlen, const guint8 *engineID, guint engineLength, guint8 *key);


static snmp_usm_auth_model_t model_md5 = {snmp_usm_password_to_key_md5, snmp_usm_auth_md5, 16};
static snmp_usm_auth_model_t model_sha1 = {snmp_usm_password_to_key_sha1, snmp_usm_auth_sha1, 20};

static value_string auth_types[] = {
	{0,"MD5"},
	{1,"SHA1"},
	{0,NULL}
};
static snmp_usm_auth_model_t* auth_models[] = {&model_md5,&model_sha1};


static value_string priv_types[] = {
	{0,"DES"},
	{1,"AES"},
	{0,NULL}
};
static snmp_usm_decoder_t priv_protos[] = {snmp_usm_priv_des, snmp_usm_priv_aes};

static snmp_ue_assoc_t* ueas = NULL;
static guint num_ueas = 0;
static uat_t* assocs_uat = NULL;
static snmp_ue_assoc_t* localized_ues = NULL;
static snmp_ue_assoc_t* unlocalized_ues = NULL;
/****/



static snmp_usm_params_t usm_p = {FALSE,FALSE,0,0,0,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL,FALSE};

/* Subdissector tables */
static dissector_table_t variable_oid_dissector_table;

#define TH_AUTH   0x01
#define TH_CRYPT  0x02
#define TH_REPORT 0x04

/* desegmentation of SNMP-over-TCP */
static gboolean snmp_desegment = TRUE;

/* Global variables */

guint32 MsgSecurityModel;
tvbuff_t *oid_tvb=NULL;
tvbuff_t *value_tvb=NULL;

static dissector_handle_t snmp_handle;
static dissector_handle_t data_handle;

static next_tvb_list_t var_list;

static int hf_snmp_v3_flags_auth = -1;
static int hf_snmp_v3_flags_crypt = -1;
static int hf_snmp_v3_flags_report = -1;

static int hf_snmp_engineid_conform = -1;
static int hf_snmp_engineid_enterprise = -1;
static int hf_snmp_engineid_format = -1;
static int hf_snmp_engineid_ipv4 = -1;
static int hf_snmp_engineid_ipv6 = -1;
static int hf_snmp_engineid_mac = -1;
static int hf_snmp_engineid_text = -1;
static int hf_snmp_engineid_time = -1;
static int hf_snmp_engineid_data = -1;
static int hf_snmp_decryptedPDU = -1;
static int hf_snmp_msgAuthentication = -1;

static int hf_snmp_noSuchObject_value = -1;
static int hf_snmp_noSuchInstance_value = -1;
static int hf_snmp_endOfMibView_value = -1;
static int hf_snmp_integer32_value = -1;
static int hf_snmp_octestring_value = -1;
static int hf_snmp_oid_value = -1;
static int hf_snmp_null_value = -1;
static int hf_snmp_ipv4_value = -1;
static int hf_snmp_ipv6_value = -1;
static int hf_snmp_anyaddress_value = -1;
static int hf_snmp_unsigned32_value = -1;
static int hf_snmp_unknown_value = -1;
static int hf_snmp_opaque_value = -1;
static int hf_snmp_nsap_value = -1;
static int hf_snmp_counter_value = -1;
static int hf_snmp_timeticks_value = -1;
static int hf_snmp_arbitrary_value = -1;
static int hf_snmp_big_counter_value = -1;
static int hf_snmp_gauge32_value = -1;
static int hf_snmp_objectname = -1;

#include "packet-snmp-hf.c"

static int hf_smux_version = -1;
static int hf_smux_pdutype = -1;

/* Initialize the subtree pointers */
static gint ett_smux = -1;
static gint ett_snmp = -1;
static gint ett_engineid = -1;
static gint ett_msgFlags = -1;
static gint ett_encryptedPDU = -1;
static gint ett_decrypted = -1;
static gint ett_authParameters = -1;
static gint ett_internet = -1;
static gint ett_varbind = -1;
static gint ett_decoding_error = -1;

#include "packet-snmp-ett.c"

static const true_false_string auth_flags = {
	"OK",
	"Failed"
};

/* defined in net-SNMP; include/net-snmp/library/snmp.h */
#undef SNMP_MSG_GET
#undef SNMP_MSG_SET
#undef SNMP_MSG_GETNEXT
#undef SNMP_MSG_RESPONSE
#undef SNMP_MSG_TRAP
#undef SNMP_MSG_GETBULK
#undef SNMP_MSG_INFORM
#undef SNMP_MSG_TRAP2
#undef SNMP_MSG_REPORT
#undef SNMP_NOSUCHOBJECT
#undef SNMP_NOSUCHINSTANCE
#undef SNMP_ENDOFMIBVIEW

/* Security Models */

#define SNMP_SEC_ANY			0
#define SNMP_SEC_V1				1
#define SNMP_SEC_V2C			2
#define SNMP_SEC_USM			3

static const value_string sec_models[] = {
	{ SNMP_SEC_ANY,			"Any" },
	{ SNMP_SEC_V1,			"V1" },
	{ SNMP_SEC_V2C,			"V2C" },
	{ SNMP_SEC_USM,			"USM" },
	{ 0,				NULL }
};

/* SMUX PDU types */
#define SMUX_MSG_OPEN 		0
#define SMUX_MSG_CLOSE		1
#define SMUX_MSG_RREQ		2
#define SMUX_MSG_RRSP		3
#define SMUX_MSG_SOUT		4

static const value_string smux_types[] = {
	{ SMUX_MSG_OPEN,	"Open" },
	{ SMUX_MSG_CLOSE,	"Close" },
	{ SMUX_MSG_RREQ,	"Registration Request" },
	{ SMUX_MSG_RRSP,	"Registration Response" },
	{ SMUX_MSG_SOUT,	"Commit Or Rollback" },
	{ 0,			NULL }
};


#define SNMP_IPA    0		/* IP Address */
#define SNMP_CNT    1		/* Counter (Counter32) */
#define SNMP_GGE    2		/* Gauge (Gauge32) */
#define SNMP_TIT    3		/* TimeTicks */
#define SNMP_OPQ    4		/* Opaque */
#define SNMP_NSP    5		/* NsapAddress */
#define SNMP_C64    6		/* Counter64 */
#define SNMP_U32    7		/* Uinteger32 */

#define SERR_NSO    0
#define SERR_NSI    1
#define SERR_EOM    2

static int dissect_snmp_VarBind(gboolean implicit_tag _U_,
								tvbuff_t *tvb,
								int offset,
								asn1_ctx_t *actx,
								proto_tree *tree,
								int hf_index _U_) {
	int seq_offset, name_offset, value_offset, value_start;
	guint32 seq_len, name_len, value_len;
	gint8 ber_class;
	gboolean pc;
	gint32 tag;
	gboolean ind;
	guint8* oid_bytes;
	oid_info_t* oid_info;
	guint oid_matched, oid_left;
	proto_item *pi_varbind, *pi_value = NULL;
	proto_tree *pt;
	char* label;
	int hfid = -1;
	int min_len = 0, max_len = 0;

	seq_offset = offset;
	
	/* first we must read the VarBind's sequence header */
	offset = get_ber_identifier(tvb, offset, &ber_class, &pc, &tag);
	offset = get_ber_length(NULL, tvb, offset, &seq_len, &ind);
	
	seq_len += offset - seq_offset;
	
	if (!pc && ber_class==BER_CLASS_UNI && tag==BER_UNI_TAG_SEQUENCE) {
		proto_item* pi = proto_tree_add_text(tree, tvb, seq_offset, seq_len,"VarBind must be an universal class sequence");
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "VarBind is not an universal class sequence");
		return dissect_unknown_ber(actx->pinfo, tvb, seq_offset, pt);
	}
	
	if (ind){
		proto_item* pi = proto_tree_add_text(tree, tvb, seq_offset, seq_len,"Indicator must be clear in VarBind");
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "VarBind has indicator set");
		return dissect_unknown_ber(actx->pinfo, tvb, seq_offset, pt);
	}
	
	/* then we have the name's header */
	
	offset = get_ber_identifier(tvb, offset, &ber_class, &pc, &tag);
	name_offset = offset = get_ber_length(NULL, tvb, offset, &name_len, &ind);
	
	if (! ( !pc && ber_class==BER_CLASS_UNI && tag==BER_UNI_TAG_OID) ) {
		proto_item* pi = proto_tree_add_text(tree, tvb, seq_offset, seq_len,"ObjectName must be an OID in primitive encoding");
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "ObjectName not an OID");
		return dissect_unknown_ber(actx->pinfo, tvb, seq_offset, pt);
	}
	
	if (ind){
		proto_item* pi = proto_tree_add_text(tree, tvb, seq_offset, seq_len,"Indicator must be clear in ObjectName");
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "ObjectName has indicator set");
		return dissect_unknown_ber(actx->pinfo, tvb, seq_offset, pt);
	}
	
	offset += name_len;
	value_start = offset;
	offset = get_ber_identifier(tvb, offset, &ber_class, &pc, &tag);
	value_offset = offset = get_ber_length(NULL, tvb, offset, &value_len, &ind);
	
	if (! (!pc) ) {
		proto_item* pi = proto_tree_add_text(tree, tvb, seq_offset, seq_len,"the value must be in primitive encoding");
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "value not in primitive encoding");
		return dissect_unknown_ber(actx->pinfo, tvb, seq_offset, pt);
	}
	
	oid_bytes = ep_tvb_memdup(tvb, name_offset, name_len);
	oid_info = oid_get_from_encoded(oid_bytes, name_len, &oid_matched, &oid_left);
		
	if (oid_left == 1 && ber_class != BER_CLASS_CON && tag != BER_UNI_TAG_NULL) {
		if ((oid_info->value_type->ber_class != BER_CLASS_ANY) &&
			(ber_class != oid_info->value_type->ber_class))
			goto expected_different;
		
		if ((oid_info->value_type->ber_tag != BER_TAG_ANY) &&
			(tag != oid_info->value_type->ber_tag))
			goto expected_different;
		
		max_len = oid_info->value_type->max_len == -1 ? 0xffffff : oid_info->value_type->max_len;
		min_len  = oid_info->value_type->min_len;
		
		if ((int)value_len < min_len || (int)value_len > max_len)
			goto expected_other_size;

		pi_varbind = proto_tree_add_item(tree,oid_info->value_hfid,tvb,value_offset,value_len,FALSE);
	} else {
		pi_varbind = proto_tree_add_text(tree,tvb,seq_offset,seq_len,"VarBind");
	}
	
	pt = proto_item_add_subtree(pi_varbind,ett_varbind);
	proto_tree_add_item(pt,hf_snmp_objectname,tvb,name_offset,name_len,FALSE);

	if (ber_class == BER_CLASS_CON) {
		if (value_len != 0)
			goto expected_other_size; 

		switch (tag) {
			case SERR_NSO:
				hfid = hf_snmp_noSuchObject_value;
				break;
			case SERR_NSI:
				hfid = hf_snmp_noSuchInstance_value;
				break;
			case SERR_EOM:
				hfid = hf_snmp_endOfMibView_value;
				break;
			default: {
				proto_item* pi = proto_tree_add_text(tree,tvb,0,0,"Wrong tag for Error Value: expected [0,1,2] got: %d",tag);
				pt = proto_item_add_subtree(pi,ett_decoding_error);
				expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "Wrong tag for SNMP VarBind error value");
				return dissect_unknown_ber(actx->pinfo, tvb, value_start, tree);
			}				
		}
	} else {
		
		switch(ber_class|(tag<<4)) {
			case BER_CLASS_UNI|(BER_UNI_TAG_INTEGER<<4):
				max_len = 4; min_len = 1;
				if (value_len > (guint)max_len && value_len < (guint)min_len) goto expected_other_size; 
				hfid = hf_snmp_integer32_value;
				break;
			case BER_CLASS_UNI|(BER_UNI_TAG_OCTETSTRING<<4):
				hfid = hf_snmp_octestring_value;
				break;
			case BER_CLASS_UNI|(BER_UNI_TAG_OID<<4):
				max_len = -1; min_len = 2;
				if (value_len < (guint)min_len) goto expected_other_size; 
				hfid = hf_snmp_oid_value;
				break;
			case BER_CLASS_UNI|(BER_UNI_TAG_NULL<<4):
				max_len = 0; min_len = 0;
				if (value_len != 0) goto expected_other_size; 
				hfid = hf_snmp_null_value;
				break;
			case BER_CLASS_APP: /* | (SNMP_IPA<<4)*/
				switch(value_len) {
					case 4: hfid = hf_snmp_ipv4_value; break;
					case 16: hfid = hf_snmp_ipv6_value; break;
					default: hfid = hf_snmp_anyaddress_value; break;
				}
				break;
			case BER_CLASS_APP|(SNMP_U32<<4):
				hfid = hf_snmp_unsigned32_value;
				break;
			case BER_CLASS_APP|(SNMP_GGE<<4):
				hfid = hf_snmp_gauge32_value;
				break;
			case BER_CLASS_APP|(SNMP_CNT<<4):
				hfid = hf_snmp_counter_value;
				break;
			case BER_CLASS_APP|(SNMP_TIT<<4):
				hfid = hf_snmp_timeticks_value;
				break;
			case BER_CLASS_APP|(SNMP_OPQ<<4):
				hfid = hf_snmp_opaque_value;
				break;
			case BER_CLASS_APP|(SNMP_NSP<<4):
				hfid = hf_snmp_nsap_value;
				break;
			case BER_CLASS_APP|(SNMP_C64<<4):
				hfid = hf_snmp_big_counter_value;
				break;
			default: 
				hfid = hf_snmp_unknown_value;
				break;
		}
	} 
	
	pi_value = proto_tree_add_item(pt,hfid,tvb,value_offset,value_len,FALSE);

	if (oid_left != 1) {
		const char* unres = oid_encoded2string(oid_bytes, name_len);
		char* repr = ep_alloc(ITEM_LABEL_LENGTH+1);
		
		proto_item_fill_label(pi_value->finfo, repr);
		repr = strstr(repr,": ");
		
		label = g_strdup_printf("%s: %s",unres,repr);
		proto_item_set_text(pi_varbind,label);

	}


	return seq_offset + seq_len;

expected_other_size: {
		proto_item* pi = proto_tree_add_text(tree,tvb,0,0,"Wrong value length: %u  expecting: %u <= len <= %u",
											 value_len,
											 min_len,
											 max_len == -1 ? 0xFFFFFF : max_len);
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "Wrong length for SNMP VarBind/value");
		return dissect_unknown_ber(actx->pinfo, tvb, value_start, pt);
	}

expected_different: {
		proto_item* pi = proto_tree_add_text(tree,tvb,0,0,"Wrong class/tag for Value expected: %d,%d got: %d,%d",
											 oid_info->value_type->ber_class,
											 oid_info->value_type->ber_tag,
											 ber_class,
											 tag);
		pt = proto_item_add_subtree(pi,ett_decoding_error);
		expert_add_info_format(actx->pinfo, pi, PI_MALFORMED, PI_WARN, "Wrong class/tag for SNMP VarBind/value");
		return dissect_unknown_ber(actx->pinfo, tvb, value_start, tree);
	}

}


gchar* format_oid(subid_t *oid, guint oid_length) {
	return (void*)oid_resolved(oid_length,oid);
}

void new_format_oid(subid_t *oid, guint oid_length, gchar **non_decoded, gchar **decoded) {
	*decoded = (void*)oid_resolved(oid_length,oid);
	*non_decoded = (void*)oid_subid2string(oid,oid_length);
}


#define F_SNMP_ENGINEID_CONFORM 0x80
#define SNMP_ENGINEID_RFC1910 0x00
#define SNMP_ENGINEID_RFC3411 0x01

static const true_false_string tfs_snmp_engineid_conform = {
  "RFC3411 (SNMPv3)",
  "RFC1910 (Non-SNMPv3)"
};

#define SNMP_ENGINEID_FORMAT_IPV4 0x01
#define SNMP_ENGINEID_FORMAT_IPV6 0x02
#define SNMP_ENGINEID_FORMAT_MACADDRESS 0x03
#define SNMP_ENGINEID_FORMAT_TEXT 0x04
#define SNMP_ENGINEID_FORMAT_OCTETS 0x05

static const value_string snmp_engineid_format_vals[] = {
	{ SNMP_ENGINEID_FORMAT_IPV4,	"IPv4 address" },
	{ SNMP_ENGINEID_FORMAT_IPV6,	"IPv6 address" },
	{ SNMP_ENGINEID_FORMAT_MACADDRESS,	"MAC address" },
	{ SNMP_ENGINEID_FORMAT_TEXT,	"Text, administratively assigned" },
	{ SNMP_ENGINEID_FORMAT_OCTETS,	"Octets, administratively assigned" },
	{ 0,   	NULL }
};

/*
 * SNMP Engine ID dissection according to RFC 3411 (SnmpEngineID TC)
 * or historic RFC 1910 (AgentID)
 */
int
dissect_snmp_engineid(proto_tree *tree, tvbuff_t *tvb, int offset, int len)
{
    proto_item *item = NULL;
    guint8 conformance, format;
    guint32 enterpriseid, seconds;
    nstime_t ts;
    int len_remain = len;

    /* first bit: engine id conformance */
    if (len_remain<4) return offset;
    conformance = ((tvb_get_guint8(tvb, offset)>>7) && 0x01);
    proto_tree_add_item(tree, hf_snmp_engineid_conform, tvb, offset, 1, FALSE);

    /* 4-byte enterprise number/name */
    if (len_remain<4) return offset;
    enterpriseid = tvb_get_ntohl(tvb, offset);
    if (conformance)
      enterpriseid -= 0x80000000; /* ignore first bit */
    proto_tree_add_uint(tree, hf_snmp_engineid_enterprise, tvb, offset, 4, enterpriseid);
    offset+=4;
    len_remain-=4;

    switch(conformance) {

    case SNMP_ENGINEID_RFC1910:
      /* 12-byte AgentID w/ 8-byte trailer */
      if (len_remain==8) {
	proto_tree_add_text(tree, tvb, offset, 8, "AgentID Trailer: 0x%s",
			    tvb_bytes_to_str(tvb, offset, 8));
	offset+=8;
	len_remain-=8;
      } else {
	proto_tree_add_text(tree, tvb, offset, len_remain, "<Data not conforming to RFC1910>");
	return offset;
      }
      break;

    case SNMP_ENGINEID_RFC3411: /* variable length: 5..32 */

      /* 1-byte format specifier */
      if (len_remain<1) return offset;
      format = tvb_get_guint8(tvb, offset);
      item = proto_tree_add_uint_format(tree, hf_snmp_engineid_format, tvb, offset, 1, format, "Engine ID Format: %s (%d)",
			  val_to_str(format, snmp_engineid_format_vals, "Reserved/Enterprise-specific"), format);
      offset+=1;
      len_remain-=1;

      switch(format) {
      case SNMP_ENGINEID_FORMAT_IPV4:
	/* 4-byte IPv4 address */
	if (len_remain==4) {
	  proto_tree_add_item(tree, hf_snmp_engineid_ipv4, tvb, offset, 4, FALSE);
	  offset+=4;
	  len_remain=0;
	}
	break;
      case SNMP_ENGINEID_FORMAT_IPV6:
	/* 16-byte IPv6 address */
	if (len_remain==16) {
	  proto_tree_add_item(tree, hf_snmp_engineid_ipv6, tvb, offset, 16, FALSE);
	  offset+=16;
	  len_remain=0;
	}
	break;
      case SNMP_ENGINEID_FORMAT_MACADDRESS:
	/* 6-byte MAC address */
	if (len_remain==6) {
	  proto_tree_add_item(tree, hf_snmp_engineid_mac, tvb, offset, 6, FALSE);
	  offset+=6;
	  len_remain=0;
	}
	break;
      case SNMP_ENGINEID_FORMAT_TEXT:
	/* max. 27-byte string, administratively assigned */
	if (len_remain<=27) {
	  proto_tree_add_item(tree, hf_snmp_engineid_text, tvb, offset, len_remain, FALSE);
	  offset+=len_remain;
	  len_remain=0;
	}
	break;
      case 128:
	/* most common enterprise-specific format: (ucd|net)-snmp random */
	if ((enterpriseid==2021)||(enterpriseid==8072)) {
	  proto_item_append_text(item, (enterpriseid==2021) ? ": UCD-SNMP Random" : ": Net-SNMP Random");
	  /* demystify: 4B random, 4B epoch seconds */
	  if (len_remain==8) {
	    proto_tree_add_item(tree, hf_snmp_engineid_data, tvb, offset, 4, FALSE);
	    seconds = tvb_get_letohl(tvb, offset+4);
	    ts.secs = seconds;
	    proto_tree_add_time_format(tree, hf_snmp_engineid_time, tvb, offset+4, 4,
                                  &ts, "Engine ID Data: Creation Time: %s",
                                  abs_time_secs_to_str(seconds));
	    offset+=8;
	    len_remain=0;
	  }
	}
	break;
      case SNMP_ENGINEID_FORMAT_OCTETS:
      default:
	/* max. 27 bytes, administratively assigned or unknown format */
	if (len_remain<=27) {
	  proto_tree_add_item(tree, hf_snmp_engineid_data, tvb, offset, len_remain, FALSE);
	  offset+=len_remain;
	  len_remain=0;
	}
	break;
      }
    }

    if (len_remain>0) {
      proto_tree_add_text(tree, tvb, offset, len_remain, "<Data not conforming to RFC3411>");
      offset+=len_remain;
    }
    return offset;
}


static void set_ue_keys(snmp_ue_assoc_t* n ) {
	guint key_size = n->user.authModel->key_size;

	n->user.authKey.data = se_alloc(key_size);
	n->user.authKey.len = key_size;
	n->user.authModel->pass2key(n->user.authPassword.data,
								n->user.authPassword.len,
								n->engine.data,
								n->engine.len,
								n->user.authKey.data);

	n->user.privKey.data = se_alloc(key_size);
	n->user.privKey.len = key_size;
	n->user.authModel->pass2key(n->user.privPassword.data,
								n->user.privPassword.len,
								n->engine.data,
								n->engine.len,
								n->user.privKey.data);
}

static snmp_ue_assoc_t* ue_se_dup(snmp_ue_assoc_t* o) {
	snmp_ue_assoc_t* d = se_memdup(o,sizeof(snmp_ue_assoc_t));

	d->user.authModel = o->user.authModel;

	d->user.privProtocol = o->user.privProtocol;

	d->user.userName.data = se_memdup(o->user.userName.data,o->user.userName.len);
	d->user.userName.len = o->user.userName.len;

	d->user.authPassword.data = o->user.authPassword.data ? se_memdup(o->user.authPassword.data,o->user.authPassword.len) : NULL;
	d->user.authPassword.len = o->user.authPassword.len;

	d->user.privPassword.data = o->user.privPassword.data ? se_memdup(o->user.privPassword.data,o->user.privPassword.len) : NULL;
	d->user.privPassword.len = o->user.privPassword.len;

	d->engine.len = o->engine.len;

	if (d->engine.len) {
		d->engine.data = se_memdup(o->engine.data,o->engine.len);
		set_ue_keys(d);
	}

	return d;

}


#define CACHE_INSERT(c,a) if (c) { snmp_ue_assoc_t* t = c; c = a; c->next = t; } else { c = a; a->next = NULL; }

static void renew_ue_cache(void) {
	if (num_ueas) {
		guint i;

		localized_ues = NULL;
		unlocalized_ues = NULL;

		for(i = 0; i < num_ueas; i++) {
			snmp_ue_assoc_t* a = ue_se_dup(&(ueas[i]));

			if (a->engine.len) {
				CACHE_INSERT(localized_ues,a);

			} else {
				CACHE_INSERT(unlocalized_ues,a);
			}

		}
	} else {
		localized_ues = NULL;
		unlocalized_ues = NULL;
	}
}


static snmp_ue_assoc_t* localize_ue( snmp_ue_assoc_t* o, const guint8* engine, guint engine_len ) {
	snmp_ue_assoc_t* n = se_memdup(o,sizeof(snmp_ue_assoc_t));

	n->engine.data = se_memdup(engine,engine_len);
	n->engine.len = engine_len;

	set_ue_keys(n);

	return n;
}


#define localized_match(a,u,ul,e,el) \
	( a->user.userName.len == ul \
	&& a->engine.len == el \
	&& memcmp( a->user.userName.data, u, (a->user.userName.len < ul) ? a->user.userName.len : ul ) == 0 \
	&& memcmp( a->engine.data,   e, (a->engine.len   < el) ? a->engine.len   : el ) == 0 )

#define unlocalized_match(a,u,l) \
	( a->user.userName.len == l && memcmp( a->user.userName.data, u, a->user.userName.len < l ? a->user.userName.len : l) == 0 )

static snmp_ue_assoc_t* get_user_assoc(tvbuff_t* engine_tvb, tvbuff_t* user_tvb) {
	static snmp_ue_assoc_t* a;
	guint given_username_len;
	guint8* given_username;
	guint given_engine_len;
	guint8* given_engine;

	if ( ! (localized_ues || unlocalized_ues ) ) return NULL;

	if (! ( user_tvb && engine_tvb ) ) return NULL;

	given_username_len = tvb_length_remaining(user_tvb,0);
	given_username = ep_tvb_memdup(user_tvb,0,-1);
	given_engine_len = tvb_length_remaining(engine_tvb,0);
	given_engine = ep_tvb_memdup(engine_tvb,0,-1);

	for (a = localized_ues; a; a = a->next) {
		if ( localized_match(a, given_username, given_username_len, given_engine, given_engine_len) ) {
			return a;
		}
	}

	for (a = unlocalized_ues; a; a = a->next) {
		if ( unlocalized_match(a, given_username, given_username_len) ) {
			snmp_ue_assoc_t* n = localize_ue( a, given_engine, given_engine_len );
			CACHE_INSERT(localized_ues,n);
			return n;
		}
	}

	return NULL;
}

static gboolean snmp_usm_auth_md5(snmp_usm_params_t* p, guint8** calc_auth_p, guint* calc_auth_len_p, gchar const** error) {
	guint msg_len;
	guint8* msg;
	guint auth_len;
	guint8* auth;
	guint8* key;
	guint key_len;
	guint8 calc_auth[16];
	guint start;
	guint end;
	guint i;

	if (!p->auth_tvb) {
		*error = "No Authenticator";
		return FALSE;
	}

	key = p->user_assoc->user.authKey.data;
	key_len = p->user_assoc->user.authKey.len;

	if (! key ) {
		*error = "User has no authKey";
		return FALSE;
	}


	auth_len = tvb_length_remaining(p->auth_tvb,0);

	if (auth_len != 12) {
		*error = "Authenticator length wrong";
		return FALSE;
	}

	msg_len = tvb_length_remaining(p->msg_tvb,0);
	msg = ep_tvb_memdup(p->msg_tvb,0,msg_len);


	auth = ep_tvb_memdup(p->auth_tvb,0,auth_len);

	start = p->auth_offset - p->start_offset;
	end = 	start + auth_len;

	/* fill the authenticator with zeros */
	for ( i = start ; i < end ; i++ ) {
		msg[i] = '\0';
	}

	md5_hmac(msg, msg_len, key, key_len, calc_auth);

	if (calc_auth_p) *calc_auth_p = calc_auth;
	if (calc_auth_len_p) *calc_auth_len_p = 12;

	return ( memcmp(auth,calc_auth,12) != 0 ) ? FALSE : TRUE;
}


static gboolean snmp_usm_auth_sha1(snmp_usm_params_t* p _U_, guint8** calc_auth_p, guint* calc_auth_len_p,  gchar const** error _U_) {
	guint msg_len;
	guint8* msg;
	guint auth_len;
	guint8* auth;
	guint8* key;
	guint key_len;
	guint8 calc_auth[20];
	guint start;
	guint end;
	guint i;

	if (!p->auth_tvb) {
		*error = "No Authenticator";
		return FALSE;
	}

	key = p->user_assoc->user.authKey.data;
	key_len = p->user_assoc->user.authKey.len;

	if (! key ) {
		*error = "User has no authKey";
		return FALSE;
	}


	auth_len = tvb_length_remaining(p->auth_tvb,0);


	if (auth_len != 12) {
		*error = "Authenticator length wrong";
		return FALSE;
	}

	msg_len = tvb_length_remaining(p->msg_tvb,0);
	msg = ep_tvb_memdup(p->msg_tvb,0,msg_len);

	auth = ep_tvb_memdup(p->auth_tvb,0,auth_len);

	start = p->auth_offset - p->start_offset;
	end = 	start + auth_len;

	/* fill the authenticator with zeros */
	for ( i = start ; i < end ; i++ ) {
		msg[i] = '\0';
	}

	sha1_hmac(key, key_len, msg, msg_len, calc_auth);

	if (calc_auth_p) *calc_auth_p = calc_auth;
	if (calc_auth_len_p) *calc_auth_len_p = 12;

	return ( memcmp(auth,calc_auth,12) != 0 ) ? FALSE : TRUE;
}

static tvbuff_t* snmp_usm_priv_des(snmp_usm_params_t* p _U_, tvbuff_t* encryptedData _U_, gchar const** error _U_) {
#ifdef HAVE_LIBGCRYPT
    gcry_error_t err;
    gcry_cipher_hd_t hd = NULL;

	guint8* cleartext;
	guint8* des_key = p->user_assoc->user.privKey.data; /* first 8 bytes */
	guint8* pre_iv = &(p->user_assoc->user.privKey.data[8]); /* last 8 bytes */
	guint8* salt;
	gint salt_len;
	gint cryptgrm_len;
	guint8* cryptgrm;
	tvbuff_t* clear_tvb;
	guint8 iv[8];
	guint i;


	salt_len = tvb_length_remaining(p->priv_tvb,0);

	if (salt_len != 8)  {
		*error = "decryptionError: msgPrivacyParameters length != 8";
		return NULL;
	}

	salt = ep_tvb_memdup(p->priv_tvb,0,salt_len);

	/*
	 The resulting "salt" is XOR-ed with the pre-IV to obtain the IV.
	 */
	for (i=0; i<8; i++) {
		iv[i] = pre_iv[i] ^ salt[i];
	}

	cryptgrm_len = tvb_length_remaining(encryptedData,0);

	if (cryptgrm_len % 8) {
		*error = "decryptionError: the length of the encrypted data is not a mutiple of 8 octets";
		return NULL;
	}

	cryptgrm = ep_tvb_memdup(encryptedData,0,-1);

	cleartext = ep_alloc(cryptgrm_len);

	err = gcry_cipher_open(&hd, GCRY_CIPHER_DES, GCRY_CIPHER_MODE_CBC, 0);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

    err = gcry_cipher_setiv(hd, iv, 8);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	err = gcry_cipher_setkey(hd,des_key,8);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	err = gcry_cipher_decrypt(hd, cleartext, cryptgrm_len, cryptgrm, cryptgrm_len);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	gcry_cipher_close(hd);

	clear_tvb = tvb_new_real_data(cleartext, cryptgrm_len, cryptgrm_len);

	return clear_tvb;

on_gcry_error:
	*error = (void*)gpg_strerror(err);
	if (hd) gcry_cipher_close(hd);
	return NULL;
#else
	*error = "libgcrypt not present, cannot decrypt";
	return NULL;
#endif
}

static tvbuff_t* snmp_usm_priv_aes(snmp_usm_params_t* p _U_, tvbuff_t* encryptedData _U_, gchar const** error _U_) {
#ifdef HAVE_LIBGCRYPT
    gcry_error_t err;
    gcry_cipher_hd_t hd = NULL;

	guint8* cleartext;
	guint8* aes_key = p->user_assoc->user.privKey.data; /* first 16 bytes */
	guint8 iv[16];
	gint priv_len;
	gint cryptgrm_len;
	guint8* cryptgrm;
	tvbuff_t* clear_tvb;

	priv_len = tvb_length_remaining(p->priv_tvb,0);

	if (priv_len != 8)  {
		*error = "decryptionError: msgPrivacyParameters length != 8";
		return NULL;
	}

	iv[0] = (p->boots & 0xff000000) >> 24;
	iv[1] = (p->boots & 0x00ff0000) >> 16;
	iv[2] = (p->boots & 0x0000ff00) >> 8;
	iv[3] = (p->boots & 0x000000ff);
	iv[4] = (p->time & 0xff000000) >> 24;
	iv[5] = (p->time & 0x00ff0000) >> 16;
	iv[6] = (p->time & 0x0000ff00) >> 8;
	iv[7] = (p->time & 0x000000ff);
	tvb_memcpy(p->priv_tvb,&(iv[8]),0,8);

	cryptgrm_len = tvb_length_remaining(encryptedData,0);
	cryptgrm = ep_tvb_memdup(encryptedData,0,-1);

	cleartext = ep_alloc(cryptgrm_len);

	err = gcry_cipher_open(&hd, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CFB, 0);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

    err = gcry_cipher_setiv(hd, iv, 16);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	err = gcry_cipher_setkey(hd,aes_key,16);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	err = gcry_cipher_decrypt(hd, cleartext, cryptgrm_len, cryptgrm, cryptgrm_len);
	if (err != GPG_ERR_NO_ERROR) goto on_gcry_error;

	gcry_cipher_close(hd);

	clear_tvb = tvb_new_real_data(cleartext, cryptgrm_len, cryptgrm_len);

	return clear_tvb;

on_gcry_error:
	*error = (void*)gpg_strerror(err);
	if (hd) gcry_cipher_close(hd);
	return NULL;
#else
	*error = "libgcrypt not present, cannot decrypt";
	return NULL;
#endif
}


gboolean check_ScopedPdu(tvbuff_t* tvb) {
	int offset;
	gint8 class;
	gboolean pc;
	gint32 tag;
	int hoffset, eoffset;
	guint32 len;

	offset = get_ber_identifier(tvb, 0, &class, &pc, &tag);
	offset = get_ber_length(NULL, tvb, offset, NULL, NULL);

	if ( ! (((class!=BER_CLASS_APP) && (class!=BER_CLASS_PRI) )
			&& ( (!pc) || (class!=BER_CLASS_UNI) || (tag!=BER_UNI_TAG_ENUMERATED) )
			)) return FALSE;

	if((tvb_get_guint8(tvb, offset)==0)&&(tvb_get_guint8(tvb, offset+1)==0))
		return TRUE;

	hoffset = offset;

	offset = get_ber_identifier(tvb, offset, &class, &pc, &tag);
	offset = get_ber_length(NULL, tvb, offset, &len, NULL);
	eoffset = offset + len;

	if (eoffset <= hoffset) return FALSE;

	if ((class!=BER_CLASS_APP)&&(class!=BER_CLASS_PRI))
		if( (class!=BER_CLASS_UNI)
			||((tag<BER_UNI_TAG_NumericString)&&(tag!=BER_UNI_TAG_OCTETSTRING)&&(tag!=BER_UNI_TAG_UTF8String)) )
			return FALSE;

	return TRUE;

}

#include "packet-snmp-fn.c"


guint
dissect_snmp_pdu(tvbuff_t *tvb, int offset, packet_info *pinfo,
    proto_tree *tree, int proto, gint ett, gboolean is_tcp)
{

	guint length_remaining;
	gint8 class;
	gboolean pc, ind = 0;
	gint32 tag;
	guint32 len;
	guint message_length;
	int start_offset = offset;
	guint32 version = 0;

	proto_tree *snmp_tree = NULL;
	proto_item *item = NULL;
	asn1_ctx_t asn1_ctx;
	asn1_ctx_init(&asn1_ctx, ASN1_ENC_BER, TRUE, pinfo);


	usm_p.msg_tvb = tvb;
	usm_p.start_offset = offset_from_real_beginning(tvb,0) ;
	usm_p.engine_tvb = NULL;
	usm_p.user_tvb = NULL;
	usm_p.auth_item = NULL;
	usm_p.auth_tvb = NULL;
	usm_p.auth_offset = 0;
	usm_p.priv_tvb = NULL;
	usm_p.user_assoc = NULL;
	usm_p.authenticated = FALSE;
	usm_p.encrypted = FALSE;
	usm_p.boots = 0;
	usm_p.time = 0;
	usm_p.authOK = FALSE;

	/*
	 * This will throw an exception if we don't have any data left.
	 * That's what we want.  (See "tcp_dissect_pdus()", which is
	 * similar, but doesn't have to deal with ASN.1.
	 * XXX - can we make "tcp_dissect_pdus()" provide enough
	 * information to the "get_pdu_len" routine so that we could
	 * have that routine deal with ASN.1, and just use
	 * "tcp_dissect_pdus()"?)
	 */
	length_remaining = tvb_ensure_length_remaining(tvb, offset);

	/* NOTE: we have to parse the message piece by piece, since the
	 * capture length may be less than the message length: a 'global'
	 * parsing is likely to fail.
	 */

	/*
	 * If this is SNMP-over-TCP, we might have to do reassembly
	 * in order to read the "Sequence Of" header.
	 */
	if (is_tcp && snmp_desegment && pinfo->can_desegment) {
		/*
		 * This is TCP, and we should, and can, do reassembly.
		 *
		 * Is the "Sequence Of" header split across segment
		 * boundaries?  We requre at least 6 bytes for the
		 * header, which allows for a 4-byte length (ASN.1
		 * BER).
		 */
		if (length_remaining < 6) {
			pinfo->desegment_offset = offset;
			pinfo->desegment_len = 6 - length_remaining;

			/*
			 * Return 0, which means "I didn't dissect anything
			 * because I don't have enough data - we need
			 * to desegment".
			 */
			return 0;
		}
	}

	/*
	 * OK, try to read the "Sequence Of" header; this gets the total
	 * length of the SNMP message.
	 */
	/* Set tree to 0 to not display internakl BER fields if option used.*/
	offset = dissect_ber_identifier(pinfo, 0, tvb, offset, &class, &pc, &tag);
	offset = dissect_ber_length(pinfo, 0, tvb, offset, &len, &ind);

	message_length = len + 2;
	offset = dissect_ber_integer(FALSE, &asn1_ctx, 0, tvb, offset, -1, &version);


	/*
	 * If this is SNMP-over-TCP, we might have to do reassembly
	 * to get all of this message.
	 */
	if (is_tcp && snmp_desegment && pinfo->can_desegment) {
		/*
		 * Yes - is the message split across segment boundaries?
		 */
		if (length_remaining < message_length) {
			/*
			 * Yes.  Tell the TCP dissector where the data
			 * for this message starts in the data it handed
			 * us, and how many more bytes we need, and
			 * return.
			 */
			pinfo->desegment_offset = start_offset;
			pinfo->desegment_len =
			    message_length - length_remaining;

			/*
			 * Return 0, which means "I didn't dissect anything
			 * because I don't have enough data - we need
			 * to desegment".
			 */
			return 0;
		}
	}

	next_tvb_init(&var_list);

	if (check_col(pinfo->cinfo, COL_PROTOCOL)) {
		col_set_str(pinfo->cinfo, COL_PROTOCOL,
		    proto_get_protocol_short_name(find_protocol_by_id(proto)));
	}

	if (tree) {
		item = proto_tree_add_item(tree, proto, tvb, offset,
		    message_length, FALSE);
		snmp_tree = proto_item_add_subtree(item, ett);
	}

	switch (version){
	case 0: /* v1 */
	case 1: /* v2c */
		offset = dissect_snmp_Message(FALSE , tvb, start_offset, &asn1_ctx, snmp_tree, -1);
		break;
	case 2: /* v2u */
		offset = dissect_snmp_Messagev2u(FALSE , tvb, start_offset, &asn1_ctx, snmp_tree, -1);
		break;
			/* v3 */
	case 3:
		offset = dissect_snmp_SNMPv3Message(FALSE , tvb, start_offset, &asn1_ctx, snmp_tree, -1);
		break;
	default:
		/*
		 * Return the length remaining in the tvbuff, so
		 * if this is SNMP-over-TCP, our caller thinks there's
		 * nothing left to dissect.
		 */
		proto_tree_add_text(snmp_tree, tvb, offset, -1,"Unknown version");
		return length_remaining;
		break;
	}

	next_tvb_call(&var_list, pinfo, tree, NULL, data_handle);

	return offset;
}

static gint
dissect_snmp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	conversation_t  *conversation;
	int offset;
	gint8 tmp_class;
	gboolean tmp_pc;
	gint32 tmp_tag;
	guint32 tmp_length;
	gboolean tmp_ind;

	/*
	 * See if this looks like SNMP or not. if not, return 0 so
	 * wireshark can try som other dissector instead.
	 */
	/* All SNMP packets are BER encoded and consist of a SEQUENCE
	 * that spans the entire PDU. The first item is an INTEGER that
	 * has the values 0-2 (version 1-3).
	 * if not it is not snmp.
	 */
	/* SNMP starts with a SEQUENCE */
	offset = get_ber_identifier(tvb, 0, &tmp_class, &tmp_pc, &tmp_tag);
	if((tmp_class!=BER_CLASS_UNI)||(tmp_tag!=BER_UNI_TAG_SEQUENCE)){
		return 0;
	}
	/* then comes a length which spans the rest of the tvb */
	offset = get_ber_length(NULL, tvb, offset, &tmp_length, &tmp_ind);
	if(tmp_length!=(guint32)tvb_reported_length_remaining(tvb, offset)){
		return 0;
	}
	/* then comes an INTEGER (version)*/
	offset = get_ber_identifier(tvb, offset, &tmp_class, &tmp_pc, &tmp_tag);
	if((tmp_class!=BER_CLASS_UNI)||(tmp_tag!=BER_UNI_TAG_INTEGER)){
		return 0;
	}
	/* do we need to test that version is 0 - 2 (version1-3) ? */


	/*
	 * The first SNMP packet goes to the SNMP port; the second one
	 * may come from some *other* port, but goes back to the same
	 * IP address and port as the ones from which the first packet
	 * came; all subsequent packets presumably go between those two
	 * IP addresses and ports.
	 *
	 * If this packet went to the SNMP port, we check to see if
	 * there's already a conversation with one address/port pair
	 * matching the source IP address and port of this packet,
	 * the other address matching the destination IP address of this
	 * packet, and any destination port.
	 *
	 * If not, we create one, with its address 1/port 1 pair being
	 * the source address/port of this packet, its address 2 being
	 * the destination address of this packet, and its port 2 being
	 * wildcarded, and give it the SNMP dissector as a dissector.
	 */
	if (pinfo->destport == UDP_PORT_SNMP) {
	  conversation = find_conversation(pinfo->fd->num, &pinfo->src, &pinfo->dst, PT_UDP,
					   pinfo->srcport, 0, NO_PORT_B);
	  if( (conversation == NULL) || (conversation->dissector_handle!=snmp_handle) ){
	    conversation = conversation_new(pinfo->fd->num, &pinfo->src, &pinfo->dst, PT_UDP,
					    pinfo->srcport, 0, NO_PORT2);
	    conversation_set_dissector(conversation, snmp_handle);
	  }
	}

	return dissect_snmp_pdu(tvb, 0, pinfo, tree, proto_snmp, ett_snmp, FALSE);
}
static void
dissect_snmp_tcp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	int offset = 0;
	guint message_len;

	while (tvb_reported_length_remaining(tvb, offset) > 0) {
		message_len = dissect_snmp_pdu(tvb, 0, pinfo, tree,
		    proto_snmp, ett_snmp, TRUE);
		if (message_len == 0) {
			/*
			 * We don't have all the data for that message,
			 * so we need to do desegmentation;
			 * "dissect_snmp_pdu()" has set that up.
			 */
			break;
		}
		offset += message_len;
	}
}

static void
dissect_smux(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	proto_tree *smux_tree = NULL;
	proto_item *item = NULL;

	if (check_col(pinfo->cinfo, COL_PROTOCOL))
		col_set_str(pinfo->cinfo, COL_PROTOCOL, "SMUX");

	if (tree) {
		item = proto_tree_add_item(tree, proto_smux, tvb, 0, -1, FALSE);
		smux_tree = proto_item_add_subtree(item, ett_smux);
	}

	dissect_SMUX_PDUs_PDU(tvb, pinfo, tree);
}


/*
  MD5 Password to Key Algorithm
  from RFC 3414 A.2.1
*/
static void snmp_usm_password_to_key_md5(const guint8 *password,
								  guint   passwordlen,
								  const guint8 *engineID,
								  guint   engineLength,
								  guint8 *key)  {
	md5_state_t     MD;
	guint8     *cp, password_buf[64];
	guint32      password_index = 0;
	guint32      count = 0, i;
	guint8		key1[16];
	md5_init(&MD);   /* initialize MD5 */

	/**********************************************/
	/* Use while loop until we've done 1 Megabyte */
	/**********************************************/
	while (count < 1048576) {
		cp = password_buf;
		for (i = 0; i < 64; i++) {
			/*************************************************/
			/* Take the next octet of the password, wrapping */
			/* to the beginning of the password as necessary.*/
			/*************************************************/
			*cp++ = password[password_index++ % passwordlen];
		}
		md5_append(&MD, password_buf, 64);
		count += 64;
	}
	md5_finish(&MD, key1);          /* tell MD5 we're done */

	/*****************************************************/
	/* Now localize the key with the engineID and pass   */
	/* through MD5 to produce final key                  */
	/* May want to ensure that engineLength <= 32,       */
	/* otherwise need to use a buffer larger than 64     */
	/*****************************************************/

	md5_init(&MD);
	md5_append(&MD, key1, 16);
	md5_append(&MD, engineID, engineLength);
	md5_append(&MD, key1, 16);
	md5_finish(&MD, key);

	return;
}




/*
   SHA1 Password to Key Algorithm COPIED from RFC 3414 A.2.2
 */

static void snmp_usm_password_to_key_sha1(const guint8 *password,
								   guint   passwordlen,
								   const guint8 *engineID,
								   guint   engineLength,
								   guint8 *key ) {
	sha1_context     SH;
	guint8     *cp, password_buf[72];
	guint32      password_index = 0;
	guint32      count = 0, i;

	sha1_starts(&SH);   /* initialize SHA */

	/**********************************************/
	/* Use while loop until we've done 1 Megabyte */
	/**********************************************/
	while (count < 1048576) {
		cp = password_buf;
		for (i = 0; i < 64; i++) {
			/*************************************************/
			/* Take the next octet of the password, wrapping */
			/* to the beginning of the password as necessary.*/
			/*************************************************/
			*cp++ = password[password_index++ % passwordlen];
		}
		sha1_update (&SH, password_buf, 64);
		count += 64;
	}
	sha1_finish(&SH, key);

	/*****************************************************/
	/* Now localize the key with the engineID and pass   */
	/* through SHA to produce final key                  */
	/* May want to ensure that engineLength <= 32,       */
	/* otherwise need to use a buffer larger than 72     */
	/*****************************************************/
	memcpy(password_buf, key, 20);
	memcpy(password_buf+20, engineID, engineLength);
	memcpy(password_buf+20+engineLength, key, 20);

	sha1_starts(&SH);
	sha1_update(&SH, password_buf, 40+engineLength);
	sha1_finish(&SH, key);
	return;
 }


static void process_prefs(void) {}

static void* snmp_users_copy_cb(void* dest, const void* orig, unsigned len _U_) {
	const snmp_ue_assoc_t* o = orig;
	snmp_ue_assoc_t* d = dest;

	d->auth_model = o->auth_model;
	d->user.authModel = auth_models[o->auth_model];

	d->priv_proto = o->priv_proto;
	d->user.privProtocol = priv_protos[o->priv_proto];

	d->user.userName.data = g_memdup(o->user.userName.data,o->user.userName.len);
	d->user.userName.len = o->user.userName.len;

	d->user.authPassword.data = o->user.authPassword.data ? g_memdup(o->user.authPassword.data,o->user.authPassword.len) : NULL;
	d->user.authPassword.len = o->user.authPassword.len;

	d->user.privPassword.data = o->user.privPassword.data ? g_memdup(o->user.privPassword.data,o->user.privPassword.len) : NULL;
	d->user.privPassword.len = o->user.privPassword.len;

	d->engine.len = o->engine.len;
	if (o->engine.data) {
		d->engine.data = g_memdup(o->engine.data,o->engine.len);
	}

	d->user.authKey.data = o->user.authKey.data ? g_memdup(o->user.authKey.data,o->user.authKey.len) : NULL;
	d->user.authKey.len = o->user.authKey.len;

	d->user.privKey.data = o->user.privKey.data ? g_memdup(o->user.privKey.data,o->user.privKey.len) : NULL;
	d->user.privKey.len = o->user.privKey.len;

	return d;
}

static void snmp_users_free_cb(void* p) {
	snmp_ue_assoc_t* ue = p;
	if (ue->user.userName.data) g_free(ue->user.userName.data);
	if (ue->user.authPassword.data) g_free(ue->user.authPassword.data);
	if (ue->user.privPassword.data) g_free(ue->user.privPassword.data);
	if (ue->user.authKey.data) g_free(ue->user.authKey.data);
	if (ue->user.privKey.data) g_free(ue->user.privKey.data);
	if (ue->engine.data) g_free(ue->engine.data);
}

static void snmp_users_update_cb(void* p _U_, const char** err) {
	snmp_ue_assoc_t* ue = p;
	GString* es = g_string_new("");

	*err = NULL;

	if (! ue->user.userName.len) g_string_append(es,"no userName, ");
	if (ue->user.authPassword.len < 8) g_string_sprintfa(es,"short authPassword (%d), ", ue->user.authPassword.len);
	if (ue->user.privPassword.len < 8) g_string_sprintfa(es,"short privPassword (%d), ", ue->user.privPassword.len);

	if (es->len) {
		g_string_truncate(es,es->len-2);
		*err = ep_strdup(es->str);
	}

	g_string_free(es,TRUE);

	return;
}

UAT_LSTRING_CB_DEF(snmp_users,userName,snmp_ue_assoc_t,user.userName.data,user.userName.len)
UAT_LSTRING_CB_DEF(snmp_users,authPassword,snmp_ue_assoc_t,user.authPassword.data,user.authPassword.len)
UAT_LSTRING_CB_DEF(snmp_users,privPassword,snmp_ue_assoc_t,user.privPassword.data,user.privPassword.len)
UAT_BUFFER_CB_DEF(snmp_users,engine_id,snmp_ue_assoc_t,engine.data,engine.len)
UAT_VS_DEF(snmp_users,auth_model,snmp_ue_assoc_t,0,"MD5")
UAT_VS_DEF(snmp_users,priv_proto,snmp_ue_assoc_t,0,"DES")

	/*--- proto_register_snmp -------------------------------------------*/
void proto_register_snmp(void) {
  /* List of fields */
  static hf_register_info hf[] = {
		{ &hf_snmp_v3_flags_auth,
		{ "Authenticated", "snmp.v3.flags.auth", FT_BOOLEAN, 8,
		    TFS(&flags_set_truth), TH_AUTH, "", HFILL }},
		{ &hf_snmp_v3_flags_crypt,
		{ "Encrypted", "snmp.v3.flags.crypt", FT_BOOLEAN, 8,
		    TFS(&flags_set_truth), TH_CRYPT, "", HFILL }},
		{ &hf_snmp_v3_flags_report,
		{ "Reportable", "snmp.v3.flags.report", FT_BOOLEAN, 8,
		    TFS(&flags_set_truth), TH_REPORT, "", HFILL }},
		{ &hf_snmp_engineid_conform, {
		    "Engine ID Conformance", "snmp.engineid.conform", FT_BOOLEAN, 8,
		    TFS(&tfs_snmp_engineid_conform), F_SNMP_ENGINEID_CONFORM, "Engine ID RFC3411 Conformance", HFILL }},
		{ &hf_snmp_engineid_enterprise, {
		    "Engine Enterprise ID", "snmp.engineid.enterprise", FT_UINT32, BASE_DEC,
		    VALS(sminmpec_values), 0, "Engine Enterprise ID", HFILL }},
		{ &hf_snmp_engineid_format, {
		    "Engine ID Format", "snmp.engineid.format", FT_UINT8, BASE_DEC,
		    VALS(snmp_engineid_format_vals), 0, "Engine ID Format", HFILL }},
		{ &hf_snmp_engineid_ipv4, {
		    "Engine ID Data: IPv4 address", "snmp.engineid.ipv4", FT_IPv4, BASE_NONE,
		    NULL, 0, "Engine ID Data: IPv4 address", HFILL }},
		{ &hf_snmp_engineid_ipv6, {
		    "Engine ID Data: IPv6 address", "snmp.engineid.ipv6", FT_IPv6, BASE_NONE,
		    NULL, 0, "Engine ID Data: IPv6 address", HFILL }},
		{ &hf_snmp_engineid_mac, {
		    "Engine ID Data: MAC address", "snmp.engineid.mac", FT_ETHER, BASE_NONE,
		    NULL, 0, "Engine ID Data: MAC address", HFILL }},
		{ &hf_snmp_engineid_text, {
		    "Engine ID Data: Text", "snmp.engineid.text", FT_STRING, BASE_NONE,
		    NULL, 0, "Engine ID Data: Text", HFILL }},
		{ &hf_snmp_engineid_time, {
		    "Engine ID Data: Time", "snmp.engineid.time", FT_ABSOLUTE_TIME, BASE_NONE,
		    NULL, 0, "Engine ID Data: Time", HFILL }},
		{ &hf_snmp_engineid_data, {
		    "Engine ID Data", "snmp.engineid.data", FT_BYTES, BASE_HEX,
		    NULL, 0, "Engine ID Data", HFILL }},
		  { &hf_snmp_msgAuthentication,
				{ "Authentication", "snmp.v3.auth", FT_BOOLEAN, 8,
					TFS(&auth_flags), 0, "", HFILL }},
		  { &hf_snmp_decryptedPDU, {
					"Decrypted ScopedPDU", "snmp.decrypted_pdu", FT_BYTES, BASE_HEX,
					NULL, 0, "Decrypted PDU", HFILL }},
  { &hf_snmp_noSuchObject_value, { "noSuchObject", "snmp.noSuchObject", FT_NONE, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_noSuchInstance_value, { "noSuchInstance", "snmp.noSuchInstance", FT_NONE, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_endOfMibView_value, { "endOfMibView", "snmp.endOfMibView", FT_NONE, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_integer32_value, { "Value (Integer32)", "snmp.value.int", FT_INT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_octestring_value, { "Value (OctetString)", "snmp.value.octets", FT_BYTES, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_oid_value, { "Value (OID)", "snmp.value.oid", FT_OID, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_null_value, { "Value (Null)", "snmp.value.null", FT_NONE, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_ipv4_value, { "Value (IpAddress)", "snmp.value.ipv4", FT_IPv4, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_ipv6_value, { "Value (IpAddress)", "snmp.value.ipv6", FT_IPv6, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_anyaddress_value, { "Value (IpAddress)", "snmp.value.addr", FT_BYTES, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_unsigned32_value, { "Value (Unsigned32)", "snmp.value.u32", FT_INT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_gauge32_value, { "Value (Gauge32)", "snmp.value.g32", FT_INT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_unknown_value, { "Value (Unknown)", "snmp.value.unk", FT_BYTES, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_counter_value, { "Value (Counter32)", "snmp.value.counter", FT_UINT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_nsap_value, { "Value (NSAP)", "snmp.value.nsap", FT_UINT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_timeticks_value, { "Value (Timeticks)", "snmp.value.timeticks", FT_UINT64, BASE_DEC,  NULL, 0, "", HFILL }},
  { &hf_snmp_opaque_value, { "Value (Opaque)", "snmp.value.opaque", FT_BYTES, BASE_NONE,  NULL, 0, "", HFILL }},
  { &hf_snmp_objectname, { "Object Name", "snmp.name", FT_OID, BASE_NONE,  NULL, 0, "", HFILL }},
	  
#include "packet-snmp-hfarr.c"
  };

  /* List of subtrees */
  static gint *ett[] = {
	  &ett_snmp,
	  &ett_engineid,
	  &ett_msgFlags,
	  &ett_encryptedPDU,
	  &ett_decrypted,
	  &ett_authParameters,
	  &ett_internet,
	  &ett_varbind,
	  &ett_decoding_error,
#include "packet-snmp-ettarr.c"
  };
  module_t *snmp_module;
  static uat_field_t users_fields[] = {
	  UAT_FLD_BUFFER(snmp_users,engine_id,"Engine-id for this entry (empty = any)"),
	  UAT_FLD_LSTRING(snmp_users,userName,"The username"),
	  UAT_FLD_VS(snmp_users,auth_model,auth_types,"Algorithm to be used for authentication."),
	  UAT_FLD_LSTRING(snmp_users,authPassword,"The password used for authenticating packets for this entry"),
	  UAT_FLD_VS(snmp_users,priv_proto,priv_types,"Algorithm to be used for privacy."),
	  UAT_FLD_LSTRING(snmp_users,privPassword,"The password used for encrypting packets for this entry"),
	  UAT_END_FIELDS
  };
  
  assocs_uat = uat_new("SNMP Users",
					   sizeof(snmp_ue_assoc_t),
					   "snmp_users",
					   (void**)&ueas,
					   &num_ueas,
					   UAT_CAT_CRYPTO,
					   "ChSNMPUsersSection",
					   snmp_users_copy_cb,
					   snmp_users_update_cb,
					   snmp_users_free_cb,
					   users_fields);
  
  /* Register protocol */
  proto_snmp = proto_register_protocol(PNAME, PSNAME, PFNAME);
  new_register_dissector("snmp", dissect_snmp, proto_snmp);

  /* Register fields and subtrees */
  proto_register_field_array(proto_snmp, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));


	/* Register configuration preferences */
	snmp_module = prefs_register_protocol(proto_snmp, process_prefs);
	prefs_register_bool_preference(snmp_module, "display_oid",
		"Show SNMP OID in info column",
		"Whether the SNMP OID should be shown in the info column",
		&display_oid);

	prefs_register_obsolete_preference(snmp_module, "mib_modules");
	prefs_register_obsolete_preference(snmp_module, "var_in_tree");
	prefs_register_obsolete_preference(snmp_module, "users_file");

	prefs_register_bool_preference(snmp_module, "desegment",
	    "Reassemble SNMP-over-TCP messages\nspanning multiple TCP segments",
	    "Whether the SNMP dissector should reassemble messages spanning multiple TCP segments."
	    " To use this option, you must also enable \"Allow subdissectors to reassemble TCP streams\" in the TCP protocol settings.",
	    &snmp_desegment);


  prefs_register_uat_preference(snmp_module, "users_table",
								"Users Table",
								"Table of engine-user associations used for authentication and decryption",
								assocs_uat);
  
  
	variable_oid_dissector_table = register_dissector_table("snmp.variable_oid",
															"SNMP Variable OID",
															FT_STRING, BASE_NONE);

	register_init_routine(renew_ue_cache);
}


/*--- proto_reg_handoff_snmp ---------------------------------------*/
void proto_reg_handoff_snmp(void) {
	dissector_handle_t snmp_tcp_handle;

	snmp_handle = find_dissector("snmp");

	dissector_add("udp.port", UDP_PORT_SNMP, snmp_handle);
	dissector_add("udp.port", UDP_PORT_SNMP_TRAP, snmp_handle);
	dissector_add("ethertype", ETHERTYPE_SNMP, snmp_handle);
	dissector_add("ipx.socket", IPX_SOCKET_SNMP_AGENT, snmp_handle);
	dissector_add("ipx.socket", IPX_SOCKET_SNMP_SINK, snmp_handle);
	dissector_add("hpext.dxsap", HPEXT_SNMP, snmp_handle);

	snmp_tcp_handle = create_dissector_handle(dissect_snmp_tcp, proto_snmp);
	dissector_add("tcp.port", TCP_PORT_SNMP, snmp_tcp_handle);
	dissector_add("tcp.port", TCP_PORT_SNMP_TRAP, snmp_tcp_handle);

	data_handle = find_dissector("data");

	/*
	 * Process preference settings.
	 *
	 * We can't do this in the register routine, as preferences aren't
	 * read until all dissector register routines have been called (so
	 * that all dissector preferences have been registered).
	 */
	process_prefs();

}

void
proto_register_smux(void)
{
	static hf_register_info hf[] = {
		{ &hf_smux_version,
		{ "Version", "smux.version", FT_UINT8, BASE_DEC, NULL,
		    0x0, "", HFILL }},
		{ &hf_smux_pdutype,
		{ "PDU type", "smux.pdutype", FT_UINT8, BASE_DEC, VALS(smux_types),
		    0x0, "", HFILL }},
	};
	static gint *ett[] = {
		&ett_smux,
	};

	proto_smux = proto_register_protocol("SNMP Multiplex Protocol",
	    "SMUX", "smux");
	proto_register_field_array(proto_smux, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));

}

void
proto_reg_handoff_smux(void)
{
	dissector_handle_t smux_handle;

	smux_handle = create_dissector_handle(dissect_smux, proto_smux);
	dissector_add("tcp.port", TCP_PORT_SMUX, smux_handle);
}


