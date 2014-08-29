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
static pthread_mutex_t gmtx = PTHREAD_MUTEX_INITIALIZER;
static vp_lock_t *curl_locks[CURL_LOCK_DATA_LAST+1];

static inline
char *vp_alloc(char *b, size_t *bsz, size_t sz)
{
  AN(bsz);
  if(*bsz < sz) {
    if(b)
      b = realloc(b, sz);
    else
      b = malloc(sz);
    if(b)
      *bsz = sz;
    else
      *bsz = 0;
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
struct peer_req *vp_req_init(struct vmod_peer *vp, struct sess *sp, struct http *hp)
{
  uint16_t i;
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AN(sp); AN(sp->wrk);
  if(!hp)
    hp = sp->http;
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

#if 0
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
#endif

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
  p->alive = 0;
  p->tid = 0;
  p->imtx = NULL;
  p->icond = NULL;
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
    AN(b);
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
int vp_queue_req(struct vmod_peer *vp, struct peer_req *r, struct sess *sp)
{
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  AZ(pthread_mutex_lock(&vp->mtx));
  if((vp->qlen+1) / vp->nthreads > VMOD_PEER_MAX_QUEUE_DEPTH) {
    if(vp->nthreads >= vp->max || vp_start_thread(vp) != 0) {
      VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: queue depth would exceed %u/%u, waiters=%u, add more threads?",
              (unsigned)vp->qlen, (unsigned)vp->nthreads, (unsigned)vp->nwaiters);
      VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: discarding %p %s", r, r->url);
      AZ(pthread_mutex_unlock(&vp->mtx));
      vp_req_free(r);
      return -1;
    }
  }
  VTAILQ_INSERT_HEAD(&vp->q, r, list);
  if(++vp->qlen > 1 && vp->nwaiters > 1)
    AZ(pthread_cond_broadcast(&vp->cond));
  else
    AZ(pthread_cond_signal(&vp->cond));
  VSL(SLT_Debug, (sp ? sp->id : 0), "vmod_peer: qlen=%u/threads=%u/waiters=%u",
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
    b = vp_alloc(b,&bsz,strlen(h->key)+strlen(h->value)+10);
    AN(b);
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
    free(b);
    b = NULL;
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
  AN(b);
  cp = stpcpy(b,"http://");
  AN(cp);
  cp = stpcpy(cp,r->host);
  AN(cp);
  if(*r->url != '/')
    *cp++ = '/';
  AN(stpcpy(cp, r->url));
  curl_easy_setopt(r->c, CURLOPT_URL, b);
  VSL(SLT_Debug, 0, "peer req %p url %s",r,b);
  cr = curl_easy_perform(r->c);
  if(cr != 0) {
    const char *err = curl_easy_strerror(cr);
    VSL(SLT_Error, 0, "peer req %p (%s) error: %s",r,b,err);
  }
  curl_easy_getinfo(r->c, CURLINFO_RESPONSE_CODE, &status);
  VSL(SLT_Debug, 0, "peer req %p (%s) complete: %ld", r, b, status);
  free(b); b = NULL;
  if(req_headers)
    curl_slist_free_all(req_headers);
  if(body)
    curl_free(body);
}

static struct vmod_peer
*vmod_peer_thread_register(struct vmod_pthr *thr)
{
  struct vmod_peer *vp = NULL;
  pthread_mutex_t *mtx;
  pthread_cond_t *cond;
  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AN(thr->imtx);
  AN(thr->icond);
  AZ(pthread_mutex_lock(thr->imtx));
  mtx = thr->imtx;
  cond = thr->icond;
  AN(mtx);
  AN(cond);
  AZ(thr->alive);
  assert(mtx == thr->imtx);
  assert(cond == thr->icond);
  AZ(thr->alive);
  thr->alive++;
  thr->imtx = NULL;
  thr->icond = NULL;
  vp = thr->vp;
  AZ(pthread_cond_signal(cond));
  AZ(pthread_mutex_unlock(mtx));
  AN(vp);
  VSL(SLT_Debug, 0, "peer thread %p registered",(void*)thr->tid);
  return vp;
}

static
void vmod_peer_thread_unregister(struct vmod_pthr *thr) {
  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AN(thr->alive);
  thr->alive--;
  VSL(SLT_Debug, 0, "peer thread %p unregistered",(void*)thr->tid);
  AZ(thr->alive);
}

static
VMOD_THREAD_FUNC vmod_peer_thread(void *vctx)
{
  vmod_pthr_t *thr = (struct vmod_pthr*)vctx;
  struct vmod_peer *vp;

  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  vp = vmod_peer_thread_register(thr);
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
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

  AZ(pthread_mutex_lock(&gmtx));
  vmod_peer_thread_unregister(thr);
  AZ(pthread_mutex_unlock(&vp->mtx));
  AZ(pthread_mutex_unlock(&gmtx));
  return NULL;
}

static
int vp_start_thread(struct vmod_peer *ctx)
{
  struct vmod_pthr *p;
  static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  AZ(pthread_mutex_lock(&mtx));
  p = vp_alloc_thread(ctx);
  CHECK_OBJ_NOTNULL(p, VMOD_PEER_THREAD_MAGIC);
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
void vmod_peer_stop(void *v)
{
  struct peer_req *r, *r2;
  struct vmod_peer *ctx = (struct vmod_peer*)v;
  int nalive;

  AZ(pthread_mutex_lock(&gmtx));
  if(!initialized) {
    AZ(pthread_mutex_unlock(&gmtx));
    return;
  }
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  do {
    int i;

    AZ(pthread_mutex_lock(&ctx->mtx));

    VTAILQ_FOREACH_SAFE(r, &ctx->q, list, r2) {
      VTAILQ_REMOVE(&ctx->q,r,list);
      vp_req_free(r);
    }

    for(i = nalive = 0; i < ctx->nthreads; i++) {
      ctx->threads[i].shutdown = &ctx->shutdown;
      nalive += ctx->threads[i].alive;
    }

    ctx->shutdown++;
    if(nalive) {
      for(i = 0; i < ctx->nthreads; i++) {
        pthread_t tid;
        if((tid = ctx->threads[i].tid) != (pthread_t)0) {
          CHECK_OBJ_NOTNULL(&ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
          AZ(pthread_cond_broadcast(&ctx->cond));
          AZ(pthread_mutex_unlock(&ctx->mtx));
          AZ(pthread_mutex_unlock(&gmtx));
          pthread_join(tid,NULL);
          AZ(pthread_mutex_lock(&gmtx));
          AZ(pthread_mutex_lock(&ctx->mtx));
          assert(!ctx->threads[i].alive);
          tid = ctx->threads[i].tid = (pthread_t)0;
        }
      }
    }

    AZ(pthread_mutex_unlock(&ctx->mtx));
  } while(nalive > 0);

  if(ctx->csh) {
    curl_share_cleanup(ctx->csh);
    ctx->csh = NULL;
  }

  pthread_cond_destroy(&ctx->cond);
  pthread_mutex_destroy(&ctx->mtx);
  if(ctx->host)
    VPFREE(ctx->host);
  if(ctx->threads)
    VPFREE(ctx->threads);
  ctx->magic = 0;
  free(ctx);

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
  AZ(pthread_mutex_unlock(&gmtx));
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

/* vmod initialization */
int init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
  int rv = 0;
  struct vmod_peer *ctx;

  AZ(pthread_mutex_lock(&gmtx));

  ctx = calloc(1,sizeof(*ctx));
  AN(ctx);
  AZ(priv->priv);
  AZ(priv->free);
  ctx->magic = VMOD_PEER_MAGIC;
  ctx->nthreads = 0;
  ctx->nwaiters = 0;
  ctx->min = ctx->max = 1;
  AZ(pthread_mutex_init(&ctx->mtx,NULL));
  AZ(pthread_cond_init(&ctx->cond,NULL));
  VTAILQ_INIT(&ctx->q);
  ctx->qlen = 0;
  ctx->tsz = 0;
  ctx->host = NULL;
  ctx->port = ctx->c_to = ctx->d_to = -1;
  priv->priv = ctx;
  priv->free = vmod_peer_stop;

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
    ctx->csh = curl_share_init();
    AN(ctx->csh);
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_LOCKFUNC, vp_lock_curl_data));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_UNLOCKFUNC, vp_unlock_curl_data));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_USERDATA, ctx));
    while(rv == 0 && ctx->nthreads < ctx->min)
      rv = vp_start_thread_safe(ctx);
  }
  AZ(pthread_mutex_unlock(&gmtx));
  return rv;
}

void vmod_set(struct sess *sp, struct vmod_priv *priv, const char *host, int port)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(&ctx->mtx));

  if(port <= 0 || port > 65535)
    port = -1;
  else
    ctx->port = port;

  if(host && *host) {
    char *b = NULL;
    size_t bsz = 0;
    if (ctx->host)
      VPFREE(ctx->host);

    if (port != -1 && ctx->port != port) {
      size_t sz = strlen(host)+8;
      b = vp_alloc(b,&bsz,sz);
      AN(b);
      (void)snprintf(b,sz,"%s:%u",host,(unsigned)port);
      host = b;
    }
    ctx->host = strdup(host);
    if(b) {
      free(b);
      b = NULL;
    }
  } else if(ctx->host)
    VPFREE(ctx->host);

  AZ(pthread_mutex_unlock(&ctx->mtx));
}

void vmod_set_threads(struct sess *sp, struct vmod_priv *priv, int min, int max)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  int rv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  if(max > 0 && min > 0 && max < min)
    max = min;

  AZ(pthread_mutex_lock(&ctx->mtx));
  if(min > 0) {
    ctx->min = min;
    if (ctx->max < min)
      ctx->max = min;
  }
  if(max > 0) {
    ctx->max = max;
    if (ctx->min > max)
      ctx->min = max;
  }

  while(ctx->nthreads > ctx->max) {
    /* too many threads, get rid of some */
    pthread_t tid;
    int shutdown = 1;
    vmod_pthr_t *t = &ctx->threads[ctx->nthreads-1];
    if(!t->alive) {
      ctx->nthreads--;
      continue;
    }
    CHECK_OBJ_NOTNULL(t, VMOD_PEER_THREAD_MAGIC);
    tid = t->tid;
    t->shutdown = &shutdown;
    if(ctx->nwaiters != 1)
      AZ(pthread_cond_broadcast(&ctx->cond));
    else
      AZ(pthread_cond_signal(&ctx->cond));
    AZ(pthread_mutex_unlock(&ctx->mtx));
    pthread_join(tid, NULL);
    AZ(pthread_mutex_lock(&ctx->mtx));
    assert(!t->alive);
    ctx->nthreads--;
    t->magic = 0;
  }
  for(rv = 0; rv == 0 && ctx->nthreads < ctx->min; ) {
    AZ(pthread_mutex_unlock(&ctx->mtx));
    rv = vp_start_thread_safe(ctx);
    if (rv != 0) {
      if(errno != 0)
        VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer cannot start new thread: %s",
            strerror(errno));
    }
    AZ(pthread_mutex_lock(&ctx->mtx));
  }
  AZ(pthread_mutex_unlock(&ctx->mtx));
}

int vmod_threads(struct sess *sp, struct vmod_priv *priv)
{
  int i,n = 0;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  for(i = 0; i < ctx->nthreads; i++) {
    n += ctx->threads[i].alive;
  }
  AZ(pthread_mutex_unlock(&ctx->mtx));
  return n;
}

int vmod_min_threads(struct sess *sp, struct vmod_priv *priv)
{
  int n;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  n = ctx->min;
  AZ(pthread_mutex_unlock(&ctx->mtx));
  return n;
}

int vmod_max_threads(struct sess *sp, struct vmod_priv *priv)
{
  int n;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  n = ctx->max;
  AZ(pthread_mutex_unlock(&ctx->mtx));
  return n;
}

int vmod_pending(struct sess *sp, struct vmod_priv *priv)
{
  size_t qlen;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return -1;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  qlen = ctx->qlen;
  AZ(pthread_mutex_unlock(&ctx->mtx));

  return qlen;
}

/* set connect timeout in ms */
void vmod_set_connect_timeout(struct sess *sp, struct vmod_priv *priv, int ms)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  ctx->c_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&ctx->mtx));
}

/* set connect timeout in ms */
void vmod_set_timeout(struct sess *sp, struct vmod_priv *priv, int ms)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  ctx->d_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&ctx->mtx));
}

/* add the current request + post body to the pending list */
static
void vmod_enqueue_post_ap(struct sess *sp, struct vmod_priv *priv,
                         const char *s, va_list ap)
{
  const char *p;
  ssize_t l = 0;
  struct peer_req *r;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  r = vp_req_init(ctx,sp,NULL);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  for(p = s; p != vrt_magic_string_end; p = va_arg(ap, const char*)) {
    if (p != NULL) {
      size_t vl = strlen(p);
      if(vp_req_append(r,p,vl) != 0 && r->body && r->body->s_error != 0) {
        VSL(SLT_Error, sp->id, "peer req %p (%s) queue_req_body: %s",
            p,r->url,strerror(r->body->s_error));
        l = -1;
        break;
      }
      l += vl;
    }
  }

  if(l >= 0)
    (void)vp_queue_req(ctx,r,sp);
  else
    vp_req_free(r);
}

void vmod_enqueue_req_body(struct sess *sp, struct vmod_priv *priv,
                           const char *s, ...)
{
  va_list ap;
  va_start(ap,s);
  vmod_enqueue_post_ap(sp,priv,s,ap);
  va_end(ap);
}

void vmod_queue_req_ap(struct sess *sp, struct vmod_priv *priv,
                           const char *s, ...)
{
  va_list ap;
  va_start(ap,s);
  vmod_enqueue_post_ap(sp,priv,s,ap);
  va_end(ap);
}

/* add the current request to the pending list */
void vmod_enqueue_req(struct sess *sp, struct vmod_priv *priv)
{
  struct peer_req *r;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  r = vp_req_init(ctx,sp,NULL);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  (void)vp_queue_req(ctx,r,sp);
}

void vmod_queue_req(struct sess *sp, struct vmod_priv *priv)
{
  vmod_enqueue_req(sp,priv);
}
