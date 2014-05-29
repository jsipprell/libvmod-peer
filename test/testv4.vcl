vcl 4.0;
import peer;

backend vnet085 {
  .host = "166.108.31.99";
  .port = "80";
  .connect_timeout = 5s;
  .first_byte_timeout = 2m;
  .between_bytes_timeout = 30s;
}

sub vcl_init {
  new neighbor = peer.ip("127.0.0.1",9999);
  neighbor.set_connect_timeout(100);
  neighbor.set_timeout(1000);
  return (ok);
}

sub vcl_recv {
  if (req.restarts == 0) {
    set req.backend_hint = vnet085;
  }
}

sub vcl_hit {
  set req.method = "PURGE";
  neighbor.enqueue();
}

sub vcl_backend_fetch {
  unset bereq.http.X-Forwarded-For;
}

sub vcl_backend_response {
  unset beresp.http.Set-Cookie;
  unset beresp.http.Cache-Control;
  if (!beresp.http.Vary) {
    if (beresp.status == 200) {
      set beresp.ttl = 20s;
    } else if (beresp.status > 300 && beresp.status < 400) {
      set beresp.ttl = 60s;
    } else {
      set beresp.ttl = 0s;
      set beresp.uncacheable = true;
    }
    return (deliver);
  }
}
