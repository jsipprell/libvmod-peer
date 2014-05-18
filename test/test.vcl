import std;
import peer;

backend rpxy005t {
  .host = "152.52.29.46";
  .port = "80";
  .connect_timeout = 3s;
  .first_byte_timeout = 3s;
}

backend rpxy012t {
  .host = "152.52.29.39";
  .port = "80";
  .connect_timeout = 2s;
  .first_byte_timeout = 2s;
}

director x round-robin {
  {
    .backend = rpxy005t;
  }
  {
    .backend = rpxy012t;
  }
}

sub vcl_init {
  peer.set("152.52.29.39",8888);
  peer.set_connect_timeout(100);
  peer.set_timeout(200);
  peer.set_threads(2, 0);
}

sub vcl_recv {
  if (req.restarts == 0) {
    set req.backend = x;
    set req.http.host = "qa1.kansascity.com";
    unset req.http.cookie;
  }

  if (req.url == "/status") {
    set req.http.Peer-Pending-Requests = peer.pending();
    error 200 "Status";
  }
}

sub vcl_error {
  if (obj.status == 200 && req.url == "/status") {
    set obj.http.Content-Type = "text/plain";
    synthetic {"
Pending peer requests: "} + req.http.Peer-Pending-Requests + {"
          Min Threads: "} + peer.min_threads() + {"
          Max Threads: "} + peer.max_threads() + {"
        Alive Threads: "} + peer.threads() + {"
"};
    return (deliver);
  }
}

sub vcl_hit {
  set req.http.connection = "close";
  set req.http.proxy-connection = "close";
  set req.http.Orig-Request = req.request;
  set req.request = "PURGE";
  peer.queue_req();
  set req.request = req.http.Orig-Request;
  unset req.http.Orig-Request;
  peer.queue_req();
}

sub vcl_miss {
  set req.request = "PURGE";
  peer.queue_req();
  set req.request = bereq.request;
  if (bereq.request == "HEAD") {
    set bereq.request = "GET";
  }
  peer.queue_req();
}

sub vcl_pass {
  set req.request = "PURGE";
  peer.queue_req();
  set req.request = bereq.request;
}

sub vcl_fetch {
  if (beresp.http.connection) {
    set beresp.http.Origin-Connection = beresp.http.connection;
  }
  if (beresp.http.proxy-connection) {
    set beresp.http.Origin-Proxy-Connection = beresp.http.proxy-connection;
  }
  unset beresp.http.set-cookie;
}

sub vcl_deliver {
  if (resp.http.connection !~ "close") {
    set resp.http.Connection = "close";
  }
  if (resp.http.proxy-connection !~ "close") {
    set resp.http.Proxy-Connection = "close";
  }
  if (req.request == "HEAD") {
    set resp.http.Content-Length = "0";
  }
}
