=========
vmod_peer
=========

--------------------------------
Varnish Peer Cache Helper Module
--------------------------------

SYNOPSIS
========

import peer;

DESCRIPTION
===========

Varnish module that utilizes libcurl calls to assist in multiplexing HTTP
operations to other Varnish neighbors or even remote Content Distribution
Networks. Such HTTP calls are performed asyncronously via a separate thread
pool.

FUNCTIONS
=========

set
---

Prototype
        ::

              set(STRING, INT)
Return value
        VOID
Description
        Configures the IP address and port number of a cache peer. Hostnames
        can technically be used, but doing so unless your libcurl has
        explicitly been compiled to use a thread-safe dns resolver library is
        *not recommended*.

        If the port number is 0 the standard HTTP port (80) will be used.

        This function should probably be called just once from within `vcl_init`.

        *Note: This function does* **not** *set the HTTP "Host" header.* The
        ``Host`` header is set to whatever the current ``req.http`` headers
        contain at the time `peer.queue_req()` or `peer.queue_req_body()` is
        called. If you need to set ``Host`` change ``req.http`` immediately
        before queueing a request and then change it back immediately after.
Example
        ::

            peer.set("10.9.1.111",8888);

set_threads
-----------

Prototype
        ::

            set_threads(INT, INT)
Return value
        VOID
Description
        Configures the minimum and maximum number of worker threads which will
        be started in order to perform queued HTTP operations in the
        background. By default both minimum and maximum are set to 1, which is
        also the minimum number configureable. Setting min or max to 0 results
        in no change to the actual min/max setting but can be used to change
        one without the other. Attempts to set conflicting or non-sense values
        will be silently ignored.

        When the maximum number of threads is greater than the minimum, the
        number of running threads will be scaled based on workload.

        Setting min to a value greater than the current number of running
        worker threads will result in new threads spawning immediately while
        conversely setting max *below* the current number will result in excess
        threads terminating immediately.
Example
        ::

            // Set the new maximum threads to 4 without changing the mininum
            peer.set_threads(0,4);

set_timeout
-----------

Prototype
        ::

            set_timeout(INT)
Return value
        VOID
Description
        Set the global (per-operation) HTTP timeout (*starting after connect*)
        in milliseconds. Operations which exceed this time will fail in the
        background and add an **Error** entry in the Varnish shared memory log.
Example
        ::

            // Set post-connect timeout to .5 seconds
            peer.set_timeout(500);

set_connect_timeout
-------------------

Prototype
        ::

            set_connect_timeout(INT)
Return value
        VOID
Description
        Set the global (per-operation) HTTP connection timeout in milliseconds.
        Operations which exceed this time before HTTP has started will fail in
        the background and add an **Error** entry in the Varnish shared memory
        log.
Example
        ::

            // Set connect timeout to .1 seconds
            peer.set_connect_timeout(100);

queue_req
---------

Prototype
        ::

            queue_req()

Return value
        VOID
Description
        Queue a new asyncronous HTTP request for processing by a background
        thread.  The request will be based on the state of the Varnish ``req``
        object at the time this function is called. All headers in ``req.http``
        will be copied to this request except for the `Connection` header.
        *vmod_peer* does not use http keepalives and thus `Connection: close`
        is **always** used. In addition to headers, ``req.url``,
        ``req.request`` and ``req.proto`` are also used when preparing the new
        request. Once this function has been called, ``req`` can be altered,
        delivered or discarded at will without changing the background request
        in progress.
Caveats
        If `peer.set("ip",port)` has not been called at least once before this
        function, the ``req.http.host`` header will be used to establish the
        background HTTP connection. *This is almost assuredly what you do not
        want!* However, calling `peer.set` just once in `vcl_init` is
        sufficient for all future requests.
Example
        ::

            // Turn GET requests into PURGEs for a neighboring cache
            if (req.request == "GET") {
                set req.request = "PURGE";
                peer.queue_req();
                set req.request = "GET";
            }

queue_req_body
--------------

Prototype
        ::

            queue_req_body(STRING_LIST)
Return value
        VOID
Description
        Identical to `peer.queue_rec()` except that it takes a single
        argument which will be used as HTTP ``POST`` content.  The content
        will be automatically URL-encoding before being sent. Note that using
        this function will cause the HTTP operation to *always* operate as a
        ``POST`` irrespective of value of `req.request` (although
        `req.request` will be the method actually sent in the HTTP command).
Example
        ::

            // Send an HTTP POST.
            set req.request = "POST';
            peer.queue_req_body({"Form entry
            sent "} + now);

threads
-------

Prototype
        ::

            threads(VOID)
Return value
        INT
Description
        Returns the number of threads currently running in the thread pool
        dedicated to handling `vmod_peer` HTTP requests. No distinction is
        made between busy threads and those waiting for new requests but
        this can generally be estimated by examining this value and the
        ``peer.pending()`` value.

pending
-------

Prototype
        ::

            pending(VOID)
Return value
        INT
Description
        Returns the number of outstanding HTTP requests that have not
        yet been processed. Requests are considered pending up until
        they are initiated, **not** when completed.

min_threads
-----------

Prototype
        ::

            min_threads(VOID)
Return value
        INT
Description
        Returns the minimum number of threads maintained by the HTTP request
        handling pool. This number should be the same as the first argument in
        the most recent call to ``peer.set_threads(min,max);``.

        The default value is 1.

max_threads
-----------

Prototype
        ::

          max_threads(VOID)
Return value
        INT
Description
        Returns the maximum number of threads maintained by the HTTP
        request handling pool. This number should be the same as the first
        argument in the most recent call to ``peer.set_threads(min,max);``

        The default value is 1.

        If the maximum has been set to a value greater than the minimum,
        the number of actively running threads will be adjusted dynamically
        based on the pending queue size.

