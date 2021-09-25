// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

/**
 * CLI tool
 * Currently can (a) remove inodes and (b) merge snapshot/clone layers
 */

#include <vector>
#include <algorithm>

#include "cpp-btree/safe_btree_set.h"
#include "epoll_manager.h"
#include "cluster_client.h"
#include "pg_states.h"
#include "base64.h"

#define RM_LISTING 1
#define RM_REMOVING 2
#define RM_END 3

const char *exe_name = NULL;

struct rm_pg_t
{
    pg_num_t pg_num;
    osd_num_t rm_osd_num;
    std::set<object_id> objects;
    std::set<object_id>::iterator obj_pos;
    uint64_t obj_count = 0, obj_done = 0, obj_prev_done = 0;
    int state = 0;
    int in_flight = 0;
};

struct rm_inode_t;
struct snap_merger_t;
struct snap_flattener_t;
struct snap_remover_t;

class cli_tool_t
{
protected:
    uint64_t iodepth = 0, parallel_osds = 0;
    bool progress = true;
    bool list_first = false;
    int log_level = 0;
    int mode = 0;

    ring_loop_t *ringloop = NULL;
    epoll_manager_t *epmgr = NULL;
    cluster_client_t *cli = NULL;
    ring_consumer_t consumer;
    bool started = false;
    int waiting = 0;

    rm_inode_t *remover = NULL;
    snap_merger_t *merger = NULL;
    snap_flattener_t *flattener = NULL;
    snap_remover_t *snap_remover = NULL;

public:
    static json11::Json::object parse_args(int narg, const char *args[])
    {
        json11::Json::object cfg;
        json11::Json::array cmd;
        cfg["progress"] = "1";
        for (int i = 1; i < narg; i++)
        {
            if (!strcmp(args[i], "-h") || !strcmp(args[i], "--help"))
            {
                help();
            }
            else if (args[i][0] == '-' && args[i][1] == '-')
            {
                const char *opt = args[i]+2;
                cfg[opt] = !strcmp(opt, "json") || !strcmp(opt, "wait-list") || i == narg-1 ? "1" : args[++i];
            }
            else
            {
                cmd.push_back(std::string(args[i]));
            }
        }
        if (!cmd.size())
        {
            std::string exe(exe_name);
            if (exe.substr(exe.size()-11) == "vitastor-rm")
            {
                cmd.push_back("rm");
            }
        }
        cfg["command"] = cmd;
        return cfg;
    }

    static void help()
    {
        printf(
            "Vitastor inode removal tool\n"
            "(c) Vitaliy Filippov, 2020 (VNPL-1.1)\n\n"
            "USAGE:\n"
            "  %s rm [--etcd_address <etcd_address>] --pool <pool> --inode <inode>\n"
            "        [--wait-list] [--iodepth 32] [--parallel_osds 4] [--progress 1]\n"
            "  %s merge [--etcd_address <etcd_address>] <from> <to> [--target <from>]\n"
            "        [--iodepth 128] [--progress 1] [--cas 0|1]\n",
            exe_name, exe_name
        );
        exit(0);
    }

    void run(json11::Json cfg);
    void start_work();
    void continue_work();

    friend struct rm_inode_t;
    friend struct snap_merger_t;
    friend struct snap_flattener_t;
    friend struct snap_remover_t;

    void change_parent(inode_t cur, inode_t new_parent)
    {
        auto cur_cfg_it = cli->st_cli.inode_config.find(cur);
        if (cur_cfg_it == cli->st_cli.inode_config.end())
        {
            fprintf(stderr, "Inode 0x%lx disappeared\n", cur);
            exit(1);
        }
        inode_config_t *cur_cfg = &cur_cfg_it->second;
        std::string cur_name = cur_cfg->name;
        std::string cur_cfg_key = base64_encode(cli->st_cli.etcd_prefix+
            "/config/inode/"+std::to_string(INODE_POOL(cur))+
            "/"+std::to_string(INODE_NO_POOL(cur)));
        json11::Json::object cur_cfg_json = json11::Json::object {
            { "name", cur_cfg->name },
            { "size", cur_cfg->size },
        };
        if (new_parent)
        {
            if (INODE_POOL(cur) != INODE_POOL(new_parent))
                cur_cfg_json["parent_pool"] = (uint64_t)INODE_POOL(new_parent);
            cur_cfg_json["parent_id"] = (uint64_t)INODE_NO_POOL(new_parent);
        }
        if (cur_cfg->readonly)
        {
            cur_cfg_json["readonly"] = true;
        }
        waiting++;
        cli->st_cli.etcd_txn(json11::Json::object {
            { "compare", json11::Json::array {
                json11::Json::object {
                    { "target", "MOD" },
                    { "key", cur_cfg_key },
                    { "result", "LESS" },
                    { "mod_revision", cur_cfg->mod_revision+1 },
                },
            } },
            { "success", json11::Json::array {
                json11::Json::object {
                    { "request_put", json11::Json::object {
                        { "key", cur_cfg_key },
                        { "value", base64_encode(json11::Json(cur_cfg_json).dump()) },
                    } }
                },
            } },
        }, ETCD_SLOW_TIMEOUT, [this, new_parent, cur, cur_name](std::string err, json11::Json res)
        {
            if (err != "")
            {
                fprintf(stderr, "Error changing parent of %s: %s\n", cur_name.c_str(), err.c_str());
                exit(1);
            }
            if (!res["succeeded"].bool_value())
            {
                fprintf(stderr, "Inode %s was modified during snapshot deletion\n", cur_name.c_str());
                exit(1);
            }
            if (new_parent)
            {
                auto new_parent_it = cli->st_cli.inode_config.find(new_parent);
                std::string new_parent_name = new_parent_it != cli->st_cli.inode_config.end()
                    ? new_parent_it->second.name : "<unknown>";
                printf(
                    "Parent of layer %s (inode %lu in pool %u) changed to %s (inode %lu in pool %u)\n",
                    cur_name.c_str(), INODE_NO_POOL(cur), INODE_POOL(cur),
                    new_parent_name.c_str(), INODE_NO_POOL(new_parent), INODE_POOL(new_parent)
                );
            }
            else
            {
                printf(
                    "Parent of layer %s (inode %lu in pool %u) detached\n",
                    cur_name.c_str(), INODE_NO_POOL(cur), INODE_POOL(cur)
                );
            }
            waiting--;
            ringloop->wakeup();
        });
    }

    inode_config_t* get_inode_cfg(const std::string & name)
    {
        for (auto & ic: cli->st_cli.inode_config)
        {
            if (ic.second.name == name)
            {
                return &ic.second;
            }
        }
        fprintf(stderr, "Layer %s not found\n", name.c_str());
        exit(1);
    }
};

struct rm_inode_t
{
    uint64_t inode = 0;
    pool_id_t pool_id = 0;

    cli_tool_t *parent = NULL;
    inode_list_t *lister = NULL;
    std::vector<rm_pg_t*> lists;
    uint64_t total_count = 0, total_done = 0, total_prev_pct = 0;
    uint64_t pgs_to_list = 0;
    bool lists_done = false;
    bool finished = false;

    void start_delete()
    {
        lister = parent->cli->list_inode_start(inode, [this](inode_list_t *lst, std::set<object_id>&& objects, pg_num_t pg_num, osd_num_t primary_osd, int status)
        {
            rm_pg_t *rm = new rm_pg_t({
                .pg_num = pg_num,
                .rm_osd_num = primary_osd,
                .objects = objects,
                .obj_count = objects.size(),
                .obj_done = 0,
                .obj_prev_done = 0,
            });
            rm->obj_pos = rm->objects.begin();
            lists.push_back(rm);
            if (parent->list_first)
            {
                parent->cli->list_inode_next(lister, 1);
            }
            if (status & INODE_LIST_DONE)
            {
                lists_done = true;
            }
            pgs_to_list--;
            continue_delete();
        });
        if (!lister)
        {
            fprintf(stderr, "Failed to list inode %lu from pool %u objects\n", INODE_NO_POOL(inode), INODE_POOL(inode));
            exit(1);
        }
        pgs_to_list = parent->cli->list_pg_count(lister);
        parent->cli->list_inode_next(lister, parent->parallel_osds);
    }

    void send_ops(rm_pg_t *cur_list)
    {
        if (parent->cli->msgr.osd_peer_fds.find(cur_list->rm_osd_num) ==
            parent->cli->msgr.osd_peer_fds.end())
        {
            // Initiate connection
            parent->cli->msgr.connect_peer(cur_list->rm_osd_num, parent->cli->st_cli.peer_states[cur_list->rm_osd_num]);
            return;
        }
        while (cur_list->in_flight < parent->iodepth && cur_list->obj_pos != cur_list->objects.end())
        {
            osd_op_t *op = new osd_op_t();
            op->op_type = OSD_OP_OUT;
            op->peer_fd = parent->cli->msgr.osd_peer_fds[cur_list->rm_osd_num];
            op->req = (osd_any_op_t){
                .rw = {
                    .header = {
                        .magic = SECONDARY_OSD_OP_MAGIC,
                        .id = parent->cli->next_op_id(),
                        .opcode = OSD_OP_DELETE,
                    },
                    .inode = cur_list->obj_pos->inode,
                    .offset = cur_list->obj_pos->stripe,
                    .len = 0,
                },
            };
            op->callback = [this, cur_list](osd_op_t *op)
            {
                cur_list->in_flight--;
                if (op->reply.hdr.retval < 0)
                {
                    fprintf(stderr, "Failed to remove object %lx:%lx from PG %u (OSD %lu) (retval=%ld)\n",
                        op->req.rw.inode, op->req.rw.offset,
                        cur_list->pg_num, cur_list->rm_osd_num, op->reply.hdr.retval);
                }
                delete op;
                cur_list->obj_done++;
                total_done++;
                continue_delete();
            };
            cur_list->obj_pos++;
            cur_list->in_flight++;
            parent->cli->msgr.outbox_push(op);
        }
    }

    void continue_delete()
    {
        if (finished)
        {
            return;
        }
        if (parent->list_first && !lists_done)
        {
            return;
        }
        for (int i = 0; i < lists.size(); i++)
        {
            if (!lists[i]->in_flight && lists[i]->obj_pos == lists[i]->objects.end())
            {
                delete lists[i];
                lists.erase(lists.begin()+i, lists.begin()+i+1);
                i--;
                if (!lists_done)
                {
                    parent->cli->list_inode_next(lister, 1);
                }
            }
            else
            {
                send_ops(lists[i]);
            }
        }
        if (parent->progress && total_count > 0 && total_done*1000/total_count != total_prev_pct)
        {
            printf("\rRemoved %lu/%lu objects, %lu more PGs to list...", total_done, total_count, pgs_to_list);
            total_prev_pct = total_done*1000/total_count;
        }
        if (lists_done && !lists.size())
        {
            printf("Done, inode %lu in pool %u data removed\n", INODE_NO_POOL(inode), pool_id);
            finished = true;
        }
    }

    bool is_done()
    {
        return finished;
    }
};

struct snap_rw_op_t
{
    uint64_t offset = 0;
    void *buf = NULL;
    cluster_op_t op;
    int todo = 0;
    uint32_t start = 0, end = 0;
};

// Layer merge is the base for multiple operations:
// 1) Delete snapshot "up" = merge child layer into the parent layer, remove the child
//    and rename the parent to the child
// 2) Delete snapshot "down" = merge parent layer into the child layer and remove the parent
// 3) Flatten image = merge parent layers into the child layer and break the connection
struct snap_merger_t
{
    cli_tool_t *parent;

    // -- CONFIGURATION --
    // merge from..to into target (target may be one of from..to)
    std::string from_name, to_name, target_name;
    // inode=>rank (bigger rank means child layers)
    std::map<inode_t,int> sources;
    // delete merged source inode data during merge
    bool delete_source = false;
    // use CAS writes (0 = never, 1 = auto, 2 = always)
    int use_cas = 1;
    // don't necessarily delete source data, but perform checks as if we were to do it
    bool check_delete_source = false;
    // interval between fsyncs
    int fsync_interval = 128;

    // -- STATE --
    inode_t target;
    int target_rank;
    bool inside_continue = false;
    int state = 0;
    int lists_todo = 0;
    uint64_t target_block_size = 0;
    btree::safe_btree_set<uint64_t> merge_offsets;
    btree::safe_btree_set<uint64_t>::iterator oit;
    std::map<inode_t, std::vector<uint64_t>> layer_lists;
    std::map<inode_t, uint64_t> layer_block_size;
    std::map<inode_t, uint64_t> layer_list_pos;
    int in_flight = 0;
    uint64_t last_fsync_offset = 0;
    uint64_t last_written_offset = 0;
    int deleted_unsynced = 0;
    uint64_t processed = 0, to_process = 0;

    void start_merge()
    {
        check_delete_source = delete_source || check_delete_source;
        inode_config_t *from_cfg = parent->get_inode_cfg(from_name);
        inode_config_t *to_cfg = parent->get_inode_cfg(to_name);
        inode_config_t *target_cfg = target_name == "" ? from_cfg : parent->get_inode_cfg(target_name);
        if (to_cfg->num == from_cfg->num)
        {
            fprintf(stderr, "Only one layer specified, nothing to merge\n");
            exit(1);
        }
        // Check that to_cfg is actually a child of from_cfg and target_cfg is somewhere between them
        std::vector<inode_t> chain_list;
        inode_config_t *cur = to_cfg;
        chain_list.push_back(cur->num);
        layer_block_size[cur->num] = get_block_size(cur->num);
        while (cur->parent_id != from_cfg->num &&
            cur->parent_id != to_cfg->num &&
            cur->parent_id != 0)
        {
            auto it = parent->cli->st_cli.inode_config.find(cur->parent_id);
            if (it == parent->cli->st_cli.inode_config.end())
            {
                fprintf(stderr, "Parent inode of layer %s (id %ld) not found\n", cur->name.c_str(), cur->parent_id);
                exit(1);
            }
            cur = &it->second;
            chain_list.push_back(cur->num);
            layer_block_size[cur->num] = get_block_size(cur->num);
        }
        if (cur->parent_id != from_cfg->num)
        {
            fprintf(stderr, "Layer %s is not a child of %s\n", to_name.c_str(), from_name.c_str());
            exit(1);
        }
        chain_list.push_back(from_cfg->num);
        layer_block_size[from_cfg->num] = get_block_size(from_cfg->num);
        int i = chain_list.size()-1;
        for (inode_t item: chain_list)
        {
            sources[item] = i--;
        }
        if (sources.find(target_cfg->num) == sources.end())
        {
            fprintf(stderr, "Layer %s is not between %s and %s\n", target_name.c_str(), to_name.c_str(), from_name.c_str());
            exit(1);
        }
        target = target_cfg->num;
        target_rank = sources.at(target);
        int to_rank = sources.at(to_cfg->num);
        bool to_has_children = false;
        // Check that there are no other inodes dependent on altered layers
        //
        // 1) everything between <target> and <to> except <to> is not allowed
        //    to have children other than <to> if <to> is a child of <target>:
        //
        //    <target> - <layer 3> - <to>
        //            \- <layer 4> <--------X--------- NOT ALLOWED
        //
        // 2) everything between <from> and <target>, except <target>, is not allowed
        //    to have children other than <target> if sources are to be deleted after merging:
        //
        //    <from> - <layer 1> - <target> - <to>
        //          \- <layer 2> <---------X-------- NOT ALLOWED
        for (auto & ic: parent->cli->st_cli.inode_config)
        {
            auto it = sources.find(ic.second.num);
            if (it == sources.end() && ic.second.parent_id != 0)
            {
                it = sources.find(ic.second.parent_id);
                if (it != sources.end())
                {
                    int parent_rank = it->second;
                    if (parent_rank < to_rank && (parent_rank >= target_rank || check_delete_source))
                    {
                        fprintf(
                            stderr, "Layers at or above %s, but below %s are not allowed"
                                " to have other children, but %s is a child of %s\n",
                            (check_delete_source ? from_name.c_str() : target_name.c_str()),
                            to_name.c_str(), ic.second.name.c_str(),
                            parent->cli->st_cli.inode_config.at(ic.second.parent_id).name.c_str()
                        );
                        exit(1);
                    }
                    if (parent_rank >= to_rank)
                    {
                        to_has_children = true;
                    }
                }
            }
        }
        if ((target_rank < to_rank || to_has_children) && use_cas == 1)
        {
            // <to> has children itself, no need for CAS
            use_cas = 0;
        }
        sources.erase(target);
        printf(
            "Merging %ld layer(s) into target %s%s (inode %lu in pool %u)\n",
            sources.size(), target_cfg->name.c_str(),
            use_cas ? " online (with CAS)" : "", INODE_NO_POOL(target), INODE_POOL(target)
        );
        target_block_size = get_block_size(target);
        continue_merge_reent();
    }

    uint64_t get_block_size(inode_t inode)
    {
        auto & pool_cfg = parent->cli->st_cli.pool_config.at(INODE_POOL(inode));
        uint64_t pg_data_size = (pool_cfg.scheme == POOL_SCHEME_REPLICATED ? 1 : pool_cfg.pg_size-pool_cfg.parity_chunks);
        return parent->cli->get_bs_block_size() * pg_data_size;
    }

    void continue_merge_reent()
    {
        if (!inside_continue)
        {
            inside_continue = true;
            continue_merge();
            inside_continue = false;
        }
    }

    bool is_done()
    {
        return state == 7;
    }

    void continue_merge()
    {
        if (state == 1)
            goto resume_1;
        else if (state == 2)
            goto resume_2;
        else if (state == 3)
            goto resume_3;
        else if (state == 4)
            goto resume_4;
        else if (state == 5)
            goto resume_5;
        else if (state == 6)
            goto resume_6;
        else if (state == 7)
            return;
        // First list lower layers
        list_layers(true);
        state = 1;
    resume_1:
        while (lists_todo > 0)
        {
            // Wait for lists
            return;
        }
        if (merge_offsets.size() > 0)
        {
            state = 2;
            oit = merge_offsets.begin();
            processed = 0;
            to_process = merge_offsets.size();
    resume_2:
            // Then remove blocks already filled in target by issuing zero-length reads and checking bitmaps
            while (in_flight < parent->iodepth*parent->parallel_osds && oit != merge_offsets.end())
            {
                in_flight++;
                check_if_full(*oit);
                oit++;
                processed++;
                if (parent->progress && !(processed % 128))
                {
                    printf("\rFiltering target blocks: %lu/%lu", processed, to_process);
                }
            }
            if (in_flight > 0 || oit != merge_offsets.end())
            {
                // Wait until reads finish
                return;
            }
            if (parent->progress)
            {
                printf("\r%lu full blocks of target filtered out\n", to_process-merge_offsets.size());
            }
        }
        state = 3;
    resume_3:
        // Then list upper layers
        list_layers(false);
        state = 4;
    resume_4:
        while (lists_todo > 0)
        {
            // Wait for lists
            return;
        }
        state = 5;
        processed = 0;
        to_process = merge_offsets.size();
        oit = merge_offsets.begin();
    resume_5:
        // Now read, overwrite and optionally delete offsets one by one
        while (in_flight < parent->iodepth*parent->parallel_osds && oit != merge_offsets.end())
        {
            in_flight++;
            read_and_write(*oit);
            oit++;
            processed++;
            if (parent->progress && !(processed % 128))
            {
                printf("\rOverwriting blocks: %lu/%lu", processed, to_process);
            }
        }
        if (in_flight > 0 || oit != merge_offsets.end())
        {
            // Wait until overwrites finish
            return;
        }
        if (parent->progress)
        {
            printf("\rOverwriting blocks: %lu/%lu\n", to_process, to_process);
        }
        state = 6;
    resume_6:
        // Done
        printf("Done, layers from %s to %s merged into %s\n", from_name.c_str(), to_name.c_str(), target_name.c_str());
        state = 7;
    }

    void list_layers(bool lower)
    {
        for (auto & sp: sources)
        {
            inode_t src = sp.first;
            if (lower ? (sp.second < target_rank) : (sp.second > target_rank))
            {
                lists_todo++;
                inode_list_t* lst = parent->cli->list_inode_start(src, [this, src](
                    inode_list_t *lst, std::set<object_id>&& objects, pg_num_t pg_num, osd_num_t primary_osd, int status)
                {
                    uint64_t layer_block = layer_block_size.at(src);
                    for (object_id obj: objects)
                    {
                        merge_offsets.insert(obj.stripe - obj.stripe % target_block_size);
                        for (int i = target_block_size; i < layer_block; i += target_block_size)
                        {
                            merge_offsets.insert(obj.stripe - obj.stripe % target_block_size + i);
                        }
                    }
                    if (delete_source)
                    {
                        // Also store individual lists
                        auto & layer_list = layer_lists[src];
                        int pos = layer_list.size();
                        layer_list.resize(pos + objects.size());
                        for (object_id obj: objects)
                        {
                            layer_list[pos++] = obj.stripe;
                        }
                    }
                    if (status & INODE_LIST_DONE)
                    {
                        auto & name = parent->cli->st_cli.inode_config.at(src).name;
                        printf("Got listing of layer %s (inode %lu in pool %u)\n", name.c_str(), INODE_NO_POOL(src), INODE_POOL(src));
                        if (delete_source)
                        {
                            // Sort the inode listing
                            std::sort(layer_lists[src].begin(), layer_lists[src].end());
                        }
                        lists_todo--;
                        continue_merge_reent();
                    }
                    else
                    {
                        parent->cli->list_inode_next(lst, 1);
                    }
                });
                parent->cli->list_inode_next(lst, parent->parallel_osds);
            }
        }
    }

    // Check if <offset> is fully written in <target> and remove it from merge_offsets if so
    void check_if_full(uint64_t offset)
    {
        cluster_op_t *op = new cluster_op_t;
        op->opcode = OSD_OP_READ_BITMAP;
        op->inode = target;
        op->offset = offset;
        op->len = 0;
        op->callback = [this](cluster_op_t *op)
        {
            if (op->retval < 0)
            {
                fprintf(stderr, "error reading target bitmap at offset %lx: %s\n", op->offset, strerror(-op->retval));
            }
            else
            {
                uint64_t bitmap_bytes = target_block_size/parent->cli->get_bs_bitmap_granularity()/8;
                int i;
                for (i = 0; i < bitmap_bytes; i++)
                {
                    if (((uint8_t*)op->bitmap_buf)[i] != 0xff)
                    {
                        break;
                    }
                }
                if (i == bitmap_bytes)
                {
                    // full
                    merge_offsets.erase(op->offset);
                }
            }
            delete op;
            in_flight--;
            continue_merge_reent();
        };
        parent->cli->execute(op);
    }

    // Read <offset> from <to>, write it to <target> and optionally delete it
    // from all layers except <target> after fsync'ing
    void read_and_write(uint64_t offset)
    {
        snap_rw_op_t *rwo = new snap_rw_op_t;
        // Initialize counter to 1 to later allow write_subop() to return immediately
        // (even though it shouldn't really do that)
        rwo->todo = 1;
        rwo->buf = malloc(target_block_size);
        rwo->offset = offset;
        rwo_read(rwo);
    }

    void rwo_read(snap_rw_op_t *rwo)
    {
        cluster_op_t *op = &rwo->op;
        op->opcode = OSD_OP_READ;
        op->inode = target;
        op->offset = rwo->offset;
        op->len = target_block_size;
        op->iov.push_back(rwo->buf, target_block_size);
        op->callback = [this, rwo](cluster_op_t *op)
        {
            if (op->retval != op->len)
            {
                fprintf(stderr, "error reading target at offset %lx: %s\n", op->offset, strerror(-op->retval));
                exit(1);
            }
            next_write(rwo);
        };
        parent->cli->execute(op);
    }

    void next_write(snap_rw_op_t *rwo)
    {
        // Write each non-empty range using an individual operation
        // FIXME: Allow to use single write with "holes" (OSDs don't allow it yet)
        uint32_t gran = parent->cli->get_bs_bitmap_granularity();
        uint64_t bitmap_size = target_block_size / gran;
        while (rwo->end < bitmap_size)
        {
            auto bit = ((*(uint8_t*)(rwo->op.bitmap_buf + (rwo->end >> 3))) & (1 << (rwo->end & 0x7)));
            if (!bit)
            {
                if (rwo->end > rwo->start)
                {
                    // write start->end
                    rwo->todo++;
                    write_subop(rwo, rwo->start*gran, rwo->end*gran, use_cas ? 1+rwo->op.version : 0);
                    rwo->start = rwo->end;
                    if (use_cas)
                    {
                        // Submit one by one if using CAS writes
                        return;
                    }
                }
                rwo->start = rwo->end = rwo->end+1;
            }
            else
            {
                rwo->end++;
            }
        }
        if (rwo->end > rwo->start)
        {
            // write start->end
            rwo->todo++;
            write_subop(rwo, rwo->start*gran, rwo->end*gran, use_cas ? 1+rwo->op.version : 0);
            rwo->start = rwo->end;
            if (use_cas)
            {
                return;
            }
        }
        rwo->todo--;
        // Just in case, if everything is done
        autofree_op(rwo);
    }

    void write_subop(snap_rw_op_t *rwo, uint32_t start, uint32_t end, uint64_t version)
    {
        cluster_op_t *subop = new cluster_op_t;
        subop->opcode = OSD_OP_WRITE;
        subop->inode = target;
        subop->offset = rwo->offset+start;
        subop->len = end-start;
        subop->version = version;
        subop->iov.push_back(rwo->buf+start, end-start);
        subop->callback = [this, rwo](cluster_op_t *subop)
        {
            rwo->todo--;
            if (subop->retval != subop->len)
            {
                if (use_cas && subop->retval == -EINTR)
                {
                    // CAS failure - reread and repeat optimistically
                    rwo->start = subop->offset - rwo->offset;
                    rwo_read(rwo);
                    delete subop;
                    return;
                }
                fprintf(stderr, "error writing target at offset %lx: %s\n", subop->offset, strerror(-subop->retval));
                exit(1);
            }
            // Increment CAS version
            rwo->op.version++;
            if (use_cas)
                next_write(rwo);
            else
                autofree_op(rwo);
            delete subop;
        };
        parent->cli->execute(subop);
    }

    void delete_offset(inode_t inode_num, uint64_t offset)
    {
        cluster_op_t *subop = new cluster_op_t;
        subop->opcode = OSD_OP_DELETE;
        subop->inode = inode_num;
        subop->offset = offset;
        subop->len = 0;
        subop->callback = [this](cluster_op_t *subop)
        {
            if (subop->retval != 0)
            {
                fprintf(stderr, "error deleting from layer 0x%lx at offset %lx: %s", subop->inode, subop->offset, strerror(-subop->retval));
            }
            delete subop;
        };
        parent->cli->execute(subop);
    }

    void autofree_op(snap_rw_op_t *rwo)
    {
        if (!rwo->todo)
        {
            if (last_written_offset < rwo->op.offset+target_block_size)
            {
                last_written_offset = rwo->op.offset+target_block_size;
            }
            if (delete_source)
            {
                deleted_unsynced++;
                if (deleted_unsynced >= fsync_interval)
                {
                    uint64_t from = last_fsync_offset, to = last_written_offset;
                    cluster_op_t *subop = new cluster_op_t;
                    subop->opcode = OSD_OP_SYNC;
                    subop->callback = [this, from, to](cluster_op_t *subop)
                    {
                        delete subop;
                        // We can now delete source data between <from> and <to>
                        // But to do this we have to keep all object lists in memory :-(
                        for (auto & lp: layer_list_pos)
                        {
                            auto & layer_list = layer_lists.at(lp.first);
                            uint64_t layer_block = layer_block_size.at(lp.first);
                            int cur_pos = lp.second;
                            while (cur_pos < layer_list.size() && layer_list[cur_pos]+layer_block < to)
                            {
                                delete_offset(lp.first, layer_list[cur_pos]);
                                cur_pos++;
                            }
                            lp.second = cur_pos;
                        }
                    };
                    parent->cli->execute(subop);
                }
            }
            free(rwo->buf);
            delete rwo;
            in_flight--;
            continue_merge_reent();
        }
    }
};

// Flatten a layer: merge all parents into a layer and break the connection completely
struct snap_flattener_t
{
    cli_tool_t *parent;

    // target to flatten
    std::string target_name;
    // writers are stopped, we can safely change writable layers
    bool writers_stopped = false;
    // use CAS writes (0 = never, 1 = auto, 2 = always)
    int use_cas = 1;
    // interval between fsyncs
    int fsync_interval = 128;

    std::string top_parent_name;
    inode_t target_id = 0;
    int state = 0;
    snap_merger_t *merger = NULL;

    void get_merge_parents()
    {
        // Get all parents of target
        inode_config_t *target_cfg = parent->get_inode_cfg(target_name);
        target_id = target_cfg->num;
        std::vector<inode_t> chain_list;
        inode_config_t *cur = target_cfg;
        chain_list.push_back(cur->num);
        while (cur->parent_id != 0 && cur->parent_id != target_cfg->num)
        {
            auto it = parent->cli->st_cli.inode_config.find(cur->parent_id);
            if (it == parent->cli->st_cli.inode_config.end())
            {
                fprintf(stderr, "Parent inode of layer %s (id %ld) not found\n", cur->name.c_str(), cur->parent_id);
                exit(1);
            }
            cur = &it->second;
            chain_list.push_back(cur->num);
        }
        if (cur->parent_id != 0)
        {
            fprintf(stderr, "Layer %s has a loop in parents\n", target_name.c_str());
            exit(1);
        }
        top_parent_name = cur->name;
    }

    bool is_done()
    {
        return state == 5;
    }

    void loop()
    {
        if (state == 1)
            goto resume_1;
        else if (state == 2)
            goto resume_2;
        else if (state == 3)
            goto resume_3;
        // Get parent layers
        get_merge_parents();
        // Start merger
        merger = new snap_merger_t();
        merger->parent = parent;
        merger->from_name = top_parent_name;
        merger->to_name = target_name;
        merger->target_name = target_name;
        merger->delete_source = false;
        merger->use_cas = this->use_cas;
        merger->fsync_interval = this->fsync_interval;
        merger->start_merge();
        // Wait for it
        while (!merger->is_done())
        {
            state = 1;
            return;
resume_1:
            merger->continue_merge_reent();
        }
        delete merger;
        // Change parent
        parent->change_parent(target_id, 0);
        // Wait for it to complete
        state = 2;
resume_2:
        if (parent->waiting > 0)
            return;
        state = 3;
resume_3:
        // Done
        return;
    }
};

// Remove layer(s): similar to merge, but alters metadata and processes multiple merge targets
// If the requested snapshot chain has only 1 child and --writers-stopped is specified
// then that child can be merged "down" into the snapshot chain.
// Otherwise we iterate over all children of the chain, merge removed parents into them,
// and delete children afterwards.
//
// Example:
//
// <parent> - <from> - <layer 2> - <to> - <child 1>
//                 \           \       \- <child 2>
//                  \           \- <child 3>
//                   \-<child 4>
//
// 1) Merge <from>..<to> to <child 2>
// 2) Set <child 2> parent to <parent>
// 3) Variant #1, trickier, beneficial when <child 1> has less data than <to>
//    (not implemented yet):
//    - Merge <to>..<child 1> to <to>
//    - Rename <to> to <child 1>
//      It can be done without extra precautions if <child 1> is a read-only layer itself
//      Otherwise it should be either done offline or by pausing writers
//    - <to> is now deleted, repeat deletion with <from>..<layer 2>
// 4) Variant #2, simple:
//    - Repeat 1-2 with <child 1>
//    - Delete <to>
// 5) Process all other children
struct snap_remover_t
{
    cli_tool_t *parent;

    // remove from..to
    std::string from_name, to_name;
    // writers are stopped, we can safely change writable layers
    bool writers_stopped = false;
    // use CAS writes (0 = never, 1 = auto, 2 = always)
    int use_cas = 1;
    // interval between fsyncs
    int fsync_interval = 128;

    std::vector<inode_t> merge_children;
    std::vector<inode_t> chain_list;
    inode_t new_parent = 0;
    int state = 0;
    int current_child = 0;
    snap_merger_t *merger = NULL;
    rm_inode_t *remover = NULL;

    void get_merge_children()
    {
        // Get all children of from..to
        inode_config_t *from_cfg = parent->get_inode_cfg(from_name);
        inode_config_t *to_cfg = parent->get_inode_cfg(to_name);
        // Check that to_cfg is actually a child of from_cfg
        // FIXME de-copypaste the following piece of code with snap_merger_t
        inode_config_t *cur = to_cfg;
        chain_list.push_back(cur->num);
        while (cur->num != from_cfg->num && cur->parent_id != 0)
        {
            auto it = parent->cli->st_cli.inode_config.find(cur->parent_id);
            if (it == parent->cli->st_cli.inode_config.end())
            {
                fprintf(stderr, "Parent inode of layer %s (id %ld) not found\n", cur->name.c_str(), cur->parent_id);
                exit(1);
            }
            cur = &it->second;
            chain_list.push_back(cur->num);
        }
        if (cur->num != from_cfg->num)
        {
            fprintf(stderr, "Layer %s is not a child of %s\n", to_name.c_str(), from_name.c_str());
            exit(1);
        }
        new_parent = from_cfg->parent_id;
        // Calculate ranks
        std::map<inode_t,int> sources;
        int i = chain_list.size()-1;
        for (inode_t item: chain_list)
        {
            sources[item] = i--;
        }
        for (auto & ic: parent->cli->st_cli.inode_config)
        {
            if (!ic.second.parent_id)
            {
                continue;
            }
            auto it = sources.find(ic.second.parent_id);
            if (it != sources.end() && sources.find(ic.second.num) == sources.end())
            {
                merge_children.push_back(ic.second.num);
            }
        }
    }

    bool is_done()
    {
        return state == 5;
    }

    void loop()
    {
        if (state == 1)
            goto resume_1;
        else if (state == 2)
            goto resume_2;
        else if (state == 3)
            goto resume_3;
        else if (state == 4)
            goto resume_4;
        else if (state == 5)
            goto resume_5;
        // Get children to merge
        get_merge_children();
        // Merge children one by one
        for (current_child = 0; current_child < merge_children.size(); current_child++)
        {
            start_merge_child();
            while (!merger->is_done())
            {
                state = 1;
                return;
resume_1:
                merger->continue_merge_reent();
            }
            delete merger;
            parent->change_parent(merge_children[current_child], new_parent);
            state = 2;
resume_2:
            if (parent->waiting > 0)
                return;
        }
        // Delete sources
        for (current_child = 0; current_child < chain_list.size(); current_child++)
        {
            start_delete_source();
            while (!remover->is_done())
            {
                state = 3;
                return;
resume_3:
                remover->continue_delete();
            }
            delete remover;
            delete_inode_config(chain_list[current_child]);
            state = 4;
resume_4:
            if (parent->waiting > 0)
                return;
        }
        state = 5;
resume_5:
        // Done
        return;
    }

    void delete_inode_config(inode_t cur)
    {
        auto cur_cfg_it = parent->cli->st_cli.inode_config.find(cur);
        if (cur_cfg_it == parent->cli->st_cli.inode_config.end())
        {
            fprintf(stderr, "Inode 0x%lx disappeared\n", cur);
            exit(1);
        }
        inode_config_t *cur_cfg = &cur_cfg_it->second;
        std::string cur_name = cur_cfg->name;
        std::string cur_cfg_key = base64_encode(parent->cli->st_cli.etcd_prefix+
            "/config/inode/"+std::to_string(INODE_POOL(cur))+
            "/"+std::to_string(INODE_NO_POOL(cur)));
        parent->waiting++;
        parent->cli->st_cli.etcd_txn(json11::Json::object {
            { "compare", json11::Json::array {
                json11::Json::object {
                    { "target", "MOD" },
                    { "key", cur_cfg_key },
                    { "result", "LESS" },
                    { "mod_revision", cur_cfg->mod_revision+1 },
                },
            } },
            { "success", json11::Json::array {
                json11::Json::object {
                    { "request_delete_range", json11::Json::object {
                        { "key", cur_cfg_key },
                    } }
                },
            } },
        }, ETCD_SLOW_TIMEOUT, [this, cur_name](std::string err, json11::Json res)
        {
            if (err != "")
            {
                fprintf(stderr, "Error deleting %s: %s\n", cur_name.c_str(), err.c_str());
                exit(1);
            }
            if (!res["succeeded"].bool_value())
            {
                fprintf(stderr, "Layer %s configuration was modified during deletion\n", cur_name.c_str());
                exit(1);
            }
            printf("Layer %s deleted\n", cur_name.c_str());
            parent->waiting--;
            parent->ringloop->wakeup();
        });
    }

    void start_merge_child()
    {
        auto target = parent->cli->st_cli.inode_config.find(merge_children[current_child]);
        if (target == parent->cli->st_cli.inode_config.end())
        {
            fprintf(stderr, "Inode %ld disappeared\n", merge_children[current_child]);
            exit(1);
        }
        merger = new snap_merger_t();
        merger->parent = parent;
        merger->from_name = from_name;
        merger->to_name = target->second.name;
        merger->target_name = target->second.name;
        merger->delete_source = false;
        merger->use_cas = this->use_cas;
        merger->fsync_interval = this->fsync_interval;
        merger->start_merge();
    }

    void start_delete_source()
    {
        auto source = parent->cli->st_cli.inode_config.find(chain_list[current_child]);
        if (source == parent->cli->st_cli.inode_config.end())
        {
            fprintf(stderr, "Inode %ld disappeared\n", chain_list[current_child]);
            exit(1);
        }
        remover = new rm_inode_t();
        remover->parent = parent;
        remover->inode = chain_list[current_child];
        remover->pool_id = INODE_POOL(remover->inode);
        remover->start_delete();
    }
};

void cli_tool_t::run(json11::Json cfg)
{
    json11::Json::array cmd = cfg["command"].array_items();
    if (!cmd.size())
    {
        fprintf(stderr, "command is missing\n");
        exit(1);
    }
    else if (cmd[0] == "rm")
    {
        // Delete inode
        remover = new rm_inode_t();
        remover->parent = this;
        remover->inode = cfg["inode"].uint64_value();
        remover->pool_id = cfg["pool"].uint64_value();
        if (remover->pool_id)
            remover->inode = (remover->inode & ((1l << (64-POOL_ID_BITS)) - 1)) | (((uint64_t)remover->pool_id) << (64-POOL_ID_BITS));
        remover->pool_id = INODE_POOL(remover->inode);
        if (!remover->pool_id)
        {
            fprintf(stderr, "pool is missing\n");
            exit(1);
        }
    }
    else if (cmd[0] == "merge")
    {
        // Merge layer data without affecting metadata
        merger = new snap_merger_t();
        merger->parent = this;
        merger->from_name = cmd.size() > 1 ? cmd[1].string_value() : "";
        merger->to_name = cmd.size() > 2 ? cmd[2].string_value() : "";
        merger->target_name = cfg["target"].string_value();
        if (merger->from_name == "" || merger->to_name == "")
        {
            fprintf(stderr, "Beginning or end of the merge sequence is missing\n");
            exit(1);
        }
        merger->delete_source = cfg["delete-source"].string_value() != "";
        merger->fsync_interval = cfg["fsync-interval"].uint64_value();
        if (!merger->fsync_interval)
            merger->fsync_interval = 128;
        if (!cfg["cas"].is_null())
            merger->use_cas = cfg["cas"].uint64_value() ? 2 : 0;
    }
    else if (cmd[0] == "flatten")
    {
        // Merge layer data without affecting metadata
        flattener = new snap_flattener_t();
        flattener->parent = this;
        flattener->target_name = cmd.size() > 1 ? cmd[1].string_value() : "";
        if (flattener->target_name == "")
        {
            fprintf(stderr, "Layer to flatten argument is missing\n");
            exit(1);
        }
        flattener->fsync_interval = cfg["fsync-interval"].uint64_value();
        if (!flattener->fsync_interval)
            flattener->fsync_interval = 128;
        if (!cfg["cas"].is_null())
            flattener->use_cas = cfg["cas"].uint64_value() ? 2 : 0;
    }
    else if (cmd[0] == "snap-rm")
    {
        // Remove multiple snapshots and rebase their children
        snap_remover = new snap_remover_t();
        snap_remover->parent = this;
        snap_remover->from_name = cmd.size() > 1 ? cmd[1].string_value() : "";
        snap_remover->to_name = cmd.size() > 2 ? cmd[2].string_value() : "";
        if (snap_remover->from_name == "")
        {
            fprintf(stderr, "Layer to remove argument is missing\n");
            exit(1);
        }
        if (snap_remover->to_name == "")
        {
            snap_remover->to_name = snap_remover->from_name;
        }
        snap_remover->fsync_interval = cfg["fsync-interval"].uint64_value();
        if (!snap_remover->fsync_interval)
            snap_remover->fsync_interval = 128;
        if (!cfg["cas"].is_null())
            snap_remover->use_cas = cfg["cas"].uint64_value() ? 2 : 0;
    }
    else
    {
        fprintf(stderr, "unknown command: %s\n", cmd[0].string_value().c_str());
        exit(1);
    }
    iodepth = cfg["iodepth"].uint64_value();
    if (!iodepth)
        iodepth = 32;
    parallel_osds = cfg["parallel_osds"].uint64_value();
    if (!parallel_osds)
        parallel_osds = 4;
    log_level = cfg["log_level"].int64_value();
    progress = cfg["progress"].uint64_value() ? true : false;
    list_first = cfg["wait-list"].uint64_value() ? true : false;
    // Create client
    ringloop = new ring_loop_t(512);
    epmgr = new epoll_manager_t(ringloop);
    cli = new cluster_client_t(ringloop, epmgr->tfd, cfg);
    cli->on_ready([this]() { start_work(); });
    // Initialize job
    consumer.loop = [this]()
    {
        if (started)
            continue_work();
        ringloop->submit();
    };
    ringloop->register_consumer(&consumer);
    // Loop until it completes
    while (1)
    {
        ringloop->loop();
        ringloop->wait();
    }
}

void cli_tool_t::start_work()
{
    if (remover)
    {
        remover->start_delete();
    }
    else if (merger)
    {
        merger->start_merge();
    }
    else if (flattener)
    {
        flattener->loop();
    }
    else if (snap_remover)
    {
        snap_remover->loop();
    }
    started = true;
}

void cli_tool_t::continue_work()
{
    if (remover)
    {
        remover->continue_delete();
        if (remover->is_done())
        {
            exit(0);
        }
    }
    else if (merger)
    {
        merger->continue_merge_reent();
        if (merger->is_done())
        {
            exit(0);
        }
    }
    else if (flattener)
    {
        flattener->loop();
        if (flattener->is_done())
        {
            exit(0);
        }
    }
    else if (snap_remover)
    {
        snap_remover->loop();
        if (snap_remover->is_done())
        {
            exit(0);
        }
    }
}

int main(int narg, const char *args[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    exe_name = args[0];
    cli_tool_t *p = new cli_tool_t();
    p->run(cli_tool_t::parse_args(narg, args));
    return 0;
}
