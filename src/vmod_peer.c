#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include <curl/curl.h>

#include "vmod_peer.h"

#ifndef VP_NEW_GLOBAL_LOCK
#define VP_NEW_GLOBAL_LOCK(lck) vp_mutex_new(lck,vcl)
#endif

#ifndef VP_NEW_LOCK
#define VP_NEW_LOCK(lck) vp_mutex_new(lck,wq)
#endif

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
  struct lock mtx;
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
  struct lock *imtx;
  pthread_cond_t *icond;
  struct vmod_peer *vp;
};

static int initialized = 0;
static struct lock gmtx;

static vp_lock_t *curl_locks[CURL_LOCK_DATA_LAST+1];
static unsigned int max_q_depth = VMOD_PEER_MAX_QUEUE_DEPTH;

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

/* Note: always call with mutex locked */
static inline unsigned vp_count_alive_threads(struct vmod_peer *ctx)
{
  unsigned i,nalive;

  for(i = 0, nalive = 0; i < ctx->nthreads; i++) {
    if(ctx->threads[i].magic && ctx->threads[i].tid != (pthread_t)0)
      nalive += ctx->threads[i].alive;
  }
  return nalive;
}

#define VP_THREADS_ALIVE(ctx) vp_count_alive_threads(ctx)

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
  vp_mutex_lock(&vp->mtx);
  if(vp->host) {
    r->host = strdup(vp->host);
    AN(r->host);
  }
  vp_mutex_unlock(&vp->mtx);
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
  vmod_pthr_t *p = NULL;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  vp_mutex_lock(&gmtx);
  if(ctx->tsz == 0) {
    ctx->threads = calloc(VMOD_PEER_MAX_THREAD_POOL,sizeof(*p));
    AN(ctx->threads);
    ctx->tsz = VMOD_PEER_MAX_THREAD_POOL;
  } else {
    int i;
    assert(ctx->tsz >= ctx->nthreads);
    for(i = 0; i < ctx->nthreads; i++) {
      if(ctx->threads[i].tid == (pthread_t)0) {
        CAST_OBJ_NOTNULL(p, &ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
        AZ(p->alive);
        break;
      }
    }
  }

  if(!p) {
    assert(ctx->nthreads < ctx->tsz);
    p = &ctx->threads[ctx->nthreads++];
  }
  p->magic = VMOD_PEER_THREAD_MAGIC;
  p->vp = ctx;
  p->alive = 0;
  p->tid = 0;
  p->imtx = NULL;
  p->icond = NULL;
  p->shutdown = &ctx->shutdown;
  vp_mutex_unlock(&gmtx);
  return p;
}

static
size_t vp_write_data(char *ptr, size_t sz, size_t n, void *rv)
{
  CHECK_OBJ_NOTNULL((struct peer_req*)rv, VMOD_PEER_REQ_MAGIC);

  /* noop basically */
  return sz * n;
}

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
    VSL(SLT_Debug, 0, "vmod_peer req %p > %s", r, b);
    break;
  case CURLINFO_HEADER_IN:
    VSL(SLT_Debug, 0, "vmod_peer req %p < %s", r, b);
    break;
  case CURLINFO_TEXT:
    VSL(SLT_Debug, 0, "vmod_peer req %p: %s", r, b);
    break;
  default:
    VSL(SLT_Debug, 0, "vmod_peer req %p unexpected debug type %d",r,(int)t);
    break;
  }

  if(b)
    free(b);
  return 0;
}

static
int vp_queue_req(struct vmod_peer *vp, struct peer_req *r, struct sess *sp)
{
  unsigned nalive;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  vp_mutex_lock(&vp->mtx);
  nalive = VP_THREADS_ALIVE(vp);
  if(!nalive || ((vp->qlen+1) / nalive > max_q_depth)) {
    if(nalive && nalive >= vp->max) {
      VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: queue depth would exceed %u/%u, waiters=%u, add more threads? (max=%u)",
              (unsigned)vp->qlen, (unsigned)nalive, (unsigned)vp->nwaiters, (unsigned)vp->max);
      VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: discarding %p %s", r, r->url);
      errno = EBUSY;
      goto vp_queue_req_err;
    } else if(vp_start_thread(vp) != 0) {
      VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: discarding %p %s, cannot start new thread: %s",
              r, r->url, strerror(errno));
      goto vp_queue_req_err;
    }
  }
  VTAILQ_INSERT_HEAD(&vp->q, r, list);
  if(++vp->qlen > 1 && vp->nwaiters > 1)
    AZ(pthread_cond_broadcast(&vp->cond));
  else
    AZ(pthread_cond_signal(&vp->cond));
  VSL(SLT_Debug, (sp ? sp->id : 0), "vmod_peer: qlen=%u/threads=%u/nalive=%u/waiters=%u",
            (unsigned)vp->qlen,(unsigned)vp->nthreads,
            (unsigned)VP_THREADS_ALIVE(vp),(unsigned)vp->nwaiters);
  vp_mutex_unlock(&vp->mtx);
  return 0;
vp_queue_req_err:
  vp_req_free(r);
  if(vp_mutex_isheld(&vp->mtx))
    vp_mutex_unlock(&vp->mtx);
  return -1;
}

static
void vp_process_req(struct vmod_peer *vp, struct vmod_pthr *thr, struct peer_req *r)
{
  static pthread_key_t tkey;
  static int init_tkey = 1;
  struct curl_slist *req_headers = NULL;
  char *errstr = NULL;
  char *cp,*b = NULL;
  char *body;
  size_t blen,bsz = 0;
  int cl_set = 0, expect_set = 0;
  CURLcode cr;
  long status = 0;
  vmod_hdr_t *h;

  CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
  CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);

  if(init_tkey) {
    vp_mutex_lock(&gmtx);
    if(init_tkey) {
      AZ(pthread_key_create(&tkey,free));
      init_tkey--;
    }
    vp_mutex_unlock(&gmtx);
  }

  errstr = pthread_getspecific(tkey);
  if(!errstr) {
    errstr = malloc(CURL_ERROR_SIZE+1);
    AN(errstr);
    *errstr = '\0';
    pthread_setspecific(tkey,errstr);
  }

  if(!r->c)
    r->c = curl_easy_init();
  AN(r->c);

  if(vp->csh)
    curl_easy_setopt(r->c, CURLOPT_SHARE, vp->csh);

  AN(errstr);
  curl_easy_setopt(r->c, CURLOPT_ERRORBUFFER, errstr);

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
#if 0
#ifndef DEBUG
  if(!(params->diag_bitmap & 0x1))
#endif
#endif
    curl_easy_setopt(r->c, CURLOPT_FAILONERROR, 1L);

  if(params->diag_bitmap & 0x1) {
    curl_easy_setopt(r->c, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(r->c, CURLOPT_DEBUGFUNCTION, vp_debug_callback);
    curl_easy_setopt(r->c, CURLOPT_DEBUGDATA, r);
  }

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
  VSL(SLT_Debug, 0, "vmod_peer req %p url %s",r,b);
  cr = curl_easy_perform(r->c);
  if(cr != 0) {
    VSL(SLT_Error, 0, "vmod_peer req %p (%s) error: %s",r,b,errstr);
  } else {
    curl_easy_getinfo(r->c, CURLINFO_RESPONSE_CODE, &status);
    VSL(SLT_Debug, 0, "vmod_peer req %p (%s) complete: %ld", r, b, status);
  }
  if(req_headers)
    curl_slist_free_all(req_headers);
  if(body)
    curl_free(body);
  if(b)
    free(b);
}

static struct vmod_peer
*vmod_peer_thread_register(struct vmod_pthr *thr)
{
  struct vmod_peer *vp = NULL;
  pthread_t tid;
  struct lock *mtx;
  pthread_cond_t *cond;
  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AN(thr->imtx);
  AN(thr->icond);
  vp_mutex_lock(thr->imtx);
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
  tid = thr->tid;
  AZ(pthread_cond_signal(cond));
  vp_mutex_unlock(mtx);
  AN(vp);
  VSL(SLT_Debug, 0, "vmod_peer: thread %p registered",(void*)thr->tid);
  return vp;
}

static
void vmod_peer_thread_unregister(struct vmod_pthr *thr) {
  CHECK_OBJ_NOTNULL(thr, VMOD_PEER_THREAD_MAGIC);
  AN(thr->alive);
  thr->alive--;
  VSL(SLT_Debug, 0, "vmod_peer: thread %p unregistered",(void*)thr->tid);
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
  vp_mutex_lock(&vp->mtx);
  do {
    struct peer_req *r;
    assert(thr->vp == vp);
    CHECK_OBJ_NOTNULL(vp, VMOD_PEER_MAGIC);
    vp->nwaiters++;
    while(!*thr->shutdown && vp->qlen == 0) {
      vp_mutex_assertheld(&vp->mtx);
      vp_mutex_condwait(&vp->cond,&vp->mtx);
    }
    vp->nwaiters--;
    vp_mutex_assertheld(&vp->mtx);
    for(r = VTAILQ_FIRST(&vp->q); !*thr->shutdown && vp->qlen > 0; r = VTAILQ_FIRST(&vp->q)) {
      CHECK_OBJ_NOTNULL(r, VMOD_PEER_REQ_MAGIC);
      VTAILQ_REMOVE(&vp->q, r, list);
      assert(vp->qlen > 0);
      vp->qlen--;
      vp_mutex_unlock(&vp->mtx);
      vp_process_req(vp,thr,r);
      vp_mutex_lock(&vp->mtx);
      vp_req_free(r);
    }
  } while(!*thr->shutdown);

  vp_mutex_lock(&gmtx);
  vmod_peer_thread_unregister(thr);
  vp_mutex_unlock(&gmtx);
  vp_mutex_unlock(&vp->mtx);
  return NULL;
}

static
int vp_start_thread(struct vmod_peer *ctx)
{
  struct vmod_pthr *p;
  static struct lock mtx = {0};
  static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


  if(!mtx.priv) {
    vp_mutex_lock(&gmtx);
    if(!mtx.priv) {
      vp_mutex_new_ex(&mtx,lck_herder,"vmod_peer_thr");
      vp_mutex_assert(&mtx,0);
    }
    vp_mutex_unlock(&gmtx);
  }
  vp_mutex_lock(&mtx);
  p = vp_alloc_thread(ctx);
  CHECK_OBJ_NOTNULL(p, VMOD_PEER_THREAD_MAGIC);
  p->imtx = &mtx;
  p->icond = &cond;
  p->shutdown = &ctx->shutdown;
  AZ(pthread_create(&p->tid,NULL,vmod_peer_thread,p));
  while(p->alive == 0) {
    vp_mutex_assertheld(&mtx);
    vp_mutex_condwait(&cond,&mtx);
  }
  vp_mutex_assertheld(&mtx);
  AZ(p->imtx);
  AZ(p->icond);
  vp_mutex_unlock(&mtx);
  return 0;
}

static
int vp_start_thread_safe(struct vmod_peer *ctx)
{
  int locked = 0;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  if(!vp_mutex_isheld(&ctx->mtx)) {
    vp_mutex_lock(&ctx->mtx);
    locked++;
  }
  vp_start_thread(ctx);
  if(locked)
    vp_mutex_unlock(&ctx->mtx);
  return 0;
}

static
void vmod_peer_stop(void *v)
{
  struct peer_req *r, *r2;
  struct vmod_peer *ctx = (struct vmod_peer*)v;
  int i,nalive;

  vp_mutex_lock(&gmtx);
  if(!initialized) {
    vp_mutex_unlock(&gmtx);
    return;
  }
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  if(!vp_mutex_isheld(&ctx->mtx))
    vp_mutex_lock(&ctx->mtx);
  ctx->shutdown++;
  for(i = 0; i < ctx->nthreads; i++)
    ctx->threads[i].shutdown = &ctx->shutdown;

  for(nalive = VP_THREADS_ALIVE(ctx); nalive > 0; nalive = VP_THREADS_ALIVE(ctx),
                                                  ctx->shutdown++) {
    for(i = 0; i < ctx->nthreads; i++) {
      pthread_t tid;
      if((tid = ctx->threads[i].tid) != (pthread_t)0) {
        CHECK_OBJ_NOTNULL(&ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
        if(ctx->nwaiters > 1)
          AZ(pthread_cond_broadcast(&ctx->cond));
        else
          AZ(pthread_cond_signal(&ctx->cond));
        vp_mutex_unlock(&ctx->mtx);
        vp_mutex_unlock(&gmtx);
        pthread_join(tid,NULL);
        vp_mutex_lock(&gmtx);
        vp_mutex_lock(&ctx->mtx);
        assert(pthread_equal(tid,ctx->threads[i].tid));
        assert(!ctx->threads[i].alive);
        tid = ctx->threads[i].tid = (pthread_t)0;
      }
    }
    ctx->shutdown++;
  }

  VTAILQ_FOREACH_SAFE(r, &ctx->q, list, r2) {
    VTAILQ_REMOVE(&ctx->q,r,list);
    vp_req_free(r);
  }
  ctx->qlen = 0;
  if(ctx->csh) {
    curl_share_cleanup(ctx->csh);
    ctx->csh = NULL;
  }

  vp_mutex_unlock(&ctx->mtx);
  AZ(pthread_cond_destroy(&ctx->cond));
  vp_mutex_delete(&ctx->mtx);
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
  vp_mutex_unlock(&gmtx);
  vp_mutex_deinit();
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

  vp_mutex_init();
  if(!initialized) {
    AZ(gmtx.priv);
    VP_NEW_GLOBAL_LOCK(&gmtx);
    AZ(vp_mutex_set_recursion(&gmtx,1,NULL));
  } else
    AN(gmtx.priv);

  vp_mutex_assert(&gmtx,0);
  vp_mutex_lock(&gmtx);

  ctx = calloc(1,sizeof(*ctx));
  AN(ctx);
  AZ(priv->priv);
  AZ(priv->free);
  ctx->magic = VMOD_PEER_MAGIC;
  ctx->nthreads = 0;
  ctx->nwaiters = 0;
  ctx->min = ctx->max = 1;
  VP_NEW_LOCK(&ctx->mtx);
  AZ(pthread_cond_init(&ctx->cond,NULL));
  vp_mutex_assert(&ctx->mtx,0);
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
  vp_mutex_unlock(&gmtx);
  return rv;
}

void vmod_set(struct sess *sp, struct vmod_priv *priv, const char *host, int port)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  vp_mutex_lock(&ctx->mtx);

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

  vp_mutex_unlock(&ctx->mtx);
}

static void rebalance_threading(struct sess *sp, struct vmod_peer *ctx)
{
  int i,rv;
  int locked = 0;
  unsigned target,nalive;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);
  if(!vp_mutex_isheld(&ctx->mtx)) {
    vp_mutex_lock(&ctx->mtx);
    locked = 1;
  }
  if(ctx->shutdown || !ctx->tsz) {
    if(locked)
      vp_mutex_unlock(&ctx->mtx);
    return;
  }
  vp_mutex_lock(&gmtx);
  nalive = VP_THREADS_ALIVE(ctx);
  AN(max_q_depth);
  target = (ctx->qlen / max_q_depth)+1;
  if(ctx->max > 0 && target > ctx->max)
    target = ctx->max;
  if(ctx->min > 0 && target < ctx->min)
    target = ctx->min;

  for(i = ctx->nthreads-1; i > 0 && nalive != target && !ctx->shutdown;
                                                i = (i == 0 ? ctx->nthreads-1 : i-1),
                                                nalive = VP_THREADS_ALIVE(ctx)) {
    /* too many threads, get rid of some */
    pthread_t tid;
    int shutdown = 1;
    vmod_pthr_t *t;

    if(!ctx->threads[i].magic)
      continue;

    CAST_OBJ_NOTNULL(t, &ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
    tid = t->tid;

    if(tid == (pthread_t)0 && !t->alive) {
      if(i == ctx->nthreads-1) {
        t->magic = 0;
        ctx->nthreads--;
      }
      continue;
    }
    assert(tid != (pthread_t)0);

    if(nalive > target) {
      t->shutdown = &shutdown;

      if(ctx->nwaiters > 1)
        AZ(pthread_cond_broadcast(&ctx->cond));
      else
        AZ(pthread_cond_signal(&ctx->cond));
      vp_mutex_unlock(&gmtx);
      vp_mutex_unlock(&ctx->mtx);
      pthread_join(tid, NULL);
      vp_mutex_lock(&ctx->mtx);
      vp_mutex_lock(&gmtx);
      AN(ctx->threads);
      assert(i < ctx->nthreads && pthread_equal(ctx->threads[i].tid,tid));
      CAST_OBJ_NOTNULL(t, &ctx->threads[i], VMOD_PEER_THREAD_MAGIC);
      assert(!t->alive);
      t->tid = (pthread_t)0;
      t->shutdown = &ctx->shutdown;
      if(i+1 == ctx->nthreads)
        ctx->nthreads--;
    } else if(nalive < target) {
      rv = vp_start_thread(ctx);
      if(rv != 0) {
        if(errno != 0)
          VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: cannot start new thread: %s",
              strerror(errno));
        if(target > 0)
          target--;
      }
    }
  }

  if(locked)
    vp_mutex_unlock(&ctx->mtx);
  vp_mutex_unlock(&gmtx);
}

void vmod_set_threads(struct sess *sp, struct vmod_priv *priv, int min, int max)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  if(max > VMOD_PEER_MAX_THREAD_POOL)
    max = VMOD_PEER_MAX_THREAD_POOL;

  if(max > 0 && min > 0 && max < min)
    max = min;

  vp_mutex_lock(&ctx->mtx);
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

  vp_mutex_unlock(&ctx->mtx);
  rebalance_threading(sp,ctx);
}

int vmod_threads(struct sess *sp, struct vmod_priv *priv)
{
  int i,n = 0;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  for(i = 0; i < ctx->nthreads; i++) {
    n += ctx->threads[i].alive;
  }
  vp_mutex_unlock(&ctx->mtx);
  return n;
}

int vmod_min_threads(struct sess *sp, struct vmod_priv *priv)
{
  int n;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  n = ctx->min;
  vp_mutex_unlock(&ctx->mtx);
  return n;
}

int vmod_max_threads(struct sess *sp, struct vmod_priv *priv)
{
  int n;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  n = ctx->max;
  vp_mutex_unlock(&ctx->mtx);
  return n;
}

int vmod_pending(struct sess *sp, struct vmod_priv *priv)
{
  size_t qlen;
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return -1;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  qlen = ctx->qlen;
  vp_mutex_unlock(&ctx->mtx);

  return qlen;
}

static pthread_cond_t vp_gcond = PTHREAD_COND_INITIALIZER;
static pthread_t vp_gc = (pthread_t)0;
static unsigned vp_gw = 0;

void vmod_lock(struct sess *sp)
{
  pthread_t tid = pthread_self();
  vp_mutex_lock(&gmtx);
  vp_mutex_assertheld(&gmtx);

  if(vp_gc != (pthread_t)0 && !pthread_equal(vp_gc,tid)) {
    vp_gw++;
    while(vp_gc != (pthread_t)0 && !pthread_equal(vp_gc,tid)) {
      vp_mutex_assertheld(&gmtx);
      vp_mutex_condwait(&vp_gcond,&gmtx);
      vp_mutex_assertheld(&gmtx);
    }
    vp_mutex_assertheld(&gmtx);
    vp_gw--;

    if(vp_gc == (pthread_t)0)
      vp_gc = tid;
  }

  vp_mutex_unlock(&gmtx);
  vp_mutex_assert(&gmtx,0);
}

void vmod_unlock(struct sess *sp)
{
  pthread_t tid = pthread_self();
  vp_mutex_assert(&gmtx,0);
  vp_mutex_lock(&gmtx);
  if(vp_gc == (pthread_t)0) {
    VSL(SLT_Error,(sp ? sp->id : 0),"attempt to unlock ignored, lock %p is not held",gmtx.priv);
    vp_mutex_unlock(&gmtx);
    return;
  } else if(!pthread_equal(vp_gc,tid)) {
    VSL(SLT_Error,(sp ? sp->id : 0),"attempt to unlock %p when held by other thread",gmtx.priv);
    vp_mutex_unlock(&gmtx);
    return;
  }
  vp_mutex_assertheld(&gmtx);

  if(pthread_equal(vp_gc,tid)) {
    vp_mutex_assertheld(&gmtx);
    if(vp_gw > 1)
      pthread_cond_broadcast(&vp_gcond);
    else
      pthread_cond_signal(&vp_gcond);
    vp_gc = (pthread_t)0;
  }
  vp_mutex_assertheld(&gmtx);
  vp_mutex_unlock(&gmtx);
  vp_mutex_assert(&gmtx,0);
}

/* set global maximum number of requests allowed to be queued before a new thread is
 * started (up to max)
 */
void vmod_set_thread_maxq(struct sess *sp, struct vmod_priv *priv, int max)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;
  unsigned int old_max;
  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  if(max < 2) {
    VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: per_thread_maxq must be larger than 1");
    return;
  }
  vp_mutex_lock(&gmtx);
  old_max = max_q_depth;
  max_q_depth = max;
  if(old_max != max_q_depth) {
    vp_mutex_unlock(&gmtx);
    rebalance_threading(sp,ctx);
  } else {
    vp_mutex_unlock(&gmtx);
  }
}

/* set connect timeout in ms */
void vmod_set_connect_timeout(struct sess *sp, struct vmod_priv *priv, int ms)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  if(sp == NULL)
    return;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  ctx->c_to = (ms ? ms : -1);
  vp_mutex_unlock(&ctx->mtx);
}

/* set connect timeout in ms */
void vmod_set_timeout(struct sess *sp, struct vmod_priv *priv, int ms)
{
  struct vmod_peer *ctx = (struct vmod_peer*)priv->priv;

  CHECK_OBJ_NOTNULL(ctx, VMOD_PEER_MAGIC);

  vp_mutex_lock(&ctx->mtx);
  ctx->d_to = (ms ? ms : -1);
  vp_mutex_unlock(&ctx->mtx);
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
        VSL(SLT_Error, (sp ? sp->id : 0), "vmod_peer: peer req %p (%s) vp_enqueue: %s",
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

void vmod_queue_req_body(struct sess *sp, struct vmod_priv *priv,
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

