/* Dialog state. See sip_dialog.h. */

#include "sip_dialog.h"

#include <string.h>

int sip_nameaddr_format(char *out, size_t cap, const sip_nameaddr_t *na)
{
    sip_wbuf_t w;
    sip_w_init(&w, out, cap);
    if (na->display.len) {
        /* a quoted display is kept escaped; an unquoted one is tokens + LWS,
         * which never contain '"' or '\', so quoting it verbatim is safe */
        sip_w_cstr(&w, "\"");
        sip_w_str(&w, na->display);
        sip_w_cstr(&w, "\" ");
    }
    sip_w_cstr(&w, "<");
    sip_w_str(&w, na->uri);
    sip_w_cstr(&w, ">");
    return w.overflow ? -1 : (int)w.len;
}

static int add_route(sip_dialog_t *d, sip_str_t raw)
{
    size_t need = raw.len + 1;
    if (d->routes_len + need > sizeof(d->routes) || d->n_routes == 255) {
        return -1;
    }
    memcpy(d->routes + d->routes_len, raw.p, raw.len);
    d->routes[d->routes_len + raw.len] = '\0';
    d->routes_len = (uint16_t)(d->routes_len + need);
    d->n_routes++;
    return 0;
}

/* i-th route entry */
static const char *route_at(const sip_dialog_t *d, int i)
{
    const char *p = d->routes;
    while (i-- > 0) {
        p += strlen(p) + 1;
    }
    return p;
}

int sip_dialog_set_target(sip_dialog_t *d, const sip_msg_t *m)
{
    if (m->n_contact < 1 || m->contact[0].star) {
        return -1;
    }
    sip_uri_t u;
    if (sip_uri_parse(m->contact[0].uri, &u) != 0) {
        return -1;
    }
    if (m->contact[0].uri.len >= sizeof(d->remote_target)) {
        return -1; /* checked first: sip_str_copy would wipe the old target */
    }
    return sip_str_copy(d->remote_target, sizeof(d->remote_target), m->contact[0].uri) < 0 ? -1 : 0;
}

int sip_dialog_init_uas(sip_dialog_t *d, const sip_msg_t *req, const char *local_tag)
{
    memset(d, 0, sizeof(*d));
    if (sip_str_copy(d->call_id, sizeof(d->call_id), req->call_id) < 0 ||
        sip_str_copy(d->remote_tag, sizeof(d->remote_tag), req->from.tag) < 0 ||
        strlen(local_tag) >= sizeof(d->local_tag) ||
        sip_nameaddr_format(d->remote_na, sizeof(d->remote_na), &req->from) < 0 ||
        sip_nameaddr_format(d->local_na, sizeof(d->local_na), &req->to) < 0) {
        return -1;
    }
    strcpy(d->local_tag, local_tag);
    if (sip_dialog_set_target(d, req) != 0) {
        return -1;
    }
    /* UAS: route set = Record-Route in order (§12.1.1) */
    for (int i = 0; i < req->n_rr; i++) {
        if (add_route(d, req->rr[i].raw) != 0) {
            return -1;
        }
    }
    d->remote_cseq = req->cseq;
    d->remote_cseq_valid = true;
    d->local_cseq = 0;
    return 0;
}

int sip_dialog_init_uac(sip_dialog_t *d, const sip_msg_t *resp, const char *call_id,
                        const char *local_na, const char *local_tag, const char *remote_na,
                        const char *request_uri, uint32_t local_cseq)
{
    memset(d, 0, sizeof(*d));
    if (strlen(call_id) >= sizeof(d->call_id) || strlen(local_na) >= sizeof(d->local_na) ||
        strlen(local_tag) >= sizeof(d->local_tag) || strlen(remote_na) >= sizeof(d->remote_na) ||
        sip_str_copy(d->remote_tag, sizeof(d->remote_tag), resp->to.tag) < 0) {
        return -1;
    }
    strcpy(d->call_id, call_id);
    strcpy(d->local_na, local_na);
    strcpy(d->local_tag, local_tag);
    strcpy(d->remote_na, remote_na);
    if (sip_dialog_set_target(d, resp) != 0) {
        /* no Contact (e.g. some 18x): keep the original Request-URI */
        if (strlen(request_uri) >= sizeof(d->remote_target)) {
            return -1;
        }
        strcpy(d->remote_target, request_uri);
    }
    /* UAC: route set = Record-Route in reverse order (§12.1.2) */
    for (int i = resp->n_rr - 1; i >= 0; i--) {
        if (add_route(d, resp->rr[i].raw) != 0) {
            return -1;
        }
    }
    d->local_cseq = local_cseq;
    return 0;
}

bool sip_dialog_matches_request(const sip_dialog_t *d, const sip_msg_t *req)
{
    return sip_str_eq(req->call_id, sip_str_c(d->call_id)) &&
           sip_str_eq(req->to.tag, sip_str_c(d->local_tag)) &&
           sip_str_eq(req->from.tag, sip_str_c(d->remote_tag));
}

void sip_dialog_write_request(const sip_dialog_t *d, sip_wbuf_t *w, const char *method,
                              uint32_t cseq, const char *sent_by, const char *branch)
{
    bool strict = false;
    sip_nameaddr_t first;
    if (d->n_routes > 0) {
        sip_uri_t u;
        const char *r0 = route_at(d, 0);
        if (sip_nameaddr_parse(sip_str_c(r0), &first, false) == 0 &&
            sip_uri_parse(first.uri, &u) == 0 && !u.lr) {
            strict = true;
        }
    }
    /* §12.2.1.1 */
    if (strict) {
        /* strict router: its URI becomes the Request-URI (minus params not
         * allowed there; we keep it as-is apart from header params) */
        sip_w_fmt(w, "%s ", method);
        sip_w_str(w, first.uri);
        sip_w_cstr(w, " SIP/2.0\r\n");
    } else {
        sip_w_fmt(w, "%s %s SIP/2.0\r\n", method, d->remote_target);
    }
    sip_w_fmt(w, "Via: SIP/2.0/UDP %s;rport;branch=%s\r\n", sent_by, branch);
    sip_w_cstr(w, "Max-Forwards: 70\r\n");
    sip_w_fmt(w, "From: %s;tag=%s\r\n", d->local_na, d->local_tag);
    if (d->remote_tag[0]) {
        sip_w_fmt(w, "To: %s;tag=%s\r\n", d->remote_na, d->remote_tag);
    } else {
        sip_w_fmt(w, "To: %s\r\n", d->remote_na);
    }
    sip_w_fmt(w, "Call-ID: %s\r\n", d->call_id);
    sip_w_fmt(w, "CSeq: %lu %s\r\n", (unsigned long)cseq, method);
    for (int i = strict ? 1 : 0; i < d->n_routes; i++) {
        sip_w_fmt(w, "Route: %s\r\n", route_at(d, i));
    }
    if (strict) {
        sip_w_fmt(w, "Route: <%s>\r\n", d->remote_target);
    }
}
