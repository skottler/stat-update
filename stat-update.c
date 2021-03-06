#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#include <ev.h>
#include "ebb.h"

#include "hiredis.h"
#include "async.h"
#include "adapters/libev.h"

static const char OUT1[] = "HTTP/1.0 302 Moved Temporarily\r\nServer: rubygems stat-update/1.0\r\nContent-Length: 0\r\nLocation: ";
static const char OUT2[] = ".gem\r\n\r\n";

static const char TRAGIC[] = "HTTP/1.0 404 Not Found\r\nServer: rubygems stat-update/1.0\r\nContent-Length: 13\r\n\r\nBad request\r\n";

struct str {
  size_t capa;
  size_t len;
  char* ptr;
};

typedef struct str str;

void str_assoc(str* s, char* ptr, size_t capa) {
  s->ptr = ptr;
  s->capa = capa - 1; // reserve space for a null byte
  s->len = 0;
}

int str_append(str* s, const char* ptr, size_t len) {
  if(s->len + len > s->capa) return 0;
  memcpy(s->ptr + s->len, ptr, len);

  s->len += len;
  s->ptr[s->len] = 0;

  return 1;
}

int str_append_str(str* d, str* s) {
  return str_append(d, s->ptr, s->len);
}

#define CONN_FULL_NAME_SIZE 256
#define OUTPUT_BUFF 1024
#define AGENT_BUFF 256

struct conn {
  char full_name_data[CONN_FULL_NAME_SIZE];
  str full_name;

  int agent_idx;

  char agent_data[AGENT_BUFF];
  str agent;

  char output_data[OUTPUT_BUFF];
  str output;
};

typedef struct conn conn;

void conn_init(conn* c) {
  str_assoc(&c->full_name, c->full_name_data, CONN_FULL_NAME_SIZE);
  c->agent_idx = -1;
  str_assoc(&c->agent, c->agent_data, AGENT_BUFF);
  str_assoc(&c->output, c->output_data, OUTPUT_BUFF);
}

struct serv {
  char* redis_host;
  char* http_prefix;
  int http_prefix_len;

  struct ev_loop* loop;
  int redis_connected;
  redisAsyncContext* redis;
  ev_timer reconnect_timer;
};

#define TODAY_SIZE 16

struct redis_conn {
  char* full_name;
  char today[TODAY_SIZE];
};

void on_close(ebb_connection *connection) {
  free(connection->data);
  free(connection);
}

static void continue_responding(ebb_connection *connection) {
  int r;
  conn *data = connection->data;
  //printf("response complete \n");
  ebb_connection_schedule_close(connection);
}

/*
        commands = [
          %W!incr downloads!,
          %W!incr downloads:rubygem:#{name}!,
          %W!incr downloads:version:#{full_name}!,

          %W!zincrby downloads:today:#{today} 1 #{full_name}!,
          %W!zincrby downloads:all 1 #{full_name}!,

          %W!hincrby downloads:version_history:#{full_name} #{today} 1!,
          %W!hincrby downloads:rubygem_history:#{name} #{today} 1!
        ]
 */ 

static void get_full_name(redisAsyncContext *c, void *r, void *privdata) {
  struct redis_conn* rc = privdata;

  redisReply *reply = r;
  if(!reply) goto cleanup;

  if(reply->type == REDIS_REPLY_ERROR) {
    printf("Error getting name: %s\n", reply->str);
    goto cleanup;
  } else if(reply->type != REDIS_REPLY_STRING) {
    printf("Key has wrong type (not string)\n");
    goto cleanup;
  }

  char* name = reply->str;

  if(!name) goto cleanup;
  char* full_name = rc->full_name;

  redisAsyncCommand(c, 0, 0, "INCR downloads");
  redisAsyncCommand(c, 0, 0, "INCR downloads:rubygem:%s", name);
  redisAsyncCommand(c, 0, 0, "INCR downloads:version:%s", full_name);

  redisAsyncCommand(c, 0, 0, "ZINCRBY downloads:today:%s 1 %s",
                    rc->today, full_name);
  redisAsyncCommand(c, 0, 0, "ZINCRBY downloads:all 1 %s",full_name);

  redisAsyncCommand(c, 0, 0, "HINCRBY downloads:version_history:%s %s 1",
                    full_name, rc->today);
  redisAsyncCommand(c, 0, 0, "HINCRBY downloads:rubygem_history:%s %s 1",
                    name, rc->today);

cleanup:
  free(rc->full_name);
  free(privdata);
}

static void request_complete(ebb_request *request) {
  //printf("request complete \n");
  ebb_connection *connection = request->data;
  conn *data = connection->data;
  struct serv* s = connection->server->data;

  if(data->full_name.len == 0) {
tragic:
    ebb_connection_write(connection, TRAGIC,
                         sizeof(TRAGIC), continue_responding);
    free(request);
    return;
  }

  str* out = &data->output;

  if(!str_append(out, OUT1, sizeof(OUT1) - 1)) goto tragic;
  if(!str_append(out, s->http_prefix, s->http_prefix_len)) goto tragic;
  if(!str_append_str(out, &data->full_name)) goto tragic;
  if(!str_append(out, OUT2, sizeof(OUT2) - 1)) goto tragic;

  ebb_connection_write(connection, out->ptr, out->len, continue_responding);

  struct redis_conn* rc = malloc(sizeof(struct redis_conn));

  time_t now;
  struct tm now_tm;

  time(&now);
  gmtime_r(&now, &now_tm);
  strftime(rc->today, sizeof(rc->today), "%Y-%m-%d", &now_tm);
  rc->full_name = strdup(data->full_name.ptr);

  if(s->redis_connected) {
    // "RubyGems/1.8.17 x86-linux Ruby/1.8.7 (2010-12-23 patchlevel 330)"
    if(data->agent.len > 20) {
      char* rg_ver = data->agent.ptr + 9;
      char* sp = strchr(rg_ver, ' ');
      if(!sp) goto skip;
      *sp = 0;

      redisAsyncCommand(s->redis, 0, 0, "HINCRBY usage:rubygem_version:%s %s 1",
                        rc->today, rg_ver);

      char* rg_plat = sp + 1;
      sp = strchr(rg_plat, ' ');
      if(!sp) goto skip;
      *sp = 0;

      redisAsyncCommand(s->redis, 0, 0, "HINCRBY usage:ruby_platform:%s %s 1",
                        rc->today, rg_plat);

      char* ruby_ver = sp + 6;
      sp = strchr(ruby_ver, ' ');
      if(!sp) goto skip;
      *sp = 0;

      redisAsyncCommand(s->redis, 0, 0, "HINCRBY usage:ruby_version:%s %s 1",
                        rc->today, ruby_ver);

      char* ruby_rel = sp + 2;
      sp = strchr(ruby_rel, ' ');
      if(!sp) goto skip;
      *sp = 0;

      redisAsyncCommand(s->redis, 0, 0, "HINCRBY usage:ruby_release:%s %s 1",
                        rc->today, ruby_rel);

    }
skip:

    redisAsyncCommand(s->redis, get_full_name, rc,
                      "HGET v:%s name", data->full_name.ptr);
  }

  free(request);
}

static void request_path(ebb_request* request, const char* at, size_t len) {
  ebb_connection *connection = request->data;
  conn *data = connection->data;

  if(len > 10 && // /gems/ + .gem
      strncmp(at, "/gems/", 5) == 0 &&
      strncmp(at + (len - 4), ".gem", 4) == 0) {
    str_append(&data->full_name, at + 6, len - 10);
  }
}

static void request_header_field(ebb_request* r, const char* at,
                                 size_t len, int idx) {
  ebb_connection *connection = r->data;
  conn *data = connection->data;

  if(strncmp("User-Agent", at, len) == 0) {
    data->agent_idx = idx;
  }
}

static void request_header_value(ebb_request* r, const char* at,
                                 size_t len, int idx) {
  ebb_connection *connection = r->data;
  conn *data = connection->data;

  if(data->agent_idx == idx && len > 8) {
    if(strncmp("RubyGems/", at, 8) == 0) {
      str_append(&data->agent, at, len);
    } else if(strncmp("Ruby, RubyGems/", at, 15) == 0) {
      str_append(&data->agent, at+6, len-6);
    }
  }
}

static ebb_request* new_request(ebb_connection *connection) {
  //printf("request %d\n", ++c);
  ebb_request *request = malloc(sizeof(ebb_request));
  ebb_request_init(request);
  request->data = connection;
  request->on_path = request_path;
  request->on_complete = request_complete;
  request->on_header_field = request_header_field;
  request->on_header_value = request_header_value;
  return request;
}

ebb_connection* new_connection(ebb_server *server, struct sockaddr_in *addr) {
  conn *data = malloc(sizeof(conn));
  if(data == NULL) return NULL;

  ebb_connection *connection = malloc(sizeof(ebb_connection));
  if(connection == NULL) {
    free(data);
    return NULL;
  }

  conn_init(data);

  ebb_connection_init(connection);
  connection->data = data;
  connection->new_request = new_request;
  connection->on_close = on_close;
  
  return connection;
}

void start_redis(struct serv* s);

static void reconnect_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  struct serv* s = w->data;
  start_redis(s);
}

static void connect_db(const redisAsyncContext* c, int status) {
  struct serv* s = c->data;

  if(status != REDIS_OK) {
    printf("Error connecting: %s\n", c->errstr);

    ev_timer_init(&(s->reconnect_timer), reconnect_cb, 5., 0.);
    ev_timer_start(s->loop, &(s->reconnect_timer));
    s->reconnect_timer.data = s;
    
    return;
  }

  s->redis_connected = 1;
  printf("Connected to redis on %s.\n", s->redis_host);
}

static void disconnect_cb(const redisAsyncContext* c, int status) {
  // Reconnect it.
  printf("reconnecting...");
  struct serv* s = c->data;

  start_redis(s);
}

void start_redis(struct serv* s) {
  s->redis = redisAsyncConnect(s->redis_host, 6379);
  s->redis->data = s;

  redisLibevAttach(s->loop, s->redis);
  redisAsyncSetConnectCallback(s->redis, connect_db);
  redisAsyncSetDisconnectCallback(s->redis, disconnect_cb);
}

static void die(const char* msg) {
  printf("Error: %s\n", msg);
  exit(1);
}

int main(int argc, char **argv)  {
  struct serv s;
  s.redis_host = "127.0.0.1";
  s.http_prefix = "http://production.cf.rubygems.org/gems/";
  s.redis_connected = 0;
  s.reconnect_timer.data = &s;

  int port = 5000;

  for(int i = 1; i < argc; i++) {
    if(argv[i][0] == '-') {
      switch(argv[i][1]) {
      case 'h':
        if(++i == argc) die("-h requires an option");
        s.http_prefix = argv[i];
        break;
      case 'p':
        if(++i == argc) die("-p requires an option");
        port = atoi(argv[i]);
        break;
      case 'r':
        if(++i == argc) die("-r requires an option");
        s.redis_host = argv[i];
        break;
      }
    }
  }

  s.http_prefix_len = strlen(s.http_prefix);

  struct ev_loop *loop = ev_default_loop(0);
  ebb_server server;

  ebb_server_init(&server, loop);

  server.new_connection = new_connection;

  s.loop = loop;
  server.data = &s;

  start_redis(&s);

  printf("rubygems redis stats server : port %d\n", port);
  ebb_server_listen_on_port(&server, port);
  ev_loop(loop, 0);

  return 0;
}
