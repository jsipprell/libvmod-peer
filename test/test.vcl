import std;
import peer;

backend rpxy005t {
  .host = "152.52.29.46";
  .port = "80";
  .connect_timeout = 3s;
  .first_byte_timeout = 3s;
}

sub vcl_recv {
  if (req.restarts == 0) {
    set req.backend = rpxy005t;
    set req.http.host = "qa1.kansascity.com";
    unset req.http.cookie;
  }
}

sub vcl_hit {
  set req.http.connection = "close";
  set req.http.proxy-connection = "close";
}

sub vcl_miss {
  if (bereq.request == "HEAD") {
    set bereq.request = "GET";
  }
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
