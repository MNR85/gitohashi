/*
 * gitohashi protocol
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#define LWS_DLL
#define LWS_INTERNAL
#include <libwebsockets.h>
#include <string.h>
#include <stdlib.h>

#include <libjsongit2.h>

/*
 * This belongs to the opaque lws task once it is enqueued, and is freed when
 * the task goes out of scope
 */

struct task_data_gitohashi {
	char buf[LWS_PRE + 4096];
	char url[1024], alang[128], ua[256], inm[36];
	int frametype;
	struct jg2_ctx *ctx;
	size_t used;
	char final;
	char outlive;
};

struct pss_gitohashi {
	struct lws *wsi;
	int state;
};

struct vhd_gitohashi {
	const char *html, *vpath, *repo_base_dir, *acl_user, *avatar_url;
	const struct lws_protocols *cache_protocol;
	struct lws_context *context;
	struct jg2_vhost *jg2_vhost;
	lws_sorted_usec_list_t sul;
	struct lws_threadpool *tp;
	struct lws_vhost *vhost;
};


static void
cleanup_task_private_data(struct lws *wsi, void *user)
{
	struct task_data_gitohashi *priv = (struct task_data_gitohashi *)user;

	if (priv->ctx)
		jg2_ctx_destroy(priv->ctx);

	free(priv);
}

static enum lws_threadpool_task_return
task_function(void *user, enum lws_threadpool_task_status s)
{
	struct task_data_gitohashi *priv = (struct task_data_gitohashi *)user;
	int n, flags = 0, opa;
	char outlive = 0;

	/*
	 * first time, we must do the http reply, and either acquire the
	 * jg2 ctx or finish the transaction
	 */
	if (!priv->ctx)
		return LWS_TP_RETURN_SYNC;

	/* we sent the last bit already */

	if (!priv->outlive && priv->frametype == LWS_WRITE_HTTP_FINAL)
		return LWS_TP_RETURN_FINISHED;

	priv->frametype = LWS_WRITE_HTTP;
	n = jg2_ctx_fill(priv->ctx, priv->buf + LWS_PRE,
			 sizeof(priv->buf) - LWS_PRE, &priv->used, &outlive);

	opa = priv->outlive;
	if (outlive) {
		priv->outlive = 1;
		flags = LWS_TP_RETURN_FLAG_OUTLIVE;
	}

	if (n < 0)
		return LWS_TP_RETURN_STOPPED;

	if (n || priv->final) {
		priv->frametype = LWS_WRITE_HTTP_FINAL;
		priv->final = 1;
	}

	if (priv->used) {
		if (opa)
			/*
			 * he can't send anything when in outlive mode.
			 * take it as his having finished.
			 */
			return LWS_TP_RETURN_FINISHED;

		return LWS_TP_RETURN_SYNC | flags;
	}

	return LWS_TP_RETURN_CHECKING_IN | flags;
}

static int
http_reply(struct lws *wsi, struct vhd_gitohashi *vhd,
	   struct pss_gitohashi *pss, struct task_data_gitohashi *priv)
{
	unsigned char *p = (unsigned char *)&priv->buf[LWS_PRE], *start = p,
		      *end = (unsigned char *)priv->buf + sizeof(priv->buf);
	const char *mimetype = NULL;
	struct jg2_ctx_create_args args;
	unsigned long length = 0;
	char etag[36];
	int n;

	memset(&args, 0, sizeof(args));
	args.repo_path = priv->url;
	args.flags = JG2_CTX_FLAG_HTML;
	args.mimetype = &mimetype;
	args.length = &length;
	args.etag = etag;
	args.etag_length = sizeof(etag);

	if (priv->alang[0])
		args.accept_language = priv->alang;

	if (priv->inm[0])
		args.client_etag = priv->inm;


	/*
	 * Let's assess from his user agent if he's a bot.  Caching
	 * content generated by a bot is less than worthless since they
	 * spider the whole repo more or less randomly, flushing the
	 * parts of the cache generated by real users which may be
	 * interesting for other users too.
	 */

	if (priv->ua[0] &&
	    (strstr(priv->ua, "bot") || strstr(priv->ua, "Bot")))
		args.flags |= JG2_CTX_FLAG_BOT;

	p = start;

	if (jg2_ctx_create(vhd->jg2_vhost, &priv->ctx, &args)) {

		lwsl_info("%s: jg2_ctx_create fail: %s\n", __func__, start);

		lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN,
				"403 Forbidden");
		return 0;
	}

	/* does he actually already have a current version of it? */

	n = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_IF_NONE_MATCH);
	if (etag[0] && n && !strcmp(etag, priv->inm)) {

		lwsl_debug("%s: ETAG match %s\n", __func__, etag);

		/* we don't need to send the payload... lose the ctx */

		jg2_ctx_destroy(priv->ctx);
		priv->ctx = NULL;

		/* inform the client he already has the latest guy */

		if (lws_add_http_header_status(wsi, HTTP_STATUS_NOT_MODIFIED,
					       &p, end))
			return -1;

		if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ETAG,
				(unsigned char *)etag, n, &p, end))
			return -1;

		if (lws_finalize_http_header(wsi, &p, end))
			return 1;

		n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS |
						     LWS_WRITE_H2_STREAM_END);
		if (n != (p - start)) {
			lwsl_err("_write returned %d from %ld\n", n,
				 (long)(p - start));
			return -1;
		}

		goto transaction_completed;
	}

	/* nope... he doesn't already have it, so we must issue it */

	if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
			mimetype, length? length :
			LWS_ILLEGAL_HTTP_CONTENT_LEN, &p, end))
		return 1;

	/*
	 * if we know the etag already, issue it so we can recognize
	 * if he asks for it again while he already has it
	 */

	if (etag[0] && lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ETAG,
						    (unsigned char *)etag,
						    strlen(etag), &p, end))
		return 1;

	if (lws_finalize_write_http_header(wsi, start, &p, end))
		return 1;

	return 0;

transaction_completed:

	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

/*
 * Called from a threadpool thread context...
 */

void refchange(void * user)
{
	struct pss_gitohashi *pss = (struct pss_gitohashi *)user;

	lwsl_notice("%s: %p\n", __func__, pss);

//	if (!pss)
//		return;

	// lws_callback_on_writable(pss->wsi);
}

static const char *hex = "0123456789abcdef";

static const char *
md5_to_hex_cstr(char *md5_hex_33, const unsigned char *md5)
{
	int n;

	if (!md5) {
		*md5_hex_33++ = '?';
		*md5_hex_33++ = '\0';

		return md5_hex_33 - 2;
	}
	for (n = 0; n < 16; n++) {
		*md5_hex_33++ = hex[((*md5) >> 4) & 0xf];
		*md5_hex_33++ = hex[*(md5++) & 0xf];
	}
	*md5_hex_33 = '\0';

	return md5_hex_33 - 32;
}

/*
 * Called from a threadpool thread context...
 */

int avatar(void *avatar_arg, const unsigned char *md5)
{
	struct vhd_gitohashi *vhd = (struct vhd_gitohashi *)avatar_arg;
	typedef int (*mention_t)(const struct lws_protocols *pcol,
			struct lws_vhost *vh, const char *path);
	char md[256];

	if (!vhd->cache_protocol)
		vhd->cache_protocol = lws_vhost_name_to_protocol(
					vhd->vhost, "avatar-proxy");

	if (!vhd->cache_protocol)
		return 0;

	md5_to_hex_cstr(md, md5);

	((mention_t)(void *)vhd->cache_protocol->user)
			(vhd->cache_protocol, vhd->vhost, md);

	return 0;
}

static void
dump_cb(lws_sorted_usec_list_t *sul)
{
	struct vhd_gitohashi *vhd = lws_container_of(sul, struct vhd_gitohashi, sul);
	/*
	 * in debug mode, dump the threadpool stat to the logs once
	 * a second
	 */
	//lws_threadpool_dump(vhd->tp);
	lws_sul_schedule(vhd->context, 0, &vhd->sul, dump_cb, 1 * LWS_US_PER_SEC);
}

static int
callback_gitohashi(struct lws *wsi, enum lws_callback_reasons reason,
	       void *user, void *in, size_t len)
{
	struct vhd_gitohashi *vhd = (struct vhd_gitohashi *)
			      lws_protocol_vh_priv_get(lws_get_vhost(wsi),
						       lws_get_protocol(wsi));
	char buf[LWS_PRE + 4096];
	unsigned char *p = (unsigned char *)&buf[LWS_PRE],
		      *end = (unsigned char *)buf + sizeof(buf);
	struct pss_gitohashi *pss = (struct pss_gitohashi *)user;
	struct lws_threadpool_create_args cargs;
	struct lws_threadpool_task_args targs;
	struct task_data_gitohashi *priv;
	struct jg2_vhost_config config;
	const char *csize, *flags, *z;
	int n, uid, gid;
	void *_user;

	switch (reason) {

	/* --------------- protocol --------------- */

	case LWS_CALLBACK_PROTOCOL_INIT: /* per vhost */
		lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
					    lws_get_protocol(wsi),
					    sizeof(struct vhd_gitohashi));
		vhd = (struct vhd_gitohashi *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
						 lws_get_protocol(wsi));

		vhd->vhost = lws_get_vhost(wsi);
		vhd->context = lws_get_context(wsi);

		if (lws_pvo_get_str(in, "html-file", &vhd->html) ||
		    lws_pvo_get_str(in, "vpath", &vhd->vpath) ||
		    lws_pvo_get_str(in, "repo-base-dir", &vhd->repo_base_dir) ||
		    lws_pvo_get_str(in, "acl-user", &vhd->acl_user) ||
		    lws_pvo_get_str(in, "avatar-url", &vhd->avatar_url)) {

			lwsl_err("%s: required pvos: html-file, vpath,"
				 "repo-base-dir, acl-user, avatar-url\n",
				 __func__);

			return -1;
		}


		memset(&cargs, 0, sizeof(cargs));

		cargs.max_queue_depth = 12;
		cargs.threads = 4;

		if (!lws_pvo_get_str(in, "threads", &z))
			cargs.threads = atoi(z);
		if (!lws_pvo_get_str(in, "max_queue_depth", &z))
			cargs.max_queue_depth = atoi(z);

		vhd->tp = lws_threadpool_create(lws_get_context(wsi), &cargs,
						"%s",
						lws_get_vhost_name(vhd->vhost));
		if (!vhd->tp)
			return -1;

		memset(&config, 0, sizeof(config));
		config.virtual_base_urlpath = vhd->vpath;
		config.refchange = refchange;
		config.avatar = avatar;
		config.avatar_arg = vhd;
		config.avatar_url = vhd->avatar_url;
		config.repo_base_dir = vhd->repo_base_dir;
		config.vhost_html_filepath = vhd->html;
		config.acl_user = vhd->acl_user;

		/* optional... no caching if not set */
		if (!lws_pvo_get_str(in, "cache-base",
				     &config.json_cache_base)) {
			lws_get_effective_uid_gid(lws_get_context(wsi), &uid,
						  &gid);
			config.cache_uid = uid;

			/* optional, default size if not set */
			if (!lws_pvo_get_str(in, "cache-size", &csize))
				config.cache_size_limit = atoi(csize);
		}

		/* optional... flags */
		if (!lws_pvo_get_str(in, "flags", &flags))
			config.flags = atoi(flags);

		if (config.flags & JG2_VHOST_BLOG_MODE &&
		    lws_pvo_get_str(in, "blog-repo-name",
				    &config.blog_repo_name)) {
			lwsl_err("%s: if blog_mode set in flags, "
				 "blog_repo_name is required\n", __func__);

			lws_threadpool_destroy(vhd->tp);

			return -1;
		}

		vhd->jg2_vhost = jg2_vhost_create(&config);
		if (!vhd->jg2_vhost) {
			lws_threadpool_destroy(vhd->tp);
			return -1;
		}

		lws_sul_schedule(vhd->context, 0, &vhd->sul, dump_cb, 1);
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY: /* per vhost */
		jg2_vhost_destroy(vhd->jg2_vhost);
		lws_threadpool_finish(vhd->tp);
		lws_threadpool_destroy(vhd->tp);
		vhd->jg2_vhost = NULL;
		break;

	/* --------------- http --------------- */

	case LWS_CALLBACK_HTTP:

		/*
		 * The jg2 ctx is not going to be created until a thread
		 * becomes available.  But our headers will be scrubbed when
		 * we return from this.
		 *
		 * So we must stash any interesting header content in the user
		 * priv struct before queuing the task.
		 */

		memset(&targs, 0, sizeof(targs));
		priv = targs.user = malloc(sizeof(*priv));
		if (!priv)
			return 1;

		memset(priv, 0, sizeof(*priv));

		targs.wsi = wsi;
		targs.task = task_function;
		targs.cleanup = cleanup_task_private_data;

		/*
		 * "in" contains the url part after our mountpoint, if any.
		 *
		 * Our strategy is to record the URL for the duration of the
		 * transaction and return the user's configured html template,
		 * plus JSON prepared based on the URL.  That lets the page
		 * display remotely in one roundtrip (+tls) without having to
		 * wait for the ws link to come up.
		 *
		 * Serving anything other than the configured html template
		 * will have to come from outside this mount URL path.
		 *
		 * Stash in and the urlargs into pss->priv->url[]
		 */

		p = (unsigned char *)priv->url;
		end = p + sizeof(priv->url) - 1;
		if ((int)len >= end - p)
			len = end - p - 1;
		memcpy(p, in, len);
		p += len;

		n = 0;
		while (lws_hdr_copy_fragment(wsi, (char *)p + 1, end - p - 2,
					     WSI_TOKEN_HTTP_URI_ARGS, n) > 0) {
			if (!n)
				*p = '?';
			else
				*p = '&';

			p += strlen((char *)p);
			n++;
		}

		*p++ = '\0';

		if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_USER_AGENT) &&
		    lws_hdr_copy(wsi, priv->ua, sizeof(priv->ua),
				 WSI_TOKEN_HTTP_USER_AGENT) < 0)
			priv->ua[0] = '\0';

		if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_ACCEPT_LANGUAGE) &&
		    lws_hdr_copy(wsi, priv->alang,
				 sizeof(priv->alang),
				 WSI_TOKEN_HTTP_ACCEPT_LANGUAGE) < 0)
			priv->alang[0] = '\0';

		n = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_IF_NONE_MATCH);
		if (n && lws_hdr_copy(wsi, priv->inm,
				      sizeof(priv->inm),
				      WSI_TOKEN_HTTP_IF_NONE_MATCH) < 0)
			priv->inm[0] = '\0';

		/*
		 * that's all the info we need... queue the task to do the
		 * actual business (priv is passed by targs.user)
		 */

		if (!lws_threadpool_enqueue(vhd->tp, &targs, "goh-%s",
					    (const char *)in)) {
			lwsl_user("%s: Couldn't enqueue task\n", __func__);
			cleanup_task_private_data(wsi, priv);
			return 1;
		}

		lws_set_timeout(wsi, PENDING_TIMEOUT_THREADPOOL, 30);

		/*
		 * the task will get serviced, see that it doesn't have any
		 * jg2 ctx yet, and SYNC until we got a WRITEABLE callback
		 * (which will usually be very quickly, because this HTTP
		 * callback only happens when we are writeable, and we didn't
		 * write anything yet)
		 */

		return 0;

	case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
		if (pss) {
			lwsl_info("%s: HTTP_DROP_PROTOCOL: %s %p\n", __func__,
				   (const char *)in, wsi);
			if (lws_threadpool_get_task_wsi(wsi))
				lws_threadpool_dequeue_task(lws_threadpool_get_task_wsi(wsi));
		}
		return 0;

	case LWS_CALLBACK_CLOSED_HTTP:
		return 0;

	case LWS_CALLBACK_HTTP_WRITEABLE:

		if (!pss)
			break;

		n = lws_threadpool_task_status(lws_threadpool_get_task_wsi(wsi), &_user);
		lwsl_info("%s: LWS_CALLBACK_SERVER_WRITEABLE: %p: "
			   "priv %p, status %d\n", __func__, wsi, _user, n);
		switch(n) {
		case LWS_TP_STATUS_FINISHED:
		case LWS_TP_STATUS_STOPPED:
		case LWS_TP_STATUS_QUEUED:
		case LWS_TP_STATUS_RUNNING:
		case LWS_TP_STATUS_STOPPING:
			return 0;

		case LWS_TP_STATUS_SYNCING:
			/* the task has paused for us to do something */
			break;

		default:
			/* wsi has no discernable task */
			//lwsl_err("%s: HTTP_WRITABLE: wsi has no task\n",
			//		__func__);
			return 1;
		}

		priv = (struct task_data_gitohashi *)_user;

		if (!priv->ctx) {
			/*
			 * Do the http response and maybe acquire the jg2 ctx.
			 * Sometimes that was all we needed to do (eg, ETAG
			 * matched) and there's no jg2_ctx: the transaction is
			 * completed then.
			 */
			n = http_reply(wsi, vhd, pss, priv);
			if (!priv->ctx) {
				/* unblock him and stop him as we are done */
				lws_threadpool_task_sync(lws_threadpool_get_task_wsi(wsi), 1);

				return n;
			}

			lws_set_timeout(wsi, PENDING_TIMEOUT_THREADPOOL_TASK,
					60);

			/*
			 * otherwise we are just getting started... unblock the
			 * worker thread and start the business of filling
			 * buffers and sending them on.
			 */

			goto sync_end;
		}

		if (priv->used) {
			lwsl_info("  writing %d\n", (int)priv->used);
			lwsl_hexdump_debug(priv->buf + LWS_PRE, priv->used);
			if (lws_write(wsi, (unsigned char *)priv->buf + LWS_PRE,
				      priv->used, priv->frametype) !=
				      (int)priv->used) {
				lwsl_err("%s: lws_write failed\n", __func__);

				return -1;
			}
			priv->used = 0;

			if (priv->frametype == LWS_WRITE_HTTP_FINAL) {
				lws_threadpool_task_sync(lws_threadpool_get_task_wsi(wsi), !priv->outlive);
				goto transaction_completed;
			}
		}

sync_end:
		lws_threadpool_task_sync(lws_threadpool_get_task_wsi(wsi), 0);

		return 0;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);

transaction_completed:
	if (lws_http_transaction_completed(wsi))
		return -1;

	return 0;
}

#define LWS_PLUGIN_PROTOCOL_GITOHASHI \
	{ \
		"gitohashi", \
		callback_gitohashi, \
		sizeof(struct pss_gitohashi), \
		4096, \
	}

#if !defined (LWS_PLUGIN_STATIC)

static const struct lws_protocols protocols[] = {
	LWS_PLUGIN_PROTOCOL_GITOHASHI
};

LWS_EXTERN LWS_VISIBLE int
init_protocol_gitohashi(struct lws_context *context,
				struct lws_plugin_capability *c)
{
	if (c->api_magic != LWS_PLUGIN_API_MAGIC) {
		lwsl_err("Plugin API %d, library API %d",
			 LWS_PLUGIN_API_MAGIC, c->api_magic);
		return 1;
	}

	c->protocols = protocols;
	c->count_protocols = LWS_ARRAY_SIZE(protocols);
	c->extensions = NULL;
	c->count_extensions = 0;

	return 0;
}

LWS_EXTERN LWS_VISIBLE int
destroy_protocol_gitohashi(struct lws_context *context)
{
	return 0;
}
#endif
