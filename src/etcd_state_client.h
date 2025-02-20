// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 or GNU GPL-2.0+ (see README.md for details)

#pragma once

#include "json11/json11.hpp"
#include "osd_id.h"
#include "timerfd_manager.h"

#define ETCD_CONFIG_WATCH_ID 1
#define ETCD_PG_STATE_WATCH_ID 2
#define ETCD_PG_HISTORY_WATCH_ID 3
#define ETCD_OSD_STATE_WATCH_ID 4

#define MAX_ETCD_ATTEMPTS 5
#define ETCD_SLOW_TIMEOUT 5000
#define ETCD_QUICK_TIMEOUT 1000

#define DEFAULT_BLOCK_SIZE 128*1024

struct etcd_kv_t
{
    std::string key;
    json11::Json value;
    uint64_t mod_revision;
};

struct pg_config_t
{
    bool exists;
    osd_num_t primary;
    std::vector<osd_num_t> target_set;
    std::vector<std::vector<osd_num_t>> target_history;
    std::vector<osd_num_t> all_peers;
    bool pause;
    osd_num_t cur_primary;
    int cur_state;
    uint64_t epoch;
};

struct pool_config_t
{
    bool exists;
    pool_id_t id;
    std::string name;
    uint64_t scheme;
    uint64_t pg_size, pg_minsize, parity_chunks;
    uint64_t pg_count;
    uint64_t real_pg_count;
    std::string failure_domain;
    uint64_t max_osd_combinations;
    uint64_t pg_stripe_size;
    std::map<pg_num_t, pg_config_t> pg_config;
};

struct inode_config_t
{
    uint64_t num;
    std::string name;
    uint64_t size;
    inode_t parent_id;
    bool readonly;
    // Change revision of the metadata in etcd
    uint64_t mod_revision;
};

struct inode_watch_t
{
    std::string name;
    inode_config_t cfg;
};

struct websocket_t;

struct etcd_state_client_t
{
protected:
    std::vector<inode_watch_t*> watches;
    websocket_t *etcd_watch_ws = NULL;
    uint64_t bs_block_size = DEFAULT_BLOCK_SIZE;
    void add_etcd_url(std::string);
public:
    std::vector<std::string> etcd_addresses;
    std::string etcd_prefix;
    int log_level = 0;
    timerfd_manager_t *tfd = NULL;

    int etcd_watches_initialised = 0;
    uint64_t etcd_watch_revision = 0;
    std::map<pool_id_t, pool_config_t> pool_config;
    std::map<osd_num_t, json11::Json> peer_states;
    std::map<inode_t, inode_config_t> inode_config;
    std::map<std::string, inode_t> inode_by_name;

    std::function<void(std::map<std::string, etcd_kv_t> &)> on_change_hook;
    std::function<void(json11::Json::object &)> on_load_config_hook;
    std::function<json11::Json()> load_pgs_checks_hook;
    std::function<void(bool)> on_load_pgs_hook;
    std::function<void(pool_id_t, pg_num_t)> on_change_pg_history_hook;
    std::function<void(osd_num_t)> on_change_osd_state_hook;

    json11::Json::object & serialize_inode_cfg(inode_config_t *cfg);
    etcd_kv_t parse_etcd_kv(const json11::Json & kv_json);
    void etcd_call(std::string api, json11::Json payload, int timeout, std::function<void(std::string, json11::Json)> callback);
    void etcd_txn(json11::Json txn, int timeout, std::function<void(std::string, json11::Json)> callback);
    void start_etcd_watcher();
    void load_global_config();
    void load_pgs();
    void parse_state(const etcd_kv_t & kv);
    void parse_config(const json11::Json & config);
    inode_watch_t* watch_inode(std::string name);
    void close_watch(inode_watch_t* watch);
    ~etcd_state_client_t();
};
