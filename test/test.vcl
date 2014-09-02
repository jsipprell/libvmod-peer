import std;
import peer;

probe healthcheck {
  .url = "/varnish-health-probe.txt";
  .interval = 6s;
  .timeout = 2s;
  .window = 8;
  .threshold = 5;
  .initial = 5;
  .expected_response = 200;
}

backend rweb029t {
  .host = "10.1.76.38";
  .port = "81";
  .probe = healthcheck;
  .saintmode_threshold = 0;
}

backend rweb030t {
  .host = "10.1.76.52";
  .port = "81";
  .probe = healthcheck;
  .saintmode_threshold = 0;
}

director x random {
  {
    .backend = rweb029t;
    .weight = 1;
  }
  {
    .backend = rweb030t;
    .weight = 1;
  }
}

sub vcl_init {
  peer.set("10.4.76.39",8888);
  #peer.set("10.1.76.39",8888);
  peer.set_connect_timeout(11000);
  peer.set_timeout(30000);
  peer.set_threads(1, 4);
  peer.set_thread_maxq(5);
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
  set req.http.Host = "qa1.kansascity.com";
  peer.queue_req();
}

sub vcl_miss {
  set req.request = bereq.request;
  if (bereq.request == "HEAD") {
    set bereq.request = "GET";
  }
  set req.http.Host = "qa1.kansas.com";
  peer.queue_req();
  set req.http.Host = bereq.http.Host;
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
  if(beresp.status == 200) {
    set beresp.ttl = 20s;
  }
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
