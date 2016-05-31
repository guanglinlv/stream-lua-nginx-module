
/*
 * Copyright (C) Yichun Zhang (agentzh)
 * Copyright (C) Rye Yao (ryecn)
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include "ngx_stream_lua_accessby.h"
#include "ngx_stream_lua_util.h"
#include "ngx_stream_lua_cache.h"


static ngx_int_t ngx_stream_lua_access_by_chunk(lua_State *L,
        ngx_stream_session_t *s);


ngx_int_t
ngx_stream_lua_access_handler(ngx_stream_session_t *s)
{
    ngx_stream_lua_srv_conf_t       *lscf;
    ngx_stream_lua_ctx_t            *ctx;
    ngx_int_t                        rc;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0,
                   "stream lua access handler fd:%d",
                   (int) s->connection->fd);

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    if (lscf->access_handler == NULL) {
        dd("no access handler found");
        return NGX_DECLINED;
    }

    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);

    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            return NGX_ERROR;
        }
    }

    dd("entered? %d", (int) ctx->entered_access_phase);

    if (ctx->entered_access_phase) {
        dd("calling wev handler");
        rc = ctx->resume_handler(s, ctx);
        dd("wev handler returns %d", (int) rc);
        return rc;
    }

    dd("calling access handler");
    return lscf->access_handler(s, ctx);
}

ngx_int_t
ngx_stream_lua_access_handler_file(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    u_char                          *script_path;
    ngx_stream_lua_srv_conf_t       *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    script_path = ngx_stream_lua_rebase_path(s->connection->pool,
                                             lscf->access_src.data,
                                             lscf->access_src.len);

    dd("1");
    if (script_path == NULL) {
        return NGX_ERROR;
    }

    dd("2");
    L = ngx_stream_lua_get_lua_vm(s, NULL);

    dd("3");
    /*  load Lua script file (w/ cache)        sp = 1 */
    rc = ngx_stream_lua_cache_loadfile(s->connection->log, L, script_path,
                                       lscf->access_src_key);
    dd("4");
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    dd("5");
    /*  make sure we have a valid code chunk */
    ngx_stream_lua_assert(lua_isfunction(L, -1));

    dd("6");
    return ngx_stream_lua_access_by_chunk(L, s);
}


ngx_int_t
ngx_stream_lua_access_handler_inline(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx)
{
    lua_State                       *L;
    ngx_int_t                        rc;
    ngx_stream_lua_srv_conf_t       *lscf;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    L = ngx_stream_lua_get_lua_vm(s, NULL);

    /*  load Lua inline script (w/ cache) sp = 1 */
    rc = ngx_stream_lua_cache_loadbuffer(s->connection->log, L,
                                         lscf->access_src.data,
                                         lscf->access_src.len,
                                         lscf->access_src_key,
                                         (const char *)
                                         lscf->access_chunkname);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_stream_lua_access_by_chunk(L, s);
}


static ngx_int_t
ngx_stream_lua_access_by_chunk(lua_State *L, ngx_stream_session_t *s)
{
    int                          co_ref;
    ngx_int_t                    rc;
    lua_State                   *co;
    ngx_connection_t            *c;
    ngx_stream_lua_ctx_t        *ctx;
    ngx_stream_lua_cleanup_t    *cln;

    ngx_stream_lua_srv_conf_t      *lscf;


    dd("access by chunk");

    /*  {{{ new coroutine to handle session */
    co = ngx_stream_lua_new_thread(s, L, &co_ref);

    if (co == NULL) {

        return NGX_ERROR;
    }

    /*  move code closure to new coroutine */
    lua_xmove(L, co, 1);

    /*  set closure's env table to new coroutine's globals table */
    ngx_stream_lua_get_globals_table(co);
    lua_setfenv(co, -2);

    /*  save nginx session in coroutine globals table */
    ngx_stream_lua_set_session(co, s);

    /*  {{{ initialize session context */
    ctx = ngx_stream_get_module_ctx(s, ngx_stream_lua_module);
    dd("ctx = %p", ctx);

    if (ctx == NULL) {
        ctx = ngx_stream_lua_create_ctx(s);
        if (ctx == NULL) {
            return NGX_ERROR;
        }
    }

    ngx_stream_lua_reset_ctx(s, L, ctx);

    dd("setting entered");

    ctx->entered_access_phase = 1;

    ctx->cur_co_ctx = &ctx->entry_co_ctx;
    ctx->cur_co_ctx->co = co;
    ctx->cur_co_ctx->co_ref = co_ref;
#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif

    /*  }}} */

    /*  {{{ register session cleanup hooks */
    if (ctx->cleanup == NULL) {
        if (s->ctx) {
            cln = ngx_stream_lua_cleanup_add(s, 0);
        }
        if (cln == NULL) {
            return NGX_ERROR;
        }
        cln->handler = ngx_stream_lua_session_cleanup_handler;
        cln->data = ctx;
        ctx->cleanup = &cln->handler;
    }
    /*  }}} */

    ctx->context = NGX_STREAM_LUA_CONTEXT_ACCESS;

    lscf = ngx_stream_get_module_srv_conf(s, ngx_stream_lua_module);

    c = s->connection;

    c->read->handler = ngx_stream_lua_session_handler;
    c->write->handler = ngx_stream_lua_session_handler;

    if (lscf->check_client_abort) {
        ctx->read_event_handler = ngx_stream_lua_rd_check_broken_connection;

        if (!c->read->active) {
            if (ngx_add_event(c->read, NGX_READ_EVENT, 0) != NGX_OK) {
                return NGX_ERROR;
            }
        }

    } else {
        ctx->read_event_handler = ngx_stream_lua_block_reading;
    }

    rc = ngx_stream_lua_run_thread(L, s, ctx, 0);

    if (rc == NGX_ERROR || rc >= NGX_OK) {
        return rc;
    }

    if (rc == NGX_AGAIN) {
        /* Not implemented yet */
        // rc = ngx_stream_lua_run_posted_threads(s->connection, L, s, ctx);

        // if (rc == NGX_ERROR || rc == NGX_DONE || rc > NGX_OK) {
        //     return rc;
        // }

        // if (rc != NGX_OK) {
        //     return NGX_DECLINED;
        // }
    } else if (rc == NGX_DONE) {
        /** TODO support resumable access handler */
        // ngx_stream_lua_finalize_session(s, NGX_DONE);

        // rc = ngx_stream_lua_run_posted_threads(s->connection, L, s, ctx);

        // if (rc == NGX_ERROR || rc == NGX_DONE || rc == NGX_ABORT || rc > NGX_OK) {
        //     return rc;
        // }

        // if (rc != NGX_OK) {
        //     return NGX_DECLINED;
        // }
        // return NGX_DECLINED;
    } else if (rc == NGX_ABORT) {
        // ngx_stream_lua_finalize_session(s, NGX_ABORT);
        return rc;
    }

#if 1
    if (rc == NGX_OK) {
        return rc;
    }
#endif

    return NGX_DECLINED;
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
