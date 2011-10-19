/*
 * Copyright (C) 2010-2011 Wandenberg Peixoto <wandenberg@gmail.com>, Rogério Carvalho Schneider <stockrt@gmail.com>
 *
 * This file is part of Nginx Push Stream Module.
 *
 * Nginx Push Stream Module is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nginx Push Stream Module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Nginx Push Stream Module.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * ngx_http_push_stream_module_utils.c
 *
 * Created: Oct 26, 2010
 * Authors: Wandenberg Peixoto <wandenberg@gmail.com>, Rogério Carvalho Schneider <stockrt@gmail.com>
 */

#include <ngx_http_push_stream_module_utils.h>

static void            nxg_http_push_stream_free_channel_memory_locked(ngx_slab_pool_t *shpool, ngx_http_push_stream_channel_t *channel);
static void            ngx_http_push_stream_run_cleanup_pool_handler(ngx_pool_t *p, ngx_pool_cleanup_pt handler);

static ngx_inline void
ngx_http_push_stream_ensure_qtd_of_messages_locked(ngx_http_push_stream_channel_t *channel, ngx_uint_t max_messages, ngx_flag_t expired)
{
    ngx_http_push_stream_msg_t             *sentinel, *msg;

    if (max_messages == NGX_CONF_UNSET_UINT) {
        return;
    }

    sentinel = &channel->message_queue;

    while (!ngx_queue_empty(&sentinel->queue) && ((channel->stored_messages > max_messages) || expired)) {
        msg = (ngx_http_push_stream_msg_t *)ngx_queue_next(&sentinel->queue);

        if (expired && ((msg->expires == 0) || (msg->expires > ngx_time()))) {
            break;
        }

        NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(channel->stored_messages);
        ngx_queue_remove(&msg->queue);
        ngx_http_push_stream_mark_message_to_delete_locked(msg);
    }

}


static void
ngx_http_push_stream_delete_unrecoverable_channels(ngx_http_push_stream_shm_data_t *data, ngx_slab_pool_t *shpool, ngx_rbtree_node_t *node)
{
    ngx_http_push_stream_channel_t              *channel;
    ngx_http_push_stream_pid_queue_t            *cur_worker;
    ngx_http_push_stream_subscriber_t           *cur;
    ngx_http_push_stream_worker_data_t          *workers_data = ((ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data)->ipc;
    ngx_http_push_stream_worker_data_t          *thisworker_data = workers_data + ngx_process_slot;
    ngx_http_push_stream_worker_subscriber_t    *worker_subscriber;
    ngx_http_push_stream_subscription_t         *cur_subscription;

    channel = (ngx_http_push_stream_channel_t *) node;

    if ((channel != NULL) && (channel->node.key != 0) && (&channel->node != data->tree.sentinel) && (&channel->node != data->channels_to_delete.sentinel) && (&channel->node != data->unrecoverable_channels.sentinel)) {

        if ((channel != NULL) && (channel->node.key != 0) && (channel->node.left != NULL)) {
            ngx_http_push_stream_delete_unrecoverable_channels(data, shpool, node->left);
        }

        if ((channel != NULL) && (channel->node.key != 0) && (channel->node.right != NULL)) {
            ngx_http_push_stream_delete_unrecoverable_channels(data, shpool, node->right);
        }

        if ((channel != NULL) && (channel->node.key != 0)) {
            // remove subscribers if any
            if (channel->subscribers > 0) {
                cur_worker = &channel->workers_with_subscribers;

                // find the current work
                while ((cur_worker = (ngx_http_push_stream_pid_queue_t *) ngx_queue_next(&cur_worker->queue)) != &channel->workers_with_subscribers) {
                    if (cur_worker->slot == ngx_process_slot) {

                        // to each subscriber of this channel in this worker
                        while(!ngx_queue_empty(&cur_worker->subscriber_sentinel.queue)) {
                            cur = (ngx_http_push_stream_subscriber_t *) ngx_queue_next(&cur_worker->subscriber_sentinel.queue);

                            // find the subscriber subscriptions on the worker
                            worker_subscriber = thisworker_data->worker_subscribers_sentinel;
                            while ((worker_subscriber = (ngx_http_push_stream_worker_subscriber_t *) ngx_queue_next(&worker_subscriber->queue)) != thisworker_data->worker_subscribers_sentinel) {
                                if (worker_subscriber->request == cur->request) {

                                    // find the subscription for the channel being deleted
                                    cur_subscription = &worker_subscriber->subscriptions_sentinel;
                                    while ((cur_subscription = (ngx_http_push_stream_subscription_t *) ngx_queue_next(&cur_subscription->queue)) != &worker_subscriber->subscriptions_sentinel) {
                                        if (cur_subscription->channel == channel) {
                                            NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(channel->subscribers);

                                            ngx_shmtx_lock(&shpool->mutex);
                                            // remove the reference from subscription for channel
                                            ngx_queue_remove(&cur_subscription->queue);
                                            // remove the reference from channel for subscriber
                                            ngx_queue_remove(&cur->queue);
                                            ngx_shmtx_unlock(&shpool->mutex);

                                            ngx_http_push_stream_send_response_message(cur->request, channel, channel->channel_deleted_message);

                                            break;
                                        }
                                    }

                                    // subscriber does not have any other subscription, the connection may be closed
                                    if (ngx_queue_empty(&worker_subscriber->subscriptions_sentinel.queue)) {
                                        ngx_http_push_stream_send_response_finalize(worker_subscriber->request);
                                    }

                                    break;
                                }
                            }
                        }
                    }
                }

            }

            // channel has not subscribers and can be released
            if (channel->subscribers == 0) {
                ngx_shmtx_lock(&shpool->mutex);
                // avoid more than one worker try to free channel memory
                if ((channel != NULL) && (channel->node.key != 0) && (channel->node.left != NULL) && (channel->node.right != NULL)) {
                    ngx_rbtree_delete(&data->unrecoverable_channels, &channel->node);
                    nxg_http_push_stream_free_channel_memory_locked(shpool, channel);
                }
                ngx_shmtx_unlock(&shpool->mutex);
            }
        }
    }
}


static ngx_inline void
ngx_http_push_stream_delete_worker_channel(void)
{
    ngx_slab_pool_t                             *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_shm_data_t             *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;

    if (data->unrecoverable_channels.root != data->unrecoverable_channels.sentinel) {
        ngx_http_push_stream_delete_unrecoverable_channels(data, shpool, data->unrecoverable_channels.root);
    }
}


ngx_http_push_stream_msg_t *
ngx_http_push_stream_convert_buffer_to_msg_on_shared_locked(ngx_buf_t *buf, ngx_http_push_stream_channel_t *channel, ngx_int_t id, ngx_str_t *event_id, ngx_pool_t *temp_pool)
{
    return ngx_http_push_stream_convert_char_to_msg_on_shared_locked(buf->pos, ngx_buf_size(buf), channel, id, event_id, temp_pool);
}


ngx_http_push_stream_msg_t *
ngx_http_push_stream_convert_char_to_msg_on_shared_locked(u_char *data, size_t len, ngx_http_push_stream_channel_t *channel, ngx_int_t id, ngx_str_t *event_id, ngx_pool_t *temp_pool)
{
    ngx_slab_pool_t                           *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_template_queue_t     *sentinel = &ngx_http_push_stream_module_main_conf->msg_templates;
    ngx_http_push_stream_template_queue_t     *cur = sentinel;
    ngx_http_push_stream_msg_t                *msg;
    int                                        i = 0;

    if ((msg = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_push_stream_msg_t))) == NULL) {
        return NULL;
    }

    msg->event_id = NULL;
    msg->event_id_message = NULL;
    msg->formatted_messages = NULL;

    if ((msg->raw = ngx_slab_alloc_locked(shpool, sizeof(ngx_str_t) + len + 1)) == NULL) {
        ngx_http_push_stream_free_message_memory_locked(shpool, msg);
        return NULL;
    }

    msg->raw->len = len;
    msg->raw->data = (u_char *) (msg->raw + 1);
    ngx_memset(msg->raw->data, '\0', len + 1);
    // copy the message to shared memory
    ngx_memcpy(msg->raw->data, data, len);

    if (event_id != NULL) {
        if ((msg->event_id = ngx_slab_alloc_locked(shpool, sizeof(ngx_str_t) + event_id->len + 1)) == NULL) {
            ngx_http_push_stream_free_message_memory_locked(shpool, msg);
            return NULL;
        }

        msg->event_id->len = event_id->len;
        msg->event_id->data = (u_char *) (msg->event_id + 1);
        ngx_memset(msg->event_id->data, '\0', event_id->len + 1);
        ngx_memcpy(msg->event_id->data, event_id->data, event_id->len);

        u_char *aux = ngx_http_push_stream_str_replace(NGX_HTTP_PUSH_STREAM_EVENTSOURCE_ID_TEMPLATE.data, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_EVENT_ID.data, event_id->data, 0, temp_pool);
        if (aux == NULL) {
            ngx_http_push_stream_free_message_memory_locked(shpool, msg);
            return NULL;
        }

        ngx_str_t *chunk = ngx_http_push_stream_get_formatted_chunk(aux, ngx_strlen(aux), temp_pool);
        if ((chunk == NULL) || (msg->event_id_message = ngx_slab_alloc_locked(shpool, sizeof(ngx_str_t) + chunk->len + 1)) == NULL) {
            ngx_http_push_stream_free_message_memory_locked(shpool, msg);
            return NULL;
        }

        msg->event_id_message->len = chunk->len;
        msg->event_id_message->data = (u_char *) (msg->event_id_message + 1);
        ngx_memset(msg->event_id_message->data, '\0', msg->event_id_message->len + 1);
        ngx_memcpy(msg->event_id_message->data, chunk->data, msg->event_id_message->len);
    }

    msg->deleted = 0;
    msg->expires = 0;
    msg->queue.prev = NULL;
    msg->queue.next = NULL;
    msg->id = id;

    if ((msg->formatted_messages = ngx_slab_alloc_locked(shpool, sizeof(ngx_str_t)*ngx_http_push_stream_module_main_conf->qtd_templates)) == NULL) {
        ngx_http_push_stream_free_message_memory_locked(shpool, msg);
        return NULL;
    }
    while ((cur = (ngx_http_push_stream_template_queue_t *) ngx_queue_next(&cur->queue)) != sentinel) {
        ngx_str_t *aux = NULL;
        if (cur->eventsource) {
            ngx_http_push_stream_line_t     *lines, *cur_line;

            if ((lines = ngx_http_push_stream_split_by_crlf(msg->raw, temp_pool)) == NULL) {
                return NULL;
            }

            cur_line = lines;
            while ((cur_line = (ngx_http_push_stream_line_t *) ngx_queue_next(&cur_line->queue)) != lines) {
                if ((cur_line->line = ngx_http_push_stream_format_message(channel, msg, cur_line->line, cur->template, temp_pool)) == NULL) {
                    break;
                }
            }
            aux = ngx_http_push_stream_join_with_crlf(lines, temp_pool);
        } else {
            aux = ngx_http_push_stream_format_message(channel, msg, msg->raw, cur->template, temp_pool);
        }

        if (aux == NULL) {
            ngx_http_push_stream_free_message_memory_locked(shpool, msg);
            return NULL;
        }

        ngx_str_t *chunk = ngx_http_push_stream_get_formatted_chunk(aux->data, aux->len, temp_pool);

        ngx_str_t *formmated = (msg->formatted_messages + i);
        if ((chunk == NULL) || ((formmated->data = ngx_slab_alloc_locked(shpool, chunk->len + 1)) == NULL)) {
            ngx_http_push_stream_free_message_memory_locked(shpool, msg);
            return NULL;
        }

        formmated->len = chunk->len;
        ngx_memset(formmated->data, '\0', formmated->len + 1);
        ngx_memcpy(formmated->data, chunk->data, formmated->len);

        i++;
    }

    return msg;
}


static ngx_int_t
ngx_http_push_stream_send_only_header_response(ngx_http_request_t *r, ngx_int_t status_code, const ngx_str_t *explain_error_message)
{
    ngx_int_t rc;

    r->header_only = 1;
    r->headers_out.content_length_n = 0;
    r->headers_out.status = status_code;
    if (explain_error_message != NULL) {
        ngx_http_push_stream_add_response_header(r, &NGX_HTTP_PUSH_STREAM_HEADER_EXPLAIN, explain_error_message);
    }

    rc = ngx_http_send_header(r);

    if (rc > NGX_HTTP_SPECIAL_RESPONSE) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return rc;
}

static ngx_table_elt_t *
ngx_http_push_stream_add_response_header(ngx_http_request_t *r, const ngx_str_t *header_name, const ngx_str_t *header_value)
{
    ngx_table_elt_t     *h = ngx_list_push(&r->headers_out.headers);


    if (h == NULL) {
        return NULL;
    }
    h->hash = 1;
    h->key.len = header_name->len;
    h->key.data = header_name->data;
    h->value.len = header_value->len;
    h->value.data = header_value->data;

    return h;
}

static ngx_str_t *
ngx_http_push_stream_get_header(ngx_http_request_t *r, const ngx_str_t *header_name)
{
    ngx_table_elt_t             *h;
    ngx_list_part_t             *part;
    ngx_uint_t                   i;
    ngx_str_t                   *aux = NULL;

    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = part->elts;
            i = 0;
        }

        if ((h[i].key.len == header_name->len) && (ngx_strncasecmp(h[i].key.data, header_name->data, header_name->len) == 0)) {
            aux = ngx_http_push_stream_create_str(r->pool, h[i].value.len);
            if (aux != NULL) {
                ngx_memcpy(aux->data, h[i].value.data, h[i].value.len);
            }
            break;
        }
    }

    return aux;
}

static ngx_int_t
ngx_http_push_stream_send_response_content_header(ngx_http_request_t *r, ngx_http_push_stream_loc_conf_t *pslcf)
{
    ngx_int_t rc = NGX_OK;

    if (pslcf->header_template.len > 0) {
        rc = ngx_http_push_stream_send_response_text(r, pslcf->header_template.data, pslcf->header_template.len, 0);
    }

    return rc;
}

static ngx_int_t
ngx_http_push_stream_send_response_message(ngx_http_request_t *r, ngx_http_push_stream_channel_t *channel, ngx_http_push_stream_msg_t *msg)
{
    ngx_http_push_stream_loc_conf_t *pslcf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);
    ngx_int_t rc = NGX_OK;

    if (pslcf->eventsource_support && (msg->event_id_message != NULL)) {
        rc = ngx_http_push_stream_send_response_text(r, msg->event_id_message->data, msg->event_id_message->len, 0);
    }

    if (rc != NGX_ERROR) {
        ngx_str_t *str = ngx_http_push_stream_get_formatted_message(r, channel, msg, r->pool);
        if (str != NULL) {
            rc = ngx_http_push_stream_send_response_text(r, str->data, str->len, 0);
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_push_stream_send_response_text(ngx_http_request_t *r, const u_char *text, uint len, ngx_flag_t last_buffer)
{
    ngx_buf_t     *b;
    ngx_chain_t   *out;

    if ((text == NULL) || (r->connection->error)) {
        return NGX_ERROR;
    }

    out = (ngx_chain_t *) ngx_pcalloc(r->pool, sizeof(ngx_chain_t));
    b = ngx_calloc_buf(r->pool);
    if ((out == NULL) || (b == NULL)) {
        return NGX_ERROR;
    }

    b->last_buf = last_buffer;
    b->flush = 1;
    b->memory = 1;
    b->pos = (u_char *) text;
    b->start = b->pos;
    b->end = b->pos + len;
    b->last = b->end;

    out->buf = b;
    out->next = NULL;

    return ngx_http_output_filter(r, out);
}

static void
ngx_http_push_stream_run_cleanup_pool_handler(ngx_pool_t *p, ngx_pool_cleanup_pt handler)
{
    ngx_pool_cleanup_t       *c;

    for (c = p->cleanup; c; c = c->next) {
        if ((c->handler == handler) && (c->data != NULL)) {
            c->handler(c->data);
            return;
        }
    }
}

/**
 * Should never be called inside a locked block
 * */
static void
ngx_http_push_stream_send_response_finalize(ngx_http_request_t *r)
{
    ngx_http_push_stream_loc_conf_t *pslcf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);

    ngx_http_push_stream_run_cleanup_pool_handler(r->pool, (ngx_pool_cleanup_pt) ngx_http_push_stream_subscriber_cleanup);

    if (pslcf->footer_template.len > 0) {
        ngx_http_push_stream_send_response_text(r, pslcf->footer_template.data, pslcf->footer_template.len, 0);
    }

    ngx_http_push_stream_send_response_text(r, NGX_HTTP_PUSH_STREAM_LAST_CHUNK.data, NGX_HTTP_PUSH_STREAM_LAST_CHUNK.len, 1);
    ngx_http_finalize_request(r, NGX_DONE);
}

static void
ngx_http_push_stream_send_response_finalize_for_longpolling_by_timeout(ngx_http_request_t *r)
{
    ngx_http_push_stream_run_cleanup_pool_handler(r->pool, (ngx_pool_cleanup_pt) ngx_http_push_stream_subscriber_cleanup);

    ngx_http_push_stream_add_polling_headers(r, ngx_time(), 0, r->pool);
    r->headers_out.status = NGX_HTTP_NOT_MODIFIED;
    ngx_http_send_header(r);

    ngx_http_push_stream_send_response_text(r, NGX_HTTP_PUSH_STREAM_LAST_CHUNK.data, NGX_HTTP_PUSH_STREAM_LAST_CHUNK.len, 1);
    ngx_http_finalize_request(r, NGX_DONE);
}

static void
ngx_http_push_stream_delete_channel(ngx_str_t *id, ngx_pool_t *temp_pool)
{
    ngx_http_push_stream_channel_t         *channel;
    ngx_slab_pool_t                        *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_shm_data_t        *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;
    ngx_http_push_stream_pid_queue_t       *cur;

    ngx_shmtx_lock(&shpool->mutex);

    channel = ngx_http_push_stream_find_channel_locked(id, ngx_cycle->log);
    if (channel != NULL) {
        // remove channel from tree
        channel->deleted = 1;
        (channel->broadcast) ? NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(data->broadcast_channels) : NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(data->channels);

        // move the channel to unrecoverable tree
        ngx_rbtree_delete(&data->tree, (ngx_rbtree_node_t *) channel);
        channel->node.key = ngx_crc32_short(channel->id.data, channel->id.len);
        ngx_rbtree_insert(&data->unrecoverable_channels, (ngx_rbtree_node_t *) channel);


        // remove all messages
        ngx_http_push_stream_ensure_qtd_of_messages_locked(channel, 0, 0);

        // apply channel deleted message text to message template
        if ((channel->channel_deleted_message = ngx_http_push_stream_convert_char_to_msg_on_shared_locked(ngx_http_push_stream_module_main_conf->channel_deleted_message_text.data, ngx_http_push_stream_module_main_conf->channel_deleted_message_text.len, channel, NGX_HTTP_PUSH_STREAM_CHANNEL_DELETED_MESSAGE_ID, NULL, temp_pool)) == NULL) {
            ngx_shmtx_unlock(&(shpool)->mutex);
            ngx_log_error(NGX_LOG_ERR, temp_pool->log, 0, "push stream module: unable to allocate memory to channel deleted message");
            return;
        }

        // send signal to each worker with subscriber to this channel
        cur = &channel->workers_with_subscribers;

        while ((cur = (ngx_http_push_stream_pid_queue_t *) ngx_queue_next(&cur->queue)) != &channel->workers_with_subscribers) {
            ngx_http_push_stream_alert_worker_delete_channel(cur->pid, cur->slot, ngx_cycle->log);
        }
    }

    ngx_shmtx_unlock(&(shpool)->mutex);
}


static void
ngx_http_push_stream_collect_expired_messages_and_empty_channels(ngx_http_push_stream_shm_data_t *data, ngx_slab_pool_t *shpool, ngx_rbtree_node_t *node, ngx_flag_t force)
{
    ngx_http_push_stream_channel_t     *channel;

    channel = (ngx_http_push_stream_channel_t *) node;
    if ((channel != NULL) && (channel->deleted == 0) && (&channel->node != data->tree.sentinel) && (&channel->node != data->channels_to_delete.sentinel) && (&channel->node != data->unrecoverable_channels.sentinel)) {

        if ((channel != NULL) && (channel->deleted == 0) && (channel->node.left != NULL)) {
            ngx_http_push_stream_collect_expired_messages_and_empty_channels(data, shpool, node->left, force);
        }

        if ((channel != NULL) && (channel->deleted == 0) && (channel->node.right != NULL)) {
            ngx_http_push_stream_collect_expired_messages_and_empty_channels(data, shpool, node->right, force);
        }

        ngx_shmtx_lock(&shpool->mutex);

        if ((channel != NULL) && (channel->deleted == 0)) {

            ngx_http_push_stream_ensure_qtd_of_messages_locked(channel, (force) ? 0 : channel->stored_messages, 1);

            if ((channel->stored_messages == 0) && (channel->subscribers == 0)) {
                channel->deleted = 1;
                channel->expires = ngx_time() + ngx_http_push_stream_module_main_conf->shm_cleanup_objects_ttl;
                (channel->broadcast) ? NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(data->broadcast_channels) : NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(data->channels);

                // move the channel to trash tree
                ngx_rbtree_delete(&data->tree, (ngx_rbtree_node_t *) channel);
                channel->node.key = ngx_crc32_short(channel->id.data, channel->id.len);
                ngx_rbtree_insert(&data->channels_to_delete, (ngx_rbtree_node_t *) channel);
            }
        }

        ngx_shmtx_unlock(&shpool->mutex);
    }
}


static void
ngx_http_push_stream_collect_expired_messages(ngx_http_push_stream_shm_data_t *data, ngx_slab_pool_t *shpool, ngx_rbtree_node_t *node, ngx_flag_t force)
{
    ngx_http_push_stream_channel_t     *channel;

    channel = (ngx_http_push_stream_channel_t *) node;
    if ((channel != NULL) && (channel->deleted == 0) && (&channel->node != data->tree.sentinel) && (&channel->node != data->channels_to_delete.sentinel) && (&channel->node != data->unrecoverable_channels.sentinel)) {

        if ((channel != NULL) && (channel->deleted == 0) && (channel->node.left != NULL)) {
            ngx_http_push_stream_collect_expired_messages(data, shpool, node->left, force);
        }

        if ((channel != NULL) && (channel->deleted == 0) && (channel->node.right != NULL)) {
            ngx_http_push_stream_collect_expired_messages(data, shpool, node->right, force);
        }

        ngx_shmtx_lock(&shpool->mutex);

        if ((channel != NULL) && (channel->deleted == 0)) {
            ngx_http_push_stream_ensure_qtd_of_messages_locked(channel, (force) ? 0 : channel->stored_messages, 1);
        }

        ngx_shmtx_unlock(&shpool->mutex);
    }
}


static void
ngx_http_push_stream_free_memory_of_expired_channels_locked(ngx_rbtree_t *tree, ngx_slab_pool_t *shpool, ngx_rbtree_node_t *node, ngx_flag_t force)
{
    ngx_rbtree_node_t                  *sentinel;
    ngx_http_push_stream_channel_t     *channel;

    sentinel = tree->sentinel;


    if (node != sentinel) {

        if (node->left != NULL) {
            ngx_http_push_stream_free_memory_of_expired_channels_locked(tree, shpool, node->left, force);
        }

        if (node->right != NULL) {
            ngx_http_push_stream_free_memory_of_expired_channels_locked(tree, shpool, node->right, force);
        }

        channel = (ngx_http_push_stream_channel_t *) node;

        if ((ngx_time() > channel->expires) || force) {
            ngx_rbtree_delete(tree, node);
            nxg_http_push_stream_free_channel_memory_locked(shpool, channel);
        }
    }
}


static void
nxg_http_push_stream_free_channel_memory_locked(ngx_slab_pool_t *shpool, ngx_http_push_stream_channel_t *channel)
{
    // delete the worker-subscriber queue
    ngx_http_push_stream_pid_queue_t     *cur;

    while ((cur = (ngx_http_push_stream_pid_queue_t *)ngx_queue_next(&channel->workers_with_subscribers.queue)) != &channel->workers_with_subscribers) {
        ngx_queue_remove(&cur->queue);
        ngx_slab_free_locked(shpool, cur);
    }

    ngx_slab_free_locked(shpool, channel->id.data);
    ngx_slab_free_locked(shpool, channel);
}


static ngx_int_t
ngx_http_push_stream_memory_cleanup()
{
    ngx_slab_pool_t                        *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_shm_data_t        *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;

    ngx_http_push_stream_collect_expired_messages_and_empty_channels(data, shpool, data->tree.root, 0);
    ngx_http_push_stream_free_memory_of_expired_messages_and_channels(0);

    return NGX_OK;
}


static ngx_int_t
ngx_http_push_stream_buffer_cleanup()
{
    ngx_slab_pool_t                        *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_shm_data_t        *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;

    ngx_http_push_stream_collect_expired_messages(data, shpool, data->tree.root, 0);

    return NGX_OK;
}


static ngx_int_t
ngx_http_push_stream_free_memory_of_expired_messages_and_channels(ngx_flag_t force)
{
    ngx_slab_pool_t                        *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;
    ngx_http_push_stream_shm_data_t        *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;
    ngx_http_push_stream_msg_t             *sentinel, *cur, *next;

    sentinel = &data->messages_to_delete;

    ngx_shmtx_lock(&shpool->mutex);
    cur = (ngx_http_push_stream_msg_t *)ngx_queue_next(&sentinel->queue);
    while (cur != sentinel) {
        next = (ngx_http_push_stream_msg_t *)ngx_queue_next(&cur->queue);
        if ((ngx_time() > cur->expires) || force) {
            ngx_queue_remove(&cur->queue);
            ngx_http_push_stream_free_message_memory_locked(shpool, cur);
        }
        cur = next;
    }
    ngx_http_push_stream_free_memory_of_expired_channels_locked(&data->channels_to_delete, shpool, data->channels_to_delete.root, force);
    ngx_shmtx_unlock(&shpool->mutex);

    return NGX_OK;
}


static void
ngx_http_push_stream_free_message_memory_locked(ngx_slab_pool_t *shpool, ngx_http_push_stream_msg_t *msg)
{
    u_int i;

    if (msg->formatted_messages != NULL) {
        for(i = 0; i < ngx_http_push_stream_module_main_conf->qtd_templates; i++) {
            ngx_str_t *formmated = (msg->formatted_messages + i);
            if ((formmated != NULL) && (formmated->data != NULL)) {
                ngx_slab_free_locked(shpool, formmated->data);
            }
        }

        ngx_slab_free_locked(shpool, msg->formatted_messages);
    }

    if (msg->raw != NULL) ngx_slab_free_locked(shpool, msg->raw);
    if (msg->event_id != NULL) ngx_slab_free_locked(shpool, msg->event_id);
    if (msg->event_id_message != NULL) ngx_slab_free_locked(shpool, msg->event_id_message);
    if (msg != NULL) ngx_slab_free_locked(shpool, msg);
}


static void
ngx_http_push_stream_mark_message_to_delete_locked(ngx_http_push_stream_msg_t *msg)
{
    ngx_http_push_stream_shm_data_t        *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;

    msg->deleted = 1;
    msg->expires = ngx_time() + ngx_http_push_stream_module_main_conf->shm_cleanup_objects_ttl;
    ngx_queue_insert_tail(&data->messages_to_delete.queue, &msg->queue);
}


static void
ngx_http_push_stream_timer_set(ngx_msec_t timer_interval, ngx_event_t *event, ngx_event_handler_pt event_handler, ngx_flag_t start_timer)
{
    if ((timer_interval != NGX_CONF_UNSET_MSEC) && start_timer) {
        ngx_slab_pool_t     *shpool = (ngx_slab_pool_t *) ngx_http_push_stream_shm_zone->shm.addr;

        if (event->handler == NULL) {
            ngx_shmtx_lock(&shpool->mutex);
            if (event->handler == NULL) {
                event->handler = event_handler;
                event->data = NULL;
                event->log = ngx_cycle->log;
                ngx_http_push_stream_timer_reset(timer_interval, event);
            }
            ngx_shmtx_unlock(&shpool->mutex);
        }
    }
}


static void
ngx_http_push_stream_timer_reset(ngx_msec_t timer_interval, ngx_event_t *timer_event)
{
    if (!ngx_exiting && (timer_interval != NGX_CONF_UNSET_MSEC)) {
        if (timer_event->timedout) {
            #if defined nginx_version && nginx_version >= 7066
                ngx_time_update();
            #else
                ngx_time_update(0, 0);
            #endif
        }
        ngx_add_timer(timer_event, timer_interval);
    }
}


static void
ngx_http_push_stream_ping_timer_wake_handler(ngx_event_t *ev)
{
    ngx_http_request_t                 *r = (ngx_http_request_t *) ev->data;
    ngx_http_push_stream_loc_conf_t    *pslcf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);
    ngx_int_t                           rc;

    if (pslcf->eventsource_support) {
        rc = ngx_http_push_stream_send_response_text(r, NGX_HTTP_PUSH_STREAM_EVENTSOURCE_PING_MESSAGE_CHUNK.data, NGX_HTTP_PUSH_STREAM_EVENTSOURCE_PING_MESSAGE_CHUNK.len, 0);
    } else {
        rc = ngx_http_push_stream_send_response_message(r, NULL, ngx_http_push_stream_ping_msg);
    }

    if (rc == NGX_ERROR) {
        ngx_http_push_stream_send_response_finalize(r);
    } else {
        ngx_http_push_stream_subscriber_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_push_stream_module);
        ngx_http_push_stream_timer_reset(pslcf->ping_message_interval, ctx->ping_timer);
    }
}

static void
ngx_http_push_stream_disconnect_timer_wake_handler(ngx_event_t *ev)
{
    ngx_http_request_t                    *r = (ngx_http_request_t *) ev->data;
    ngx_http_push_stream_subscriber_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_push_stream_module);

    if (ctx->longpolling) {
        ngx_http_push_stream_send_response_finalize_for_longpolling_by_timeout(r);
    } else {
        ngx_http_push_stream_send_response_finalize(r);
    }
}

static void
ngx_http_push_stream_memory_cleanup_timer_wake_handler(ngx_event_t *ev)
{
    ngx_http_push_stream_memory_cleanup();
    ngx_http_push_stream_timer_reset(ngx_http_push_stream_module_main_conf->memory_cleanup_interval, &ngx_http_push_stream_memory_cleanup_event);
}

static void
ngx_http_push_stream_buffer_timer_wake_handler(ngx_event_t *ev)
{
    ngx_http_push_stream_buffer_cleanup();
    ngx_http_push_stream_timer_reset(ngx_http_push_stream_module_main_conf->buffer_cleanup_interval, &ngx_http_push_stream_buffer_cleanup_event);
}

static u_char *
ngx_http_push_stream_str_replace(u_char *org, u_char *find, u_char *replace, ngx_uint_t offset, ngx_pool_t *pool)
{
    if (org == NULL) {
        return NULL;
    }

    ngx_uint_t len_org = ngx_strlen(org);
    ngx_uint_t len_find = ngx_strlen(find);
    ngx_uint_t len_replace = ngx_strlen(replace);

    u_char      *result = org;

    if (len_find > 0) {
        u_char *ret = (u_char *) ngx_strstr(org + offset, find);
        if (ret != NULL) {
            u_char *tmp = ngx_pcalloc(pool, len_org + len_replace + len_find + 1);
            if (tmp == NULL) {
                ngx_log_error(NGX_LOG_ERR, pool->log, 0, "push stream module: unable to allocate memory to apply text replace");
                return NULL;
            }

            ngx_memset(tmp, '\0', len_org + len_replace + len_find + 1);

            u_int len_found = ret-org;
            ngx_memcpy(tmp, org, len_found);
            ngx_memcpy(tmp + len_found, replace, len_replace);
            ngx_memcpy(tmp + len_found + len_replace, org + len_found + len_find, len_org - len_found - len_find);

            result = ngx_http_push_stream_str_replace(tmp, find, replace, len_found + len_replace, pool);
        }
    }

    return result;
}


static ngx_str_t *
ngx_http_push_stream_get_formatted_message(ngx_http_request_t *r, ngx_http_push_stream_channel_t *channel, ngx_http_push_stream_msg_t *message, ngx_pool_t *pool)
{
    ngx_http_push_stream_loc_conf_t        *pslcf = ngx_http_get_module_loc_conf(r, ngx_http_push_stream_module);
    if (pslcf->message_template_index > 0) {
        return message->formatted_messages + pslcf->message_template_index - 1;
    }
    return message->raw;
}


static ngx_str_t *
ngx_http_push_stream_format_message(ngx_http_push_stream_channel_t *channel, ngx_http_push_stream_msg_t *message, ngx_str_t *text, ngx_str_t *message_template, ngx_pool_t *temp_pool)
{
    u_char                    *txt = NULL;
    ngx_str_t                 *str = NULL;

    u_char char_id[NGX_INT_T_LEN];
    ngx_memset(char_id, '\0', NGX_INT_T_LEN);
    u_char *channel_id = (channel != NULL) ? channel->id.data : NGX_HTTP_PUSH_STREAM_EMPTY.data;
    u_char *event_id = (message->event_id != NULL) ? message->event_id->data : NGX_HTTP_PUSH_STREAM_EMPTY.data;

    ngx_sprintf(char_id, "%d", message->id);

    txt = ngx_http_push_stream_str_replace(message_template->data, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_ID.data, char_id, 0, temp_pool);
    txt = ngx_http_push_stream_str_replace(txt, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_EVENT_ID.data, event_id, 0, temp_pool);
    txt = ngx_http_push_stream_str_replace(txt, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_CHANNEL.data, channel_id, 0, temp_pool);
    txt = ngx_http_push_stream_str_replace(txt, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_TEXT.data, text->data, 0, temp_pool);

    if (txt == NULL) {
        ngx_log_error(NGX_LOG_ERR, temp_pool->log, 0, "push stream module: unable to allocate memory to replace message values on template");
        return NULL;
    }

    if ((str = ngx_pcalloc(temp_pool, sizeof(ngx_str_t))) == NULL) {
        ngx_log_error(NGX_LOG_ERR, temp_pool->log, 0, "push stream module: unable to allocate memory to return message applied to template");
        return NULL;
    }

    str->data = txt;
    str->len = ngx_strlen(txt);
    return str;
}


static void
ngx_http_push_stream_worker_subscriber_cleanup_locked(ngx_http_push_stream_worker_subscriber_t *worker_subscriber)
{
    ngx_http_push_stream_subscription_t     *cur, *sentinel;
    ngx_http_push_stream_shm_data_t         *data = (ngx_http_push_stream_shm_data_t *) ngx_http_push_stream_shm_zone->data;
    ngx_http_push_stream_subscriber_ctx_t   *ctx = ngx_http_get_module_ctx(worker_subscriber->request, ngx_http_push_stream_module);

    if (ctx != NULL) {
        if ((ctx->disconnect_timer != NULL) && ctx->disconnect_timer->timer_set) {
            ngx_del_timer(ctx->disconnect_timer);
        }

        if ((ctx->ping_timer != NULL) && ctx->ping_timer->timer_set) {
            ngx_del_timer(ctx->ping_timer);
        }
    }

    sentinel = &worker_subscriber->subscriptions_sentinel;

    while ((cur = (ngx_http_push_stream_subscription_t *) ngx_queue_next(&sentinel->queue)) != sentinel) {
        NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(cur->channel->subscribers);
        ngx_queue_remove(&cur->subscriber->queue);
        ngx_queue_remove(&cur->queue);
    }
    ngx_queue_init(&sentinel->queue);
    ngx_queue_remove(&worker_subscriber->queue);
    ngx_queue_init(&worker_subscriber->queue);
    worker_subscriber->clndata->worker_subscriber = NULL;
    NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER(data->subscribers);
    NGX_HTTP_PUSH_STREAM_DECREMENT_COUNTER((data->ipc + ngx_process_slot)->subscribers);
}


static ngx_http_push_stream_content_subtype_t *
ngx_http_push_stream_match_channel_info_format_and_content_type(ngx_http_request_t *r, ngx_uint_t default_subtype)
{
    ngx_uint_t      i;
    ngx_http_push_stream_content_subtype_t *subtype = &subtypes[default_subtype];

    if (r->headers_in.accept) {
        u_char     *cur = r->headers_in.accept->value.data;
        size_t      rem = 0;

        while((cur != NULL) && (cur = ngx_strnstr(cur, "/", r->headers_in.accept->value.len)) != NULL) {
            cur = cur + 1;
            rem = r->headers_in.accept->value.len - (r->headers_in.accept->value.data - cur);

            for(i=0; i<(sizeof(subtypes) / sizeof(ngx_http_push_stream_content_subtype_t)); i++) {
                if (ngx_strncmp(cur, subtypes[i].subtype, rem < subtypes[i].len ? rem : subtypes[i].len) == 0) {
                    subtype = &subtypes[i];
                    // force break while
                    cur = NULL;
                    break;
                }
            }
        }
    }

    return subtype;
}

static ngx_str_t *
ngx_http_push_stream_get_formatted_current_time(ngx_pool_t *pool)
{
    ngx_tm_t                            tm;
    ngx_str_t                          *currenttime;

    currenttime = ngx_http_push_stream_create_str(pool, 19); //ISO 8601 pattern
    if (currenttime != NULL) {
        ngx_gmtime(ngx_time(), &tm);
        ngx_sprintf(currenttime->data, (char *) NGX_HTTP_PUSH_STREAM_DATE_FORMAT_ISO_8601.data, tm.ngx_tm_year, tm.ngx_tm_mon, tm.ngx_tm_mday, tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec);
    } else {
        currenttime = &NGX_HTTP_PUSH_STREAM_EMPTY;
    }

    return currenttime;
}

static ngx_str_t *
ngx_http_push_stream_get_formatted_hostname(ngx_pool_t *pool)
{
    ngx_str_t                          *hostname;

    hostname = ngx_http_push_stream_create_str(pool, sizeof(ngx_str_t) + ngx_cycle->hostname.len);
    if (hostname != NULL) {
        ngx_memcpy(hostname->data, ngx_cycle->hostname.data, ngx_cycle->hostname.len);
    } else {
        hostname = &NGX_HTTP_PUSH_STREAM_EMPTY;
    }

    return hostname;
}


static ngx_str_t *
ngx_http_push_stream_get_formatted_chunk(const u_char *text, off_t len, ngx_pool_t *temp_pool)
{
    ngx_str_t            *chunk;

    /* the "0000000000000000" is 64-bit hexadimal string */
    chunk = ngx_http_push_stream_create_str(temp_pool, sizeof("0000000000000000" CRLF CRLF CRLF) + len);
    if (chunk != NULL) {
        ngx_sprintf(chunk->data, "%xO" CRLF "%s" CRLF CRLF, len + sizeof(CRLF) - 1, text);
        chunk->len = ngx_strlen(chunk->data);
    }
    return chunk;
}


static ngx_str_t *
ngx_http_push_stream_create_str(ngx_pool_t *pool, uint len)
{
    ngx_str_t *aux = (ngx_str_t *) ngx_pcalloc(pool, sizeof(ngx_str_t) + len + 1);
    if (aux != NULL) {
        aux->data = (u_char *) (aux + 1);
        aux->len = len;
        ngx_memset(aux->data, '\0', len + 1);
    }
    return aux;
}


static ngx_http_push_stream_line_t *
ngx_http_push_stream_add_line_to_queue(ngx_http_push_stream_line_t *sentinel, u_char *text, u_int len, ngx_pool_t *temp_pool)
{
    ngx_http_push_stream_line_t        *cur = NULL;
    ngx_str_t                          *line;
    if (len > 0) {
        cur = ngx_pcalloc(temp_pool, sizeof(ngx_http_push_stream_line_t));
        line = ngx_http_push_stream_create_str(temp_pool, len);
        if ((cur == NULL) || (line == NULL)) {
            return NULL;
        }
        cur->line = line;
        ngx_memcpy(cur->line->data, text, len);
        ngx_queue_insert_tail(&sentinel->queue, &cur->queue);
    }
    return cur;
}

static ngx_http_push_stream_line_t *
ngx_http_push_stream_split_by_crlf(ngx_str_t *msg, ngx_pool_t *temp_pool)
{
    ngx_http_push_stream_line_t        *sentinel = NULL;
    u_char                             *pos = NULL, *start = NULL, *crlf_pos, *cr_pos, *lf_pos;
    u_int                               step = 0, len = 0;

    if ((sentinel = ngx_pcalloc(temp_pool, sizeof(ngx_http_push_stream_line_t))) == NULL) {
        return NULL;
    }

    ngx_queue_init(&sentinel->queue);

    start = msg->data;
    do {
        crlf_pos = (u_char *) ngx_strstr(start, CRLF);
        cr_pos = (u_char *) ngx_strstr(start, "\r");
        lf_pos = (u_char *) ngx_strstr(start, "\n");

        pos = crlf_pos;
        step = 2;
        if ((pos == NULL) || (cr_pos < pos)) {
            pos = cr_pos;
            step = 1;
        }

        if ((pos == NULL) || (lf_pos < pos)) {
            pos = lf_pos;
            step = 1;
        }

        if (pos != NULL) {
            len = pos - start;
            if ((len > 0) && (ngx_http_push_stream_add_line_to_queue(sentinel, start, len, temp_pool) == NULL)) {
                return NULL;
            }
            start = pos + step;
        }

    } while (pos != NULL);

    len = (msg->data + msg->len) - start;
    if ((len > 0) && (ngx_http_push_stream_add_line_to_queue(sentinel, start, len, temp_pool) == NULL)) {
        return NULL;
    }

    return sentinel;
}


static ngx_str_t *
ngx_http_push_stream_join_with_crlf(ngx_http_push_stream_line_t *lines, ngx_pool_t *temp_pool)
{
    ngx_http_push_stream_line_t     *cur;
    ngx_str_t                       *result = NULL, *tmp = &NGX_HTTP_PUSH_STREAM_EMPTY;

    if (ngx_queue_empty(&lines->queue)) {
        return &NGX_HTTP_PUSH_STREAM_EMPTY;
    }

    cur = lines;
    while ((cur = (ngx_http_push_stream_line_t *) ngx_queue_next(&cur->queue)) != lines) {
        if ((cur->line == NULL) || (result = ngx_http_push_stream_create_str(temp_pool, tmp->len + cur->line->len)) == NULL) {
            return NULL;
        }

        ngx_memcpy(result->data, tmp->data, tmp->len);
        ngx_memcpy((result->data + tmp->len), cur->line->data, cur->line->len);

        tmp = result;
    }

    return result;
}


static ngx_str_t *
ngx_http_push_stream_apply_template_to_each_line(ngx_str_t *text, const ngx_str_t *message_template, ngx_pool_t *temp_pool)
{
    ngx_http_push_stream_line_t     *lines, *cur;
    ngx_str_t                       *result = NULL;

    lines = ngx_http_push_stream_split_by_crlf(text, temp_pool);
    if (lines != NULL) {
        cur = lines;
        while ((cur = (ngx_http_push_stream_line_t *) ngx_queue_next(&cur->queue)) != lines) {
            cur->line->data = ngx_http_push_stream_str_replace(message_template->data, NGX_HTTP_PUSH_STREAM_TOKEN_MESSAGE_TEXT.data, cur->line->data, 0, temp_pool);
            if (cur->line->data == NULL) {
                return NULL;
            }
            cur->line->len = ngx_strlen(cur->line->data);
        }
        result = ngx_http_push_stream_join_with_crlf(lines, temp_pool);
    }

    return result;
}

static void
ngx_http_push_stream_add_polling_headers(ngx_http_request_t *r, time_t last_modified_time, ngx_int_t tag, ngx_pool_t *temp_pool)
{
    if (last_modified_time > 0) {
        r->headers_out.last_modified_time = last_modified_time;
    }

    if (tag >= 0) {
        ngx_str_t *etag = ngx_http_push_stream_create_str(temp_pool, NGX_INT_T_LEN);
        if (etag != NULL) {
            ngx_sprintf(etag->data, "%ui", tag);
            etag->len = ngx_strlen(etag->data);
            r->headers_out.etag = ngx_http_push_stream_add_response_header(r, &NGX_HTTP_PUSH_STREAM_HEADER_ETAG, etag);
        }
    }
}
