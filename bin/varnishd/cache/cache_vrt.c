/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Runtime support for compiled VCL programs
 */

#include "config.h"

#include <stdlib.h>

#include "cache_varnishd.h"

#include "cache_objhead.h"
#include "vav.h"
#include "vcl.h"
#include "vct.h"
#include "venc.h"
#include "vend.h"
#include "vrt_obj.h"
#include "vsa.h"
#include "vsha256.h"
#include "vtcp.h"
#include "vtim.h"
#include "vcc_interface.h"

#include "common/heritage.h"
#include "common/vsmw.h"
#include "proxy/cache_proxy.h"

const void * const vrt_magic_string_end = &vrt_magic_string_end;
const void * const vrt_magic_string_unset = &vrt_magic_string_unset;
const struct strands *vrt_null_strands = &(struct strands){
	.n = 0,
	.p = (const char *[1]){NULL}
};

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_synth(VRT_CTX, VCL_INT code, VCL_STRING reason)
{
	const char *ret;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	assert(ctx->req != NULL || ctx->bo != NULL);

	ret = ctx->req == NULL ? "error" : "synth";
	if (code < 0) {
		VRT_fail(ctx, "return(%s()) status code (%jd) is negative",
		    ret, (intmax_t)code);
		return;
	}
	if (code > 65535) {
		VRT_fail(ctx, "return(%s()) status code (%jd) > 65535",
		    ret, (intmax_t)code);
		return;
	}
	if ((code % 1000) < 100) {
		VRT_fail(ctx,
		    "illegal return(%s()) status code (%jd) (..0##)",
		    ret, (intmax_t)code);
		return;
	}

	if (reason && !WS_Inside(ctx->ws, reason, NULL)) {
		reason = WS_Copy(ctx->ws, reason, -1);
		if (!reason) {
			VRT_fail(ctx, "Workspace overflow");
			return;
		}
	}

	if (ctx->req == NULL) {
		CHECK_OBJ_NOTNULL(ctx->bo, BUSYOBJ_MAGIC);
		ctx->bo->err_code = (uint16_t)code;
		ctx->bo->err_reason = reason ? reason
		    : http_Status2Reason(ctx->bo->err_code % 1000, NULL);
		return;
	}

	ctx->req->err_code = (uint16_t)code;
	ctx->req->err_reason = reason ? reason
	    : http_Status2Reason(ctx->req->err_code % 1000, NULL);
}

/*--------------------------------------------------------------------*/

void
VPI_acl_log(VRT_CTX, const char *msg)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(msg);
	if (ctx->vsl != NULL)
		VSLb(ctx->vsl, SLT_VCL_acl, "%s", msg);
	else
		VSL(SLT_VCL_acl, 0, "%s", msg);
}

int
VRT_acl_match(VRT_CTX, VCL_ACL acl, VCL_IP ip)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(acl, VRT_ACL_MAGIC);
	if (ip == NULL) {
		VRT_fail(ctx, "Cannot match a null IP address");
		return (0);
	}
	assert(VSA_Sane(ip));
	return (acl->match(ctx, ip));
}

static int
acl_tbl_cmp(int fam, const uint8_t *key, const uint8_t *b)
{
	int rv;

	rv = fam - (int)b[3];
	if (rv == 0 && b[1] > 0)
		rv = memcmp(key, b + 4, b[1]);
	if (rv == 0 && b[2])
		rv = (int)(key[b[1]] & b[2]) - (int)b[4 + b[1]];
	return (rv);
}

static const uint8_t *
bbsearch(int fam, const uint8_t *key, const uint8_t *base0,
    size_t nmemb, size_t size)
{
	const uint8_t *base = base0;
	size_t lim;
	int cmp;
	const uint8_t *p;

	for (lim = nmemb; lim != 0; lim >>= 1) {
		p = base + (lim >> 1) * size;
		cmp = acl_tbl_cmp(fam, key, p);
		if (cmp == 0)
			return (p);
		if (cmp > 0) {
			/* key > p: move right */
			base = p + size;
			lim--;
		} /* else move left */
	}
	return (NULL);
}

int
VPI_acl_table(VRT_CTX, VCL_IP p, unsigned n, unsigned m, const uint8_t *tbl,
    const char * const *str, const char *fin)
{
	int fam;
	const uint8_t *key;
	const uint8_t *ptr;
	size_t sz;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(p);
	AN(n);
	assert(m == 20);
	AN(tbl);
	AN(fin);

	fam = VRT_VSA_GetPtr(ctx, p, &key);
	ptr = bbsearch(fam, key, tbl, n, m);

	if (ptr != NULL) {
		sz = ptr - tbl;
		AZ(sz % m);
		sz /= m;
		if (str != NULL)
			VPI_acl_log(ctx, str[sz]);
		return *ptr;
	}
	if (str != NULL)
		VPI_acl_log(ctx, fin);
	return (0);
}

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_hit_for_pass(VRT_CTX, VCL_DURATION d)
{
	struct objcore *oc;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->bo == NULL) {
		VSLb(ctx->vsl, SLT_Error,
		    "Note: Ignoring DURATION argument to return(pass);");
		return;
	}
	CHECK_OBJ_NOTNULL(ctx->bo, BUSYOBJ_MAGIC);
	oc = ctx->bo->fetch_objcore;
	oc->ttl = d;
	oc->grace = 0.0;
	oc->keep = 0.0;
	VSLb(ctx->vsl, SLT_TTL, "HFP %.0f %.0f %.0f %.0f uncacheable",
	    oc->ttl, oc->grace, oc->keep, oc->t_origin);
}

/*--------------------------------------------------------------------*/

VCL_HTTP
VRT_selecthttp(VRT_CTX, enum gethdr_e where)
{
	VCL_HTTP hp;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	switch (where) {
	case HDR_REQ:
		hp = ctx->http_req;
		break;
	case HDR_REQ_TOP:
		hp = ctx->http_req_top;
		break;
	case HDR_BEREQ:
		hp = ctx->http_bereq;
		break;
	case HDR_BERESP:
		hp = ctx->http_beresp;
		break;
	case HDR_RESP:
		hp = ctx->http_resp;
		break;
	default:
		WRONG("VRT_selecthttp 'where' invalid");
	}
	return (hp);
}

/*--------------------------------------------------------------------*/

VCL_STRING
VRT_GetHdr(VRT_CTX, VCL_HEADER hs)
{
	VCL_HTTP hp;
	const char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (hs->where == HDR_OBJ) {
		CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
		CHECK_OBJ_NOTNULL(ctx->req->objcore, OBJCORE_MAGIC);
		return (HTTP_GetHdrPack(ctx->req->wrk, ctx->req->objcore,
		    hs->what));
	}
	hp = VRT_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (!http_GetHdr(hp, hs->what, &p))
		return (NULL);
	return (p);
}

/*--------------------------------------------------------------------
 * Alloc Strands with space for n elements on workspace
 *
 * Error handling is deliberately left to the caller
 */

struct strands *
VRT_AllocStrandsWS(struct ws *ws, int n)
{
	struct strands *s;
	const char **p;

	s = WS_Alloc(ws, sizeof *s);
	p = WS_Alloc(ws, n * sizeof *p);

	if (s == NULL || p == NULL)
		return (NULL);

	s->n = n;
	s->p = p;

	return (s);
}

/*--------------------------------------------------------------------
 * Compare two STRANDS
 */

int
VRT_CompareStrands(VCL_STRANDS a, VCL_STRANDS b)
{
	const char *pa = NULL, *pb = NULL;
	int na = 0, nb = 0;

	while (1) {
		if (pa != NULL && *pa == '\0')
			pa = NULL;
		if (pb != NULL && *pb == '\0')
			pb = NULL;
		if (pa == NULL && na < a->n)
			pa = a->p[na++];
		else if (pb == NULL && nb < b->n)
			pb = b->p[nb++];
		else if (pa == NULL && pb == NULL)
			return (0);
		else if (pa == NULL)
			return (-1);
		else if (pb == NULL)
			return (1);
		else if (*pa == '\0')
			pa = NULL;
		else if (*pb == '\0')
			pb = NULL;
		else if (*pa != *pb)
			return (*pa - *pb);
		else {
			pa++;
			pb++;
		}
	}
}

/*--------------------------------------------------------------------
 * STRANDS to BOOL
 */

VCL_BOOL
VRT_Strands2Bool(VCL_STRANDS s)
{
	int i;

	AN(s);
	for (i = 0; i < s->n; i++)
		if (s->p[i] != NULL)
			return (1);
	return (0);
}

/*--------------------------------------------------------------------
 * Hash a STRANDS
 */

uint32_t
VRT_HashStrands32(VCL_STRANDS s)
{
	struct VSHA256Context sha_ctx;
	unsigned char sha256[VSHA256_LEN];
	const char *p;
	int i;

	AN(s);
	VSHA256_Init(&sha_ctx);
	for (i = 0; i < s->n; i++) {
		p = s->p[i];
		if (p != NULL && *p != '\0')
			VSHA256_Update(&sha_ctx, p, strlen(p));
	}
	VSHA256_Final(sha256, &sha_ctx);

	/* NB: for some reason vmod_director's shard director specifically
	 * relied on little-endian decoding of the last 4 octets. In order
	 * to maintain a stable hash function to share across consumers we
	 * need to stick to that.
	 */
	return (vle32dec(sha256 + VSHA256_LEN - 4));
}

/*--------------------------------------------------------------------
 * Collapse a STRING_LIST in the space provided, or return NULL
 */

char *
VRT_StringList(char *d, unsigned dl, const char *p, va_list ap)
{
	char *b, *e;
	unsigned x;

	b = d;
	e = b + dl;
	while (p != vrt_magic_string_end && b < e) {
		if (p != NULL && *p != '\0') {
			x = strlen(p);
			if (b + x < e)
				memcpy(b, p, x);
			b += x;
		}
		p = va_arg(ap, const char *);
	}
	if (b >= e)
		return (NULL);
	*b++ = '\0';
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST into a workspace.
 */

const char *
VRT_String(struct ws *ws, const char *h, const char *p, va_list ap)
{
	char *b, *e;
	const char *q;
	unsigned u, x;
	va_list aq;

	u = WS_ReserveAll(ws);
	e = b = WS_Reservation(ws);
	e += u;

	va_copy(aq, ap);
	do
		q = va_arg(aq, const char *);
	while (q == NULL || (q != vrt_magic_string_end && *q == '\0'));

	if (h != NULL && p == NULL && q == vrt_magic_string_end &&
	    WS_Inside(ws, h, NULL)) {
		va_end(aq);
		WS_Release(ws, 0);
		return (h);
	}

	if (h == NULL && p != NULL && q == vrt_magic_string_end &&
	    WS_Inside(ws, p, NULL)) {
		va_end(aq);
		WS_Release(ws, 0);
		return (p);
	}

	if (h == NULL && p == NULL) {
		if (q == vrt_magic_string_end) {
			va_end(aq);
			WS_Release(ws, 0);
			return ("");
		}
		do
			p = va_arg(aq, const char *);
		while (p == NULL || (p != vrt_magic_string_end && *p == '\0'));
		if (p == vrt_magic_string_end && WS_Inside(ws, q, NULL)) {
			va_end(aq);
			WS_Release(ws, 0);
			return (q);
		}
		p = NULL;
		va_end(aq);
	}

	if (h != NULL) {
		x = strlen(h);
		if (b + x < e)
			memcpy(b, h, x);
		b += x;
		if (b < e)
			*b = ' ';
		b++;
	}
	b = VRT_StringList(b, e > b ? e - b : 0, p, ap);
	if (b == NULL || b == e) {
		WS_MarkOverflow(ws);
		WS_Release(ws, 0);
		return (NULL);
	}
	e = b;
	b = WS_Reservation(ws);
	WS_Release(ws, e - b);
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST on the current workspace
 */

VCL_STRING
VRT_CollectString(VRT_CTX, const char *p, ...)
{
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	va_start(ap, p);
	b = VRT_String(ctx->ws, NULL, p, ap);
	va_end(ap);
	if (b == NULL)
		VRT_fail(ctx, "Workspace overflow");
	return (b);
}

/*--------------------------------------------------------------------
 * Collapse STRANDS into the space provided, or return NULL
 */

char *
VRT_Strands(char *d, size_t dl, VCL_STRANDS s)
{
	char *b;
	const char *e;
	unsigned x;

	AN(d);
	AN(s);
	b = d;
	e = b + dl;
	for (int i = 0; i < s->n; i++)
		if (s->p[i] != NULL && *s->p[i] != '\0') {
			x = strlen(s->p[i]);
			if (b + x >= e)
				return (NULL);
			memcpy(b, s->p[i], x);
			b += x;
		}
	assert(b < e);
	*b++ = '\0';
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge STRANDS into a workspace.
 */

VCL_STRING
VRT_StrandsWS(struct ws *ws, const char *h, VCL_STRANDS s)
{
	const char *q = NULL;
	struct vsb vsb[1];
	int i;

	WS_Assert(ws);
	AN(s);

	for (i = 0; i < s->n; i++) {
		if (s->p[i] != NULL && *s->p[i] != '\0') {
			q = s->p[i];
			break;
		}
	}

	if (q == NULL) {
		if (h == NULL)
			return ("");
		if (WS_Inside(ws, h, NULL))
			return (h);
	} else if (h == NULL && WS_Inside(ws, q, NULL)) {
		for (i++; i < s->n; i++)
			if (s->p[i] != NULL && *s->p[i] != '\0')
				break;
		if (i == s->n)
			return (q);
	}

	WS_VSB_new(vsb, ws);
	if (h != NULL)
		VSB_cat(vsb, h);
	for (i = 0; i < s->n; i++) {
		if (s->p[i] != NULL && *s->p[i] != '\0')
			VSB_cat(vsb, s->p[i]);
	}
	return (WS_VSB_finish(vsb, ws, NULL));
}

/*--------------------------------------------------------------------
 * Copy and merge STRANDS on the current workspace
 */

VCL_STRING
VRT_CollectStrands(VRT_CTX, VCL_STRANDS s)
{
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	b = VRT_StrandsWS(ctx->ws, NULL, s);
	if (b == NULL)
		VRT_fail(ctx, "Workspace overflow");
	return (b);
}

/*--------------------------------------------------------------------
 * upper/lower-case STRANDS (onto workspace)
 */

#include <stdio.h>

VCL_STRING
VRT_UpperLowerStrands(VRT_CTX, VCL_STRANDS s, int up)
{
	unsigned u;
	char *b, *e, *r;
	const char *p, *q = NULL;
	int i, copy = 0;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	AN(s);
	u = WS_ReserveAll(ctx->ws);
	r = b = WS_Reservation(ctx->ws);
	e = b + u;
	for (i = 0; i < s->n; i++) {
		if (s->p[i] == NULL || s->p[i][0] == '\0')
			continue;
		if (q != NULL)
			copy = 1;
		for(p = q = s->p[i]; *p != '\0'; p++) {
			if ((up && vct_islower(*p)) ||
			    (!up && vct_isupper(*p))) {
				*b++ = *p ^ 0x20;
				copy = 1;
			} else if (b < e) {
				*b++ = *p;
			}
			if (copy && b == e)
				break;
		}
		if (copy && b == e) {
			WS_Release(ctx->ws, 0);
			VRT_fail(ctx, "Workspace overflow");
			return (NULL);
		}
	}
	assert(b <= e);
	if (!copy) {
		WS_Release(ctx->ws, 0);
		return (q);
	}
	assert(b < e);
	*b++ = '\0';
	assert(b <= e);
	WS_ReleaseP(ctx->ws, b);
	return (r);
}

// rfc7230,l,1243,1244
// ASCII VCHAR + TAB + obs-text (0x80-ff)
static inline VCL_BOOL
validhdr(const char *p)
{
	AN(p);
	for(;*p != '\0'; p++)
		if (! vct_ishdrval(*p))
			return (0);
	return (1);
}

/*--------------------------------------------------------------------*/
VCL_BOOL
VRT_ValidHdr(VRT_CTX, VCL_STRANDS s)
{
	int i;

	(void) ctx;

	for (i = 0; i < s->n; i++) {
		if (s->p[i] == NULL || s->p[i][0] == '\0')
			continue;
		if (! validhdr(s->p[i]))
			return (0);
	}

	return (1);
}
/*--------------------------------------------------------------------*/

VCL_VOID
VRT_SetHdr(VRT_CTX , VCL_HEADER hs, const char *p, ...)
{
	VCL_HTTP hp;
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(hs);
	AN(hs->what);
	hp = VRT_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (p == vrt_magic_string_unset) {
		http_Unset(hp, hs->what);
	} else {
		va_start(ap, p);
		b = VRT_String(hp->ws, hs->what + 1, p, ap);
		va_end(ap);
		if (b == NULL) {
			VSLb(ctx->vsl, SLT_LostHeader, "%s", hs->what + 1);
			return;
		}
		if (FEATURE(FEATURE_VALIDATE_HEADERS) && ! validhdr(b)) {
			VRT_fail(ctx, "Bad header %s", b);
			return;
		}
		http_Unset(hp, hs->what);
		http_SetHeader(hp, b);
	}
}

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_handling(VRT_CTX, unsigned hand)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	assert(hand != VCL_RET_FAIL);
	AN(ctx->handling);
	AZ(*ctx->handling);
	assert(hand > 0);
	assert(hand < VCL_RET_MAX);
	*ctx->handling = hand;
}

unsigned
VRT_handled(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(ctx->handling);
	return (*ctx->handling);
}

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_fail(VRT_CTX, const char *fmt, ...)
{
	va_list ap;

	assert(ctx->vsl != NULL || ctx->msg != NULL);
	AN(ctx->handling);
	if (*ctx->handling == VCL_RET_FAIL)
		return;
	AZ(*ctx->handling);
	AN(fmt);
	AZ(strchr(fmt, '\n'));
	va_start(ap, fmt);
	if (ctx->vsl != NULL) {
		VSLbv(ctx->vsl, SLT_VCL_Error, fmt, ap);
	} else {
		AN(ctx->msg);
		VSB_vprintf(ctx->msg, fmt, ap);
		VSB_putc(ctx->msg, '\n');
	}
	va_end(ap);
	*ctx->handling = VCL_RET_FAIL;
}

/*--------------------------------------------------------------------
 * Feed data into the hash calculation
 */

VCL_VOID
VRT_hashdata(VRT_CTX, VCL_STRANDS s)
{
	int i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	AN(ctx->specific);
	AN(s);
	for (i = 0; i < s->n; i++)
		HSH_AddString(ctx->req, ctx->specific, s->p[i]);
	/*
	 * Add a 'field-separator' to make it more difficult to
	 * manipulate the hash.
	 */
	HSH_AddString(ctx->req, ctx->specific, NULL);
}

/*--------------------------------------------------------------------*/

VCL_TIME
VRT_r_now(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (ctx->now);
}

/*--------------------------------------------------------------------*/

VCL_STRING v_matchproto_()
VRT_IP_string(VRT_CTX, VCL_IP ip)
{
	char *p;
	unsigned len;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ip == NULL) {
		VRT_fail(ctx, "%s: Illegal IP", __func__);
		return (NULL);
	}
	len = WS_ReserveAll(ctx->ws);
	if (len == 0) {
		WS_Release(ctx->ws, 0);
		return (NULL);
	}
	p = WS_Reservation(ctx->ws);
	VTCP_name(ip, p, len, NULL, 0);
	WS_Release(ctx->ws, strlen(p) + 1);
	return (p);
}

int
VRT_INT_is_valid(VCL_INT arg)
{
	return (arg >= VRT_INTEGER_MIN && arg <= VRT_INTEGER_MAX);
}


VCL_STRING v_matchproto_()
VRT_INT_string(VRT_CTX, VCL_INT num)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (!VRT_INT_is_valid(num))
		VRT_fail(ctx, "INT overflow converting to string (%jX)",
		    (intmax_t)num);
	return (WS_Printf(ctx->ws, "%jd", (intmax_t)num));
}

int
VRT_REAL_is_valid(VCL_REAL arg)
{
	return (!isnan(arg) && arg >= VRT_DECIMAL_MIN && arg <= VRT_DECIMAL_MAX);
}

VCL_STRING v_matchproto_()
VRT_REAL_string(VRT_CTX, VCL_REAL num)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (!VRT_REAL_is_valid(num))
		VRT_fail(ctx, "REAL overflow converting to string (%e)", num);
	return (WS_Printf(ctx->ws, "%.3f", num));
}

VCL_STRING v_matchproto_()
VRT_TIME_string(VRT_CTX, VCL_TIME t)
{
	char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	p = WS_Alloc(ctx->ws, VTIM_FORMAT_SIZE);
	if (p == NULL) {
		VRT_fail(ctx, "Workspace overflow");
		return (NULL);
	}
	VTIM_format(t, p);
	if (*p == '\0') {
		VRT_fail(ctx, "Unformatable VCL_TIME");
		return (NULL);
	}
	return (p);
}

VCL_STRING v_matchproto_()
VRT_BACKEND_string(VCL_BACKEND d)
{
	if (d == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	return (d->vcl_name);
}

VCL_STRING v_matchproto_()
VRT_BOOL_string(VCL_BOOL val)
{

	return (val ? "true" : "false");
}

VCL_STRING v_matchproto_()
VRT_BLOB_string(VRT_CTX, VCL_BLOB val)
{
	struct vsb vsb[1];
	const char *s;

	if (val == NULL)
		return (NULL);
	WS_VSB_new(vsb, ctx->ws);
	VSB_putc(vsb, ':');
	VENC_Encode_Base64(vsb, val->blob, val->len);
	VSB_putc(vsb, ':');
	s = WS_VSB_finish(vsb, ctx->ws, NULL);
	return (s);
}

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_Rollback(VRT_CTX, VCL_HTTP hp)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (ctx->method & VCL_MET_PIPE) {
		VRT_fail(ctx, "Cannot rollback in vcl_pipe {}");
		return;
	}
	if (hp == ctx->http_req) {
		CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
		Req_Rollback(ctx);
		if (ctx->method & VCL_MET_DELIVER)
			XXXAZ(Resp_Setup_Deliver(ctx->req));
		if (ctx->method & VCL_MET_SYNTH)
			Resp_Setup_Synth(ctx->req);
	} else if (hp == ctx->http_bereq) {
		Bereq_Rollback(ctx);
	} else
		WRONG("VRT_Rollback 'hp' invalid");
}

/*--------------------------------------------------------------------*/

VCL_VOID
VRT_synth_strands(VRT_CTX, VCL_STRANDS s)
{
	struct vsb *vsb;
	int i;

	CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);
	AN(s);
	for (i = 0; i < s->n; i++) {
		if (s->p[i] != NULL)
			VSB_cat(vsb, s->p[i]);
		else
			VSB_cat(vsb, "(null)");
	}
}

VCL_VOID
VRT_synth_blob(VRT_CTX, VCL_BLOB b)
{
	struct vsb *vsb;
	CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);

	if (b->len > 0 && b->blob != NULL)
		VSB_bcat(vsb, b->blob, b->len);
}

VCL_VOID
VRT_synth_page(VRT_CTX, VCL_STRANDS s)
{
	VRT_synth_strands(ctx, s);
}

/*--------------------------------------------------------------------*/

static VCL_STRING
vrt_ban_error(VRT_CTX, VCL_STRING err)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(ctx->vsl);
	AN(err);

	VSLb(ctx->vsl, SLT_VCL_Error, "ban(): %s", err);
	return (err);
}

VCL_STRING
VRT_ban_string(VRT_CTX, VCL_STRING str)
{
	char *a1, *a2, *a3;
	char **av;
	struct ban_proto *bp;
	const char *err = NULL, *berr = NULL;
	int i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (str == NULL)
		return (vrt_ban_error(ctx, "Null argument"));

	bp = BAN_Build();
	if (bp == NULL)
		return (vrt_ban_error(ctx, "Out of Memory"));

	av = VAV_Parse(str, NULL, ARGV_NOESC);
	AN(av);
	if (av[0] != NULL) {
		err = av[0];
		VAV_Free(av);
		BAN_Abandon(bp);
		return (vrt_ban_error(ctx, err));
	}
	for (i = 0; ;) {
		a1 = av[++i];
		if (a1 == NULL) {
			err = "No ban conditions found.";
			break;
		}
		a2 = av[++i];
		if (a2 == NULL) {
			err = "Expected comparison operator.";
			break;
		}
		a3 = av[++i];
		if (a3 == NULL) {
			err = "Expected second operand.";
			break;
		}
		berr = BAN_AddTest(bp, a1, a2, a3);
		if (berr != NULL)
			break;

		if (av[++i] == NULL) {
			berr = BAN_Commit(bp);
			if (berr == NULL)
				bp = NULL;
			break;
		}
		if (strcmp(av[i], "&&")) {
			err = WS_Printf(ctx->ws, "Expected && between "
			    "conditions, found \"%s\"", av[i]);
			if (err == NULL)
				err = "Expected && between conditions "
				    "(workspace overflow)";
			break;
		}
	}
	if (berr != NULL) {
		AZ(err);
		err = WS_Copy(ctx->ws, berr, -1);
		if (err == NULL)
			err = "Unknown error (workspace overflow)";
		berr = NULL;
	}
	AZ(berr);
	if (bp != NULL)
		BAN_Abandon(bp);
	VAV_Free(av);
	if (err == NULL)
		return (NULL);
	return (vrt_ban_error(ctx, err));
}

VCL_BYTES
VRT_CacheReqBody(VRT_CTX, VCL_BYTES maxsize)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	if (ctx->method != VCL_MET_RECV) {
		VSLb(ctx->vsl, SLT_VCL_Error,
		    "req.body can only be cached in vcl_recv{}");
		return (-1);
	}
	return (VRB_Cache(ctx->req, maxsize));
}

/*--------------------------------------------------------------------
 * purges
 */

VCL_INT
VRT_purge(VRT_CTX, VCL_DURATION ttl, VCL_DURATION grace, VCL_DURATION keep)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if ((ctx->method & (VCL_MET_HIT|VCL_MET_MISS)) == 0) {
		VRT_fail(ctx,
		    "purge can only happen in vcl_hit{} or vcl_miss{}");
		return (0);
	}

	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req->wrk, WORKER_MAGIC);
	return (HSH_Purge(ctx->req->wrk, ctx->req->objcore->objhead,
	    ctx->req->t_req, ttl, grace, keep));
}

/*--------------------------------------------------------------------
 */

struct vsmw_cluster * v_matchproto_()
VRT_VSM_Cluster_New(VRT_CTX, size_t sz)
{
	struct vsmw_cluster *vc;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	assert(sz > 0);
	AN(vsc_lock);
	AN(vsc_unlock);
	AN(heritage.proc_vsmw);
	vsc_lock();
	vc = VSMW_NewCluster(heritage.proc_vsmw, sz, "VSC_cluster");
	vsc_unlock();
	return (vc);
}

void v_matchproto_()
VRT_VSM_Cluster_Destroy(VRT_CTX, struct vsmw_cluster **vsmcp)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(vsmcp);
	VSMW_DestroyCluster(heritage.proc_vsmw, vsmcp);
}

/*--------------------------------------------------------------------
 * Simple stuff
 */

int
VRT_strcmp(const char *s1, const char *s2)
{
	if (s1 == NULL || s2 == NULL)
		return (1);
	return (strcmp(s1, s2));
}

void
VRT_memmove(void *dst, const void *src, unsigned len)
{

	(void)memmove(dst, src, len);
}

VCL_BOOL
VRT_ipcmp(VRT_CTX, VCL_IP sua1, VCL_IP sua2)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

	if (sua1 == NULL || sua2 == NULL) {
		VRT_fail(ctx, "%s: Illegal IP", __func__);
		return (1);
	}
	return (VSA_Compare_IP(sua1, sua2));
}

/*
 * the pointer passed as src must have at least VCL_TASK lifetime
 */
VCL_BLOB
VRT_blob(VRT_CTX, const char *err, const void *src, size_t len, unsigned type)
{
	struct vrt_blob *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);

	p = (void *)WS_Alloc(ctx->ws, sizeof *p);
	if (p == NULL) {
		VRT_fail(ctx, "Workspace overflow (%s)", err);
		return (NULL);
	}

	p->type = type;
	p->len = len;
	p->blob = src;

	return (p);
}

int
VRT_VSA_GetPtr(VRT_CTX, const struct suckaddr *sua, const unsigned char ** dst)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(dst);

	if (sua == NULL) {
		VRT_fail(ctx, "%s: Illegal IP", __func__);
		*dst = NULL;
		return (-1);
	}
	return (VSA_GetPtr(sua, dst));
}

void
VRT_Format_Proxy(struct vsb *vsb, VCL_INT version, VCL_IP sac, VCL_IP sas,
    VCL_STRING auth)
{
	VPX_Format_Proxy(vsb, (int)version, sac, sas, auth);
}

/*
 * Clone a struct vrt_endpoint in a single malloc() allocation
 */

//lint -e{662}  Possible of out-of-bounds pointer (___ beyond end of data)
//lint -e{826}  Suspicious pointer-to-pointer conversion (area to o small
struct vrt_endpoint *
VRT_Endpoint_Clone(const struct vrt_endpoint * const vep)
{
	size_t sz;
	struct vrt_endpoint *nvep;
	struct vrt_blob *blob = NULL;
	struct suckaddr *sa;
	size_t uds_len = 0;
	char *p, *e;

	CHECK_OBJ_NOTNULL(vep, VRT_ENDPOINT_MAGIC);
	sz = sizeof *nvep;
	if (vep->ipv4)
		sz += vsa_suckaddr_len;
	if (vep->ipv6)
		sz += vsa_suckaddr_len;
	if (vep->uds_path != NULL) {
		uds_len = strlen(vep->uds_path) + 1;
		sz += uds_len;
	}
	if (vep->preamble != NULL && vep->preamble->len) {
		sz += sizeof(*blob);
		sz += vep->preamble->len;
	}
	p = calloc(1, sz);
	AN(p);
	e = p + sz;
	nvep = (void*)p;
	p += sizeof *nvep;
	INIT_OBJ(nvep, VRT_ENDPOINT_MAGIC);
	if (vep->ipv4) {
		sa = (void*)p;
		memcpy(sa, vep->ipv4, vsa_suckaddr_len);
		nvep->ipv4 = sa;
		p += vsa_suckaddr_len;
	}
	if (vep->ipv6) {
		sa = (void*)p;
		memcpy(sa, vep->ipv6, vsa_suckaddr_len);
		nvep->ipv6 = sa;
		p += vsa_suckaddr_len;
	}
	if (vep->preamble != NULL && vep->preamble->len) {
		/* Before uds because we need p to be aligned still */
		blob = (void*)p;
		p += sizeof(*blob);
		nvep->preamble = blob;
		memcpy(p, vep->preamble->blob, vep->preamble->len);
		blob->len = vep->preamble->len;
		blob->blob = p;
		p += vep->preamble->len;
	}
	if (uds_len) {
		memcpy(p, vep->uds_path, uds_len);
		nvep->uds_path = p;
		p += uds_len;
	}
	assert(p == e);
	return (nvep);
}
