// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

// Common CLI tool header

#pragma once

#include "json11/json11.hpp"
#include "object_id.h"
#include "ringloop.h"
#include <functional>

struct rm_inode_t;
struct snap_merger_t;
struct snap_flattener_t;
struct snap_remover_t;

class epoll_manager_t;
class cluster_client_t;
struct inode_config_t;

class cli_tool_t
{
public:
    uint64_t iodepth = 0, parallel_osds = 0;
    bool progress = true;
    bool list_first = false;
    int log_level = 0;
    int mode = 0;

    ring_loop_t *ringloop = NULL;
    epoll_manager_t *epmgr = NULL;
    cluster_client_t *cli = NULL;

    int waiting = 0;
    ring_consumer_t consumer;
    std::function<bool(void)> action_cb;

    void run(json11::Json cfg);

    void change_parent(inode_t cur, inode_t new_parent);
    inode_config_t* get_inode_cfg(const std::string & name);

    static json11::Json::object parse_args(int narg, const char *args[]);
    static void help();

    friend struct rm_inode_t;
    friend struct snap_merger_t;
    friend struct snap_flattener_t;
    friend struct snap_remover_t;

    std::function<bool(void)> start_rm(json11::Json);
    std::function<bool(void)> start_merge(json11::Json);
    std::function<bool(void)> start_flatten(json11::Json);
    std::function<bool(void)> start_snap_rm(json11::Json);
};
