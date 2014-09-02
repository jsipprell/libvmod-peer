=========
vmod_peer
=========

---------------------------
Varnish Peer Caching Module
---------------------------

:Manual section: 3

SYNOPSIS
========

``import peer [from "path"];``

DESCRIPTION
===========

vmod_peer is a shared library module for the varnish application acceleration HTTP cache.

It provides a mechanism for VCL (the Varnish configuration language) code to
queue up independent asyncronous HTTP requests to pre-configured peers at
specific IP:PORT combinations. This is useful for scalability purposes as it
permits the distribution of cache invalidation requests to other Varnish caches
simlutanouesly. vmod_peer's HTTP client functionality is provided by libcurl in
combination with a request workqueue and thread pool that scales based on load
demand.

*All requests are handled asyncronously by background threads, vmod_peer
provides no mechansim for blocking requests or syncronization based on waiting
until a perivously issued call has completed.*

FUNCTIONS
=========

set
---
Protatype
    VOID set(STRING, INT)
Description
    Globally sets the IP address and port number that future calls to enqueue_req()
    with communicate with.
Example
    ``peer.set("10.11.12.13",80);``

set_threads
--------------------------
Prototype
  VOID set_threads(INT, INT)
Description
  Configure the minimum and maximum number of threads allowed to be in the vmod_peer
  thread pool. Default value for min (first argument is 1), default for second arg
  is 2.
Example
  ``peer.set_threads(1,2);``

min_threads
-----------
Prototype
  INT min_threads(VOID)
Description
  Returns the minimum number of threads that should be kept running at all times.
  *Default value is 1*.
Example
  ``"min: " + peer.min_threads();``

max_threads
-----------
Prototype
  INT max_threads(VOID)
Description
  Returns the maximum number of threads that may be running. The number of running
  threads can scale up to this maximum depending on load but **never scales down
  automatically**. *Default value is the same is min_threads which disables 
  load based scaling.*
Example
  ``"max: " + peer.max_threads();``

threads
-------
Prototype
  INT threads(VOID)
Description
  Returns the current number of running threads. Running threads may be either
  waiting on requests or currentingly processing a request.
Example
  ``"alive: " + peer.threads();``

pending
-------
Prototype
  INT pending(VOID)
Description
  Returns the current number of requests waiting in the work queue for a thread
  to handle them. Requests only set in the pending queue if all threads are
  already busy handling other requests. Once the number of pending requests
  exceed the value set by ``set_thread_maxq`` a new thread will be spawned if
  possible. If the maximum number of threads is already running the request
  will be discarded.
Example
  ``"pending requests: " + peer.pending();``

set_connect_timeout
-------------------
Prototype
  VOID set_connect_timeout(INT)
Description
  Set the maximum time in milliseconds that any HTTP connection to a peer cache
  is allowed to take.
Example
  ``peer.set_connect_timeout(1200);``

set_timeout
-----------
Prototype
  VOID set_timeout(INT)
Description
  Set the maximum time in milliseconds that any HTTP requests to a peer cache
  is allowed to take. **This includes the connection time so this value must
  always be larger than the connect timeout.**
Example
  ``peer.set_tiemout(2000);``

set_thread_maxq
---------------
Prototype
  VOID set_thread_maxq(INT)
Description
  Set the maximum number of requests that will be allowed to queue up before
  a new thread is started. *The default value is 50 whih may be too low*.
  This value must **always** be at least *2*.
Example
  ``peer.set_thread_maxq(20);``

lock
----
Prototype
  VOID lock(VOID)
Description
  Acquire a cooperative global lock against the vmod_peer module so that any other
  varnish threads that call ``lock`` will block until the current thread calls
  ``unlock``. It is safe to call ``lock`` recursively from the same thread but
  additional calls perform no operations.
  **Use this with extreme caution, as it's only needed in very special cases.**
Example
  ``peer.lock();``

unlock
------
Prototype
  VOID unlock(VOID)
Description
  Unlock a previously acquireed cooperative global lock and wake up on varnish
  thread that is waiting on the lock (if there are any). If ``unlock`` is called
  without ``lock`` first being called by the same thread nothing happens.
  **Use this with extreme caution, as it's only needed in very special cases.**
Example
  ``peer.unlock();``

enqueue_req
-----------
Prototype
  VOID enqueue_req(VOID)
Description
  Schedules a new background asyncronous HTTP request to the configured cache peer
  using the headers found in the `req` objects. The request will be a "snapshot" of
  the `req` object so that future changes will affect new requests but not the
  one created by this call becomes immutable.
Example
  ``peer.enqueue_req();``


EXAMPLE VCL
===========

.. _example_vcl:

::

    sub vcl_init {
      peer.set("10.1.2.3",80);
      peer.set_timeout(2000);
      peer.set_connect_timeout(1000);
      peer.set_threads(1,2);
      peer.set_thread_maxq(10);
    }

    sub vcl_recl {
      if(req.retarts == 0) {
        unset req.http.Purge-Info;
        unset req.http.Original-Purge-Method;
        if(req.url ~ "^/purge-status/?$") {
          set req.http.Pending-Purges = peer.pending();
          error 200 "Purge Status";
        } else {
          unset req.http.Pending-Purges;
        }
      }
    }

    sub vcl_error {
      if(obj.status == 200 && req.http.Pending-Purges) {
        set obj.http.content-type = "text/plain;charset=uft-8";
        set obj.http.generated-by = "Varnish vmod_peer";
        synthetic {"
    Queued Purges: "} + req.http.Pending-Purges + {"
      Min Threads: "} + peer.min_threads() + {"
      Max Threads: "} + peer.max_threads() + {"
      Cur Threads: }" + peer.threads() + {"
    "};

     return deliver;
    }

    sub send_peer_purge {
      set req.http.Original-Purge-Method = req.request;
      set req.request = "VPURGE";
      unset req.http.Purge-Info;
      peer.enqueue_req();
      set req.http.Purge-Info = "Purge " + req.request + " queued";
      set req.request = req.http.Original-Purge-Method;
      unset req.http.Original-Purge-Method;
    }

    sub vcl_hit {
      if(req.request ~ "^V?PURGE$") {
        if (req.request == "PURGE") {
          call send_peer_purge;
        }
        purge;
        error 200 "purged";
      }
    }

    sub vcl_miss {
      if(req.request ~ "^V?PURGE$") {
        if (req.request == "PURGE") {
          call send_peer_purge;
        }
        purge;
        error 404 "not found";
      }
    }

    sub vcl_deliver {
      if(req.http.Purge-Info) {
        set resp.http.Purge-Info = req.http.Purge-Info;
      }
    }

