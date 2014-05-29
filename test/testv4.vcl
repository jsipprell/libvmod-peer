vcl 4.0;
import peer;
import std;

backend vnet085 {
  .host = "166.108.31.99";
  .port = "80";
  .connect_timeout = 5s;
  .first_byte_timeout = 2m;
  .between_bytes_timeout = 30s;
}

sub vcl_init {
  new neighbor = peer.ip("166.108.31.147",0);
  new x = peer.ip("127.0.0.1",0);
  neighbor.set_connect_timeout(1000);
  neighbor.set_timeout(3000);
  #neighbor.set_ssl_verification(force,true);
  #neighbor.set_ssl_verification(peer,true);
  return (ok);
}

sub vcl_recv {
  if (req.restarts == 0) {
    set req.backend_hint = vnet085;
  }

  if(req.method == "POST") {
    peer.cache_post();
    std.log("Debug:cache_post");
  }
}

sub vcl_miss {
  if (req.method == "POST") {
    std.log("Debug:MISS/"+x.body());
  }
}

sub vcl_hit {
  set req.method = "PURGE";
  neighbor.enqueue();
  if (req.method == "POST") {
    std.log("Debug:HIT/"+x.body());
  }
}

sub vcl_backend_fetch {
  unset bereq.http.X-Forwarded-For;
  if (bereq.method != "POST") {
    set bereq.http.Ignore = bereq.http.User-Agent;
    set bereq.http.User-Agent = "Cheesemonster/3.0";
    set bereq.http.Orig-Method = bereq.method;
    set bereq.method = "HEAD";
    neighbor.enqueue();
    set bereq.method = bereq.http.Orig-Method;
    unset bereq.http.Orig-Method;
    set bereq.http.User-Agent = bereq.http.Ignore;
    unset bereq.http.Ignore;
  } else {
    std.log("Debug:FETCH/"+x.body());
  }
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
