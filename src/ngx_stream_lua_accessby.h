
/*
 * Copyright (C) Yichun Zhang (agentzh)
 * Copyright (C) Rye Yao (ryecn)
 */


#ifndef _NGX_STREAM_LUA_ACCESS_BY_H_INCLUDED_
#define _NGX_STREAM_LUA_ACCESS_BY_H_INCLUDED_


#include "ngx_stream_lua_common.h"


ngx_int_t ngx_stream_lua_access_handler(ngx_stream_session_t *s);
ngx_int_t ngx_stream_lua_access_handler_file(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx);
ngx_int_t ngx_stream_lua_access_handler_inline(ngx_stream_session_t *s, ngx_stream_lua_ctx_t *ctx);


#endif /* _NGX_STREAM_LUA_ACCESS_BY_H_INCLUDED_ */
