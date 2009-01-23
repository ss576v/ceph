// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "config.h"
#include "include/types.h"

#include "common/Clock.h"

#include <fstream>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

// for tstring stringtable
#include "include/tstring.h"
stringtable g_stab;

// hack hack hack ugly FIXME
#include "include/atomic.h"
atomic_t buffer_total_alloc;

#include "osd/osd_types.h"

#include "common/ConfUtils.h"

int buffer::list::read_file(const char *fn)
{
  struct stat st;
  int fd = ::open(fn, O_RDONLY);
  if (fd < 0) {
    cerr << "can't open " << fn << ": " << strerror(errno) << std::endl;
    return -errno;
  }
  ::fstat(fd, &st);
  int s = ROUND_UP_TO(st.st_size, PAGE_SIZE);
  bufferptr bp = buffer::create_page_aligned(s);
  bp.set_length(st.st_size);
  append(bp);
  ::read(fd, (void*)c_str(), length());
  ::close(fd);
  return 0;
}

int buffer::list::write_file(const char *fn)
{
  int fd = ::open(fn, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if (fd < 0) {
    cerr << "can't write " << fn << ": " << strerror(errno) << std::endl;
    return -errno;
  }
  for (std::list<ptr>::const_iterator it = _buffers.begin(); 
       it != _buffers.end(); 
       it++) {
    const char *c = it->c_str();
    int left = it->length();
    while (left > 0) {
      int r = ::write(fd, c, left);
      if (r < 0) {
	::close(fd);
	return -errno;
      }
      c += r;
      left -= r;
    }
  }
  ::close(fd);
  return 0;
}



// page size crap, see page.h
int _get_bits_of(int v) {
  int n = 0;
  while (v) {
    n++;
    v = v >> 1;
  }
  return n;
}
unsigned _page_size = sysconf(_SC_PAGESIZE);
unsigned long _page_mask = ~(unsigned long)(_page_size - 1);
unsigned _page_shift = _get_bits_of(_page_size);

atomic_t _num_threads(0);

// file layouts
struct ceph_file_layout g_default_file_layout = {
 fl_stripe_unit: init_le32(1<<22),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<22),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred: init_le32(-1),
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2,
 fl_pg_pool: 1
};

struct ceph_file_layout g_default_casdata_layout = {
 fl_stripe_unit: init_le32(1<<22),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<22),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred: init_le32(-1),
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2,
 fl_pg_pool: 2
};

struct ceph_file_layout g_default_mds_dir_layout = {
 fl_stripe_unit: init_le32(1<<22),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<22),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred: init_le32(-1),
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2,
 fl_pg_pool: 0
};

struct ceph_file_layout g_default_mds_log_layout = {
 fl_stripe_unit: init_le32(1<<20),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<20),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred: init_le32(-1),
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2,
 fl_pg_pool: 0
};

struct ceph_file_layout g_default_mds_anchortable_layout = {
 fl_stripe_unit: init_le32(1<<20),
 fl_stripe_count: init_le32(1),
 fl_object_size: init_le32(1<<20),
 fl_cas_hash: init_le32(0),
 fl_object_stripe_unit: init_le32(0),
 fl_pg_preferred: init_le32(-1),
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2,
 fl_pg_pool: 0
};

const char *get_pool_name(int pool) 
{
  switch (pool) {
  case 0: return "metadata";
  case 1: return "data";
  case 2: return "casdata";
  default: return "";
  }
}

#include <msg/msg_types.h>

// fake osd failures: osd -> time
std::map<entity_name_t,float> g_fake_kill_after;

entity_addr_t g_my_addr;

md_config_t g_debug_after_conf;

md_config_t g_conf = {
  num_mon: 1,
  num_mds: 1,
  num_osd: 4,
  num_client: 1,

  mkfs: false,

  mon_host: 0,
  daemonize: false,
  file_logs: false,

  // profiling and debugging
  log: true,
  log_interval: 1,
  log_name: (char*)0,

  log_messages: true,
  log_pins: true,

  logger_calc_variance: true,

  dout_dir: "out",    // if daemonize == true
  dout_sym_dir: "out",    // if daemonize == true

  conf_file: "ceph.conf",
  
  fake_clock: false,
  fakemessenger_serialize: true,

  osd_remount_at: 0,

  kill_after: 0,

  tick: 0,

  debug: 0,
  debug_lockdep: 0,
  debug_mds: 1,
  debug_mds_balancer: 1,
  debug_mds_log: 1,
  debug_mds_log_expire: 1,
  debug_mds_migrator: 1,
  debug_buffer: 0,
  debug_timer: 0,
  debug_filer: 0,
  debug_objecter: 0,
  debug_journaler: 0,
  debug_objectcacher: 0,
  debug_client: 0,
  debug_osd: 0,
  debug_ebofs: 1,
  debug_filestore: 1,
  debug_journal: 1,
  debug_bdev: 1,         // block device
  debug_ns: 0,
  debug_ms: 0,
  debug_mon: 1,
  debug_paxos: 0,
  debug_tp: 0,
  
  debug_after: 0,
  
  // -- misc --
  use_abspaths: false,      // make monitorstore et al use absolute path (to workaround FUSE chdir("/"))

  // --- clock ---
  clock_lock: false,
  clock_tare: false,
  
  // --- messenger ---
  ms_hosts: ".ceph_hosts",
  ms_tcp_nodelay: true,
  ms_retry_interval: 2.0,  // how often to attempt reconnect 
  ms_fail_interval: 15.0,  // fail after this long
  ms_die_on_failure: false,

  ms_stripe_osds: false,
  ms_skip_rank0: false,
  ms_overlay_clients: false,


  // --- mon ---
  mon_tick_interval: 5,
  mon_osd_down_out_interval: 5,  // seconds
  mon_lease: 5,  // seconds    // lease interval
  mon_lease_renew_interval: 3, // on leader, to renew the lease
  mon_lease_ack_timeout: 10.0, // on leader, if lease isn't acked by all peons
  mon_lease_timeout: 10.0,     // on peon, if lease isn't extended
  mon_accept_timeout: 10.0,    // on leader, if paxos update isn't accepted
  mon_stop_on_last_unmount: false,
  mon_stop_with_last_mds: false,
  mon_allow_mds_bully: false,   // allow a booting mds to (forcibly) claim an mds # .. FIXME
  mon_pg_create_interval: 30.0, // no more than every 30s

  paxos_propose_interval: 1.0,  // gather updates for this long before proposing a map update
  paxos_observer_timeout: 5*60,  // gather updates for this long before proposing a map update

  // --- client ---
  client_cache_size: 1000,
  client_cache_mid: .5,
  client_cache_stat_ttl: 0, // seconds until cached stat results become invalid
  client_cache_readdir_ttl: 1,  // 1 second only
  client_use_random_mds:  false,
  client_mount_timeout: 10.0,  // retry every N seconds
  client_tick_interval: 1.0,
  client_hack_balance_reads: false,
  client_trace: 0,
  client_readahead_min: 128*1024,  // readahead at _least_ this much.
  client_readahead_max_bytes: 0,//8 * 1024*1024,
  client_readahead_max_periods: 4,  // as multiple of file layout period (object size * num stripes)
  client_snapdir: ".snap",
  fuse_direct_io: 0,
  fuse_ll: true,
  
  // --- objectcacher ---
  client_oc: true,
  client_oc_size:      1024*1024* 64,    // MB * n
  client_oc_max_dirty: 1024*1024* 48,    // MB * n  (dirty OR tx.. bigish)
  client_oc_target_dirty:  1024*1024* 8, // target dirty (keep this smallish)
  // note: the max amount of "in flight" dirty data is roughly (max - target)
  client_oc_max_sync_write: 128*1024,   // sync writes >= this use wrlock

  // --- objecter ---
  objecter_buffer_uncommitted: true,  // this must be true for proper failure handling
  objecter_map_request_interval: 15.0, // request a new map every N seconds, if we have pending io
  objecter_tick_interval: 5.0,
  objecter_timeout: 10.0,    // before we ask for a map

  // --- journaler ---
  journaler_allow_split_entries: true,
  journaler_safe: true,  // wait for COMMIT on journal writes
  journaler_write_head_interval: 15,
  journaler_cache: false, // cache writes for later readback
  journaler_prefetch_periods: 50,   // * journal object size (1~MB? see above)
  journaler_batch_interval: .001,   // seconds.. max add'l latency we artificially incur
  //journaler_batch_max: 16384,        // max bytes we'll delay flushing
  journaler_batch_max: 0,  // disable, for now....

  // --- mds ---
  mds_cache_size: 300000,
  mds_cache_mid: .7,

  mds_decay_halflife: 5,

  mds_beacon_interval: 4, //30.0,
  mds_beacon_grace: 30, //60*60.0,
  mds_blacklist_interval: 24.0*60.0,  // how long to blacklist failed nodes

  mds_session_timeout: 60,    // cap bits and leases time out if client idle
  mds_session_autoclose: 300, // autoclose idle session 
  mds_client_lease: 120,      // (assuming session stays alive)
  mds_reconnect_timeout: 30,  // seconds to wait for clients during mds restart
                              //  make it (mds_session_timeout - mds_beacon_grace)

  mds_tick_interval: 5,
  mds_scatter_nudge_interval: 5,  // how quickly dirstat changes propagate up the hierarchy

  mds_client_prealloc_inos: 100,
  mds_early_reply: true,

  mds_rdcap_ttl_ms: 60*1000,

  mds_log: true,
  mds_log_unsafe: false,      // only wait for log sync, when it's mostly safe to do so
  mds_log_max_events: -1,
  mds_log_max_segments: 100,  // segment size defined by FileLayout, above
  mds_log_max_expiring: 20,
  mds_log_pad_entry: 128,//256,//64,
  mds_log_eopen_size: 100,   // # open inodes per log entry

  mds_bal_sample_interval: 3.0,  // every 5 seconds
  mds_bal_replicate_threshold: 8000,
  mds_bal_unreplicate_threshold: 0,//500,
  mds_bal_frag: true,
  mds_bal_split_size: 10000,
  mds_bal_split_rd: 25000,
  mds_bal_split_wr: 10000,
  mds_bal_merge_size: 50,
  mds_bal_merge_rd: 1000,
  mds_bal_merge_wr: 1000,
  mds_bal_interval: 10,           // seconds
  mds_bal_fragment_interval: -1,      // seconds
  mds_bal_idle_threshold: 0, //.1,
  mds_bal_max: -1,
  mds_bal_max_until: -1,

  mds_bal_mode: 0,
  mds_bal_min_rebalance: .1,  // must be this much above average before we export anything
  mds_bal_min_start: .2,      // if we need less than this, we don't do anything
  mds_bal_need_min: .8,       // take within this range of what we need
  mds_bal_need_max: 1.2,
  mds_bal_midchunk: .3,       // any sub bigger than this taken in full
  mds_bal_minchunk: .001,     // never take anything smaller than this

  mds_trim_on_rejoin: true,
  mds_shutdown_check: 0, //30,

  mds_verify_export_dirauth: true,

  mds_local_osd: false,

  mds_thrash_exports: 0,
  mds_thrash_fragments: 0,
  mds_dump_cache_on_map: false,
  mds_dump_cache_after_rejoin: true,

  mds_hack_log_expire_for_better_stats: false,

  // --- osd ---
  osd_rep: OSD_REP_PRIMARY,

  osd_balance_reads: false,  // send from client to replica
  osd_flash_crowd_iat_threshold: 0,//100,
  osd_flash_crowd_iat_alpha: 0.125,
  osd_balance_reads_temp: 100,
  
  osd_shed_reads: false,     // forward from primary to replica
  osd_shed_reads_min_latency: .01,       // min local latency
  osd_shed_reads_min_latency_diff: .01,  // min latency difference
  osd_shed_reads_min_latency_ratio: 1.5,  // 1.2 == 20% higher than peer

  osd_immediate_read_from_cache: false,//true, // osds to read from the cache immediately?
  osd_exclusive_caching: true,         // replicas evict replicated writes

  osd_stat_refresh_interval: .5,

  osd_min_pg_size_without_alive: 2,  // smallest pg we allow to activate without telling the monitor

  osd_pg_bits: 6,  // bits per osd
  osd_lpg_bits: 1,  // bits per osd
  osd_object_layout: CEPH_OBJECT_LAYOUT_HASHINO,//LINEAR,//HASHINO,
  osd_pg_layout: CEPH_PG_LAYOUT_CRUSH,//LINEAR,//CRUSH,
  osd_min_rep: 2,
  osd_max_rep: 3,
  osd_min_raid_width: 3,
  osd_max_raid_width: 2, //6, 

  osd_maxthreads: 2,    // 0 == no threading
  osd_max_opq: 10,
  osd_mkfs: false,
  osd_age: .8,
  osd_age_time: 0,
  osd_heartbeat_interval: 1,
  osd_mon_heartbeat_interval: 30,  // if no peers, ping monitor
  osd_heartbeat_grace: 20,
  osd_mon_report_interval:  5,  // pg stats, failures, up_thru, boot.
  osd_replay_window: 45,
  osd_max_pull: 2,
  osd_preserve_trimmed_log: true,

  osd_recovery_delay_start: 15,
  osd_recovery_max_active: 5,

  osd_auto_weight: false,

  
  // --- filestore ---
  filestore: false,
  filestore_sync_interval: .2,    // seconds
  filestore_fake_attrs: false,
  filestore_fake_collections: false,   
  filestore_dev: 0,
  filestore_btrfs_trans: true,

  // --- ebofs ---
  ebofs: false,
  ebofs_cloneable: true,
  ebofs_verify: false,
  ebofs_commit_ms:      200,       // 0 = no forced commit timeout (for debugging/tracing)
  ebofs_oc_size:        10000,      // onode cache
  ebofs_cc_size:        10000,      // cnode cache
  ebofs_bc_size:        (50 *256), // 4k blocks, *256 for MB
  ebofs_bc_max_dirty:   (30 *256), // before write() will block
  ebofs_max_prefetch: 1000, // 4k blocks
  ebofs_realloc: false,    // hrm, this can cause bad fragmentation, don't use!
  ebofs_verify_csum_on_read: true,

  // journal
  journal_dio: false,
  journal_max_write_bytes: 0,
  journal_max_write_entries: 100,

  // --- block device ---
  bdev_lock: true,
  bdev_iothreads:   1,         // number of ios to queue with kernel
  bdev_idle_kick_after_ms: 100,  // ms
  bdev_el_fw_max_ms: 10000,      // restart elevator at least once every 1000 ms
  bdev_el_bw_max_ms: 3000,       // restart elevator at least once every 300 ms
  bdev_el_bidir: false,          // bidirectional elevator?
  bdev_iov_max: 512,            // max # iov's to collect into a single readv()/writev() call
  bdev_debug_check_io_overlap: true,   // [DEBUG] check for any pending io overlaps
  bdev_fake_mb: 0,
  bdev_fake_max_mb:  0

#ifdef USE_OSBDB
  ,
  bdbstore: false,
  debug_bdbstore: 1,
  bdbstore_btree: false,
  bdbstore_ffactor: 0,
  bdbstore_nelem: 0,
  bdbstore_pagesize: 0,
  bdbstore_cachesize: 0,
  bdbstore_transactional: false
#endif // USE_OSBDB
};


#include <stdlib.h>
#include <string.h>


void env_to_vec(std::vector<const char*>& args) 
{
  char *p = getenv("CEPH_ARGS");
  if (!p) return;
  
  int len = MIN(strlen(p), 1000);  // bleh.
  static char buf[1000];  
  memcpy(buf, p, len);
  buf[len] = 0;
  //cout << "CEPH_ARGS='" << p << ";" << endl;

  p = buf;
  while (*p && p < buf + len) {
    char *e = p;
    while (*e && *e != ' ')
      e++;
    *e = 0;
    args.push_back(p);
    //cout << "arg " << p << std::endl;
    p = e+1;
  }
}


void argv_to_vec(int argc, const char **argv,
                 std::vector<const char*>& args)
{
  for (int i=1; i<argc; i++)
    args.push_back(argv[i]);
}

void vec_to_argv(std::vector<const char*>& args,
                 int& argc, const char **&argv)
{
  argv = (const char**)malloc(sizeof(char*) * argc);
  argc = 1;
  argv[0] = "asdf";

  for (unsigned i=0; i<args.size(); i++) 
    argv[argc++] = args[i];
}

bool parse_ip_port(const char *s, entity_addr_t& a)
{
  int count = 0; // digit count
  int off = 0;

  while (1) {
    // parse the #.
    int val = 0;
    int numdigits = 0;
    
    while (*s >= '0' && *s <= '9') {
      int digit = *s - '0';
      //cout << "digit " << digit << endl;
      val *= 10;
      val += digit;
      numdigits++;
      s++; off++;
    }

    if (numdigits == 0) {
      cerr << "no digits at off " << off << std::endl;
      return false;           // no digits
    }
    if (count < 3 && *s != '.') {
      cerr << "should period at " << off << std::endl;
      return false;   // should have 3 periods
    }
    s++; off++;

    if (count <= 3)
      a.set_ipquad(count, val);
    else 
      a.set_port(val);
    
    count++;
    if (count == 4 && *(s-1) != ':') break;
    if (count == 5) break;  
  }
  
  return true;
}



void parse_config_option_string(string& s)
{
  char b[s.length()+1];
  strcpy(b, s.c_str());
  vector<const char*> nargs;
  char *p = b;
  while (*p) {
    nargs.push_back(p);
    while (*p && *p != ' ') p++;
    if (!*p)
      break;
    *p++ = 0;
    while (*p && *p == ' ') p++;
  }
  parse_config_options(nargs, false);
}

void sighup_handler(int signum)
{
  _dout_need_open = true;
}


#define CF_READ(section, var, inout) \
  cf.read(section, var, &g_conf.inout, g_conf.inout)

#define CF_READ_TYPE(section, var, type, inout) \
  cf.read(section, var, (type *)&g_conf.inout, (type)g_conf.inout)

#define CF_READ_STR(section, var, inout) \
  cf.read(section, var, (char **)&g_conf.inout, (char *)g_conf.inout)

void parse_config_file(const char *fname)
{
  ConfFile cf(fname);

  cf.set_auto_update(true);

  cf.parse();

  CF_READ("global", "num_mon", num_mon);
  CF_READ("global", "num_mds", num_mds);
  CF_READ("global", "num_osd", num_osd);
  CF_READ("global", "mkfs", mkfs);
  CF_READ("global", "daemonize", daemonize);
  CF_READ("global", "file_logs", file_logs);
  CF_READ("global", "log", log);
  CF_READ("global", "log_interval", log_interval);
  CF_READ_STR("global", "log_name", log_name);
  CF_READ("global", "log_messages", log_messages);
  CF_READ("global", "log_pins", log_pins);
  CF_READ_STR("global", "dout_dir", dout_dir);
  CF_READ_STR("global", "dout_sym_dir", dout_sym_dir);

  CF_READ("debug", "debug", debug);
  CF_READ("debug", "debug_lockdep", debug_lockdep);
  CF_READ("debug", "debug_mds", debug_mds);
  CF_READ("debug", "debug_mds_balancer", debug_mds_balancer);
  CF_READ("debug", "debug_mds_log_expire", debug_mds_log_expire);
  CF_READ("debug", "debug_buffer", debug_buffer);
  CF_READ("debug", "debug_filer", debug_filer);
  CF_READ("debug", "debug_journaler", debug_journaler);
  CF_READ("debug", "debug_client", debug_client);
  CF_READ("debug", "debug_ebofs", debug_ebofs);
  CF_READ("debug", "debug_journal", debug_journal);
  CF_READ("debug", "debug_ns", debug_ns);
  CF_READ("debug", "debug_mon", debug_mon);
  CF_READ("debug", "debug_tp", debug_tp);
  CF_READ("debug", "use_abspaths", use_abspaths);

  CF_READ("clock", "clock_lock", clock_lock);
  CF_READ("clock", "clock_tare", clock_tare);

  CF_READ("messenger", "ms_tcp_nodelay", ms_tcp_nodelay);
  CF_READ("messenger", "ms_retry_interval", ms_retry_interval);
  CF_READ("messenger", "ms_fail_interval", ms_fail_interval);
  CF_READ("messenger", "ms_die_on_failure", ms_die_on_failure);
  CF_READ("messenger", "ms_stripe_osds", ms_stripe_osds);
  CF_READ("messenger", "ms_skip_rank0", ms_skip_rank0);
  CF_READ("messenger", "ms_overlay_clients", ms_overlay_clients);

  CF_READ("mon", "mon_tick_interval", mon_tick_interval);
  CF_READ("mon", "mon_osd_down_out_interval", mon_osd_down_out_interval);
  CF_READ("mon", "mon_lease", mon_lease);
  CF_READ("mon", "mon_lease_renew_interval", mon_lease_renew_interval);
  CF_READ("mon", "mon_lease_ack_timeout", mon_lease_ack_timeout);
  CF_READ("mon", "mon_lease_timeout", mon_lease_timeout);
  CF_READ("mon", "mon_accept_timeout", mon_accept_timeout);
  CF_READ("mon", "mon_stop_on_last_unmount", mon_stop_on_last_unmount);
  CF_READ("mon", "mon_stop_with_last_mds", mon_stop_with_last_mds);
  CF_READ("mon", "mon_allow_mds_bully", mon_allow_mds_bully);
  CF_READ("mon", "mon_pg_create_interval", mon_pg_create_interval);

  CF_READ("paxos", "paxos_propose_interval", paxos_propose_interval);
  CF_READ("paxos", "paxos_observer_timeout", paxos_observer_timeout);

  CF_READ("client", "client_cache_size", client_cache_size);
  CF_READ("client", "client_cache_mid", client_cache_mid);
  CF_READ("client", "client_cache_stat_ttl", client_cache_stat_ttl);
  CF_READ("client", "client_cache_readdir_ttl", client_cache_readdir_ttl);
  CF_READ("client", "client_use_random_mds", client_use_random_mds);
  CF_READ("client", "client_mount_timeout", client_mount_timeout);
  CF_READ("client", "client_tick_interval", client_tick_interval);
  CF_READ("client", "client_hack_balance_reads", client_hack_balance_reads);
  CF_READ_STR("client", "client_trace", client_trace);
  CF_READ_TYPE("client", "client_readahead_min", long long, client_readahead_min);
  CF_READ_TYPE("client", "client_readahead_max_bytes", long long, client_readahead_max_bytes);
  CF_READ_TYPE("client", "client_readahead_max_periods", long long, client_readahead_max_periods);
  CF_READ_STR("client", "client_snapdir", client_snapdir);

  CF_READ("fuse", "fuse_direct_io", fuse_direct_io);
  CF_READ("fuse", "fuse_ll", fuse_ll);

  CF_READ("client_oc", "client_oc", client_oc);
  CF_READ("client_oc", "client_oc_size", client_oc_size);
  CF_READ("client_oc", "client_oc_max_dirty", client_oc_max_dirty);
  CF_READ("client_oc", "client_oc_target_dirty", client_oc_target_dirty);
  CF_READ_TYPE("client_oc", "client_oc_max_sync_write", unsigned long long, client_oc_max_sync_write);

  CF_READ("objecter", "objecter_buffer_uncommitted", objecter_buffer_uncommitted);
  CF_READ("objecter", "objecter_map_request_interval", objecter_map_request_interval);
  CF_READ("objecter", "objecter_tick_interval", objecter_tick_interval);
  CF_READ("objecter", "objecter_timeout", objecter_timeout);

  CF_READ("journaler", "journaler_allow_split_entries", journaler_allow_split_entries);
  CF_READ("journaler", "journaler_safe", journaler_safe);
  CF_READ("journaler", "journaler_write_head_interval", journaler_write_head_interval);
  CF_READ("journaler", "journaler_cache", journaler_cache);
  CF_READ("journaler", "journaler_prefetch_periods", journaler_prefetch_periods);
  CF_READ("journaler", "journaler_batch_interval", journaler_batch_interval);
  CF_READ_TYPE("journaler", "journaler_batch_max", unsigned long long, journaler_batch_max);

  CF_READ("mds", "mds_cache_size", mds_cache_size);
  CF_READ("mds", "mds_cache_mid", mds_cache_mid);
  CF_READ("mds", "mds_decay_halflife", mds_decay_halflife);
  CF_READ("mds", "mds_beacon_interval", mds_beacon_interval);
  CF_READ("mds", "mds_beacon_grace", mds_beacon_grace);
  CF_READ("mds", "mds_blacklist_interval", mds_blacklist_interval);
  CF_READ("mds", "mds_session_timeout", mds_session_timeout);
  CF_READ("mds", "mds_session_autoclose", mds_session_autoclose);
  CF_READ("mds", "mds_client_lease", mds_client_lease);
  CF_READ("mds", "mds_reconnect_timeout", mds_reconnect_timeout);
  CF_READ("mds", "mds_tick_interval", mds_tick_interval);
  CF_READ("mds", "mds_scatter_nudge_interval", mds_scatter_nudge_interval);
  CF_READ("mds", "mds_client_prealloc_inos", mds_client_prealloc_inos);
  CF_READ("mds", "mds_early_reply", mds_early_reply);
  CF_READ("mds", "mds_log", mds_log);
  CF_READ("mds", "mds_log_max_events", mds_log_max_events);
  CF_READ("mds", "mds_log_max_segments", mds_log_max_segments);
  CF_READ("mds", "mds_log_max_expiring", mds_log_max_expiring);
  CF_READ("mds", "mds_log_pad_entry", mds_log_pad_entry);
  CF_READ("mds", "mds_log_eopen_size", mds_log_eopen_size);
  CF_READ("mds", "mds_bal_sample_interval", mds_bal_sample_interval);
  CF_READ("mds", "mds_bal_replicate_threshold", mds_bal_replicate_threshold);
  CF_READ("mds", "mds_bal_unreplicate_threshold", mds_bal_unreplicate_threshold);
  CF_READ("mds", "mds_bal_frag", mds_bal_frag);
  CF_READ("mds", "mds_bal_split_size", mds_bal_split_size);
  CF_READ("mds", "mds_bal_split_rd", mds_bal_split_rd);
  CF_READ("mds", "mds_bal_split_wr", mds_bal_split_wr);
  CF_READ("mds", "mds_bal_merge_size", mds_bal_merge_size);
  CF_READ("mds", "mds_bal_merge_rd", mds_bal_merge_rd);
  CF_READ("mds", "mds_bal_merge_wr", mds_bal_merge_wr);
  CF_READ("mds", "mds_bal_interval", mds_bal_interval);
  CF_READ("mds", "mds_bal_fragment_interval", mds_bal_fragment_interval);
  CF_READ("mds", "mds_bal_idle_threshold", mds_bal_idle_threshold);
  CF_READ("mds", "mds_bal_max", mds_bal_max);
  CF_READ("mds", "mds_bal_max_until", mds_bal_max_until);
  CF_READ("mds", "mds_bal_mode", mds_bal_mode);
  CF_READ("mds", "mds_bal_min_rebalance", mds_bal_min_rebalance);
  CF_READ("mds", "mds_bal_min_start", mds_bal_min_start);
  CF_READ("mds", "mds_bal_need_min", mds_bal_need_min);
  CF_READ("mds", "mds_bal_need_max", mds_bal_need_max);
  CF_READ("mds", "mds_bal_midchunk", mds_bal_midchunk);
  CF_READ("mds", "mds_bal_minchunk", mds_bal_minchunk);
  CF_READ("mds", "mds_trim_on_rejoin", mds_trim_on_rejoin);
  CF_READ("mds", "mds_shutdown_check", mds_shutdown_check);
  CF_READ("mds", "mds_verify_export_dirauth", mds_verify_export_dirauth);
  CF_READ("mds", "mds_local_osd", mds_local_osd);
  CF_READ("mds", "mds_thrash_exports", mds_thrash_exports);
  CF_READ("mds", "mds_thrash_fragments", mds_thrash_fragments);
  CF_READ("mds", "mds_dump_cache_on_map", mds_dump_cache_on_map);
  CF_READ("mds", "mds_dump_cache_after_rejoin", mds_dump_cache_after_rejoin);
  CF_READ("mds", "mds_hack_log_expire_for_better_stats", mds_hack_log_expire_for_better_stats);

  CF_READ("osd", "osd_rep", osd_rep);
  CF_READ("osd", "osd_balance_reads", osd_balance_reads);
  CF_READ("osd", "osd_flash_crowd_iat_threshold", osd_flash_crowd_iat_threshold);
  CF_READ("osd", "osd_flash_crowd_iat_alpha", osd_flash_crowd_iat_alpha);
  CF_READ("osd", "osd_balance_reads_temp", osd_balance_reads_temp);
  CF_READ("osd", "osd_shed_reads", osd_shed_reads);
  CF_READ("osd", "osd_shed_reads_min_latency", osd_shed_reads_min_latency);
  CF_READ("osd", "osd_shed_reads_min_latency_diff", osd_shed_reads_min_latency_diff);
  CF_READ("osd", "osd_shed_reads_min_latency_ratio", osd_shed_reads_min_latency_ratio);
  CF_READ("osd", "osd_immediate_read_from_cache", osd_immediate_read_from_cache);
  CF_READ("osd", "osd_exclusive_caching", osd_exclusive_caching);
  CF_READ("osd", "osd_stat_refresh_interval", osd_stat_refresh_interval);
  CF_READ("osd", "osd_min_pg_size_without_alive", osd_min_pg_size_without_alive);
  CF_READ("osd", "osd_pg_bits", osd_pg_bits);
  CF_READ("osd", "osd_lpg_bits", osd_lpg_bits);
  CF_READ("osd", "osd_object_layout", osd_object_layout);
  CF_READ("osd", "osd_pg_layout", osd_pg_layout);
  CF_READ("osd", "osd_min_rep", osd_min_rep);
  CF_READ("osd", "osd_max_rep", osd_max_rep);
  CF_READ("osd", "osd_min_raid_width", osd_min_raid_width);
  CF_READ("osd", "osd_max_raid_width", osd_max_raid_width);
  CF_READ("osd", "osd_maxthreads", osd_maxthreads);
  CF_READ("osd", "osd_max_opq", osd_max_opq);
  CF_READ("osd", "osd_mkfs", osd_mkfs);
  CF_READ("osd", "osd_age", osd_age);
  CF_READ("osd", "osd_age_time", osd_age_time);
  CF_READ("osd", "osd_heartbeat_interval", osd_heartbeat_interval);
  CF_READ("osd", "osd_mon_heartbeat_interval", osd_mon_heartbeat_interval);
  CF_READ("osd", "osd_heartbeat_grace", osd_heartbeat_grace);
  CF_READ("osd", "osd_mon_report_interval", osd_mon_report_interval);
  CF_READ("osd", "osd_replay_window", osd_replay_window);
  CF_READ("osd", "osd_max_pull", osd_max_pull);
  CF_READ("osd", "osd_preserve_trimmed_log", osd_preserve_trimmed_log);
  CF_READ("osd", "osd_recovery_delay_start", osd_recovery_delay_start);
  CF_READ("osd", "osd_recovery_max_active", osd_recovery_max_active);
  CF_READ("osd", "osd_auto_weight", osd_auto_weight);

  CF_READ("filestore", "filestore", filestore);
  CF_READ("filestore", "filestore_sync_interval", filestore_sync_interval);
  CF_READ("filestore", "filestore_fake_attrs", filestore_fake_attrs);
  CF_READ("filestore", "filestore_fake_collections", filestore_fake_collections);
  CF_READ_STR("filestore", "filestore_dev", filestore_dev);
  CF_READ("filestore", "filestore_btrfs_trans", filestore_btrfs_trans);

  CF_READ("ebofs", "ebofs", ebofs);
  CF_READ("ebofs", "ebofs_cloneable", ebofs_cloneable);
  CF_READ("ebofs", "ebofs_verify", ebofs_verify);
  CF_READ("ebofs", "ebofs_commit_ms", ebofs_commit_ms);
  CF_READ("ebofs", "ebofs_oc_size", ebofs_oc_size);
  CF_READ("ebofs", "ebofs_cc_size", ebofs_cc_size);
  CF_READ_TYPE("ebofs", "ebofs_bc_size", unsigned long long, ebofs_bc_size);
  CF_READ_TYPE("ebofs", "ebofs_bc_max_dirty", unsigned long long, ebofs_bc_max_dirty);
  CF_READ("ebofs", "ebofs_max_prefetch", ebofs_max_prefetch);
  CF_READ("ebofs", "ebofs_realloc", ebofs_realloc);
  CF_READ("ebofs", "ebofs_verify_csum_on_read", ebofs_verify_csum_on_read);

  CF_READ("journal", "journal_dio", journal_dio);
  CF_READ("journal", "journal_max_write_bytes", journal_max_write_bytes);
  CF_READ("journal", "journal_max_write_entries", journal_max_write_entries);

  CF_READ("bdev", "bdev_lock", bdev_lock);
  CF_READ("bdev", "bdev_iothreads", bdev_iothreads);
  CF_READ("bdev", "bdev_idle_kick_after_ms", bdev_idle_kick_after_ms);
  CF_READ("bdev", "bdev_el_fw_max_ms", bdev_el_fw_max_ms);
  CF_READ("bdev", "bdev_el_bw_max_ms", bdev_el_bw_max_ms);
  CF_READ("bdev", "bdev_el_bidir", bdev_el_bidir);
  CF_READ("bdev", "bdev_iov_max", bdev_iov_max);
  CF_READ("bdev", "bdev_debug_check_io_overlap", bdev_debug_check_io_overlap);
  CF_READ("bdev", "bdev_fake_mb", bdev_fake_mb);
  CF_READ("bdev", "bdev_fake_max_mb", bdev_fake_max_mb);
#ifdef USE_OSBDB
  CF_READ("bdstore", "bdbstore", bdbstore);
  CF_READ("bdstore", "bdstore_debug", debug_bdbstore);
  CF_READ("bdstore", "bdbstore_btree", bdbstore_btree);
  CF_READ("bdstore", "bdbstore_ffactor", bdbstore_ffactor);
  CF_READ("bdstore", "bdbstore_nelem", bdbstore_nelem);
  CF_READ("bdstore", "bdbstore_pagesize", bdbstore_pagesize);
  CF_READ("bdstore", "bdbstore_cachesize", bdbstore_cachesize);
  CF_READ("bdstore", "bdbstore_transactional", bdbstore_transactional);
#endif

  cf.flush();
}

void parse_config_options(std::vector<const char*>& args, bool open)
{
  std::vector<const char*> nargs;

  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i],"--bind") == 0) 
      assert(parse_ip_port(args[++i], g_my_addr));
    else if (strcmp(args[i], "--nummon") == 0) 
      g_conf.num_mon = atoi(args[++i]);
    else if (strcmp(args[i], "--nummds") == 0) 
      g_conf.num_mds = atoi(args[++i]);
    else if (strcmp(args[i], "--numclient") == 0) 
      g_conf.num_client = atoi(args[++i]);
    else if (strcmp(args[i], "--numosd") == 0) 
      g_conf.num_osd = atoi(args[++i]);
    
    else if (strcmp(args[i], "--mon_host") == 0 ||
	     strcmp(args[i], "-m") == 0)
      g_conf.mon_host = args[++i];    
    else if (strcmp(args[i], "--daemonize") == 0 ||
	     strcmp(args[i], "-d") == 0) {
      g_conf.daemonize = true;
      g_conf.file_logs = true;
    } else if (strcmp(args[i], "--foreground") == 0 ||
	     strcmp(args[i], "-f") == 0) {
      g_conf.daemonize = false;
      g_conf.file_logs = true;
    }

    else if (strcmp(args[i], "--ms_hosts") == 0)
      g_conf.ms_hosts = args[++i];
    else if (strcmp(args[i], "--ms_stripe_osds") == 0)
      g_conf.ms_stripe_osds = true;
    else if (strcmp(args[i], "--ms_skip_rank0") == 0)
      g_conf.ms_skip_rank0 = true;
    else if (strcmp(args[i], "--ms_overlay_clients") == 0)
      g_conf.ms_overlay_clients = true;
    else if (strcmp(args[i], "--ms_die_on_failure") == 0)
      g_conf.ms_die_on_failure = true;

    /*else if (strcmp(args[i], "--tcp_log") == 0)
      g_conf.tcp_log = true;
    else if (strcmp(args[i], "--tcp_multi_out") == 0)
      g_conf.tcp_multi_out = atoi(args[++i]);
    */

    else if (strcmp(args[i], "--mkfs") == 0) 
      g_conf.osd_mkfs = g_conf.mkfs = 1; //atoi(args[++i]);

    else if (strcmp(args[i], "--fake_kill_osd_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_OSD, atoi(args[i+1]))] = atof(args[i+2]); 
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_mds_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_MDS, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_mon_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_MON, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_client_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_CLIENT, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }

    else if (strcmp(args[i], "--osd_remount_at") == 0) 
      g_conf.osd_remount_at = atoi(args[++i]);
    //else if (strcmp(args[i], "--fake_osd_sync") == 0) 
    //g_conf.fake_osd_sync = atoi(args[++i]);

    
    
    else if (//strcmp(args[i], "-o") == 0 ||
	     strcmp(args[i], "--dout_dir") == 0) 
      g_conf.dout_dir = args[++i];
    else if (//strcmp(args[i], "-o") == 0 ||
	     strcmp(args[i], "--dout_sym_dir") == 0) 
      g_conf.dout_sym_dir = args[++i];
    else if (strcmp(args[i], "--conf_file") == 0) 
      g_conf.conf_file = args[++i];

    else if (strcmp(args[i], "--lockdep") == 0)
      g_lockdep = atoi(args[++i]);

    else if (strcmp(args[i], "--debug") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug = atoi(args[++i]);
      else 
        g_debug_after_conf.debug = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_lockdep") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_lockdep = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_lockdep = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_balancer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_balancer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_balancer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_log") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_log = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_log = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_log_expire") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_log_expire = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_log_expire = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_migrator") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_migrator = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_migrator = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_buffer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_buffer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_buffer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_timer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_timer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_timer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_filer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_filer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_filer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_objecter") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_objecter = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_objecter = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_journaler") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_journaler = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_journaler = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_objectcacher") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_objectcacher = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_objectcacher = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_client") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_client = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_client = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_osd") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_osd = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_osd = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_ebofs") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_ebofs = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_ebofs = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_filestore") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_filestore = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_filestore = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_journal") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_journal = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_journal = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_bdev") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_bdev = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_bdev = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_ms") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_ms = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mon") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mon = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mon = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_paxos") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_paxos = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_paxos = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_tp") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_tp = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_tp = atoi(args[++i]);

    else if (strcmp(args[i], "--debug_after") == 0) {
      g_conf.debug_after = atoi(args[++i]);
      g_debug_after_conf = g_conf;
    }

    else if (strcmp(args[i], "--log") == 0) 
      g_conf.log = atoi(args[++i]);
    else if (strcmp(args[i], "--log_name") == 0) 
      g_conf.log_name = args[++i];

    else if (strcmp(args[i], "--fakemessenger_serialize") == 0) 
      g_conf.fakemessenger_serialize = atoi(args[++i]);


    else if (strcmp(args[i], "--clock_lock") == 0) 
      g_conf.clock_lock = atoi(args[++i]);
    else if (strcmp(args[i], "--clock_tare") == 0) 
      g_conf.clock_tare = atoi(args[++i]);

    else if (strcmp(args[i], "--objecter_buffer_uncommitted") == 0) 
      g_conf.objecter_buffer_uncommitted = atoi(args[++i]);

    else if (strcmp(args[i], "--journaler_safe") == 0) 
      g_conf.journaler_safe = atoi(args[++i]);
    else if (strcmp(args[i], "--journaler_cache") == 0) 
      g_conf.journaler_cache = atoi(args[++i]);
    else if (strcmp(args[i], "--journaler_batch_interval") == 0) 
      g_conf.journaler_batch_interval = atof(args[++i]);
    else if (strcmp(args[i], "--journaler_batch_max") == 0) 
      g_conf.journaler_batch_max = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_cache_size") == 0) 
      g_conf.mds_cache_size = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_beacon_interval") == 0) 
      g_conf.mds_beacon_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_beacon_grace") == 0) 
      g_conf.mds_beacon_grace = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_log") == 0) 
      g_conf.mds_log = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_events") == 0) 
      g_conf.mds_log_max_events = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_segments") == 0) 
      g_conf.mds_log_max_segments = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_expiring") == 0) 
      g_conf.mds_log_max_expiring = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_shutdown_check") == 0) 
      g_conf.mds_shutdown_check = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_decay_halflife") == 0) 
      g_conf.mds_decay_halflife = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_early_reply") == 0) 
      g_conf.mds_early_reply = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_client_prealloc_inos") == 0) 
      g_conf.mds_client_prealloc_inos = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_interval") == 0) 
      g_conf.mds_bal_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_rep") == 0) 
      g_conf.mds_bal_replicate_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_unrep") == 0) 
      g_conf.mds_bal_unreplicate_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_max") == 0) 
      g_conf.mds_bal_max = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_max_until") == 0) 
      g_conf.mds_bal_max_until = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_frag") == 0) 
      g_conf.mds_bal_frag = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_split_size") == 0) 
      g_conf.mds_bal_split_size = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_split_rd") == 0) 
      g_conf.mds_bal_split_rd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_split_wr") == 0) 
      g_conf.mds_bal_split_wr = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_size") == 0) 
      g_conf.mds_bal_merge_size = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_rd") == 0) 
      g_conf.mds_bal_merge_rd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_wr") == 0) 
      g_conf.mds_bal_merge_wr = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_fragment_interval") == 0) 
      g_conf.mds_bal_fragment_interval = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_mode") == 0) 
      g_conf.mds_bal_mode = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_min_start") == 0) 
      g_conf.mds_bal_min_start = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_need_min") == 0) 
      g_conf.mds_bal_need_min = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_need_max") == 0) 
      g_conf.mds_bal_need_max = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_midchunk") == 0) 
      g_conf.mds_bal_midchunk = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_minchunk") == 0) 
      g_conf.mds_bal_minchunk = atoi(args[++i]);
    
    else if (strcmp(args[i], "--mds_local_osd") == 0) 
      g_conf.mds_local_osd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_thrash_exports") == 0) 
      g_conf.mds_thrash_exports = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_thrash_fragments") == 0) 
      g_conf.mds_thrash_fragments = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_dump_cache_on_map") == 0) 
      g_conf.mds_dump_cache_on_map = true;

    else if (strcmp(args[i], "--mds_hack_log_expire_for_better_stats") == 0) 
      g_conf.mds_hack_log_expire_for_better_stats = atoi(args[++i]);
    
    else if (strcmp(args[i], "--client_use_random_mds") == 0)
      g_conf.client_use_random_mds = true;
    else if (strcmp(args[i], "--client_cache_size") == 0)
      g_conf.client_cache_size = atoi(args[++i]);
    else if (strcmp(args[i], "--client_cache_stat_ttl") == 0)
      g_conf.client_cache_stat_ttl = atoi(args[++i]);
    else if (strcmp(args[i], "--client_cache_readdir_ttl") == 0)
      g_conf.client_cache_readdir_ttl = atoi(args[++i]);
    else if (strcmp(args[i], "--client_trace") == 0)
      g_conf.client_trace = args[++i];

    else if (strcmp(args[i], "--client_readahead_min") == 0)
      g_conf.client_readahead_min = atoi(args[++i]);
    else if (strcmp(args[i], "--client_readahead_max_bytes") == 0)
      g_conf.client_readahead_max_bytes = atoi(args[++i]);
    else if (strcmp(args[i], "--client_readahead_max_periods") == 0)
      g_conf.client_readahead_max_periods = atoi(args[++i]);

    else if (strcmp(args[i], "--fuse_direct_io") == 0)
      g_conf.fuse_direct_io = atoi(args[++i]);
    else if (strcmp(args[i], "--fuse_ll") == 0)
      g_conf.fuse_ll = atoi(args[++i]);

    else if (strcmp(args[i], "--mon_osd_down_out_interval") == 0)
      g_conf.mon_osd_down_out_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mon_stop_on_last_unmount") == 0) 
      g_conf.mon_stop_on_last_unmount = atoi(args[++i]);
    else if (strcmp(args[i], "--mon_stop_with_last_mds") == 0)
      g_conf.mon_stop_with_last_mds = atoi(args[++i]);

    else if (strcmp(args[i], "--client_oc") == 0)
      g_conf.client_oc = atoi(args[++i]);
    else if (strcmp(args[i], "--client_oc_size") == 0)
      g_conf.client_oc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--client_oc_max_dirty") == 0)
      g_conf.client_oc_max_dirty = atoi(args[++i]);

    else if (strcmp(args[i], "--client_hack_balance_reads") == 0)
      g_conf.client_hack_balance_reads = atoi(args[++i]);

    else if (strcmp(args[i], "--ebofs") == 0) 
      g_conf.ebofs = true;
    else if (strcmp(args[i], "--ebofs_cloneable") == 0)
      g_conf.ebofs_cloneable = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_verify") == 0)
      g_conf.ebofs_verify = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_commit_ms") == 0)
      g_conf.ebofs_commit_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_oc_size") == 0)
      g_conf.ebofs_oc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_cc_size") == 0)
      g_conf.ebofs_cc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_bc_size") == 0)
      g_conf.ebofs_bc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_bc_max_dirty") == 0)
      g_conf.ebofs_bc_max_dirty = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_verify_csum_on_read") == 0)
      g_conf.ebofs_verify_csum_on_read = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_max_prefetch") == 0)
      g_conf.ebofs_max_prefetch = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_realloc") == 0)
      g_conf.ebofs_realloc = atoi(args[++i]);

    else if (strcmp(args[i], "--journal_dio") == 0)
      g_conf.journal_dio = atoi(args[++i]);      
    else if (strcmp(args[i], "--journal_max_write_entries") == 0)
      g_conf.journal_max_write_entries = atoi(args[++i]);      
    else if (strcmp(args[i], "--journal_max_write_bytes") == 0)
      g_conf.journal_max_write_bytes = atoi(args[++i]);      

    else if (strcmp(args[i], "--filestore") == 0)
      g_conf.filestore = true;
    else if (strcmp(args[i], "--filestore_sync_interval") == 0)
      g_conf.filestore_sync_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--filestore_dev") == 0) 
      g_conf.filestore_dev = args[++i];
    else if (strcmp(args[i], "--filestore_fake_attrs") == 0) 
      g_conf.filestore_fake_attrs = true;//atoi(args[++i]);
    else if (strcmp(args[i], "--filestore_fake_collections") == 0) 
      g_conf.filestore_fake_collections = true;//atoi(args[++i]);
    else if (strcmp(args[i], "--filestore_btrfs_trans") == 0) 
      g_conf.filestore_btrfs_trans = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_balance_reads") == 0) 
      g_conf.osd_balance_reads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_flash_crowd_iat_threshold") == 0) 
      g_conf.osd_flash_crowd_iat_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_flash_crowd_iat_alpha") == 0) 
      g_conf.osd_flash_crowd_iat_alpha = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_shed_reads") == 0) 
      g_conf.osd_shed_reads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency") == 0) 
      g_conf.osd_shed_reads_min_latency = atof(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency_diff") == 0) 
      g_conf.osd_shed_reads_min_latency_diff = atof(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency_ratio") == 0) 
      g_conf.osd_shed_reads_min_latency_ratio = atof(args[++i]);

    else if ( strcmp(args[i],"--osd_immediate_read_from_cache" ) == 0)
      g_conf.osd_immediate_read_from_cache = atoi(args[++i]);
    else if ( strcmp(args[i],"--osd_exclusive_caching" ) == 0)
      g_conf.osd_exclusive_caching = atoi(args[++i]);

    else if ( strcmp(args[i],"--osd_stat_refresh_interval" ) == 0)
      g_conf.osd_stat_refresh_interval = atof(args[++i]);

    else if (strcmp(args[i], "--osd_rep") == 0) 
      g_conf.osd_rep = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_rep_chain") == 0) 
      g_conf.osd_rep = OSD_REP_CHAIN;
    else if (strcmp(args[i], "--osd_rep_splay") == 0) 
      g_conf.osd_rep = OSD_REP_SPLAY;
    else if (strcmp(args[i], "--osd_rep_primary") == 0) 
      g_conf.osd_rep = OSD_REP_PRIMARY;
    else if (strcmp(args[i], "--osd_mkfs") == 0) 
      g_conf.osd_mkfs = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_heartbeat_interval") == 0) 
      g_conf.osd_heartbeat_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_heartbeat_grace") == 0) 
      g_conf.osd_heartbeat_grace = atoi(args[++i]);
    
    else if (strcmp(args[i], "--osd_age") == 0) 
      g_conf.osd_age = atof(args[++i]);
    else if (strcmp(args[i], "--osd_age_time") == 0) 
      g_conf.osd_age_time = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_pg_bits") == 0) 
      g_conf.osd_pg_bits = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_max_rep") == 0) 
      g_conf.osd_max_rep = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_maxthreads") == 0) 
      g_conf.osd_maxthreads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_max_pull") == 0) 
      g_conf.osd_max_pull = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_preserve_trimmed_log") == 0) 
      g_conf.osd_preserve_trimmed_log = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_recovery_delay_start") == 0) 
      g_conf.osd_recovery_delay_start = atof(args[++i]);
    else if (strcmp(args[i], "--osd_recovery_max_active") == 0) 
      g_conf.osd_recovery_max_active = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_auto_weight") == 0) 
      g_conf.osd_auto_weight = atoi(args[++i]);

    else if (strcmp(args[i], "--bdev_lock") == 0) 
      g_conf.bdev_lock = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_el_bidir") == 0) 
      g_conf.bdev_el_bidir = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_iothreads") == 0) 
      g_conf.bdev_iothreads = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_idle_kick_after_ms") == 0) 
      g_conf.bdev_idle_kick_after_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_fake_mb") == 0) 
      g_conf.bdev_fake_mb = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_fake_max_mb") == 0) 
      g_conf.bdev_fake_max_mb = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_object_layout") == 0) {
      i++;
      if (strcmp(args[i], "linear") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_LINEAR;
      else if (strcmp(args[i], "hashino") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_HASHINO;
      else if (strcmp(args[i], "hash") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_HASH;
      else assert(0);
    }
    
    else if (strcmp(args[i], "--osd_pg_layout") == 0) {
      i++;
      if (strcmp(args[i], "linear") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_LINEAR;
      else if (strcmp(args[i], "hash") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_HASH;
      else if (strcmp(args[i], "hybrid") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_HYBRID;
      else if (strcmp(args[i], "crush") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_CRUSH;
      else assert(0);
    }
    
    else if (strcmp(args[i], "--kill_after") == 0) 
      g_conf.kill_after = atoi(args[++i]);
    else if (strcmp(args[i], "--tick") == 0) 
      g_conf.tick = atoi(args[++i]);

    else if (strcmp(args[i], "--file_layout_unit") == 0) 
      g_default_file_layout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_count") == 0) 
      g_default_file_layout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_osize") == 0) 
      g_default_file_layout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_pg_type") == 0) 
      g_default_file_layout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_pg_size") == 0) 
      g_default_file_layout.fl_pg_size = atoi(args[++i]);

    else if (strcmp(args[i], "--meta_dir_layout_unit") == 0) 
      g_default_mds_dir_layout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_scount") == 0) 
      g_default_mds_dir_layout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_osize") == 0) 
      g_default_mds_dir_layout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_pg_type") == 0) 
      g_default_mds_dir_layout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_pg_size") == 0) 
      g_default_mds_dir_layout.fl_pg_size = atoi(args[++i]);

    else if (strcmp(args[i], "--meta_log_layout_unit") == 0) 
      g_default_mds_log_layout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_scount") == 0) 
      g_default_mds_log_layout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_osize") == 0) 
      g_default_mds_log_layout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_pg_type") == 0) 
      g_default_mds_log_layout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_pg_size") == 0) {
      g_default_mds_log_layout.fl_pg_size = atoi(args[++i]);
      if (!g_default_mds_log_layout.fl_pg_size)
        g_conf.mds_log = false;
    }

#ifdef USE_OSBDB
    else if (strcmp(args[i], "--bdbstore") == 0) {
      g_conf.bdbstore = true;
      g_conf.ebofs = 0;
    }
    else if (strcmp(args[i], "--bdbstore-btree") == 0) {
      g_conf.bdbstore_btree = true;
    }
    else if (strcmp(args[i], "--bdbstore-hash-ffactor") == 0) {
      g_conf.bdbstore_ffactor = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-hash-nelem") == 0) {
      g_conf.bdbstore_nelem = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-hash-pagesize") == 0) {
      g_conf.bdbstore_pagesize = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-cachesize") == 0) {
      g_conf.bdbstore_cachesize = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-transactional") == 0) {
      g_conf.bdbstore_transactional = true;
    }
    else if (strcmp(args[i], "--debug-bdbstore") == 0) {
      g_conf.debug_bdbstore = atoi(args[++i]);
    }
#endif // USE_OSBDB

    else {
      nargs.push_back(args[i]);
    }
  }

  parse_config_file(g_conf.conf_file);

  // open log file?
  if (open)
    _dout_open_log();
  
  signal(SIGHUP, sighup_handler);

  args = nargs;
}


