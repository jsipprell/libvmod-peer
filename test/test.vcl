import std;
import peer;

probe healthcheck {
  .url = "/server-status";
  .interval = 6s;
  .timeout = 2s;
  .window = 8;
  .threshold = 5;
  .initial = 2;
  .expected_response = 403;
}

backend rpxy005t {
  .host = "152.52.29.46";
  .port = "80";
  .connect_timeout = 3s;
  .first_byte_timeout = 3s;
  .probe = healthcheck;
}

backend rpxy012t {
  .host = "152.52.29.39";
  .port = "80";
  .connect_timeout = 2s;
  .first_byte_timeout = 2s;
  .probe = healthcheck;
}

backend kcstar {
  .host = "166.108.31.147";
  .port = "80";
  .connect_timeout = 3s;
  .first_byte_timeout = 6s;
  .probe = healthcheck;
}

director x round-robin {
  {
    .backend = rpxy005t;
  }
  {
    .backend = rpxy012t;
  }
  {
    .backend = kcstar;
  }
}

sub vcl_init {
  peer.set("152.52.29.46",99);
  peer.set_connect_timeout(11000);
  peer.set_timeout(30000);
  peer.set_threads(1, 2);
}

sub vcl_recv {
  if (req.restarts == 0) {
    set req.backend = x;
    set req.http.host = "eprod.kansascity.com";
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
  set req.http.Host = "www.kansascitystar.com";
  peer.queue_req();
}

sub vcl_miss {
  set req.request = bereq.request;
  if (bereq.request == "HEAD") {
    set bereq.request = "GET";
  }
  set req.http.Host = "www.kansascity.com";
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
