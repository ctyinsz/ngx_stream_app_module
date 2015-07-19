
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include <ngx_thread_pool.h>

#define NGX_STREAM_APP_CONF     0x10000000


//typedef void (*ngx_stream_app_handler_pt)(ngx_stream_session_t *s);

typedef struct {
	ngx_array_t   servers;
	ngx_thread_pool_t *tp;
	ngx_shm_t     shm;
	size_t        shm_size;
} ngx_stream_app_main_conf_t;


typedef struct {
	size_t        header_len;
	ngx_msec_t    client_timeout;
	ngx_msec_t    send_timeout;
} ngx_stream_app_srv_conf_t;

typedef struct {
	ngx_thread_task_t  task;
	void* data;
} ngx_app_task_t;

typedef struct {
	size_t      body_len;
	unsigned	header:1;
} ngx_stream_app_ctx_t;

static void ngx_stream_app_handler(ngx_stream_session_t *s);
static void ngx_app_wait_request_handler(ngx_event_t *ev);
//static void ngx_app_empty_handler(ngx_event_t *wev);

static u_char *ngx_stream_app_log_error(ngx_log_t *log, u_char *buf,
    size_t len);

static void *ngx_stream_app_create_main_conf(ngx_conf_t *cf);
static char *ngx_stream_app_init_main_conf(ngx_conf_t *cf, void *conf);

static void *ngx_stream_app_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_app_merge_srv_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_stream_app(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static void ngx_stream_app_process(void *data, ngx_log_t *log);
static void ngx_stream_app_finalize(ngx_event_t *ev);



static ngx_command_t  ngx_stream_app_commands[] = {

    { ngx_string("app"),
      NGX_STREAM_SRV_CONF|NGX_CONF_NOARGS,
      ngx_stream_app,
      0,
      0,
      NULL },
      
/*    { ngx_string("listen"),
      NGX_STREAM_APP_CONF|NGX_CONF_1MORE,
      ngx_stream_app_listen,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },
*/
    { ngx_string("res_buf_size"),
      NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_STREAM_MAIN_CONF_OFFSET,
      offsetof(ngx_stream_app_main_conf_t, shm_size),
      NULL },

    { ngx_string("header_len"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_app_srv_conf_t, header_len),
      NULL }, 

    { ngx_string("send_timeout"),
      NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_STREAM_SRV_CONF_OFFSET,
      offsetof(ngx_stream_app_srv_conf_t, send_timeout),
      NULL },
	{ ngx_string("client_timeout"),
	  NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
	  ngx_conf_set_msec_slot,
	  NGX_STREAM_SRV_CONF_OFFSET,
	  offsetof(ngx_stream_app_srv_conf_t, client_timeout),
	  NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_app_module_ctx = {
    NULL,                                  /* postconfiguration */

    ngx_stream_app_create_main_conf,      /* create main configuration */
    ngx_stream_app_init_main_conf,      /* init main configuration */

    ngx_stream_app_create_srv_conf,      /* create server configuration */
    ngx_stream_app_merge_srv_conf        /* merge server configuration */
};


ngx_module_t  ngx_stream_app_module = {
    NGX_MODULE_V1,
    &ngx_stream_app_module_ctx,          /* module context */
    ngx_stream_app_commands,             /* module directives */
    NGX_STREAM_MODULE,                     /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void
ngx_stream_app_handler(ngx_stream_session_t *s)
{
    ngx_connection_t                *c;
    ngx_stream_app_srv_conf_t     *pscf;

    c = s->connection;

    pscf = ngx_stream_get_module_srv_conf(s, ngx_stream_app_module);

    ngx_log_debug0(NGX_LOG_DEBUG_STREAM, c->log, 0,
                   "NGX_LOG_DEBUG_STREAM:app connection handler");

    s->upstream = NULL;

    s->log_handler = ngx_stream_app_log_error;

    c->write->handler = ngx_stream_app_finalize;
    c->read->handler = ngx_app_wait_request_handler;

    if (c->read->ready) {
 	   c->read->handler(c->read);
	   return;
    }
	
	if (!c->read->timer_set) {
		ngx_add_timer(c->read, pscf->client_timeout);
	}

//    ngx_reusable_connection(c, 1);
    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_stream_close_connection(c);
        return;
    }

}
/*
static void ngx_app_empty_handler(ngx_event_t *wev)
{

}
*/
static void ngx_app_wait_request_handler(ngx_event_t *ev)
{
    ngx_connection_t      *c;
    ngx_stream_session_t  *s;
	ngx_stream_app_main_conf_t  *cscf;
	ngx_stream_app_srv_conf_t  *ascf;
	ngx_stream_app_ctx_t  *s_ctx;
	ngx_app_task_t		  *t;
	ngx_buf_t             *b;
	ngx_str_t			  log_buf;
	ssize_t                n;
	size_t                 size;
	u_char				   *tmp;
    c = ev->data;
    s = c->data;

    if (ev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "client timed out");
        ngx_stream_close_connection(c);
        return;
    }

    if (c->close) {
        ngx_stream_close_connection(c);
        return;
    }

	cscf = ngx_stream_get_module_main_conf(s, ngx_stream_app_module);

	ascf = ngx_stream_get_module_srv_conf(s, ngx_stream_app_module);
		
	s_ctx = ngx_stream_get_module_ctx(s,ngx_stream_app_module);
	if(s_ctx == NULL){
		s_ctx = ngx_palloc(c->pool, sizeof(ngx_stream_app_ctx_t));
		if(s_ctx == NULL)
			return;
		s_ctx->header = 0;
		ngx_stream_set_ctx(s,s_ctx,ngx_stream_app_module);
	}

	b = c->buffer;

	if (b == NULL) {
		size = ascf->header_len;
		b = ngx_create_temp_buf(c->pool, size);
		if (b == NULL) {
			ngx_stream_close_connection(c);
			return;
		}

		c->buffer = b;

	} else if (b->start == NULL) {

		size = s_ctx->header == 0?ascf->header_len:s_ctx->body_len;

		b->start = ngx_palloc(c->pool, size);
		if (b->start == NULL) {
			ngx_stream_close_connection(c);
			return;
		}

		b->pos = b->start;
		b->last = b->start;
		b->end = b->last + size;
	}
	else {

		size = ascf->header_len + s_ctx->body_len - s->received;
//		size = b->end - b->last;
	}

	n = c->recv(c, b->last, size);
	
    if (n == NGX_AGAIN) {

        if (!c->read->timer_set) {
            ngx_add_timer(c->read, ascf->client_timeout);
        }

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            ngx_stream_close_connection(c);
            return;
        }

        return;
    }

	if (n == NGX_ERROR) {
        ngx_stream_close_connection(c);
        return;
    }
	
    if (n == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                      "client closed connection");
        ngx_stream_close_connection(c);
        return;
    }

    b->last += n;
	s->received +=n;

	c->log->action = "reading client request line";
	
	log_buf.len = s->received;
	log_buf.data = b->start;
	ngx_log_error(NGX_LOG_ALERT, c->log, 0, "%d recved [%V],[%d:%d]",n,&log_buf,b->end,b->last);

	if(b->end != b->last){
        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            ngx_stream_close_connection(c);
            return;
        }
	}
	else {
		if(s_ctx->header == 0){
			s_ctx->body_len = ngx_atoi(b->start,ascf->header_len);
			s_ctx->header = 1;
			if(s_ctx->body_len > 0 ){
				
				tmp = ngx_pcalloc(c->pool, ascf->header_len + s_ctx->body_len);
				if (tmp == NULL) {
					ngx_stream_close_connection(c);
					return;
				}

				ngx_memcpy(tmp,b->start,ascf->header_len);
				ngx_pfree(c->pool, b->start);
				b->start = tmp;
				
				b->pos = b->start + ascf->header_len;
				b->last = b->pos;
				b->end = b->last + s_ctx->body_len;
				
				ngx_app_wait_request_handler(ev);
			}
			else{
				ngx_log_error(NGX_LOG_INFO, c->log, 0, "empty request body");
				ngx_stream_close_connection(c);
				return;
			}

			ngx_log_error(NGX_LOG_ALERT, c->log, 0, "recv header,len[%d]",s_ctx->body_len);

		}
		else{
//			c->read->handler = ngx_app_empty_handler;

			t = (ngx_app_task_t *)ngx_thread_task_alloc(c->pool,
				sizeof(ngx_app_task_t) - sizeof(ngx_thread_task_t));
			if(t == NULL){
				ngx_log_error(NGX_LOG_ERR, c->log, 0, "create thread task failed");
				ngx_stream_close_connection(c);
			}
			t->data = s;
			t->task.handler = ngx_stream_app_process;
			t->task.event.handler = ngx_stream_app_finalize;
			t->task.event.data= c;
			ngx_log_error(NGX_LOG_ALERT, c->log, 0, "t->data[%d]=[%d][%d]",t->data,t->task.ctx,t);
			if(ngx_thread_task_post(cscf->tp,(ngx_thread_task_t *)t) != NGX_OK){
				ngx_log_error(NGX_LOG_ERR, c->log, 0, "post task to thread pool failed");
				ngx_stream_close_connection(c);
				return;
			}
			ngx_log_error(NGX_LOG_ALERT, c->log, 0, "after post task");
		}
	}

	return;
}

//线程池任务处理入口，处理请求
static void ngx_stream_app_process(void *data, ngx_log_t *log)
{
	ngx_stream_session_t		  *s;
	ngx_connection_t      		  *c;
	
	s = *(ngx_stream_session_t**)data;
	c = s->connection;
	
//    c->write->handler = ngx_stream_app_finalize;
//    c->read->handler =  ngx_app_empty_handler;

	c->buffer->pos = c->buffer->start;
	c->buffer->last = c->buffer->pos;

	ngx_log_error(NGX_LOG_ALERT, log, 0, "in threadpool task[%d][%d]",data,s);

	
//	ngx_stream_app_finalize(c->write);
	return;
}

//发送响应
static void ngx_stream_app_finalize(ngx_event_t *ev)
{
	ngx_stream_session_t  *s;
	ngx_connection_t      *c;
	ngx_stream_app_srv_conf_t  *cscf;

	ssize_t                n;
	c = ev->data;
	s = c->data;

	ngx_log_error(NGX_LOG_ALERT, c->log, 0, "sending task");

	cscf = ngx_stream_get_module_srv_conf(s, ngx_stream_app_module);
	
    if (c->write->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "sending response timed out");
        ngx_stream_close_connection(c);
        return;
    }

	n = c->send(c,c->buffer->last,c->buffer->end - c->buffer->last);
	if (n == NGX_AGAIN) {

		if (!c->write->timer_set) {
			ngx_add_timer(c->write, cscf->send_timeout);
		}
//		c->write->handler = ngx_stream_app_finalize;
//		c->read->handler =  ngx_app_empty_handler;
		if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
			ngx_stream_close_connection(c);
			return;
		}
		return;
	}
	if (n == NGX_ERROR) {
        ngx_stream_close_connection(c);
        return;
    }

    c->buffer->last += n;

	c->log->action = "sending  request  line to client";

	if(c->buffer->end != c->buffer->last){
		if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
			ngx_stream_close_connection(c);
			return;
		}
	}
}


static u_char *
ngx_stream_app_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char                 *p;
//    ngx_connection_t       *pc;
    ngx_stream_session_t   *s;
//    ngx_stream_upstream_t  *u;

    s = log->data;

//    u = s->upstream;

    p = buf;

//    if (u->peer.name) {
//        p = ngx_snprintf(p, len, ", app: \"%V\"", u->peer.name);
//        len -= p - buf;
//    }

//   pc = s->connection;
//	if(pc->buffer){
//		p = ngx_snprintf(p, len, ", revd: \"%V\"", pc->buffer->start);
//		len -= p - buf;
//	}

    p = ngx_snprintf(p, len,
                     ", bytes from/to client:%O/%O",
//                     ", bytes from/to upstream:%O/%O",
                     s->received, s->connection->sent);
//                     u->received, pc ? pc->sent : 0);

    return p;
}

static void *ngx_stream_app_create_main_conf(ngx_conf_t *cf)
{
	ngx_stream_app_main_conf_t *conf;

	conf = ngx_palloc(cf->pool, sizeof(ngx_stream_app_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

	conf->shm_size = NGX_CONF_UNSET_SIZE;
	conf->tp= NGX_CONF_UNSET_PTR ;

    if (ngx_array_init(&conf->servers, cf->pool, 1,
                       sizeof(ngx_stream_app_srv_conf_t *))
        != NGX_OK)
    {
        return NULL;
    }

	return conf;
}


static void *ngx_stream_app_create_srv_conf(ngx_conf_t *cf)
{
	ngx_stream_app_srv_conf_t *conf;

	conf = ngx_palloc(cf->pool, sizeof(ngx_stream_app_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }
//	conf->tp= NGX_CONF_UNSET_PTR ;
//	conf->shm_size = NGX_CONF_UNSET_SIZE ;
	conf->header_len = NGX_CONF_UNSET_SIZE ;
	conf->client_timeout = NGX_CONF_UNSET_MSEC ;
	conf->send_timeout = NGX_CONF_UNSET_MSEC ;

	return conf;
}
static char *ngx_stream_app_merge_srv_conf(ngx_conf_t *cf, void *parent,void *child)
{
    ngx_stream_app_srv_conf_t *prev = parent;
    ngx_stream_app_srv_conf_t *conf = child;

    ngx_conf_merge_msec_value(conf->send_timeout,
                              prev->send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->client_timeout,
                              prev->client_timeout, 60000);

    ngx_conf_merge_size_value(conf->header_len,
                              prev->header_len, 6);

	return NGX_CONF_OK;
}

static char *ngx_stream_app_init_main_conf(ngx_conf_t *cf, void *conf)
{
//	ngx_stream_app_srv_conf_t  **ascf;
	ngx_stream_app_main_conf_t *amcf = conf;
	ngx_str_t                   name = ngx_string("app");
//	ngx_uint_t                  i;

//	ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "init_main_conf");

	amcf->shm.size = amcf->shm_size;
	amcf->shm.log = cf->log;
	amcf->shm.name.len = sizeof("resource")-1;
	amcf->shm.name.data = (u_char *) "resource";


    if (ngx_shm_alloc(&amcf->shm) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
/*	ascf = amcf->servers.elts;
	for (i = 0; i < amcf->servers.nelts; i++) {
		ascf[i]->shm.size = amcf->shm_size;
		ascf[i]->shm.log = cf->log;
		ascf[i]->shm.name.len = sizeof("resource") - 1;
		ascf[i]->shm.name.data = (u_char *) "resource";
	}
*/
	amcf->tp = ngx_thread_pool_get(cf->cycle, &name);

//	ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "ngx_thread_pool_get");


	if(amcf->tp == NULL){
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,"invalid thread_pool [app]");
		return NGX_CONF_ERROR;
	}

	//TODO:load xml resource

	return NGX_CONF_OK;
}


///static void *
//ngx_stream_app_create_srv_conf(ngx_conf_t *cf)
//{
//    ngx_stream_app_srv_conf_t  *conf;

//    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_app_srv_conf_t));
//    if (conf == NULL) {
//        return NULL;
//    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->ssl_protocols = 0;
     *     conf->ssl_ciphers = { 0, NULL };
     *     conf->ssl_name = { 0, NULL };
     *     conf->ssl_trusted_certificate = { 0, NULL };
     *     conf->ssl_crl = { 0, NULL };
     *     conf->ssl_certificate = { 0, NULL };
     *     conf->ssl_certificate_key = { 0, NULL };
     *
     *     conf->ssl = NULL;
     *     conf->upstream = NULL;
     */

//    conf->tp = NGX_CONF_UNSET_MSEC;

//    return NULL;
//}

/*
static char *
ngx_stream_app_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{

    return NGX_CONF_OK;
}
*/

static char *
ngx_stream_app(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
//	ngx_conf_t                       pcf;
	
	char                            *rv;
    ngx_stream_core_srv_conf_t  *cscf;
	cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);
	cscf->handler = ngx_stream_app_handler;
	
	return NGX_CONF_OK;
/*
	pcf = *cf;
	cf->cmd_type = NGX_STREAM_APP_CONF;
	rv = ngx_conf_parse(cf, NULL);

	*cf = pcf;


    char                         *rv;
    void                         *mconf;
    ngx_uint_t                    m;
    ngx_conf_t                    pcf;
    ngx_stream_module_t          *module;
    ngx_stream_conf_ctx_t        *ctx, *stream_ctx;
    ngx_stream_app_srv_conf_t    *ascf, **ascfp;
    ngx_stream_core_main_conf_t  *cmcf;
	ngx_stream_core_srv_conf_t   *cscf;
	
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_stream_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    stream_ctx = cf->ctx;
    ctx->main_conf = stream_ctx->main_conf;

    ctx->srv_conf = ngx_pcalloc(cf->pool,
                                sizeof(void *) * ngx_stream_max_module);
    if (ctx->srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }
    for (m = 0; ngx_modules[m]; m++) {
        if (ngx_modules[m]->type != NGX_STREAM_MODULE) {
            continue;
        }

        module = ngx_modules[m]->ctx;

        if (module->create_srv_conf) {
            mconf = module->create_srv_conf(cf);
            if (mconf == NULL) {
                return NGX_CONF_ERROR;
            }

            ctx->srv_conf[ngx_modules[m]->ctx_index] = mconf;
        }
    }

    ascf = ctx->srv_conf[ngx_stream_app_module.ctx_index];
    ascf->ctx = ctx;

    cmcf = ctx->main_conf[ngx_stream_core_module.ctx_index];

    ascfp = ngx_array_push(&cmcf->servers);
    if (ascfp == NULL) {
        return NGX_CONF_ERROR;
    }

    *ascfp = ascf;

	cscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);
	cscf->handler = ngx_stream_app_handler;
	

    pcf = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_STREAM_APP_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = pcf;
*/
    return rv;
}

