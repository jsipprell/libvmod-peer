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
  const char *method,*proto,*url;
  const char *host;
  VTAILQ_HEAD(,vmod_hdr) headers;
  VTAILQ_ENTRY(peer_req) list;
};

typedef struct vmod_pthr vmod_pthr_t;

struct vmod_peer {
  unsigned magic;
#define VMOD_PEER_MAGIC 0x5bba391c
  unsigned nthreads;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int shutdown;
  VTAILQ_HEAD(, peer_req) q;
  size_t qlen;
  unsigned nwaiters;
  vmod_pthr_t *threads;
  size_t tsz;
};

struct vmod_pthr {
  unsigned magic;
#define VMOD_PEER_THREAD_MAGIC 0x1c93abb5
  pthread_t tid;
  int alive;
  pthread_mutex_t *imtx;
  pthread_cond_t *icond;
  struct vmod_peer *vp;
};

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
struct peer_req *vp_new_req(struct vmod_peer *ctx) {
  struct peer_req *r;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  r = calloc(1,sizeof(*r));
  AN(r);
  r->magic = VMOD_PEER_REQ_MAGIC;
  r->host = r->url = r->proto = r->method = NULL;
  VTAILQ_INIT(&r->headers);
  vp_req_add_header(ctx,r,"connection","close");
  return r;
}

static
void vp_free_req(struct peer_req *r)
{
  vmod_hdr_t *h, *h2;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(r->url)
    free(r->url);
  if(r->host)
    free(r->host);
  if(r->proto)
    free(r->proto);
  if(r->method)
    free(r->method);
  VTAILQ_FOREACH_SAFE(h, &r->headers, list, h2) {
    VTAILQ_REMOVE(&r->headers,h,list);
    free(h->key);
    free(h->value);
    free(h);
  }
}

static
struct vmod_pthr_t vp_alloc_thread(struct vmod_peer *ctx)
{
  vmod_pthr_t *p;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  if(ctx->tsz == 0) {
    ctx->threads = calloc(2,sizeof(*p));
    AN(ctx->threads);
    ctx->tsz = 2;
  } else while(ctx->tsz <= ctx->nthreads) {
    ctx->tsz *= 2;
    ctx->threads = realloc(ctx->tsz * sizeof(*p));
    AN(ctx->threads);
  }

  p = &ctx->threads[ctx->nthreads++];
  p->magic = VMOD_PEER_THREAD_MAGIC;
  p->vp = ctx;
  return p;
}

static
void vp_process_req(struct vmod_peer *vp, struct peer_req *r)
{
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
  VSL(SLT_Debug, 0, "peer req %s processed",r->url);
}

static
VMOD_THREAD_FUNC vmod_peer_thread(void *vctx)
{
  vmod_pthr_t *thr = (struct vmod_pthr*)vctx;
  struct vmod_peer *vp;

  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  vp = thr->vp;
  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  AZ(pthread_mutex_lock(thr->imtx));
  thr->alive++;
  AZ(pthread_cond_signal(thr->icond));
  AZ(pthread_mutex_unlock(thr->imtx));
  AZ(pthread_mutex_lock(&vp->mtx));
  do {
    struct peer_req *r;
    assert(thr->vp == vp);
    CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
    while(!vp->shutdown && vp->qlen == 0)
      AZ(pthread_cond_wait(&vp->cond,&vp->mtx,NULL));
    for(r = VTAILQ_FIRST(&vp->q); !vp->shutdown && vp->qlen > 0; r = VTAILQ_FIRST(&vp->q)) {
      CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
      VTAILQ_REMOVE(&vp->q, r, list);
      vp->qlen--;
      AZ(pthread_mutex_unlock(&vp->mtx));
      vp_process_req(vp,r);
      vp_free_req(r);
      AZ(pthread_mutex_lock(&vp->mtx));
    }
  } while(!vp->shutdown);

  AZ(pthread_mutex_unlock(&vp->mtx));
  return NULL;
}

static
int vp_start_thread(struct vmod_peer *ctx)
{
  struct vmod_pthr *p;
  pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  AZ(pthread_mutex_lock(&ctx->mtx));
  p = vp_alloc_thread(ctx);
  CHECK_OBJ_NOTNULL(p, VMOD_PEER_THREAD_MAGIC);
  AZ(pthread_mutex_lock(&mtx));
  p->imtx = &mtx;
  p->icond = &cond;
  AZ(pthread_create(&p->tid,NULL,vmod_peer_thread,p));
  while(p->alive == 0)
    AZ(pthread_cond_wait(&cond,&mtx));
  AZ(p->imtx);
  AZ(p->icond);
  AZ(pthread_mutex_unlock(&mtx));
  return pthread_mutex_unlock(&ctx->mtx);
}

static
void vmod_peer_stop(void *v)
{
  struct peer_req *r, *r2;
  struct vmod_peer *ctx = (struct vmod_peer*)v;
  int nalive;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  do {
    int i;

    AZ(pthread_mutex_lock(&ctx->mtx))
    for(i = nalive = 0; i < ctx->nthreads; i++)
      nalive += ctx->threads[i].alive;

    ctx->shutdown++;
    if(nalive) {
      AZ(pthread_cond_broadcast(&ctx->cond));
      for(i = 0; i < ctx->nthreads; i++) {
        if(ctx->threads[i].alive) {
          pthread_t tid;
          CHECK_OBJ_NOTNULL(&ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
          tid = ctx->threads[i].tid;
          AZ(pthread_mutex_unlock(&ctx->mtx));
          pthread_join(ctx->threads[i].tid,NULL);
          AZ(pthread_mutex_lock(&ctx->mtx));
        }
      }
    }
  } while(nalive > 0);

  VTAILQ_FOREACH_SAFE(r, &ctx->q, list, r2) {
    VTAILQ_REMOVE(&ctx->q,r,list);
    vp_free_req(r);
  }

  pthread_cond_destroy(&ctx->cond);
  pthread_mutex_destroy(&ctx->mtx);
  if(ctx->threads)
    free(ctx->threads);
  ctx->magic = 0;
  free(ctx);
}

/* vmod initialization */
int init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
  static int initialized = 0;
  struct vmod_peer *ctx;

  if (initialized)
    return 0;
  initialized = 1;

  ctx = calloc(1,sizeof(*ctx));
  AN(ctx);
  ctx->magic = VMOD_PEER_MAGIC;
  ctx->nthreads = 0;
  ctx->nwaiters = 0;
  AZ(pthread_mutex_init(&ctx->mtx,NULL));
  AZ(pthread_cond_init(&ctx->cond,NULL));
  VTAILQ_INIT(&ctx->q);
  ctx->qlen = 0;
  ctx->tsz = 0;
  priv->priv = ctx;
  priv->free = vmod_peer_stop;
  return vp_start_thread(ctx);
}

/* add the current request to the pending list */
void vmod_queue_req(struct sess *sp, struct vmod_priv *priv)
{
  uint16_t i;
  struct http *hp;
  struct peer_req *r;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  r = vp_new_req(ctx);
  hp = sp->http;
  AN(hp);
  r->method = strdup(http_GetReq(hp));
  AN(r->method);
  Tcheck(hp->hd[HTTP_HDR_URL]);
  r->url = strndup(hp->hd[HTTP_HDR_URL].b,Tlen(hp->hd[HTTP_HDR_URL]));
  AN(r->url);
  Tcheck(hp->hd[HTTP_HDR_PROTO]);
  r->proto = strndup(hp->hd[HTTP_HDR_PROTO].b,Tlen(hp->hd[HTTP_HDR_PROTO]));
  AN(r->proto);
  for(i = HTTP_HDR_FIRST; i < hp->nhd; i++) {
    if(hp->hd[i].b == NULL || hp->hdf[i] & HDF_FILTER)
      continue;
    Tcheck(hp->hd[i]);
    if (http_IsHdr(&hp->hd[i],H_Host)) {
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
          vp_req_add_header(ctx,r,k,v);
      }
      free(k);
    }
  }

  if (r->host == NULL) {
    VSL(SLT_Error, sp->id, "Cannot queue request without host header: %s",
        r->url);
    vp_free_req(r);
    return;
  }
  AZ(pthread_mutex_lock(&ctx->mtx));
  VTAILQ_INSERT_HEAD(&ctx->q, r, list);
  if(++ctx->qlen > 1 && ctx->nwaiters > 1)
    AZ(pthread_cond_broadcast(&ctx->cond));
  else if(ctx->nwaiters > 0)
    AZ(pthread_cond_signal(&ctx->cond));
  AZ(pthread_mutex_unlock(&ctx->mtx));
}
