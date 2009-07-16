
#include <lighttpd/base.h>
#include <lighttpd/url_parser.h>

void li_request_init(liRequest *req) {
	req->http_method = LI_HTTP_METHOD_UNSET;
	req->http_method_str = g_string_sized_new(0);
	req->http_version = LI_HTTP_VERSION_UNSET;

	req->uri.raw = g_string_sized_new(0);
	req->uri.scheme = g_string_sized_new(0);
	req->uri.authority = g_string_sized_new(0);
	req->uri.path = g_string_sized_new(0);
	req->uri.orig_path = g_string_sized_new(0);
	req->uri.query = g_string_sized_new(0);
	req->uri.host = g_string_sized_new(0);

	req->headers = li_http_headers_new();

	req->content_length = -1;
}

void li_request_reset(liRequest *req) {
	req->http_method = LI_HTTP_METHOD_UNSET;
	g_string_truncate(req->http_method_str, 0);
	req->http_version = LI_HTTP_VERSION_UNSET;

	g_string_truncate(req->uri.raw, 0);
	g_string_truncate(req->uri.scheme, 0);
	g_string_truncate(req->uri.authority, 0);
	g_string_truncate(req->uri.path, 0);
	g_string_truncate(req->uri.orig_path, 0);
	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.host, 0);

	li_http_headers_reset(req->headers);

	req->content_length = -1;
}

void li_request_clear(liRequest *req) {
	req->http_method = LI_HTTP_METHOD_UNSET;
	g_string_free(req->http_method_str, TRUE);
	req->http_version = LI_HTTP_VERSION_UNSET;

	g_string_free(req->uri.raw, TRUE);
	g_string_free(req->uri.scheme, TRUE);
	g_string_free(req->uri.authority, TRUE);
	g_string_free(req->uri.path, TRUE);
	g_string_free(req->uri.orig_path, TRUE);
	g_string_free(req->uri.query, TRUE);
	g_string_free(req->uri.host, TRUE);

	li_http_headers_free(req->headers);

	req->content_length = -1;
}

/* closes connection after response */
static void bad_request(liConnection *con, int status) {
	con->keep_alive = FALSE;
	con->mainvr->response.http_status = status;
	li_vrequest_handle_direct(con->mainvr);
}

static gboolean request_parse_url(liVRequest *vr) {
	liRequest *req = &vr->request;

	g_string_truncate(req->uri.query, 0);
	g_string_truncate(req->uri.path, 0);

	if (!li_parse_raw_url(&req->uri))
		return FALSE;

	/* "*" only allowed for method OPTIONS */
	if (0 == strcmp(req->uri.path->str, "*") && req->http_method != LI_HTTP_METHOD_OPTIONS)
		return FALSE;

	li_url_decode(req->uri.path);
	li_path_simplify(req->uri.path);

	if (0 == req->uri.orig_path->len) {
		g_string_append_len(req->uri.orig_path, GSTR_LEN(req->uri.path)); /* save orig path */
	}

	return TRUE;
}

gboolean li_request_validate_header(liConnection *con) {
	liRequest *req = &con->mainvr->request;
	liHttpHeader *hh;
	GList *l;

	switch (req->http_version) {
	case LI_HTTP_VERSION_1_0:
		if (!li_http_header_is(req->headers, CONST_STR_LEN("connection"), CONST_STR_LEN("keep-alive")))
			con->keep_alive = FALSE;
		break;
	case LI_HTTP_VERSION_1_1:
		if (li_http_header_is(req->headers, CONST_STR_LEN("connection"), CONST_STR_LEN("close")))
			con->keep_alive = FALSE;
		break;
	case LI_HTTP_VERSION_UNSET:
		bad_request(con, 505); /* Version not Supported */
		return FALSE;
	}

	if (req->uri.raw->len == 0) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* get hostname */
	l = li_http_header_find_first(req->headers, CONST_STR_LEN("host"));
	if (NULL != l) {
		if (NULL != li_http_header_find_next(l, CONST_STR_LEN("host"))) {
			/* more than one "host" header */
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		hh = (liHttpHeader*) l->data;
		g_string_append_len(req->uri.authority, HEADER_VALUE_LEN(hh));
		if (!li_parse_hostname(&req->uri)) {
			bad_request(con, 400); /* bad request */
			return FALSE;
		}
	}

	/* Need hostname in HTTP/1.1 */
	if (req->uri.host->len == 0 && req->http_version == LI_HTTP_VERSION_1_1) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* may override hostname */
	if (!request_parse_url(con->mainvr)) {
		bad_request(con, 400); /* bad request */
		return FALSE;
	}

	/* content-length */
	hh = li_http_header_lookup(req->headers, CONST_STR_LEN("content-length"));
	if (hh) {
		const gchar *val = HEADER_VALUE(hh);
		gint64 r;
		char *err;

		r = g_ascii_strtoll(val, &err, 10);
		if (*err != '\0') {
			_DEBUG(con->srv, con->mainvr, "content-length is not a number: %s (Status: 400)", err);
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		/**
			* negative content-length is not supported
			* and is a bad request
			*/
		if (r < 0) {
			bad_request(con, 400); /* bad request */
			return FALSE;
		}

		/**
			* check if we had a over- or underrun in the string conversion
			*/
		if (r == G_MININT64 || r == G_MAXINT64) {
			if (errno == ERANGE) {
				bad_request(con, 413); /* Request Entity Too Large */
				return FALSE;
			}
		}

		con->mainvr->request.content_length = r;
	}

	/* Expect: 100-continue */
	l = li_http_header_find_first(req->headers, CONST_STR_LEN("expect"));
	if (l) {
		gboolean expect_100_cont = FALSE;

		for ( ; l ; l = li_http_header_find_next(l, CONST_STR_LEN("expect")) ) {
			hh = (liHttpHeader*) l->data;
			if (0 == g_strcasecmp( HEADER_VALUE(hh), "100-continue" )) {
				expect_100_cont = TRUE;
			} else {
				/* we only support 100-continue */
				bad_request(con, 417); /* Expectation Failed */
				return FALSE;
			}
		}

		if (expect_100_cont && req->http_version == LI_HTTP_VERSION_1_0) {
			/* only HTTP/1.1 clients can send us this header */
			bad_request(con, 417); /* Expectation Failed */
			return FALSE;
		}
		con->expect_100_cont = expect_100_cont;
	}

	/* TODO: headers:
	 * - If-Modified-Since (different duplicate check)
	 * - If-None-Match (different duplicate check)
	 * - Range (duplicate check)
	 */

	switch(con->mainvr->request.http_method) {
	case LI_HTTP_METHOD_GET:
	case LI_HTTP_METHOD_HEAD:
		/* content-length is forbidden for those */
		if (con->mainvr->request.content_length > 0) {
			VR_ERROR(con->mainvr, "%s", "GET/HEAD with content-length -> 400");

			bad_request(con, 400); /* bad request */
			return FALSE;
		}
		con->mainvr->request.content_length = 0;
		break;
	case LI_HTTP_METHOD_POST:
		/* content-length is required for them */
		if (con->mainvr->request.content_length == -1) {
			/* content-length is missing */
			VR_ERROR(con->mainvr, "%s", "POST-request, but content-length missing -> 411");

			bad_request(con, 411); /* Length Required */
			return FALSE;
		}
		break;
	default:
		/* the may have a content-length */
		break;
	}

	return TRUE;
}

void li_physical_init(liPhysical *phys) {
	phys->path = g_string_sized_new(127);
	phys->basedir = g_string_sized_new(63);
	phys->doc_root = g_string_sized_new(63);
	phys->rel_path = g_string_sized_new(63);
	phys->pathinfo = g_string_sized_new(63);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}

void li_physical_reset(liPhysical *phys) {
	g_string_truncate(phys->path, 0);
	g_string_truncate(phys->basedir, 0);
	g_string_truncate(phys->doc_root, 0);
	g_string_truncate(phys->rel_path, 0);
	g_string_truncate(phys->pathinfo, 0);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}

void li_physical_clear(liPhysical *phys) {
	g_string_free(phys->path, TRUE);
	g_string_free(phys->basedir, TRUE);
	g_string_free(phys->doc_root, TRUE);
	g_string_free(phys->rel_path, TRUE);
	g_string_free(phys->pathinfo, TRUE);
	phys->have_stat = FALSE;
	phys->have_errno = FALSE;
}