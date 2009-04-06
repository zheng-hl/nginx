
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>


static ngx_int_t ngx_http_file_cache_exists(ngx_http_file_cache_t *cache,
    ngx_http_cache_t *c);
static ngx_http_file_cache_node_t *
    ngx_http_file_cache_lookup(ngx_http_file_cache_t *cache, u_char *key);
static void ngx_http_file_cache_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void ngx_http_file_cache_cleanup(void *data);
static time_t ngx_http_file_cache_forced_expire(ngx_http_file_cache_t *cache);
static time_t ngx_http_file_cache_expire(ngx_http_file_cache_t *cache);
static void ngx_http_file_cache_delete(ngx_http_file_cache_t *cache,
    ngx_queue_t *q, u_char *name);
static ngx_int_t
    ngx_http_file_cache_manager_sleep(ngx_http_file_cache_t *cache);
static ngx_int_t ngx_http_file_cache_noop(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_file_cache_manage_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_file_cache_add_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_file_cache_add(ngx_http_file_cache_t *cache,
    ngx_http_cache_t *c);
static ngx_int_t ngx_http_file_cache_delete_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);


static u_char  ngx_http_file_cache_key[] = { LF, 'K', 'E', 'Y', ':', ' ' };


static ngx_int_t
ngx_http_file_cache_init(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_file_cache_t  *ocache = data;

    size_t                  len;
    ngx_rbtree_node_t      *sentinel;
    ngx_http_file_cache_t  *cache;

    cache = shm_zone->data;

    if (ocache) {
        if (ngx_strcmp(cache->path->name.data, ocache->path->name.data) != 0) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "cache \"%V\" uses the \"%V\" cache path "
                          "while previously it used the \"%V\" cache path",
                          &shm_zone->name, &cache->path->name,
                          &ocache->path->name);

            return NGX_ERROR;
        }

        cache->rbtree = ocache->rbtree;
        cache->queue = ocache->queue;
        cache->shpool = ocache->shpool;
        cache->cold = ocache->cold;
        cache->size = ocache->size;
        cache->bsize = ocache->bsize;

        cache->max_size /= cache->bsize;

        return NGX_OK;
    }

    cache->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    cache->rbtree = ngx_slab_alloc(cache->shpool, sizeof(ngx_rbtree_t));
    if (cache->rbtree == NULL) {
        return NGX_ERROR;
    }

    sentinel = ngx_slab_alloc(cache->shpool, sizeof(ngx_rbtree_node_t));
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(cache->rbtree, sentinel,
                    ngx_http_file_cache_rbtree_insert_value);

    cache->queue = ngx_slab_alloc(cache->shpool, sizeof(ngx_queue_t));
    if (cache->queue == NULL) {
        return NGX_ERROR;
    }

    ngx_queue_init(cache->queue);

    cache->cold = ngx_slab_alloc(cache->shpool, sizeof(ngx_atomic_t));
    if (cache->cold == NULL) {
        return NGX_ERROR;
    }

    *cache->cold = 1;

    cache->size = ngx_slab_alloc(cache->shpool, sizeof(off_t));
    if (cache->size == NULL) {
        return NGX_ERROR;
    }

    *cache->size = 0;

    cache->bsize = ngx_fs_bsize(cache->path->name.data);

    cache->max_size /= cache->bsize;

    len = sizeof(" in cache keys zone \"\"") + shm_zone->name.len;

    cache->shpool->log_ctx = ngx_slab_alloc(cache->shpool, len);
    if (cache->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(cache->shpool->log_ctx, " in cache keys zone \"%V\"%Z",
                &shm_zone->name);

    return NGX_OK;
}


void
ngx_http_file_cache_create_key(ngx_http_request_t *r)
{
    size_t             len;
    ngx_str_t         *key;
    ngx_uint_t         i;
    ngx_md5_t          md5;
    ngx_http_cache_t  *c;

    c = r->cache;

    len = 0;

    ngx_crc32_init(c->crc32);
    ngx_md5_init(&md5);

    key = c->keys.elts;
    for (i = 0; i < c->keys.nelts; i++) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http cache key: \"%V\"", &key[i]);

        len += key[i].len;

        ngx_crc32_update(&c->crc32, key[i].data, key[i].len);
        ngx_md5_update(&md5, key[i].data, key[i].len);
    }

    c->header_start = sizeof(ngx_http_file_cache_header_t)
                      + sizeof(ngx_http_file_cache_key) + len + 1;

    ngx_crc32_final(c->crc32);
    ngx_md5_final(c->key, &md5);
}


ngx_int_t
ngx_http_file_cache_open(ngx_http_request_t *r)
{
    u_char                        *p;
    time_t                         now;
    ssize_t                        n;
    ngx_int_t                      rc, rv;
    ngx_uint_t                     cold, test;
    ngx_path_t                    *path;
    ngx_http_cache_t              *c;
    ngx_pool_cleanup_t            *cln;
    ngx_open_file_info_t           of;
    ngx_http_file_cache_t         *cache;
    ngx_http_core_loc_conf_t      *clcf;
    ngx_http_file_cache_header_t  *h;

    c = r->cache;
    cache = c->file_cache;

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_http_file_cache_exists(cache, c);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache exists: %i u:%ui e:%d",
                   rc, c->uses, c->exists);

    if (rc == NGX_ERROR) {
        return rc;
    }

    cln->handler = ngx_http_file_cache_cleanup;
    cln->data = c;

    if (rc == NGX_AGAIN) {
        return rc;
    }

    cold = *cache->cold;

    if (rc == NGX_OK) {

        if (c->error) {
            return c->error;
        }

        c->temp_file = 1;
        test = c->exists ? 1 : 0;
        rv = NGX_DECLINED;

    } else { /* rc == NGX_DECLINED */

        if (c->min_uses > 1) {

            if (!cold) {
                return NGX_AGAIN;
            }

            test = 1;
            rv = NGX_AGAIN;

        } else {
            c->temp_file = 1;
            test = cold ? 1 : 0;
            rv = NGX_DECLINED;
        }
    }

    path = cache->path;

    c->file.name.len = path->name.len + 1 + path->len
                       + 2 * NGX_HTTP_CACHE_KEY_LEN;

    c->file.name.data = ngx_pnalloc(r->pool, c->file.name.len + 1);
    if (c->file.name.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(c->file.name.data, path->name.data, path->name.len);

    p = c->file.name.data + path->name.len + 1 + path->len;
    p = ngx_hex_dump(p, c->key, NGX_HTTP_CACHE_KEY_LEN);
    *p = '\0';

    ngx_create_hashed_filename(path, c->file.name.data, c->file.name.len);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "cache file: \"%s\"", c->file.name.data);

    if (!test) {
        return NGX_DECLINED;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    ngx_memzero(&of, sizeof(ngx_open_file_info_t));

    of.uniq = c->uniq;
    of.valid = clcf->open_file_cache_valid;
    of.min_uses = clcf->open_file_cache_min_uses;
    of.events = clcf->open_file_cache_events;
    of.directio = NGX_OPEN_FILE_DIRECTIO_OFF;

    if (ngx_open_cached_file(clcf->open_file_cache, &c->file.name, &of, r->pool)
        != NGX_OK)
    {
        switch (of.err) {

        case 0:
            return NGX_ERROR;

        case NGX_ENOENT:
        case NGX_ENOTDIR:
            return rv;

        default:
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, of.err,
                          ngx_open_file_n " \"%s\" failed", c->file.name.data);
            return NGX_ERROR;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache fd: %d", of.fd);

    c->file.fd = of.fd;
    c->file.log = r->connection->log;

    c->buf = ngx_create_temp_buf(r->pool, c->body_start);
    if (c->buf == NULL) {
        return NGX_ERROR;
    }

    n = ngx_read_file(&c->file, c->buf->pos, c->body_start, 0);

    if (n == NGX_ERROR) {
        return n;
    }

    if ((size_t) n <= c->header_start) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                      "cache file \"%s\" is too small", c->file.name.data);
        return NGX_ERROR;
    }

    h = (ngx_http_file_cache_header_t *) c->buf->pos;

    if (h->crc32 != c->crc32 || (size_t) h->header_start != c->header_start) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                      "cache file \"%s\" has md5 collision", c->file.name.data);
        return NGX_DECLINED;
    }

    c->buf->last += n;

    c->valid_sec = h->valid_sec;
    c->last_modified = h->last_modified;
    c->date = h->date;
    c->valid_msec = h->valid_msec;
    c->length = of.size;
    c->body_start = h->body_start;

    r->cached = 1;

    if (cold) {

        ngx_shmtx_lock(&cache->shpool->mutex);

        if (!c->node->exists) {
            c->node->uses = 1;
            c->node->body_start = c->body_start;
            c->node->exists = 1;

            *cache->size += (c->length + cache->bsize - 1) / cache->bsize;
        }

        ngx_shmtx_unlock(&cache->shpool->mutex);
    }

    now = ngx_time();

    if (c->valid_sec < now) {

        c->uses = c->min_uses;

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http file cache expired: %T %T", c->valid_sec, now);

        return NGX_HTTP_CACHE_STALE;
    }

    /* TODO: NGX_HTTP_CACHE_AGED */

    return NGX_OK;
}


static ngx_int_t
ngx_http_file_cache_exists(ngx_http_file_cache_t *cache, ngx_http_cache_t *c)
{
    ngx_int_t                    rc;
    ngx_http_file_cache_node_t  *fcn;

    ngx_shmtx_lock(&cache->shpool->mutex);

    fcn = ngx_http_file_cache_lookup(cache, c->key);

    if (fcn) {
        ngx_queue_remove(&fcn->queue);

        if (fcn->error) {

            if (fcn->valid_sec < ngx_time()) {
                goto renew;
            }

            rc = NGX_OK;

            goto done;
        }

        fcn->uses++;
        fcn->count++;

        if (fcn->exists) {

            c->exists = fcn->exists;
            c->body_start = fcn->body_start;

            rc = NGX_OK;

            goto done;
        }

        if (fcn->uses >= c->min_uses) {

            c->exists = fcn->exists;
            c->body_start = fcn->body_start;

            rc = NGX_OK;

        } else {
            rc = NGX_AGAIN;
        }

        goto done;
    }

    fcn = ngx_slab_alloc_locked(cache->shpool,
                                sizeof(ngx_http_file_cache_node_t));
    if (fcn == NULL) {
        ngx_shmtx_unlock(&cache->shpool->mutex);

        ngx_http_file_cache_forced_expire(cache);

        ngx_shmtx_lock(&cache->shpool->mutex);

        fcn = ngx_slab_alloc_locked(cache->shpool,
                                    sizeof(ngx_http_file_cache_node_t));
        if (fcn == NULL) {
            rc = NGX_ERROR;
            goto failed;
        }
    }

    ngx_memcpy((u_char *) &fcn->node.key, c->key, sizeof(ngx_rbtree_key_t));

    ngx_memcpy(fcn->key, &c->key[sizeof(ngx_rbtree_key_t)],
               NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

    ngx_rbtree_insert(cache->rbtree, &fcn->node);

renew:

    rc = NGX_DECLINED;

    fcn->uses = 1;
    fcn->count = 1;
    fcn->valid_msec = 0;
    fcn->error = 0;
    fcn->exists = 0;
    fcn->valid_sec = 0;
    fcn->uniq = 0;
    fcn->body_start = 0;
    fcn->length = 0;

done:

    fcn->expire = ngx_time() + cache->inactive;

    ngx_queue_insert_head(cache->queue, &fcn->queue);

    c->uniq = fcn->uniq;
    c->uses = fcn->uses;
    c->error = fcn->error;
    c->node = fcn;

failed:

    ngx_shmtx_unlock(&cache->shpool->mutex);

    return rc;
}


static ngx_http_file_cache_node_t *
ngx_http_file_cache_lookup(ngx_http_file_cache_t *cache, u_char *key)
{
    ngx_int_t                    rc;
    ngx_rbtree_key_t             node_key;
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_http_file_cache_node_t  *fcn;

    ngx_memcpy((u_char *) &node_key, key, sizeof(ngx_rbtree_key_t));

    node = cache->rbtree->root;
    sentinel = cache->rbtree->sentinel;

    while (node != sentinel) {

        if (node_key < node->key) {
            node = node->left;
            continue;
        }

        if (node_key > node->key) {
            node = node->right;
            continue;
        }

        /* node_key == node->key */

        do {
            fcn = (ngx_http_file_cache_node_t *) node;

            rc = ngx_memcmp(&key[sizeof(ngx_rbtree_key_t)], fcn->key,
                            NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

            if (rc == 0) {
                return fcn;
            }

            node = (rc < 0) ? node->left : node->right;

        } while (node != sentinel && node_key == node->key);

        break;
    }

    /* not found */

    return NULL;
}


static void
ngx_http_file_cache_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p;
    ngx_http_file_cache_node_t   *cn, *cnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            cn = (ngx_http_file_cache_node_t *) node;
            cnt = (ngx_http_file_cache_node_t *) temp;

            p = (ngx_memcmp(cn->key, cnt->key,
                            NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t))
                 < 0)
                    ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


void
ngx_http_file_cache_set_header(ngx_http_request_t *r, u_char *buf)
{
    ngx_http_file_cache_header_t  *h = (ngx_http_file_cache_header_t *) buf;

    u_char            *p;
    ngx_str_t         *key;
    ngx_uint_t         i;
    ngx_http_cache_t  *c;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache set header");

    c = r->cache;

    h->valid_sec = c->valid_sec;
    h->last_modified = c->last_modified;
    h->date = c->date;
    h->crc32 = c->crc32;
    h->valid_msec = (u_short) c->valid_msec;
    h->header_start = (u_short) c->header_start;
    h->body_start = (u_short) c->body_start;

    p = buf + sizeof(ngx_http_file_cache_header_t);

    p = ngx_cpymem(p, ngx_http_file_cache_key, sizeof(ngx_http_file_cache_key));

    key = c->keys.elts;
    for (i = 0; i < c->keys.nelts; i++) {
        p = ngx_copy(p, key[i].data, key[i].len);
    }

    *p = LF;
}


void
ngx_http_file_cache_update(ngx_http_request_t *r, ngx_temp_file_t *tf)
{
    off_t                   size;
    ngx_int_t               rc;
    ngx_file_uniq_t         uniq;
    ngx_file_info_t         fi;
    ngx_http_cache_t        *c;
    ngx_ext_rename_file_t   ext;
    ngx_http_file_cache_t  *cache;

    c = r->cache;

    if (c->updated) {
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache update");

    c->updated = 1;

    cache = c->file_cache;

    uniq = 0;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache rename: \"%s\" to \"%s\"",
                   tf->file.name.data, c->file.name.data);

    ext.access = NGX_FILE_OWNER_ACCESS;
    ext.path_access = NGX_FILE_OWNER_ACCESS;
    ext.time = -1;
    ext.create_path = 1;
    ext.delete_file = 1;
    ext.log_rename_error = 1;
    ext.log = r->connection->log;

    rc = ngx_ext_rename_file(&tf->file.name, &c->file.name, &ext);

    if (rc == NGX_OK) {

        if (ngx_fd_info(tf->file.fd, &fi) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                          ngx_fd_info_n " \"%s\" failed", tf->file.name.data);

            rc = NGX_ERROR;

        } else {
            uniq = ngx_file_uniq(&fi);
        }
    }

    size = (c->length + cache->bsize - 1) / cache->bsize;

    ngx_shmtx_lock(&cache->shpool->mutex);

    c->node->count--;
    c->node->uniq = uniq;
    c->node->body_start = c->body_start;

    size = size - (c->node->length + cache->bsize - 1) / cache->bsize;

    c->node->length = c->length;

    *cache->size += size;

    if (rc == NGX_OK) {
        c->node->exists = 1;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);
}


ngx_int_t
ngx_http_cache_send(ngx_http_request_t *r)
{
    ngx_int_t          rc;
    ngx_buf_t         *b;
    ngx_chain_t        out;
    ngx_http_cache_t  *c;

    c = r->cache;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "http file cache send: %s", c->file.name.data);

    /* we need to allocate all before the header would be sent */

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
    if (b->file == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b->file_pos = c->body_start;
    b->file_last = c->length;

    b->in_file = (c->length - c->body_start) ? 1: 0;
    b->last_buf = (r == r->main) ? 1: 0;
    b->last_in_chain = 1;

    b->file->fd = c->file.fd;
    b->file->name = c->file.name;
    b->file->log = r->connection->log;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


void
ngx_http_file_cache_free(ngx_http_request_t *r, ngx_temp_file_t *tf)
{
    ngx_http_cache_t       *c;
    ngx_http_file_cache_t  *cache;

    c = r->cache;

    if (c->updated) {
        return;
    }

    c->updated = 1;

    cache = c->file_cache;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http file cache free");

    ngx_shmtx_lock(&cache->shpool->mutex);

    c->node->count--;

    if (c->error) {
        c->node->valid_sec = c->valid_sec;
        c->node->valid_msec = c->valid_msec;
        c->node->error = c->error;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);

    if (c->temp_file) {
        if (tf && tf->file.fd != NGX_INVALID_FILE) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "http file cache incomplete: \"%s\"",
                           tf->file.name.data);

            if (ngx_delete_file(tf->file.name.data) == NGX_FILE_ERROR) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                              ngx_delete_file_n " \"%s\" failed",
                              tf->file.name.data);
            }
        }
    }
}


static void
ngx_http_file_cache_cleanup(void *data)
{
    ngx_http_cache_t  *c = data;

    ngx_http_file_cache_t  *cache;

    if (c->updated) {
        return;
    }

    c->updated = 1;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->file.log, 0,
                   "http file cache cleanup");

    if (c->error) {
        return;
    }

    cache = c->file_cache;

    ngx_shmtx_lock(&cache->shpool->mutex);

    c->node->count--;

    ngx_shmtx_unlock(&cache->shpool->mutex);
}


static time_t
ngx_http_file_cache_forced_expire(ngx_http_file_cache_t *cache)
{
    u_char                      *name;
    size_t                       len;
    time_t                       wait;
    ngx_uint_t                   tries;
    ngx_path_t                  *path;
    ngx_queue_t                 *q;
    ngx_http_file_cache_node_t  *fcn;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http file cache forced expire");

    path = cache->path;
    len = path->name.len + 1 + path->len + 2 * NGX_HTTP_CACHE_KEY_LEN;

    name = ngx_alloc(len + 1, ngx_cycle->log);
    if (name == NULL) {
        return 60;
    }

    ngx_memcpy(name, path->name.data, path->name.len);

    wait = 60;
    tries = 0;

    ngx_shmtx_lock(&cache->shpool->mutex);

    for (q = ngx_queue_last(cache->queue);
         q != ngx_queue_sentinel(cache->queue);
         q = ngx_queue_prev(q))
    {
        fcn = ngx_queue_data(q, ngx_http_file_cache_node_t, queue);

        ngx_log_debug6(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                  "http file cache forced expire: #%d %d %02xd%02xd%02xd%02xd",
                  fcn->count, fcn->exists,
                  fcn->key[0], fcn->key[1], fcn->key[2], fcn->key[3]);

        if (fcn->count) {

            if (tries++ < 20) {
                continue;
            }

            wait = 1;

            break;
        }

        if (!fcn->exists) {

            ngx_queue_remove(q);
            ngx_rbtree_delete(cache->rbtree, &fcn->node);
            ngx_slab_free_locked(cache->shpool, fcn);

            break;
        }

        ngx_http_file_cache_delete(cache, q, name);

        break;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);

    ngx_free(name);

    return wait;
}


static time_t
ngx_http_file_cache_expire(ngx_http_file_cache_t *cache)
{
    u_char                      *name, *p;
    size_t                       len;
    time_t                       now, wait;
    ngx_path_t                  *path;
    ngx_queue_t                 *q;
    ngx_http_file_cache_node_t  *fcn;
    u_char                       key[2 * NGX_HTTP_CACHE_KEY_LEN];

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http file cache expire");

    path = cache->path;
    len = path->name.len + 1 + path->len + 2 * NGX_HTTP_CACHE_KEY_LEN;

    name = ngx_alloc(len + 1, ngx_cycle->log);
    if (name == NULL) {
        return 60;
    }

    ngx_memcpy(name, path->name.data, path->name.len);

    now = ngx_time();

    ngx_shmtx_lock(&cache->shpool->mutex);

    for ( ;; ) {

        if (ngx_queue_empty(cache->queue)) {
            wait = 60;
            break;
        }

        q = ngx_queue_last(cache->queue);

        fcn = ngx_queue_data(q, ngx_http_file_cache_node_t, queue);

        wait = fcn->expire - now;

        if (wait > 0) {
            wait = wait > 60 ? 60 : wait;
            break;
        }

        ngx_log_debug6(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http file cache expire: #%d %d %02xd%02xd%02xd%02xd",
                       fcn->count, fcn->exists,
                       fcn->key[0], fcn->key[1], fcn->key[2], fcn->key[3]);

        if (fcn->count) {

            p = ngx_hex_dump(key, (u_char *) &fcn->node.key,
                             sizeof(ngx_rbtree_key_t));

            len = NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t);
            (void) ngx_hex_dump(p, fcn->key, len);

            /*
             * abnormally exited workers may leave locked cache entries,
             * and although it may be safe to remove them completely,
             * we prefer to remove them from inactive queue and rbtree
             * only, and to allow other leaks
             */

            ngx_queue_remove(q);
            ngx_rbtree_delete(cache->rbtree, &fcn->node);

            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                       "ignore long locked inactive cache entry %*s, count:%d",
                       2 * NGX_HTTP_CACHE_KEY_LEN, key, fcn->count);

            continue;
        }

        if (!fcn->exists) {

            ngx_queue_remove(q);
            ngx_rbtree_delete(cache->rbtree, &fcn->node);
            ngx_slab_free_locked(cache->shpool, fcn);

            continue;
        }

        ngx_http_file_cache_delete(cache, q, name);
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);

    ngx_free(name);

    return wait;
}


static void
ngx_http_file_cache_delete(ngx_http_file_cache_t *cache, ngx_queue_t *q,
    u_char *name)
{
    u_char                      *p;
    size_t                       len;
    ngx_path_t                  *path;
    ngx_http_file_cache_node_t  *fcn;

    fcn = ngx_queue_data(q, ngx_http_file_cache_node_t, queue);

    *cache->size -= (fcn->length + cache->bsize - 1) / cache->bsize;

    path = cache->path;

    p = name + path->name.len + 1 + path->len;

    p = ngx_hex_dump(p, (u_char *) &fcn->node.key, sizeof(ngx_rbtree_key_t));

    len = NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t);
    p = ngx_hex_dump(p, fcn->key, len);
    *p = '\0';

    ngx_queue_remove(q);

    ngx_rbtree_delete(cache->rbtree, &fcn->node);

    ngx_slab_free_locked(cache->shpool, fcn);

    ngx_shmtx_unlock(&cache->shpool->mutex);

    len = path->name.len + 1 + path->len + 2 * NGX_HTTP_CACHE_KEY_LEN;

    ngx_create_hashed_filename(path, name, len);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http file cache expire: \"%s\"", name);

    if (ngx_delete_file(name) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", name);
    }

    ngx_shmtx_lock(&cache->shpool->mutex);
}


static time_t
ngx_http_file_cache_manager(void *data)
{
    ngx_http_file_cache_t  *cache = data;

    off_t           size;
    time_t          next;
    ngx_tree_ctx_t  tree;

    if (*cache->cold) {

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http file cache manager update");

        tree.init_handler = NULL;
        tree.file_handler = ngx_http_file_cache_manage_file;
        tree.pre_tree_handler = ngx_http_file_cache_noop;
        tree.post_tree_handler = ngx_http_file_cache_noop;
        tree.spec_handler = ngx_http_file_cache_delete_file;
        tree.data = cache;
        tree.alloc = 0;
        tree.log = ngx_cycle->log;

        cache->last = ngx_current_msec;
        cache->files = 0;

        if (ngx_walk_tree(&tree, &cache->path->name) == NGX_ABORT) {
            return 60;
        }

        *cache->cold = 0;

        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "http file cache: %V %.3fM, bsize: %uz",
                      &cache->path->name,
                      ((double) *cache->size * cache->bsize) / (1024 * 1024),
                      cache->bsize);
    }

    next = ngx_http_file_cache_expire(cache);

    cache->last = ngx_current_msec;
    cache->files = 0;

    for ( ;; ) {
        ngx_shmtx_lock(&cache->shpool->mutex);

        size = *cache->size;

        ngx_shmtx_unlock(&cache->shpool->mutex);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http file cache size: %O", size);

        if (size < cache->max_size) {
            return next;
        }

        next = ngx_http_file_cache_forced_expire(cache);

        if (ngx_http_file_cache_manager_sleep(cache) != NGX_OK) {
            return next;
        }
    }
}


static ngx_int_t
ngx_http_file_cache_manager_sleep(ngx_http_file_cache_t *cache)
{
    ngx_msec_t  elapsed;

    if (cache->files++ > 100) {

        ngx_time_update(0, 0);

        elapsed = ngx_abs((ngx_msec_int_t) (ngx_current_msec - cache->last));

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http file cache manager time: %M", elapsed);

        if (elapsed > 200) {

            /*
             * if processing 100 files takes more than 200ms,
             * it seems that many operations require disk i/o,
             * therefore sleep 200ms
             */

            ngx_msleep(200);

            ngx_time_update(0, 0);
        }

        cache->last = ngx_current_msec;
        cache->files = 0;
    }

    return (ngx_quit || ngx_terminate) ? NGX_ABORT : NGX_OK;
}


static ngx_int_t
ngx_http_file_cache_noop(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    return NGX_OK;
}


static ngx_int_t
ngx_http_file_cache_manage_file(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_file_cache_t  *cache;

    cache = ctx->data;

    if (ngx_http_file_cache_add_file(ctx, path) != NGX_OK) {
        (void) ngx_http_file_cache_delete_file(ctx, path);
    }

    return ngx_http_file_cache_manager_sleep(cache);
}


static ngx_int_t
ngx_http_file_cache_add_file(ngx_tree_ctx_t *ctx, ngx_str_t *name)
{
    u_char                        *p;
    ngx_fd_t                       fd;
    ngx_int_t                      n;
    ngx_uint_t                     i;
    ngx_file_info_t                fi;
    ngx_http_cache_t               c;
    ngx_http_file_cache_t         *cache;
    ngx_http_file_cache_header_t   h;

    if (name->len < 2 * NGX_HTTP_CACHE_KEY_LEN) {
        return NGX_ERROR;
    }

    ngx_memzero(&c, sizeof(ngx_http_cache_t));

    fd = ngx_open_file(name->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);

    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      ngx_open_file_n " \"%s\" failed", name->data);
        return NGX_ERROR;
    }

    c.file.fd = fd;
    c.file.name = *name;
    c.file.log = ctx->log;

    n = ngx_read_file(&c.file, (u_char *) &h,
                      sizeof(ngx_http_file_cache_header_t), 0);
    if (n == NGX_ERROR) {
        return NGX_ERROR;
    }

    if ((size_t) n < sizeof(ngx_http_file_cache_header_t)) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, 0,
                      "cache file \"%s\" is too small", name->data);
        return NGX_ERROR;
    }

    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      ngx_fd_info_n " \"%s\" failed", name->data);

    } else {
        c.uniq = ngx_file_uniq(&fi);
        c.valid_sec = h.valid_sec;
        c.valid_msec = h.valid_msec;
        c.body_start = h.body_start;
        c.length = ngx_file_size(&fi);
    }

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, ctx->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", name->data);
    }

    if (c.body_start == 0) {
        return NGX_ERROR;
    }

    p = &name->data[name->len - 2 * NGX_HTTP_CACHE_KEY_LEN];

    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        n = ngx_hextoi(p, 2);

        if (n == NGX_ERROR) {
            return NGX_ERROR;
        }

        p += 2;

        c.key[i] = (u_char) n;
    }

    cache = ctx->data;

    return ngx_http_file_cache_add(cache, &c);
}


static ngx_int_t
ngx_http_file_cache_add(ngx_http_file_cache_t *cache, ngx_http_cache_t *c)
{
    ngx_http_file_cache_node_t  *fcn;

    ngx_shmtx_lock(&cache->shpool->mutex);

    fcn = ngx_http_file_cache_lookup(cache, c->key);

    if (fcn == NULL) {

        fcn = ngx_slab_alloc_locked(cache->shpool,
                                    sizeof(ngx_http_file_cache_node_t));
        if (fcn == NULL) {
            ngx_shmtx_unlock(&cache->shpool->mutex);
            return NGX_ERROR;
        }

        ngx_memcpy((u_char *) &fcn->node.key, c->key, sizeof(ngx_rbtree_key_t));

        ngx_memcpy(fcn->key, &c->key[sizeof(ngx_rbtree_key_t)],
                   NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

        ngx_rbtree_insert(cache->rbtree, &fcn->node);

        fcn->uses = 1;
        fcn->count = 0;
        fcn->valid_msec = c->valid_msec;
        fcn->error = 0;
        fcn->exists = 1;
        fcn->uniq = c->uniq;
        fcn->valid_sec = c->valid_sec;
        fcn->body_start = c->body_start;
        fcn->length = c->length;

        *cache->size += (c->length + cache->bsize - 1) / cache->bsize;

    } else {
        ngx_queue_remove(&fcn->queue);
    }

    fcn->expire = ngx_time() + cache->inactive;

    ngx_queue_insert_head(cache->queue, &fcn->queue);

    ngx_shmtx_unlock(&cache->shpool->mutex);

    return NGX_OK;
}


static ngx_int_t
ngx_http_file_cache_delete_file(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->log, 0,
                   "http file cache delete: \"%s\"", path->data);

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", path->data);
    }

    return NGX_OK;
}


time_t
ngx_http_file_cache_valid(ngx_array_t *cache_valid, ngx_uint_t status)
{
    ngx_uint_t               i;
    ngx_http_cache_valid_t  *valid;

    if (cache_valid == NULL) {
        return 0;
    }

    valid = cache_valid->elts;
    for (i = 0; i < cache_valid->nelts; i++) {

        if (valid[i].status == 0) {
            return valid[i].valid;
        }

        if (valid[i].status == status) {
            return valid[i].valid;
        }
    }

    return 0;
}


char *
ngx_http_file_cache_set_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    off_t                   max_size;
    u_char                 *last, *p;
    time_t                  inactive;
    ssize_t                 size;
    ngx_str_t               s, name, *value;
    ngx_uint_t              i, n;
    ngx_http_file_cache_t  *cache;

    cache = ngx_pcalloc(cf->pool, sizeof(ngx_http_file_cache_t));
    if (cache == NULL) {
        return NGX_CONF_ERROR;
    }

    cache->path = ngx_pcalloc(cf->pool, sizeof(ngx_path_t));
    if (cache->path == NULL) {
        return NGX_CONF_ERROR;
    }

    inactive = 600;

    name.len = 0;
    size = 0;
    max_size = NGX_MAX_OFF_T_VALUE;

    value = cf->args->elts;

    cache->path->name = value[1];

    if (cache->path->name.data[cache->path->name.len - 1] == '/') {
        cache->path->name.len--;
    }

    if (ngx_conf_full_name(cf->cycle, &cache->path->name, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "levels=", 7) == 0) {

            p = value[i].data + 7;
            last = value[i].data + value[i].len;

            for (n = 0; n < 3 && p < last; n++) {

                if (*p > '0' && *p < '3') {

                    cache->path->level[n] = *p++ - '0';
                    cache->path->len += cache->path->level[n] + 1;

                    if (p == last) {
                        break;
                    }

                    if (*p++ == ':' && n < 2 && p != last) {
                        continue;
                    }

                    goto invalid_levels;
                }

                goto invalid_levels;
            }

            if (cache->path->len < 10 + 3) {
                continue;
            }

        invalid_levels:

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid \"levels\" \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (ngx_strncmp(value[i].data, "keys_zone=", 10) == 0) {

            name.data = value[i].data + 10;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p) {
                name.len = p - name.data;

                p++;

                s.len = value[i].data + value[i].len - p;
                s.data = p;

                size = ngx_parse_size(&s);
                if (size > 8191) {
                    continue;
                }
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid keys zone size \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (ngx_strncmp(value[i].data, "inactive=", 9) == 0) {

            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            inactive = ngx_parse_time(&s, 1);
            if (inactive < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid inactive value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "max_size=", 9) == 0) {

            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            max_size = ngx_parse_offset(&s);
            if (max_size < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid max_size value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0 || size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"keys_zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    cache->path->manager = ngx_http_file_cache_manager;
    cache->path->data = cache;

    if (ngx_add_path(cf, &cache->path) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    cache->shm_zone = ngx_shared_memory_add(cf, &name, size, cmd->post);
    if (cache->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (cache->shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }


    cache->shm_zone->init = ngx_http_file_cache_init;
    cache->shm_zone->data = cache;

    cache->inactive = inactive;
    cache->max_size = max_size;

    return NGX_CONF_OK;
}


char *
ngx_http_file_cache_valid_set_slot(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    char  *p = conf;

    time_t                    valid;
    ngx_str_t                *value;
    ngx_uint_t                i, n, status;
    ngx_array_t             **a;
    ngx_http_cache_valid_t   *v;
    static ngx_uint_t         statuses[] = { 200, 301, 302 };

    a = (ngx_array_t **) (p + cmd->offset);

    if (*a == NGX_CONF_UNSET_PTR) {
        *a = ngx_array_create(cf->pool, 1, sizeof(ngx_http_cache_valid_t));
        if (*a == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    value = cf->args->elts;
    n = cf->args->nelts - 1;

    valid = ngx_parse_time(&value[n], 1);
    if (valid < 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid time value \"%V\"", &value[n]);
        return NGX_CONF_ERROR;
    }

    if (n == 1) {

        for (i = 0; i < 3; i++) {
            v = ngx_array_push(*a);
            if (v == NULL) {
                return NGX_CONF_ERROR;
            }

            v->status = statuses[i];
            v->valid = valid;
        }

        return NGX_CONF_OK;
    }

    for (i = 1; i < n; i++) {

        if (ngx_strcmp(value[i].data, "any") == 0) {

            status = 0;

        } else {

            status = ngx_atoi(value[i].data, value[i].len);
            if (status < 100) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid status \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }
        }

        v = ngx_array_push(*a);
        if (v == NULL) {
            return NGX_CONF_ERROR;
        }

        v->status = status;
        v->valid = valid;
    }

    return NGX_CONF_OK;
}
