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
  char *host,*method,*proto,*url;
  struct vsb *body;
  CURL *c;
  VTAILQ_HEAD(,vmod_hdr) headers;
  VTAILQ_ENTRY(peer_req) list;
};

typedef struct vmod_pthr vmod_pthr_t;

struct vmod_peer {
  unsigned magic;
#define VMOD_PEER_MAGIC 0x5bba391c
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
};

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

static int initialized = 0;
static vp_lock_t *curl_locks[CURL_LOCK_DATA_LAST+1];

static inline
char *vp_alloc(char *b, size_t *bsz, size_t sz)
{
  if(*bsz < sz) {
    if(b)
      b = realloc(b, sz);
    else
      b = malloc(sz);
    *bsz = sz;
    AN(b);
  }
  return b;
}

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
static void vp_log(enum VSL_tag_e, uint32_t, const char *fmt, ...);
#define VPLOGR vp_log_req
#define VPLOG(tag,fmt,...) vp_log((tag),0,(fmt), ## __VA_ARGS__)
#define VPLOGFD(tag,fd,fmt,...) vp_log((tag),(fd),(fmt), ## __VA_ARGS__)
#define VPLOGIG VPLOGFD

static
void vp_log_req(const struct vrt_ctx *ctx, enum VSL_tag_e tag,
                                         const char *fmt, ...)
{
  unsigned u = 0;
  va_list ap;
  char *s;
  char *b = NULL;
  size_t bsz = 0;


  CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
  if (ctx->ws != NULL) {
    u = WS_Reserve(ctx->ws,0);
    if(u <= 13) {
      WS_Release(ctx->ws,u);
      return;
    }
    s = stpcpy(ctx->ws->f,"vmod_peer: ");
    AN(s);
    strncpy(s,fmt,u-12);
    s = ctx->ws->f;
  } else {
    b = vp_alloc(b, &bsz, 12 + strlen(fmt));
    AN(b);
    s = stpcpy(b, "vmod_peer: ");
    AN(s);
    strncpy(s,fmt,u-12);
    s = b;
  }

  AN(s);
  va_start(ap,fmt);
  VSLbv(ctx->vsl, tag, s, ap);
  va_end(ap);
  if(b) {
    AN(bsz);
    VPFREE(b);
  } else {
    AN(ctx->ws);
    WS_Release(ctx->ws, u);
  }
}

static
void vp_log(enum VSL_tag_e tag, uint32_t id, const char *fmt, ...)
{
  va_list ap;
  char *b = NULL;
  size_t bsz = 0;
  int l;
  b = vp_alloc(b, &bsz, 4096);
  AN(b);
  va_start(ap,fmt);
  l = vsnprintf(b,4095,fmt,ap);
  va_end(ap);
  if (l < 4094)
    VSL(tag,id,"vmod_peer: %s",b);
  VPFREE(b);
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
struct peer_req *vp_req_new(struct vmod_peer *ctx) {
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  r = calloc(1,sizeof(*r));
  AN(r);
  r->magic = VMOD_PEER_REQ_MAGIC;
  r->c = NULL;
  r->body = NULL;
  r->host = r->url = r->proto = r->method = NULL;
  VTAILQ_INIT(&r->headers);
  vp_req_add_header(ctx,r,"Connection","close");
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
}

static
struct vmod_pthr *vp_alloc_thread(struct vmod_peer *ctx)
{
  vmod_pthr_t *p;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  if(ctx->tsz == 0) {
    ctx->threads = calloc(2,sizeof(*p));
    AN(ctx->threads);
    ctx->tsz = 2;
  } else while(ctx->tsz <= ctx->nthreads) {
    AN(ctx->threads);
    ctx->tsz *= 2;
    ctx->threads = realloc(ctx->threads, ctx->tsz * sizeof(*p));
    AN(ctx->threads);
  }

  p = &ctx->threads[ctx->nthreads++];
  p->magic = VMOD_PEER_THREAD_MAGIC;
  p->vp = ctx;
  p->shutdown = &ctx->shutdown;
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
  char *b = NULL;
  size_t bsz = 0;
  struct peer_req *r = (struct peer_req*)rv;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(msg && sz > 0) {
    b = vp_alloc(b,&bsz,sz+1);
    strncpy(b,msg,sz);
    *(b+sz) = '\0';
  }

  switch(t) {
  case CURLINFO_DATA_IN:
    break;
  case CURLINFO_DATA_OUT:
  case CURLINFO_HEADER_OUT:
    VSL(SLT_Debug, 0, "peer req %p > %s", r, b);
    break;
  case CURLINFO_HEADER_IN:
    VSL(SLT_Debug, 0, "peer req %p < %s", r, b);
    break;
  case CURLINFO_TEXT:
    VSL(SLT_Debug, 0, "peer req %p: %s", r, b);
    break;
  default:
    VSL(SLT_Debug, 0, "peer req %p unexpected debug type %d",r,(int)t);
    break;
  }

  if(b)
    free(b);
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
      VPLOGR(ctx, SLT_Error, "vmod_peer: queue depth would exceed %u/%u, waiters=%u, add more threads?",
              (unsigned)vp->qlen, (unsigned)vp->nthreads, (unsigned)vp->nwaiters);
      VPLOGR(ctx, SLT_Error, "vmod_peer: discarding %p %s", r, r->url);
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
  char *cp,*b = NULL;
  char *body;
  size_t blen,bsz = 0;
  int cl_set = 0, expect_set = 0;
  CURLcode cr;
  long status = 0;
  vmod_hdr_t *h;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

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
    b = vp_alloc(b,&bsz,strlen(h->key)+strlen(h->value)+3);
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
  if(r->proto && strcasecmp(r->proto,"HTTP/1.0") == 0)
    curl_easy_setopt(r->c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);

  if(vp->c_to > -1)
    curl_easy_setopt(r->c, CURLOPT_CONNECTTIMEOUT_MS, (long)vp->c_to);
  if(vp->d_to > -1)
    curl_easy_setopt(r->c, CURLOPT_TIMEOUT_MS, (long)vp->d_to);

  b = vp_alloc(b,&bsz,strlen(r->host)+strlen(r->url)+10);
  cp = stpcpy(b,"http://");
  AN(cp);
  cp = stpcpy(cp,r->host);
  AN(cp);
  if(*r->url != '/')
    *cp++ = '/';
  AN(stpcpy(cp, r->url));
  AN(b);
  curl_easy_setopt(r->c, CURLOPT_URL, b);
  VSL(SLT_Debug, 0, "peer req %p url %s",r,b);
  cr = curl_easy_perform(r->c);
  if(cr != 0) {
    const char *err = curl_easy_strerror(cr);
    VSL(SLT_Error, 0, "peer req %p (%s) error: %s",r,b,err);
  }
  curl_easy_getinfo(r->c, CURLINFO_RESPONSE_CODE, &status);
  VSL(SLT_Debug, 0, "peer req %p (%s) complete: %ld", r, b, status);
  free(b);
  if(req_headers)
    curl_slist_free_all(req_headers);
  if(body)
    curl_free(body);
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
  VSL(SLT_Debug, 0, "peer thread %p started",vp);
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
int vp_start_thread(struct vmod_peer *ctx)
{
  struct vmod_pthr *p;
  pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  p = vp_alloc_thread(ctx);
  CHECK_OBJ_NOTNULL(p, VMOD_PEER_THREAD_MAGIC);
  AZ(pthread_mutex_lock(&mtx));
  p->imtx = &mtx;
  p->icond = &cond;
  p->shutdown = &ctx->shutdown;
  AZ(pthread_create(&p->tid,NULL,vmod_peer_thread,p));
  while(p->alive == 0)
    AZ(pthread_cond_wait(&cond,&mtx));
  AZ(p->imtx);
  AZ(p->icond);
  return pthread_mutex_unlock(&mtx);
}

static
int vp_start_thread_safe(struct vmod_peer *ctx)
{
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&ctx->mtx));
  vp_start_thread(ctx);
  return pthread_mutex_unlock(&ctx->mtx);
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
  VSL(SLT_Debug, 0, "vp_lock_curl_data data=%d access=%d",(int)data,(int)access);
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
  VSL(SLT_Debug, 0, "vp_unlock_curl_data data=%d access=%d",(int)data,(int)access);
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
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION));
    AZ(curl_share_setopt(vp->csh, CURLSHOPT_USERDATA, vp));
    while(rv == 0 && vp->nthreads < vp->min)
      rv = vp_start_thread_safe(vp);
  }

  return (rv == 0 ? vp : NULL);
}

VCL_VOID
vmod_ip__init(const struct vrt_ctx *ctx, struct vmod_peer_ip **vpp,
              const char *vcl_name, VCL_IP addr)
{
  char *host = NULL;
  const struct sockaddr *sa;
  socklen_t sl = VSA_Len(addr);

  AN(vpp);
  *vpp = NULL;
  ALLOC_OBJ(*vpp, VMOD_PEER_IP_MAGIC);
  AN(*vpp);
  sa = VSA_Get_Sockaddr(addr,&sl);
  AN(sa);
  (*vpp)->addr = VSA_Malloc(sa,sl);
  assert(VSA_Sane((*vpp)->addr));
  (*vpp)->name = vcl_name;
  (*vpp)->vp = init_function(ctx);

  if((*vpp)->addr) {
    host = VRT_IP_string(ctx, (*vpp)->addr);
    if(host) {
      AZ(pthread_mutex_lock(&(*vpp)->vp->mtx));
      if((*vpp)->vp->host)
        VPFREE((*vpp)->vp->host);
      (*vpp)->vp->host = strdup(host);
      AN((*vpp)->vp->host);
      AZ(pthread_mutex_unlock(&(*vpp)->vp->mtx));
    }
  }
}

VCL_VOID
vmod_ip__fini(struct vmod_peer_ip **vpp)
{
  AN(vpp);
  if(*vpp) {
    struct vmod_peer *vp;
    CHECK_OBJ_NOTNULL(*vpp, VMOD_PEER_IP_MAGIC);
    vp = (*vpp)->vp;
    (*vpp)->vp = NULL;
    if(vp)
      vmod_peer_stop(vp);
    VPFREE((*vpp)->addr);
    FREE_OBJ(*vpp);
    *vpp = NULL;
  }
}

VCL_VOID
V4_VMOD_PEER(ip,set,VCL_STRING host, VCL_INT port)
{
  struct vmod_peer *vp;
  CHECK_OBJ_NOTNULL(vmp, VMOD_PEER_IP_MAGIC);
  vp = vmp->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&vp->mtx));

  if(port <= 0 || port > 65535)
    port = -1;
  else
    vp->port = port;

  if(host && *host) {
    char *b = NULL;
    size_t bsz = 0;
    if (vp->host)
      VPFREE(vp->host);

    if (port != -1 && vp->port != port) {
      size_t sz = strlen(host)+8;
      b = vp_alloc(b,&bsz,sz);
      (void)snprintf(b,sz,"%s:%u",host,(unsigned)port);
      host = b;
    }
    vp->host = strdup(host);
    if(b)
      free(b);
  } else if(vp->host)
    VPFREE(vp->host);

  AZ(pthread_mutex_unlock(&vp->mtx));
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
        VPLOGR(ctx, SLT_Error, "vmod_peer cannot start new thread: %s",
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
  } else {
    AN(ctx->req);
    r = vp_req_init(vp,ctx->http_req);
  }
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  for(p = s; p != vrt_magic_string_end; p = va_arg(ap, const char*)) {
    if (p != NULL) {
      size_t vl = strlen(p);
      if(vp_req_append(r,p,vl) != 0 && r->body && r->body->s_error != 0) {
        VPLOGR(ctx, SLT_Error, "peer req %p (%s) queue_req_body: %s",
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
    r = vp_req_init(vp,ctx->http_bereq);
  } else {
    AN(ctx->req);
    r = vp_req_init(vp,ctx->http_req);
  }
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  (void)vp_enqueue(vp,r,ctx);
}
