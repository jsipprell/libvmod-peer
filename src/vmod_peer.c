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
  CURL *c;
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

static vp_lock_t *curl_locks[CURL_LOCK_DATA_LAST+1];
static pthread_key_t cur_lock_key;

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
  r->c = NULL;
  r->host = r->url = r->proto = r->method = NULL;
  VTAILQ_INIT(&r->headers);
  vp_req_add_header(ctx,r,"Connection","close");
  return r;
}

static
void vp_free_req(struct peer_req *r)
{
  vmod_hdr_t *h, *h2;
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(r->c)
    curl_easy_cleanup(r->c);
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
void vp_process_req(struct vmod_peer *vp, struct vmod_pthr *thr, struct peer_req *r)
{
  struct curl_slist *req_headers = NULL;
  char *cp,*b = NULL;
  size_t bsz = 0;
  int cl_set = 0;
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

  if(r->method) {
    if(strcasecmp(r->method,"HEAD") == 0)
      curl_easy_setopt(r->c, CURLOPT_NOBODY, 1L);
    else if(strcasecmp(r->method, "POST") == 0)
      curl_easy_setopt(r->c, CURLOPT_POST, 1L);
    else
      curl_easy_setopt(r->c, CURLOPT_HTTPGET, 1L);
  } else {
    if(!cl_set)
        curl_slist_append(req_headers,"Content-Length: 0");
    curl_easy_setopt(r->c, CURLOPT_NOBODY, 1L);
  }

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
  }
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
      vp_free_req(r);
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

  AZ(pthread_mutex_lock(&ctx->mtx));
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

    AZ(pthread_mutex_lock(&ctx->mtx));

    VTAILQ_FOREACH_SAFE(r, &ctx->q, list, r2) {
      VTAILQ_REMOVE(&ctx->q,r,list);
      vp_free_req(r);
    }

    for(i = nalive = 0; i < ctx->nthreads; i++) {
      ctx->threads[i].shutdown = &ctx->shutdown;
      nalive += ctx->threads[i].alive;
    }

    ctx->shutdown++;
    if(nalive) {
      AZ(pthread_cond_broadcast(&ctx->cond));
      for(i = 0; i < ctx->nthreads; i++) {
        if(ctx->threads[i].alive) {
          pthread_t tid;
          CHECK_OBJ_NOTNULL(&ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
          tid = ctx->threads[i].tid;
          AZ(pthread_mutex_unlock(&ctx->mtx));
          pthread_join(tid,NULL);
          AZ(pthread_mutex_lock(&ctx->mtx));
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
    free((char*)ctx->host);
  if(ctx->threads)
    free(ctx->threads);
  ctx->magic = 0;
  free(ctx);
}

static
curl_lock_access vp_lock_access_get(pthread_key_t *k)
{
  curl_lock_access *ap = pthread_getspecific(*k);
  AN(ap);
  return *ap;
}

static
void vp_lock_access_set(pthread_key_t *k, curl_lock_access a)
{
  curl_lock_access *ap = pthread_getspecific(*k);
  if(ap == NULL) {
    ap = calloc(1,sizeof(*ap));
    AN(ap);
  }
  *ap = a;
  AZ(pthread_setspecific(*k,ap));
}

static
void vp_lock_curl_data(CURLSH *s, curl_lock_data data, curl_lock_access access, void *va)
{
#if 0
  VSL(SLT_Debug, 0, "vp_lock_curl_data data=%d access=%d",(int)data,(int)access);
#endif
  vp_lock_access_set((pthread_key_t*)va,access);
  if(access == CURL_LOCK_ACCESS_SHARED)
    AZ(vp_lock_read_acquire(curl_locks[data]));
  else if(access == CURL_LOCK_ACCESS_SINGLE)
    AZ(vp_lock_write_acquire(curl_locks[data]));
}

static
void vp_unlock_curl_data(CURLSH *s, curl_lock_data data, void *va)
{
  curl_lock_access access = vp_lock_access_get((pthread_key_t*)va);
#if 0
  VSL(SLT_Debug, 0, "vp_unlock_curl_data data=%d access=%d",(int)data,(int)access);
#endif
  if(access == CURL_LOCK_ACCESS_SHARED)
    AZ(vp_lock_read_release(curl_locks[data]));
  else if(access == CURL_LOCK_ACCESS_SINGLE)
    AZ(vp_lock_write_release(curl_locks[data]));
}

/* vmod initialization */
int init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
  static int initialized = 0;
  int rv = 0;
  struct vmod_peer *ctx;

  ctx = calloc(1,sizeof(*ctx));
  AN(ctx);
  AZ(priv->priv);
  AZ(priv->free);
  ctx->magic = VMOD_PEER_MAGIC;
  ctx->nthreads = 0;
  ctx->nwaiters = 0;
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
    AZ(pthread_key_create(&cur_lock_key,free));
    rv = curl_global_init(CURL_GLOBAL_ALL);
    initialized = 1;
  }
  if(rv == 0) {
    ctx->csh = curl_share_init();
    AN(ctx->csh);
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_LOCKFUNC, vp_lock_curl_data));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_UNLOCKFUNC, vp_unlock_curl_data));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION));
    AZ(curl_share_setopt(ctx->csh, CURLSHOPT_USERDATA, &cur_lock_key));
    rv = vp_start_thread(ctx);
  }
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
      free((char*)ctx->host);

    if (port != -1 && ctx->port != port) {
      size_t sz = strlen(host)+8;
      b = vp_alloc(b,&bsz,sz);
      (void)snprintf(b,sz,"%s:%u",host,(unsigned)port);
      host = b;
    }
    ctx->host = strdup(host);
    if(b)
      free(b);
  } else if(ctx->host) {
    free((char*)ctx->host);
    ctx->host = NULL;
  }
  AZ(pthread_mutex_unlock(&ctx->mtx));
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
  size_t qlen;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  ctx->c_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&ctx->mtx));
}

/* set connect timeout in ms */
void vmod_set_timeout(struct sess *sp, struct vmod_priv *priv, int ms)
{
  size_t qlen;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  AZ(pthread_mutex_lock(&ctx->mtx));
  ctx->d_to = (ms ? ms : -1);
  AZ(pthread_mutex_unlock(&ctx->mtx));
}

/* add the current request to the pending list */
void vmod_queue_req(struct sess *sp, struct vmod_priv *priv)
{
  uint16_t i;
  struct http *hp;
  struct peer_req *r;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return;

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
  AZ(pthread_mutex_lock(&ctx->mtx));
  if(ctx->host) {
    r->host = strdup(ctx->host);
    AN(r->host);
  }
  AZ(pthread_mutex_unlock(&ctx->mtx));
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
          vp_req_set_header(ctx,r,k,v);
      }
      free(k);
    }
  }

  AZ(pthread_mutex_lock(&ctx->mtx));
  if(ctx->qlen+1 > VMOD_PEER_MAX_QUEUE_DEPTH) {
    VSL(SLT_Error, sp->id, "vmod_peer: queue depth would exceepd %u, waiters=%u, add more threads?",
                            (unsigned)ctx->qlen, (unsigned)ctx->nwaiters);
    VSL(SLT_Error, sp->id, "vmod_peer: discarding %p %s", r, r->url);
    AZ(pthread_mutex_unlock(&ctx->mtx));
    vp_free_req(r);
    return;
  }
  VTAILQ_INSERT_HEAD(&ctx->q, r, list);
  if(++ctx->qlen > 1 && ctx->nwaiters > 1)
    AZ(pthread_cond_broadcast(&ctx->cond));
  else if(ctx->nwaiters > 0)
    AZ(pthread_cond_signal(&ctx->cond));
  VSL(SLT_Debug, sp->id, "vmod_peer: qlen=%u waiters=%u",
            (unsigned)ctx->qlen,(unsigned)ctx->nwaiters);
  AZ(pthread_mutex_unlock(&ctx->mtx));
}
