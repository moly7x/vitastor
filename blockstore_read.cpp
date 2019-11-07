#include "blockstore.h"

int blockstore::fulfill_read_push(blockstore_operation *read_op, uint32_t item_start,
    uint32_t item_state, uint64_t item_version, uint64_t item_location, uint32_t cur_start, uint32_t cur_end)
{
    if (cur_end > cur_start)
    {
        if (item_state == ST_IN_FLIGHT)
        {
            // Pause until it's written somewhere
            read_op->wait_for = WAIT_IN_FLIGHT;
            read_op->wait_detail = item_version;
            return -1;
        }
        else if (item_state == ST_DEL_WRITTEN || item_state == ST_DEL_SYNCED || item_state == ST_DEL_MOVED)
        {
            // item is unallocated - return zeroes
            memset(read_op->buf + cur_start - read_op->offset, 0, cur_end - cur_start);
            return 0;
        }
        struct io_uring_sqe *sqe = get_sqe();
        if (!sqe)
        {
            // Pause until there are more requests available
            read_op->wait_for = WAIT_SQE;
            return -1;
        }
        struct ring_data_t *data = ((ring_data_t*)sqe->user_data);
        data->iov = (struct iovec){
            read_op->buf + cur_start - read_op->offset,
            cur_end - cur_start
        };
        read_op->read_vec[cur_start] = data->iov;
        io_uring_prep_readv(
            sqe,
            IS_JOURNAL(item_state) ? journal.fd : data_fd,
            &data->iov, 1,
            (IS_JOURNAL(item_state) ? journal.offset : data_offset) + item_location + cur_start - item_start
        );
        data->op = read_op;
    }
    return 0;
}

int blockstore::fulfill_read(blockstore_operation *read_op, uint32_t item_start, uint32_t item_end,
    uint32_t item_state, uint64_t item_version, uint64_t item_location)
{
    uint32_t cur_start = item_start;
    if (cur_start < read_op->offset + read_op->len && item_end > read_op->offset)
    {
        cur_start = cur_start < read_op->offset ? read_op->offset : cur_start;
        item_end = item_end > read_op->offset + read_op->len ? read_op->offset + read_op->len : item_end;
        auto fulfill_near = read_op->read_vec.lower_bound(cur_start);
        if (fulfill_near != read_op->read_vec.begin())
        {
            fulfill_near--;
            if (fulfill_near->first + fulfill_near->second.iov_len <= cur_start)
            {
                fulfill_near++;
            }
        }
        while (fulfill_near != read_op->read_vec.end() && fulfill_near->first < item_end)
        {
            if (fulfill_read_push(read_op, item_start, item_state, item_version, item_location, cur_start, fulfill_near->first) < 0)
            {
                return -1;
            }
            cur_start = fulfill_near->first + fulfill_near->second.iov_len;
            fulfill_near++;
        }
        if (fulfill_read_push(read_op, item_start, item_state, item_version, item_location, cur_start, item_end) < 0)
        {
            return -1;
        }
    }
    return 0;
}

int blockstore::dequeue_read(blockstore_operation *read_op)
{
    auto clean_it = object_db.find(read_op->oid);
    auto dirty_it = dirty_db.upper_bound((obj_ver_id){
        .oid = read_op->oid,
        .version = UINT64_MAX,
    });
    dirty_it--;
    bool clean_found = clean_it != object_db.end();
    bool dirty_found = (dirty_it != dirty_db.end() && dirty_it->first.oid == read_op->oid);
    if (!clean_found && !dirty_found)
    {
        // region is not allocated - return zeroes
        memset(read_op->buf, 0, read_op->len);
        read_op->retval = read_op->len;
        read_op->callback(read_op);
        return 1;
    }
    unsigned prev_sqe_pos = ringloop->ring->sq.sqe_tail;
    uint64_t fulfilled = 0;
    if (dirty_found)
    {
        while (dirty_it->first.oid == read_op->oid)
        {
            dirty_entry& dirty = dirty_it->second;
            if ((read_op->flags & OP_TYPE_MASK) == OP_READ_DIRTY || IS_STABLE(dirty.state))
            {
                if (fulfill_read(read_op, dirty.offset, dirty.offset + dirty.size,
                    dirty.state, dirty_it->first.version, dirty.location) < 0)
                {
                    // need to wait. undo added requests, don't dequeue op
                    ringloop->ring->sq.sqe_tail = prev_sqe_pos;
                    read_op->read_vec.clear();
                    return 0;
                }
            }
            dirty_it--;
        }
    }
    if (clean_it != object_db.end())
    {
        if (fulfill_read(read_op, 0, block_size, ST_CURRENT, 0, clean_it->second.location) < 0)
        {
            // need to wait. undo added requests, don't dequeue op
            ringloop->ring->sq.sqe_tail = prev_sqe_pos;
            read_op->read_vec.clear();
            return 0;
        }
    }
    if (!read_op->read_vec.size())
    {
        // region is not allocated - return zeroes
        memset(read_op->buf, 0, read_op->len);
        read_op->retval = read_op->len;
        read_op->callback(read_op);
        return 1;
    }
    read_op->retval = 0;
    read_op->pending_ops = read_op->read_vec.size();
    in_process_ops.insert(read_op);
    int ret = ringloop->submit();
    if (ret < 0)
    {
        throw new std::runtime_error(std::string("io_uring_submit: ") + strerror(-ret));
    }
    return 1;
}
