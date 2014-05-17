libvmod-peer
============
A varnish module (aka a _vmod_) for asyncronously notifying other caching
servers or http services from within vcl -- loosely based on
[libvmod-curl](https://github.com/varnish/libvmod-curl).

Requires libcurl and varnish 3.0.

Description
-----------
In sophisticated http architectures it is sometimes necessary to send HTTP
requests to other non-primary HTTP caches or web services from a varnish
caching layer. While this is perfectly doable with
[libvmod-curl](https://github.com/varnish/libvmod-curl), this
means that varnish worker threads must wait for libcurl operation to complete.
In cases where it's not necessary to know the result of the HTTP client
operation **and** when the server being sent the query is on some third-party
network, this can become quite problematic (think: _passing an HTTP request to
purge content from a public CDN_).

*libvmod-peer* starts a background thread when the vmod is first loaded. Calls
to ``peer.queue_req()`` will create a new background operation, schedule it
and wake up the background thread. While there are pending operations, the
background thread will use libcurl to perform each one.  ``peer.pending()``
can be used to determine how many operations are current queued and waiting
for execution.

``peer.queue_req()`` will use the exact same headers and HTTP method as
the varnish `req` object. To alter these simply alter `req` before calling
``peer.queue_rec()``. This call makes a sort of "snapshot" of the `req`
object and thus any changes made after the call will not be seen by
*libvmod-peer* and thus not part of the outbound request.

Usage
-----

    import peer;

    sub vcl_init {
      peer.set("some host",80);
    }

    sub vcl_recv {
      if (req.url == "/status") {
        error 200 "Status";
      }
    }

    sub vcl_error {
      if (obj.status == 200 && req.url == "/status") {
        synthetic {" Pending peer requests: "} + peer.pending() + {"
        "};
        return (deliver);
      }
    }

    sub vcl_hit {
      if (req.request == "PURGE") {
        purge;
        peer.queue_req();
        error 200, "Purged.";
      }
    }

    sub vcl_miss {
      if (req.request == "PURGE") {
        purge;
        peer.queue_req();
        error 404, "Not found";
      }
    }
