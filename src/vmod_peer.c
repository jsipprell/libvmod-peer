#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include <curl/curl.h>

#include "vmod_peer.h"

typedef struct vmod_hdr {
  char *key, *value;
  VTAILQ_ENTRY(vmod_hdr) list;
} vmod_hdr_t;

struct peer_req {
  unsigned magic;
#define VMOD_PEER_REQ_MAGIC 0xf4bbef1c
  const char *tag;
  char *host,*method,*proto,*url;
  struct vsb *body;
  CURL *c;
  struct vmod_peer *vp;
  struct ws *ws; /* note: if same as vp->ws, not released when req is released */
  VTAILQ_HEAD(,vmod_hdr) headers;
  VTAILQ_ENTRY(peer_req) list;
};

typedef struct vmod_pthr vmod_pthr_t;

struct vmod_peer {
  unsigned magic;
#define VMOD_PEER_MAGIC 0x5bba391c
  pthread_key_t wsk;
  unsigned nthreads,min,max;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int shutdown;
  VTAILQ_HEAD(, peer_req) q;
  size_t qlen;
  unsigned nwaiters;
  vmod_pthr_t *threads;
  size_t tsz;
  const char *host;
  int port,c_to,d_to;
  CURLSH *csh;

  /* SSL stuff */
  const char *ssl_cafile, *ssl_capath;
  uint32_t ssl_flags;
};

#define VP_SSL_VERIFY_PEER 0x0001
#define VP_SSL_VERIFY_HOST 0x0002
#define VP_SSL_ON          0x0004

struct vmod_pthr {
  unsigned magic;
#define VMOD_PEER_THREAD_MAGIC 0x1c93abb5
  pthread_t tid;
  int alive;
  int *shutdown;  /* normally points to vmod_peer shutdown */
  pthread_mutex_t *imtx;
  pthread_cond_t *icond;
  struct vmod_peer *vp;
};

extern void VTCP_name(const struct suckaddr*,char*,unsigned,char*,unsigned);
extern double VTIM_real(void);

static int initialized = 0;
static vp_lock_t *curl_locks[CURL_LOCK_DATA_LAST+1];


static inline
void vp_free(void **vp)
{
  void *p;
  AN(vp);
  p = *vp;
  *vp = NULL;
  if(p != NULL)
    free(p);
}

#define VPFREE(x) vp_free((void**)&(x))

/* forward decls */
static int vp_start_thread(struct vmod_peer*);
static int vp_start_thread_safe(struct vmod_peer*);
static void vp_log_req(const struct vrt_ctx *ctx, enum VSL_tag_e,
                                            const char *fmt, ...);
static void vp_log_bare(enum VSL_tag_e, uint32_t, const char *fmt, ...);
static void vp_log_ws(struct ws*, enum VSL_tag_e, uint32_t,
                      const char*, ...);
static void vp_log(const struct vmod_peer*, enum VSL_tag_e, uint32_t id,
                                                  const char *fmt, ...);
#define VPLOGR vp_log_req
#define VPLOG(tag,fmt,...) vp_log_bare((tag),0,(fmt), ## __VA_ARGS__)
#define VPLOGFD(tag,fd,fmt,...) vp_log((tag),(fd),(fmt), ## __VA_ARGS__)
#define VPLOGWS(ws,tag,fmt,...) vp_log_ws((ws),(tag),0,(fmt), ## __VA_ARGS__)
#define VPLOGIG VPLOGFD

#define VPLOG_Debug(vp,fmt,...) vp_log((vp),SLT_Debug,0,(fmt), ## __VA_ARGS__)
#define VPLOG_Error(vp,fmt,...) vp_log((vp),SLT_Error,0,(fmt), ## __VA_ARGS__)

static
void vp_log_ws_va(struct ws *ws, enum VSL_tag_e tag,
                  uint32_t id, const char *fmt, va_list ap)
{
  unsigned u = 0;
  char *msg,*mark;

  AN(ws);
  mark = WS_Snapshot(ws);
  u = WS_Reserve(ws,0);
  if (u == 0) {
    WS_Release(ws,0);
    return;
  }
  msg = ws->f;
  if (u) {
    unsigned v;
    assert(ws->f == msg);
    v = vsnprintf(msg,u,fmt,ap);

    if (v > 0) {
      WS_Release(ws, v+1);
      VSL(tag, 0, "vmod_peer: %s", msg);
      u = 0;
    }
  }
  if(u)
    WS_Release(ws,0);
  WS_Reset(ws,mark);
}

static
void vp_log_ws(struct ws *ws, enum VSL_tag_e tag,
               uint32_t id, const char *fmt, ...)
{
  va_list ap;
  va_start(ap,fmt);
  vp_log_ws_va(ws,tag,id,fmt,ap);
  va_end(ap);
}

static
void vp_log_req(const struct vrt_ctx *ctx, enum VSL_tag_e tag,
                                         const char *fmt, ...)
{
  unsigned u = 0;
  va_list ap;
  char *s;


  CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
  AN(ctx->ws);
  u = WS_Reserve(ctx->ws,0);
  if(u <= 13) {
    WS_Release(ctx->ws,0);
    return;
  }
  s = stpcpy(ctx->ws->f,"vmod_peer: ");
  AN(s);
  strncpy(s,fmt,u-12);
  s = ctx->ws->f;

  AN(s);
  va_start(ap,fmt);
  VSLbv(ctx->vsl, tag, s, ap);
  va_end(ap);
  WS_Release(ctx->ws, 0);
}

static
void vp_log_bare(enum VSL_tag_e tag, uint32_t id, const char *fmt, ...)
{
  va_list ap;
  char buf[4096],*b = NULL;
  int l;
  b = buf;
  va_start(ap,fmt);
  l = vsnprintf(b,4095,fmt,ap);
  va_end(ap);
  if (l < 4094)
    VSL(tag,id,"vmod_peer: %s",b);
}

static inline
struct ws *vp_ws_new(const char *label, size_t sz)
{
  struct ws *ws = NULL;
  char *m;
  if (sz <= PRNDUP(sizeof(*ws)))
    sz = PRNDUP(sz) + PRNDUP(sizeof(*ws));
  else
    sz = PRNDUP(sz);
  AN(sz);
  m = malloc(sz);
  if (m != NULL) {
    ws = (struct ws*)m;
    m += PRNDUP(sizeof *ws);
    WS_Init(ws,label,m,sz-PRNDUP(sizeof *ws));
  }
  return ws;
}

static
struct ws *vmod_peer_ws(pthread_key_t wsk)
{
  struct ws *ws;

  ws = pthread_getspecific(wsk);
  AN(cache_param);
  if(ws == NULL) {
#ifdef DEBUG
    ws = vp_ws_new("vpp",cache_param->workspace_thread * 8);
#else
    ws = vp_ws_new("vpp",cache_param->workspace_thread);
#endif
    if(ws != NULL)
      AZ(pthread_setspecific(wsk,ws));
  }
  return ws;
}

static
struct ws *vmod_peer_ip_ws(const struct vmod_peer_ip *vpp)
{
  CHECK_OBJ_NOTNULL(vpp, VMOD_PEER_IP_MAGIC);
  return vmod_peer_ws(vpp->wsk);
}

static
struct ws *vp_ws_get(const struct vmod_peer *vp)
{
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  return vmod_peer_ws(vp->wsk);
}

static
int vp_ws_check(const struct vmod_peer *vp, struct ws **p)
{
  struct ws *ws;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  ws = pthread_getspecific(vp->wsk);
  if(ws == NULL) {
    if(p)
      *p = NULL;
    return 0;
  }
  if(p)
    *p = ws;
  return 1;
}

static
void vp_ws_free(void *v)
{
  WS_Assert((struct ws*)v);
  free(v);
}

static
void vp_log(const struct vmod_peer *vp, enum VSL_tag_e tag,
             uint32_t id, const char *fmt, ...)
{
  va_list ap;
  struct ws *ws = vp_ws_get(vp);
  va_start(ap,fmt);
  vp_log_ws_va(ws,tag,id,fmt,ap);
  va_end(ap);
}

static
void vp_req_set_header(struct vmod_peer *ctx, struct peer_req *r,
                       const char *key, const char *val)
{
  vmod_hdr_t *h, *h2;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(val) {
    AN(key);
    VTAILQ_FOREACH(h, &r->headers, list) {
      if(strcasecmp(h->key,key) == 0) {
        free(h->value);
        h->value = strdup(val);
        return;
      }
    }
    h = calloc(1, sizeof(*h));
    AN(h);
    h->key = strdup(key);
    h->value = strdup(val);
    VTAILQ_INSERT_HEAD(&r->headers, h, list);
  } else VTAILQ_FOREACH_SAFE(h, &r->headers, list, h2) {
    if(strcasecmp(h->key,key) == 0) {
      VTAILQ_REMOVE(&r->headers,h,list);
      free(h->key);
      free(h->value);
      free(h);
    }
  }
}

static
void vp_req_add_header(struct vmod_peer *ctx, struct peer_req *r,
                       const char *key, const char *val)
{
  vmod_hdr_t *h, *h2;

  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  if(val) {
    h = calloc(1, sizeof(*h));
    AN(h);
    AN(key);
    h->key = strdup(key);
    h->value = strdup(val);
    VTAILQ_INSERT_HEAD(&r->headers, h, list);
  } else VTAILQ_FOREACH_SAFE(h, &r->headers, list, h2) {
    if(strcasecmp(h->key,key) == 0) {
      VTAILQ_REMOVE(&r->headers,h,list);
      free(h->key);
      free(h->value);
      free(h);
    }
  }
}

static
struct peer_req *vp_req_new(struct vmod_peer *vp) {
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  r = calloc(1,sizeof(*r));
  AN(r);
  r->magic = VMOD_PEER_REQ_MAGIC;
  r->vp = vp;
  AN(cache_param);
  if(!vp_ws_check(vp,&r->ws))
#ifdef DEBUG
    r->ws = vp_ws_new("vpq",cache_param->workspace_thread * 8);
#else
    r->ws = vp_ws_new("vpq",cache_param->workspace_thread);
#endif
  r->c = NULL;
  r->body = NULL;
  r->tag = "req";
  r->host = r->url = r->proto = r->method = NULL;
  VTAILQ_INIT(&r->headers);
  vp_req_add_header(vp,r,"Connection","close");
  return r;
}

static
struct peer_req *vp_req_init(struct vmod_peer *vp, struct http *hp)
{
  uint16_t i;
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AN(hp);
  r = vp_req_new(vp);
  r->method = strdup(http_GetReq(hp));
  AN(r->method);
  Tcheck(hp->hd[HTTP_HDR_URL]);
  r->url = strndup(hp->hd[HTTP_HDR_URL].b,Tlen(hp->hd[HTTP_HDR_URL]));
  AN(r->url);
  Tcheck(hp->hd[HTTP_HDR_PROTO]);
  r->proto = strndup(hp->hd[HTTP_HDR_PROTO].b,Tlen(hp->hd[HTTP_HDR_PROTO]));
  AN(r->proto);
  AZ(pthread_mutex_lock(&vp->mtx));
  if(vp->host) {
    r->host = strdup(vp->host);
    AN(r->host);
  }
  AZ(pthread_mutex_unlock(&vp->mtx));
  for(i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
    if(hp->hd[i].b == NULL || hp->hdf[i] & HDF_FILTER)
      continue;
    Tcheck(hp->hd[i]);
    if (http_IsHdr(&hp->hd[i],H_Host)) {
      if(!r->host)
        r->host = strndup(hp->hd[i].b,Tlen(hp->hd[i]));
    }
    if (!http_IsHdr(&hp->hd[i], H_Connection)) {
      char *v,*k = strndup(hp->hd[i].b, Tlen(hp->hd[i]));
      AN(k);
      if((v = strchr(k,':')) != NULL) {
        *v++ = '\0';
        while(vct_issp(*v))
          v++;
        if(*v)
          vp_req_set_header(vp,r,k,v);
      }
      free(k);
    }
  }
  assert(r->vp == vp);
  return r;
}

static
int vp_req_put(struct peer_req *r, const void *buf, size_t len)
{
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(r->body)
    VSB_clear(r->body);
  else
    r->body = VSB_new_auto();
  AN(r->body);
  return VSB_bcpy(r->body,buf,len);
}

static
int vp_req_append(struct peer_req *r, const void *buf, size_t len)
{
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(!r->body)
    r->body = VSB_new_auto();

  return VSB_bcat(r->body, buf, len);
}

static
void vp_req_free(struct peer_req *r)
{
  vmod_hdr_t *h, *h2;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(r->c)
    curl_easy_cleanup(r->c);
  if(r->body) {
    struct vsb *b = r->body;
    r->body = NULL;
    VSB_delete(b);
  }
  if(r->url)
    VPFREE(r->url);
  if(r->host)
    VPFREE(r->host);
  if(r->proto)
    VPFREE(r->proto);
  if(r->method)
    VPFREE(r->method);
  VTAILQ_FOREACH_SAFE(h, &r->headers, list, h2) {
    VTAILQ_REMOVE(&r->headers,h,list);
    free(h->key);
    free(h->value);
    free(h);
  }
  if(r->ws != NULL) {
    if(r->vp != NULL) {
      struct ws *ws;
      if(vp_ws_check(r->vp,&ws) && r->ws != ws)
        VPFREE(r->ws);
      else if (ws == NULL)
        VPFREE(r->ws);
    } else
      VPFREE(r->ws);
  }
}

static
struct vmod_pthr *vp_alloc_thread(struct vmod_peer *vp)
{
  vmod_pthr_t *p;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);

  if(vp->tsz == 0) {
    vp->threads = calloc(2,sizeof(*p));
    AN(vp->threads);
    vp->tsz = 2;
  } else while(vp->tsz <= vp->nthreads) {
    AN(vp->threads);
    vp->tsz *= 2;
    vp->threads = realloc(vp->threads, vp->tsz * sizeof(*p));
    AN(vp->threads);
  }

  p = &vp->threads[vp->nthreads++];
  p->magic = VMOD_PEER_THREAD_MAGIC;
  p->vp = vp;
  p->shutdown = &vp->shutdown;
  return p;
}

static
size_t vp_write_data(char *ptr, size_t sz, size_t n, void *rv)
{
  CHECK_OBJ_NOTNULL((struct peer_req*)rv, VMOD_PEER_REQ_MAGIC);

  /* noop basically */
  return sz * n;
}

#ifdef DEBUG
static
int vp_debug_callback(CURL *c, curl_infotype t, char *msg, size_t sz, void *rv)
{
  char *b,*mark;
  unsigned u;
  struct peer_req *r = (struct peer_req*)rv;
  struct ws *ws;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  CHECK_OBJ_NOTNULL(r->vp, VMOD_PEER_MAGIC);
  ws = vp_ws_get(r->vp);
  AN(ws);
  mark = WS_Snapshot(ws);
  u = WS_Reserve(ws,PRNDUP(sz+1));
  AN(u);
  b = ws->f;
  if(u < sz)
    sz = u-1;
  if(msg) {
    if(sz > 0)
      strncpy(b,msg,sz);
    *(b+sz) = '\0';
  }
  WS_Release(ws,sz+1);

  switch(t) {
  case CURLINFO_DATA_IN:
    break;
  case CURLINFO_DATA_OUT:
  case CURLINFO_HEADER_OUT:
    VPLOG_Debug(r->vp,"%s:%p > %s", r->tag, r, b);
    break;
  case CURLINFO_HEADER_IN:
    VPLOG_Debug(r->vp,"%s:%p < %s", r->tag, r, b);
    break;
  case CURLINFO_TEXT:
    VPLOG_Debug(r->vp,"%s:%p: %s", r->tag, r, b);
    break;
  default:
    VPLOG_Debug(r->vp,"%s:%p unexpected debug type %d (%u bytes)",r->tag,
                r,(int)t,(unsigned) sz);
    break;
  }

  WS_Reset(ws,mark);
  return 0;
}
#endif /* DEBUG */

static
int vp_enqueue(struct vmod_peer *vp, struct peer_req *r,
                 const struct vrt_ctx *ctx)
{
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  AZ(pthread_mutex_lock(&vp->mtx));
  if((vp->qlen+1) / vp->nthreads > VMOD_PEER_MAX_QUEUE_DEPTH) {
    if(vp->nthreads >= vp->max || vp_start_thread(vp) != 0) {
      VPLOGR(ctx, SLT_VCL_Error, "vmod_peer: queue depth would exceed %u/%u, waiters=%u, add more threads?",
              (unsigned)vp->qlen, (unsigned)vp->nthreads, (unsigned)vp->nwaiters);
      VPLOGR(ctx, SLT_VCL_Error, "vmod_peer: discarding %p %s", r, r->url);
      AZ(pthread_mutex_unlock(&vp->mtx));
      vp_req_free(r);
      return -1;
    }
  }
  VTAILQ_INSERT_HEAD(&vp->q, r, list);
  if(++vp->qlen > 1 && vp->nwaiters > 1)
    AZ(pthread_cond_broadcast(&vp->cond));
  else if(vp->nwaiters > 0)
    AZ(pthread_cond_signal(&vp->cond));
  VPLOGR(ctx, SLT_Debug, "vmod_peer: qlen=%u/threads=%u/waiters=%u",
            (unsigned)vp->qlen,(unsigned)vp->nthreads,(unsigned)vp->nwaiters);
  return pthread_mutex_unlock(&vp->mtx);
}

static
void vp_process_req(struct vmod_peer *vp, struct vmod_pthr *thr, struct peer_req *r)
{
  struct curl_slist *req_headers = NULL;
  char *cp;
  char *body,*b,*mark;
  size_t blen;
  unsigned bsz,u;
  int cl_set = 0, expect_set = 0;
  CURLcode cr;
  long status = 0;
  vmod_hdr_t *h;
  struct ws *ws;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  ws = vp_ws_get(vp);
  AN(ws);
  mark = WS_Snapshot(ws);
  if(!r->c)
    r->c = curl_easy_init();
  AN(r->c);

  if(vp->csh)
    curl_easy_setopt(r->c, CURLOPT_SHARE, vp->csh);

  VTAILQ_FOREACH(h, &r->headers, list) {
    if (strcasecmp(h->key,"content-length") == 0)
      cl_set++;
    else if(strcasecmp(h->key,"expect") == 0)
      expect_set++;
    bsz = strlen(h->key) + strlen(h->value) + 3;

    u = WS_Reserve(ws,PRNDUP(bsz));
    if(u < bsz) {
      WS_Release(ws,0);
      VPLOG_Error(vp, "out of space for header %s",h->key);
      continue;
    }
    b = ws->f;
    cp = stpcpy(b,h->key);
    AN(cp);
    *cp++ = ':';
    if(*h->value) {
      *cp++ = ' ';
      AN(stpcpy(cp,h->value));
    } else
      *cp = '\0';
    req_headers = curl_slist_append(req_headers,b);
    AN(req_headers);
    WS_Release(ws,0);
  }

  if(r->body && !VSB_done(r->body))
    AZ(VSB_finish(r->body));
  if(r->body) {
    blen = VSB_len(r->body);
    if (blen > 0) {
      body = curl_easy_escape(r->c, VSB_data(r->body), blen);
      AN(body);
      blen = strlen(body);
    } else body = NULL;
  } else {
    body = NULL;
    blen = 0;
  }

  if(r->method) {
    if(strcasecmp(r->method, "POST") == 0 || body) {
      curl_easy_setopt(r->c, CURLOPT_POST, 1L);
      if(!body || blen == 0) {
        curl_easy_setopt(r->c, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(r->c, CURLOPT_POSTFIELDS, "");
      } else {
        curl_easy_setopt(r->c, CURLOPT_POSTFIELDSIZE, (long)blen);
        curl_easy_setopt(r->c, CURLOPT_POSTFIELDS, body);
      }
      if(!expect_set && (!r->proto || strcasecmp(r->proto,"HTTP/1.1") == 0)) {
        expect_set++;
        req_headers = curl_slist_append(req_headers, "Expect: 100-continue");
        AN(req_headers);
      }
      /* don't worry about content-length */
      cl_set++;
    } else if(strcasecmp(r->method,"HEAD") == 0)
      curl_easy_setopt(r->c, CURLOPT_NOBODY, 1L);
    else
      curl_easy_setopt(r->c, CURLOPT_HTTPGET, 1L);
  } else {
    if(body && blen > 0) {
      curl_easy_setopt(r->c, CURLOPT_POST, 1L);
      curl_easy_setopt(r->c, CURLOPT_POSTFIELDSIZE, (long)blen);
      curl_easy_setopt(r->c, CURLOPT_POSTFIELDS, body);
      if(!expect_set && (!r->proto || strcasecmp(r->proto,"HTTP/1.1") == 0)) {
        expect_set++;
        req_headers = curl_slist_append(req_headers, "Expect: 100-continue");
        AN(req_headers);
      }
      cl_set++;
    }
    if(!cl_set) {
      req_headers = curl_slist_append(req_headers,"Content-Length: 0");
      AN(req_headers);
      curl_easy_setopt(r->c, CURLOPT_NOBODY, 1L);
    }
  }

  if(r->method)
    curl_easy_setopt(r->c, CURLOPT_CUSTOMREQUEST, r->method);
  if(req_headers)
    curl_easy_setopt(r->c, CURLOPT_HTTPHEADER, req_headers);
  curl_easy_setopt(r->c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(r->c, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(r->c, CURLOPT_WRITEFUNCTION, vp_write_data);
  curl_easy_setopt(r->c, CURLOPT_WRITEDATA, r);
#ifndef DEBUG
  curl_easy_setopt(r->c, CURLOPT_FAILONERROR, 1L);
#else
  curl_easy_setopt(r->c, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(r->c, CURLOPT_DEBUGFUNCTION, vp_debug_callback);
  curl_easy_setopt(r->c, CURLOPT_DEBUGDATA, r);
#endif
  if(vp->port != -1)
    curl_easy_setopt(r->c, CURLOPT_PORT, (long)vp->port);
  curl_easy_setopt(r->c, CURLOPT_HEADER, 1L);
  curl_easy_setopt(r->c, CURLOPT_TCP_NODELAY, 1L);
  curl_easy_setopt(r->c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP);
  curl_easy_setopt(r->c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(r->c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP);
  curl_easy_setopt(r->c, CURLOPT_IGNORE_CONTENT_LENGTH, 1L);
  if(r->proto) {
    if(strcasecmp(r->proto,"HTTP/1.0") == 0)
      curl_easy_setopt(r->c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
    else if(strcasecmp(r->proto,"HTTP/1.1") == 0)
      curl_easy_setopt(r->c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    else
      curl_easy_setopt(r->c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_NONE);
  }
  if(vp->c_to > -1)
    curl_easy_setopt(r->c, CURLOPT_CONNECTTIMEOUT_MS, (long)vp->c_to);
  if(vp->d_to > -1)
    curl_easy_setopt(r->c, CURLOPT_TIMEOUT_MS, (long)vp->d_to);

  if(vp->ssl_flags) {
    curl_easy_setopt(r->c, CURLOPT_SSL_VERIFYPEER,
                     (vp->ssl_flags & VP_SSL_VERIFY_PEER) ? 1L : 0L);
    curl_easy_setopt(r->c, CURLOPT_SSL_VERIFYHOST,
                     (vp->ssl_flags & VP_SSL_VERIFY_HOST) ? 1L : 0L);
    if(vp->ssl_cafile)
      curl_easy_setopt(r->c, CURLOPT_CAINFO, vp->ssl_cafile);
    if(vp->ssl_capath)
      curl_easy_setopt(r->c, CURLOPT_CAPATH, vp->ssl_capath);
  }
  bsz = strlen(r->host)+strlen(r->url)+12;
  u = WS_Reserve(ws,PRNDUP(bsz));
  if (u <= bsz) {
    WS_Release(ws,0);
    VPLOG_Error(vp,"%s:%p url:%s, nomem (%u bytes allocated, need %u)",r->tag,
                r,r->url,u,(unsigned)bsz);
  } else {
    if (vp->ssl_flags & VP_SSL_ON)
      cp = stpcpy(ws->f,"https://");
    else
      cp = stpcpy(ws->f,"http://");
    AN(cp);
    cp = stpcpy(cp,r->host);
    AN(cp);
    if(*r->url != '/')
      *cp++ = '/';
    cp = stpcpy(cp, r->url);
    AN(cp);
    b = ws->f;
    WS_ReleaseP(ws, cp+1);
    curl_easy_setopt(r->c, CURLOPT_URL, b);
    VPLOG_Debug(vp,"%s:%p url:%s",r->tag,r,b);
    cr = curl_easy_perform(r->c);
    if(cr != 0) {
      const char *err = curl_easy_strerror(cr);
      VPLOG_Error(vp,"%s:%p (%s) error: %s",r->tag,r,b,err);
    }
    curl_easy_getinfo(r->c, CURLINFO_RESPONSE_CODE, &status);
    VPLOG_Debug(vp,"%s:%p (%s) complete: %ld", r->tag, r, b, status);
  }
  if(req_headers)
    curl_slist_free_all(req_headers);
  if(body)
    curl_free(body);
  WS_Reset(ws,mark);
}

static
void vmod_peer_thread_register(struct vmod_pthr *thr)
{
  pthread_mutex_t *mtx;
  pthread_cond_t *cond;

  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AZ(thr->alive);
  mtx = thr->imtx;
  cond = thr->icond;
  AN(mtx);
  AN(cond);
  AZ(pthread_mutex_lock(mtx));
  thr->alive++;
  thr->imtx = NULL;
  thr->icond = NULL;
  AZ(pthread_cond_signal(cond));
  AZ(pthread_mutex_unlock(mtx));
}

static
void vmod_peer_thread_unregister(struct vmod_pthr *thr) {
  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AN(thr->alive);
  thr->alive--;
  AZ(thr->alive);
}

static
VMOD_THREAD_FUNC vmod_peer_thread(void *vctx)
{
  vmod_pthr_t *thr = (struct vmod_pthr*)vctx;
  struct vmod_peer *vp;

  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  vp = thr->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  vmod_peer_thread_register(thr);
  VPLOG(SLT_Debug, "peer thread %p started",vp);
  AZ(pthread_mutex_lock(&vp->mtx));
  do {
    struct peer_req *r;
    assert(thr->vp == vp);
    CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
    vp->nwaiters++;
    while(!*thr->shutdown && vp->qlen == 0)
      AZ(pthread_cond_wait(&vp->cond,&vp->mtx));
    vp->nwaiters--;
    for(r = VTAILQ_FIRST(&vp->q); !*thr->shutdown && vp->qlen > 0; r = VTAILQ_FIRST(&vp->q)) {
      CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
      VTAILQ_REMOVE(&vp->q, r, list);
      vp->qlen--;
      AZ(pthread_mutex_unlock(&vp->mtx));
      vp_process_req(vp,thr,r);
      vp_req_free(r);
      AZ(pthread_mutex_lock(&vp->mtx));
    }
  } while(!*thr->shutdown);

  vmod_peer_thread_unregister(thr);
  AZ(pthread_mutex_unlock(&vp->mtx));
  return NULL;
}

static
int vp_start_thread(struct vmod_peer *vp)
{
  struct vmod_pthr *p;
  pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  p = vp_alloc_thread(vp);
  CHECK_OBJ_NOTNULL(p, VMOD_PEER_THREAD_MAGIC);
  AZ(pthread_mutex_lock(&mtx));
  p->imtx = &mtx;
  p->icond = &cond;
  p->shutdown = &vp->shutdown;
  AZ(pthread_create(&p->tid,NULL,vmod_peer_thread,p));
  while(p->alive == 0)
    AZ(pthread_cond_wait(&cond,&mtx));
  AZ(p->imtx);
  AZ(p->icond);
  return pthread_mutex_unlock(&mtx);
}

static
int vp_start_thread_safe(struct vmod_peer *vp)
{
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&vp->mtx));
  vp_start_thread(vp);
  return pthread_mutex_unlock(&vp->mtx);
}

static
void vmod_peer_stop(struct vmod_peer *vp)
{
  struct peer_req *r, *r2;
  int nalive;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  do {
    int i;

    AZ(pthread_mutex_lock(&vp->mtx));

    VTAILQ_FOREACH_SAFE(r, &vp->q, list, r2) {
      VTAILQ_REMOVE(&vp->q,r,list);
      vp_req_free(r);
    }

    for(i = nalive = 0; i < vp->nthreads; i++) {
      vp->threads[i].shutdown = &vp->shutdown;
      nalive += vp->threads[i].alive;
    }

    vp->shutdown++;
    if(nalive) {
      AZ(pthread_cond_broadcast(&vp->cond));
      for(i = 0; i < vp->nthreads; i++) {
        if(vp->threads[i].alive) {
          pthread_t tid;
          CHECK_OBJ_NOTNULL(&vp->threads[i], VMOD_PEER_THREAD_MAGIC);
          tid = vp->threads[i].tid;
          AZ(pthread_mutex_unlock(&vp->mtx));
          pthread_join(tid,NULL);
          AZ(pthread_mutex_lock(&vp->mtx));
        }
      }
    }

    AZ(pthread_mutex_unlock(&vp->mtx));
  } while(nalive > 0);

  if(vp->csh) {
    curl_share_cleanup(vp->csh);
    vp->csh = NULL;
  }

  pthread_cond_destroy(&vp->cond);
  pthread_mutex_destroy(&vp->mtx);
  if(vp->host)
    VPFREE(vp->host);
  if(vp->threads)
    VPFREE(vp->threads);
  vp->magic = 0;
  free(vp);

  if(--initialized == 0) {
    int i;
    for(i = CURL_LOCK_DATA_NONE; i <= CURL_LOCK_DATA_LAST; i++) {
      vp_lock_t *l = curl_locks[i];
      curl_locks[i] = NULL;
      if(l)
        vp_lock_destroy(l);
    }
    curl_global_cleanup();
  }
}

static
void vp_lock_curl_data(CURLSH *s, curl_lock_data data, curl_lock_access access, void *va)
{
#if 0
  VPLOG(SLT_Debug, "vp_lock_curl_data data=%d access=%d",(int)data,(int)access);
#endif
  if(access == CURL_LOCK_ACCESS_SHARED)
    AZ(vp_lock_read_acquire(curl_locks[data]));
  else if(access == CURL_LOCK_ACCESS_SINGLE)
    AZ(vp_lock_write_acquire(curl_locks[data]));
}

static
void vp_unlock_curl_data(CURLSH *s, curl_lock_data data, void *va)
{
#if 0
  VPLOG(SLT_Debug, "vp_unlock_curl_data data=%d access=%d",(int)data,(int)access);
#endif
  AZ(vp_lock_release(curl_locks[data]));
}

static
struct vmod_peer *init_function(const struct vrt_ctx *ctx)
{
  int rv = 0;
  struct vmod_peer *vp;

  vp = calloc(1,sizeof(*vp));
  AN(vp);
  vp->magic = VMOD_PEER_MAGIC;
  vp->nthreads = 0;
  vp->nwaiters = 0;
  vp->min = vp->max = 1;
  AZ(pthread_mutex_init(&vp->mtx,NULL));
  AZ(pthread_cond_init(&vp->cond,NULL));
  VTAILQ_INIT(&vp->q);
  vp->qlen = 0;
  vp->tsz = 0;
  vp->host = NULL;
  vp->port = vp->c_to = vp->d_to = -1;
  vp->ssl_flags = 0;
  vp->ssl_cafile = vp->ssl_capath = NULL;
  if (!initialized) {
    pthread_mutexattr_t attr;
    int i;
    AZ(pthread_mutexattr_init(&attr));
    AZ(pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    for(i = CURL_LOCK_DATA_NONE; i <= CURL_LOCK_DATA_LAST; i++) {
      curl_locks[i] = vp_lock_new(&attr,NULL);
      AN(curl_locks[i]);
    }
    AZ(pthread_mutexattr_destroy(&attr));
    rv = curl_global_init(CURL_GLOBAL_ALL);
    initialized = 1;
  } else
    initialized++;

  if(rv == 0) {
    vp->csh = curl_share_init();
    AN(vp->csh);
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_LOCKFUNC, vp_lock_curl_data));
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_UNLOCKFUNC, vp_unlock_curl_data));
#ifdef CURL_LOCK_DATA_DNS
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
#endif
#ifdef CURL_LOCK_DATA_SSL_SESSION
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION));
#endif
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_USERDATA, vp));
    while(rv == 0 && vp->nthreads < vp->min)
      rv = vp_start_thread_safe(vp);
  }

  return (rv == 0 ? vp : NULL);
}

static
void vp_set_host_port(const struct vrt_ctx *ctx, struct vmod_peer *vp,
                      const char *host, int port)
{
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&vp->mtx));

  if(port <= 0 || port > 65535)
    port = -1;
  else
    vp->port = port;

  if(host && *host) {
    if (vp->host)
      VPFREE(vp->host);
    vp->host = strdup(host);
  } else if(vp->host)
    VPFREE(vp->host);

  AZ(pthread_mutex_unlock(&vp->mtx));
}

VCL_VOID
vmod_ip__init(const struct vrt_ctx *ctx, struct vmod_peer_ip **vpp,
              const char *vcl_name, VCL_IP addr, VCL_INT port)
{
  char *host = NULL;
  const struct sockaddr *sa;
  socklen_t sl = VSA_Len(addr);
  struct ws *ws;

  AN(vpp);
  *vpp = NULL;

  if (port < 0 || port > 65535) {
    VPLOG(SLT_VCL_Error, "%d is an invalid port number", port);
    port = -1;
  }
  ALLOC_OBJ(*vpp, VMOD_PEER_IP_MAGIC);
  AN(*vpp);
  sa = VSA_Get_Sockaddr(addr,&sl);
  AN(sa);
  (*vpp)->addr = VSA_Malloc(sa,sl);
  assert(VSA_Sane((*vpp)->addr));
  (*vpp)->name = vcl_name;
  (*vpp)->vp = init_function(ctx);
  AZ(pthread_key_create(&(*vpp)->wsk,vp_ws_free));
  ws = vmod_peer_ip_ws(*vpp);
  WS_Assert(ws);
  (*vpp)->vp->wsk = (*vpp)->wsk;

  if((*vpp)->addr) {
    unsigned len;

    len = WS_Reserve(ws,0);
    assert(len > 20);
    host = ws->f;
    AN(host);
    *host = '\0';
    VTCP_name((*vpp)->addr, host, len, NULL, 0);

    if(host && *host) {
      vp_set_host_port(ctx, (*vpp)->vp, host, port);
      port = -1;
    }
    WS_Release(ws,0);
  }
  if (port != -1)
    vp_set_host_port(ctx, (*vpp)->vp, NULL,port);
}

VCL_VOID
vmod_ip__fini(struct vmod_peer_ip **vpp)
{
  AN(vpp);
  if(*vpp) {
    struct ws *ws;
    struct vmod_peer *vp;
    CHECK_OBJ_NOTNULL(*vpp, VMOD_PEER_IP_MAGIC);
    vp = (*vpp)->vp;
    (*vpp)->vp = NULL;
    if(vp)
      vmod_peer_stop(vp);
    VPFREE((*vpp)->addr);
    ws = pthread_getspecific((*vpp)->wsk);
    AZ(pthread_key_delete((*vpp)->wsk));
    if(ws)
      vp_ws_free(ws);

    FREE_OBJ(*vpp);
    *vpp = NULL;
  }
}

VCL_VOID
V4_VMOD_PEER(ip,set_threads, VCL_INT min, VCL_INT max)
{
  struct vmod_peer *vp;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  int rv;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  if(max > 0 && min > 0 && max < min)
    max = min;

  AZ(pthread_mutex_lock(&vp->mtx));
  if(min > 0) {
    vp->min = min;
    if (vp->max < min)
      vp->max = min;
  }
  if(max > 0) {
    vp->max = max;
    if (vp->min > max)
      vp->min = max;
  }

  while(vp->nthreads > vp->max) {
    /* too many threads, get rid of some */
    pthread_t tid;
    int shutdown = 1;
    vmod_pthr_t *t = &vp->threads[vp->nthreads-1];
    if(!t->alive) {
      vp->nthreads--;
      continue;
    }
    CHECK_OBJ_NOTNULL(t, VMOD_PEER_THREAD_MAGIC);
    tid = t->tid;
    t->shutdown = &shutdown;
    if(vp->nwaiters != 1)
      AZ(pthread_cond_broadcast(&vp->cond));
    else
      AZ(pthread_cond_signal(&vp->cond));
    AZ(pthread_mutex_unlock(&vp->mtx));
    pthread_join(tid, NULL);
    AZ(pthread_mutex_lock(&vp->mtx));
    assert(!t->alive);
    vp->nthreads--;
    t->magic = 0;
  }
  for(rv = 0; rv == 0 && vp->nthreads < vp->min; ) {
    AZ(pthread_mutex_unlock(&vp->mtx));
    rv = vp_start_thread_safe(vp);
    if (rv != 0) {
      if(errno != 0)
        VPLOGR(ctx, SLT_VCL_Error, "vmod_peer cannot start new thread: %s",
            strerror(errno));
    }
    AZ(pthread_mutex_lock(&vp->mtx));
  }
  AZ(pthread_mutex_unlock(&vp->mtx));
}

VCL_INT
V4_VMOD_PEER(ip,threads)
{
  int i,n = 0;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  for(i = 0; i < vmp->vp->nthreads; i++) {
    n += vmp->vp->threads[i].alive;
  }
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));
  return n;
}

VCL_INT
V4_VMOD_PEER(ip,min_threads)
{
  int n;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  n = vmp->vp->min;
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));
  return n;
}

VCL_INT
V4_VMOD_PEER(ip,max_threads)
{
  int n;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  n = vmp->vp->max;
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));
  return n;
}

VCL_INT
V4_VMOD_PEER(ip,pending)
{
  size_t qlen;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  qlen = vmp->vp->qlen;
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));

  return qlen;
}

/* set connect timeout in ms */
VCL_VOID
V4_VMOD_PEER(ip,set_connect_timeout,VCL_INT ms)
{
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  vmp->vp->c_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));
}

/* set connect timeout in ms */
VCL_VOID
V4_VMOD_PEER(ip,set_timeout,VCL_INT ms)
{
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  CHECK_OBJ_NOTNULL(vmp->vp, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&vmp->vp->mtx));
  vmp->vp->d_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&vmp->vp->mtx));
}

/* add the current request + post body to the pending list */
static
void vmod_enqueue_post_va(const struct vrt_ctx *ctx,
                          struct vmod_peer *vp,
                         const char *s, va_list ap)
{
  const char *p;
  ssize_t l = 0;
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  if(ctx->bo != NULL && ctx->req == NULL) {
    /* Called from backend vcl methods, use bereq */
    r = vp_req_init(vp,ctx->http_bereq);
    CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
    r->tag = "bereq";
  } else {
    AN(ctx->req);
    r = vp_req_init(vp,ctx->http_req);
    CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
    r->tag = "req";
  }

  for(p = s; p != vrt_magic_string_end; p = va_arg(ap, const char*)) {
    if (p != NULL) {
      size_t vl = strlen(p);
      if(vp_req_append(r,p,vl) != 0 && r->body && r->body->s_error != 0) {
        VPLOGR(ctx, SLT_VCL_Error, "%s:%p (%s) queue_req_body: %s",r->tag,
            p,r->url,strerror(r->body->s_error));
        l = -1;
        break;
      }
      l += vl;
    }
  }

  if(l >= 0)
    (void)vp_enqueue(vp,r,ctx);
  else
    vp_req_free(r);
}

VCL_VOID
V4_VMOD_PEER(ip,enqueue_post,const char *s, ...)
{
  va_list ap;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  va_start(ap,s);
  vmod_enqueue_post_va(ctx,vmp->vp,s,ap);
  va_end(ap);
}

static
int __match_proto__(req_body_iter_f)
vp_iter_req_body(struct req *req, void *priv, void *ptr, size_t l)
{
  CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
  AN(priv);
  return VSB_bcat((struct vsb*)priv,ptr,l);
}

VCL_VOID
V4_VMOD_PEER(ip,enqueue_cached_post)
{
  struct req *req;
  int use_bo = 0;
  struct vsb *b;
  CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  if(ctx->bo != NULL && ctx->req == NULL) {
    req = ctx->bo->req;
    use_bo++;
  } else
    req = ctx->req;

  if(req == NULL) {
    VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
          "cannot call enqueue_cached_post() from this vcl handler (no request exists)");
    return;
  }
  b = VSB_new_auto();
  AN(b);
  HTTP1_IterateReqBody(req, vp_iter_req_body, b);
  if(req->req_body_status == REQ_BODY_FAIL || VSB_finish(b) != 0) {
    VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
          "enqueue_cached_post() failed retrieving request body (%d): %s",
          errno,strerror(errno));
  } else if(req->req_body_status != REQ_BODY_DONE && req->req_body_status != REQ_BODY_CACHED) {
    VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
          "enqueue_cached_post() cannot retrieve request body");
  } else {
    struct peer_req *r;
    if(use_bo) {
      AN(ctx->http_bereq);
      r = vp_req_init(vmp->vp,ctx->http_bereq);
      r->tag = "bereq";
    } else {
      AN(ctx->http_req);
      r = vp_req_init(vmp->vp,ctx->http_req);
      r->tag = "req";
    }
    CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
    r->body = b;
    b = NULL;
    (void)vp_enqueue(vmp->vp,r,ctx);
  }
  if(b != NULL)
    VSB_delete(b);
}

/* add the current request to the pending list */
VCL_VOID
V4_VMOD_PEER(ip,enqueue)
{
  struct vmod_peer *vp;
  struct peer_req *r;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  if(ctx->bo != NULL && ctx->req == NULL) {
    /* Called from backend vcl methods, use bereq */
    AN(ctx->http_bereq);
    r = vp_req_init(vp,ctx->http_bereq);
    r->tag = "bereq";
  } else {
    AN(ctx->http_req);
    r = vp_req_init(vp,ctx->http_req);
    r->tag = "req";
  }
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  (void)vp_enqueue(vp,r,ctx);
}

VCL_VOID
vmod_cache_post(const struct vrt_ctx *ctx)
{
  int i;
  unsigned u;
  CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);

  if(ctx->method != VCL_MET_RECV) {
    VSLb(ctx->bo->vsl,SLT_VCL_Error,
          "cache_post() cannot be called outside of vcl_recv");
    return;
  }

  CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
  AN(ctx->ws);
  u = WS_Reserve(ctx->ws,0);
  i = HTTP1_CacheReqBody(ctx->req,PRNDDN(u+1));
  WS_Release(ctx->ws,0);
  if(i == -1)
    VSLb(ctx->vsl,SLT_VCL_Error,
         "Cannot cache request body (%d): %s",errno,strerror(errno));
  else if(ctx->req->req_body_status != REQ_BODY_CACHED)
    VSLb(ctx->vsl,SLT_VCL_Error,
         "Unable to cache request body (status:%d)",(int)ctx->req->req_body_status);
}

VCL_STRING
V4_VMOD_PEER(ip,body)
{
  struct vmod_peer *vp;
  struct vsb *b;
  struct req *req = NULL;
  char *p = NULL;
  unsigned u;
  int i = 0,use_bo = 0;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  if(ctx->bo != NULL && ctx->req == NULL) {
    req = ctx->bo->req;
    use_bo++;
  } else
    req = ctx->req;

  if(req == NULL)
    return NULL;

  AN(ctx->ws);
  WS_Assert(ctx->ws);
  u = WS_Reserve(ctx->ws,0);

  if(ctx->method == VCL_MET_RECV)
    i = HTTP1_CacheReqBody(req,PRNDDN(u+1));
  WS_Release(ctx->ws,0);
  if(i == -1 || req->req_body_status != REQ_BODY_CACHED) {
    if(i == -1)
      VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
           "req.body not cached (%d): %s",errno,strerror(errno));
    else
      VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
           "req.body is not cached and/or running outside of vcl_recv");
    if(use_bo)
      VSLb_ts_busyobj(ctx->bo, "Bereq", VTIM_real());
    else
      VSLb_ts_req(req, "ReqBody", VTIM_real());
    return NULL;
  }
  b = VSB_new_auto();
  AN(b);
  HTTP1_IterateReqBody(req, vp_iter_req_body, b);
  if(req->req_body_status == REQ_BODY_FAIL || VSB_finish(b) != 0) {
    if(use_bo) {
      VSLb(ctx->bo->vsl, SLT_VCL_Error, "reading bereq cached body: %d (%s)",
            errno,strerror(errno));
      VSLb_ts_busyobj(ctx->bo, "Bereq", VTIM_real());
    } else {
      VSLb(ctx->vsl, SLT_VCL_Error, "reading req body: %d (%s)",
           errno,strerror(errno));
      VSLb_ts_req(req, "ReqBody", VTIM_real());
    }
  } else if((req->req_body_status != REQ_BODY_DONE) && (req->req_body_status != REQ_BODY_CACHED)) {
    VSLb((use_bo ? ctx->bo->vsl : ctx->vsl),SLT_VCL_Error,
          "unexpected req_body_status iterating cached request body (%d)",
          (int)req->req_body_status);
    if(use_bo)
      VSLb_ts_busyobj(ctx->bo, "Bereq", VTIM_real());
    else
      VSLb_ts_req(req, "ReqBody", VTIM_real());
  } else {
    if(VSB_len(b) > 0) {
      unsigned l = VSB_len(b)+1;
      u = WS_Reserve(ctx->ws,0);
      if(u >= l) {
        p = (char*)ctx->ws->f;
        memcpy(p,VSB_data(b),l);
        WS_Release(ctx->ws,l);
      } else
        WS_Release(ctx->ws,0);
    }
  }
  VSB_delete(b);
  return p;
}
/* SSL */
VCL_VOID
V4_VMOD_PEER(ip,set_ssl, VCL_STRING cafile, VCL_STRING capath)
{
  struct vmod_peer *vp;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&vp->mtx));
  if(cafile && *cafile)
    vp->ssl_cafile = cafile;
  else
    vp->ssl_cafile = NULL;
  if(capath && *capath)
    vp->ssl_capath = capath;
  else
    vp->ssl_capath = NULL;
  if((vp->ssl_cafile || vp->ssl_capath) && !vp->ssl_flags)
    vp->ssl_flags |= VP_SSL_ON;
  AZ(pthread_mutex_unlock(&vp->mtx));
}

VCL_VOID
V4_VMOD_PEER(ip,set_ssl_verification, VCL_ENUM vtype, VCL_BOOL enable)
{
  struct vmod_peer *vp;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AN(vtype);
  if(strcmp(vtype,"peer") == 0) {
    AZ(pthread_mutex_lock(&vp->mtx));
    if(enable)
      vp->ssl_flags |= VP_SSL_VERIFY_PEER;
    else
      vp->ssl_flags &= ~VP_SSL_VERIFY_PEER;
    AZ(pthread_mutex_unlock(&vp->mtx));
  } else if(strcmp(vtype,"host") == 0) {
    AZ(pthread_mutex_lock(&vp->mtx));
    if(enable)
      vp->ssl_flags |= VP_SSL_VERIFY_HOST;
    else
      vp->ssl_flags &= ~VP_SSL_VERIFY_HOST;
    AZ(pthread_mutex_unlock(&vp->mtx));
  } else if(strcmp(vtype,"force") == 0) {
    AZ(pthread_mutex_lock(&vp->mtx));
    if(enable)
      vp->ssl_flags |= VP_SSL_ON;
    else
      vp->ssl_flags &= ~VP_SSL_ON;
    AZ(pthread_mutex_unlock(&vp->mtx));
  } else {
    VPLOGR(ctx,SLT_VCL_Error,"unsupport ssl verification type '%s'",vtype);
  }
}
