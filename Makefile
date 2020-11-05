BLOCKSTORE_OBJS := allocator.o blockstore.o blockstore_impl.o blockstore_init.o blockstore_open.o blockstore_journal.o blockstore_read.o \
	blockstore_write.o blockstore_sync.o blockstore_stable.o blockstore_rollback.o blockstore_flush.o crc32c.o ringloop.o
# -fsanitize=address
CXXFLAGS := -g -O3 -Wall -Wno-sign-compare -Wno-comment -Wno-parentheses -Wno-pointer-arith -fPIC -fdiagnostics-color=always
all: libfio_blockstore.so osd libfio_sec_osd.so libfio_cluster.so stub_osd stub_uring_osd stub_bench osd_test dump_journal qemu_driver.so nbd_proxy rm_inode
clean:
	rm -f *.o

dump_journal: dump_journal.cpp crc32c.o blockstore_journal.h
	g++ $(CXXFLAGS) -o $@ $< crc32c.o

libblockstore.so: $(BLOCKSTORE_OBJS)
	g++ $(CXXFLAGS) -o $@ -shared $(BLOCKSTORE_OBJS) -ltcmalloc_minimal -luring
libfio_blockstore.so: ./libblockstore.so fio_engine.o json11.o
	g++ $(CXXFLAGS) -shared -o $@ fio_engine.o json11.o ./libblockstore.so -ltcmalloc_minimal -luring

OSD_OBJS := osd.o osd_secondary.o msgr_receive.o msgr_send.o osd_peering.o osd_flush.o osd_peering_pg.o \
	osd_primary.o osd_primary_subops.o etcd_state_client.o messenger.o osd_cluster.o http_client.o osd_ops.o pg_states.o \
	osd_rmw.o json11.o base64.o timerfd_manager.o epoll_manager.o
osd: ./libblockstore.so osd_main.cpp osd.h osd_ops.h $(OSD_OBJS)
	g++ $(CXXFLAGS) -o $@ osd_main.cpp $(OSD_OBJS) ./libblockstore.so -ltcmalloc_minimal -luring

stub_osd: stub_osd.o rw_blocking.o
	g++ $(CXXFLAGS) -o $@ stub_osd.o rw_blocking.o -ltcmalloc_minimal

osd_rmw_test: osd_rmw_test.o
	g++ $(CXXFLAGS) -o $@ osd_rmw_test.o

STUB_URING_OSD_OBJS := stub_uring_osd.o epoll_manager.o messenger.o msgr_send.o msgr_receive.o ringloop.o timerfd_manager.o json11.o
stub_uring_osd: $(STUB_URING_OSD_OBJS)
	g++ $(CXXFLAGS) -o $@ -ltcmalloc_minimal $(STUB_URING_OSD_OBJS) -luring
stub_bench: stub_bench.cpp osd_ops.h rw_blocking.o
	g++ $(CXXFLAGS) -o $@ stub_bench.cpp rw_blocking.o -ltcmalloc_minimal
osd_test: osd_test.cpp osd_ops.h rw_blocking.o
	g++ $(CXXFLAGS) -o $@ osd_test.cpp rw_blocking.o -ltcmalloc_minimal
osd_peering_pg_test: osd_peering_pg_test.cpp osd_peering_pg.o
	g++ $(CXXFLAGS) -o $@ $< osd_peering_pg.o -ltcmalloc_minimal

libfio_sec_osd.so: fio_sec_osd.o rw_blocking.o
	g++ $(CXXFLAGS) -ltcmalloc_minimal -shared -o $@ fio_sec_osd.o rw_blocking.o

FIO_CLUSTER_OBJS := cluster_client.o epoll_manager.o etcd_state_client.o \
	messenger.o msgr_send.o msgr_receive.o ringloop.o json11.o http_client.o osd_ops.o pg_states.o timerfd_manager.o base64.o
libfio_cluster.so: fio_cluster.o $(FIO_CLUSTER_OBJS)
	g++ $(CXXFLAGS) -ltcmalloc_minimal -shared -o $@ $< $(FIO_CLUSTER_OBJS) -luring

nbd_proxy: nbd_proxy.o $(FIO_CLUSTER_OBJS)
	g++ $(CXXFLAGS) -ltcmalloc_minimal -o $@ $< $(FIO_CLUSTER_OBJS) -luring

rm_inode: rm_inode.o $(FIO_CLUSTER_OBJS)
	g++ $(CXXFLAGS) -ltcmalloc_minimal -o $@ $< $(FIO_CLUSTER_OBJS) -luring

qemu_driver.o: qemu_driver.c qemu_proxy.h
	gcc -I qemu/b/qemu `pkg-config glib-2.0 --cflags` \
		-I qemu/include $(CXXFLAGS) -c -o $@ $<

qemu_driver.so: qemu_driver.o qemu_proxy.o $(FIO_CLUSTER_OBJS)
	g++ $(CXXFLAGS) -ltcmalloc_minimal -shared -o $@ $(FIO_CLUSTER_OBJS) qemu_driver.o qemu_proxy.o -luring

test_blockstore: ./libblockstore.so test_blockstore.cpp timerfd_interval.o
	g++ $(CXXFLAGS) -o test_blockstore test_blockstore.cpp timerfd_interval.o ./libblockstore.so -ltcmalloc_minimal -luring
test_shit: test_shit.cpp osd_peering_pg.o
	g++ $(CXXFLAGS) -o test_shit test_shit.cpp -luring -lm
test_allocator: test_allocator.cpp allocator.o
	g++ $(CXXFLAGS) -o test_allocator test_allocator.cpp allocator.o

crc32c.o: crc32c.c crc32c.h
	g++ $(CXXFLAGS) -c -o $@ $<
json11.o: json11/json11.cpp
	g++ $(CXXFLAGS) -c -o json11.o json11/json11.cpp

# Autogenerated

allocator.o: allocator.cpp allocator.h
	g++ $(CXXFLAGS) -c -o $@ $<
base64.o: base64.cpp base64.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore.o: blockstore.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_flush.o: blockstore_flush.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_impl.o: blockstore_impl.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_init.o: blockstore_init.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_journal.o: blockstore_journal.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_open.o: blockstore_open.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_read.o: blockstore_read.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_rollback.o: blockstore_rollback.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_stable.o: blockstore_stable.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_sync.o: blockstore_sync.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
blockstore_write.o: blockstore_write.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
cluster_client.o: cluster_client.cpp cluster_client.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
dump_journal.o: dump_journal.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
epoll_manager.o: epoll_manager.cpp epoll_manager.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
etcd_state_client.o: etcd_state_client.cpp base64.h etcd_state_client.h http_client.h json11/json11.hpp object_id.h osd_id.h osd_ops.h pg_states.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
fio_cluster.o: fio_cluster.cpp cluster_client.h epoll_manager.h etcd_state_client.h fio/arch/arch.h fio/fio.h fio/optgroup.h fio_headers.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
fio_engine.o: fio_engine.cpp blockstore.h fio/arch/arch.h fio/fio.h fio/optgroup.h fio_headers.h json11/json11.hpp object_id.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
fio_sec_osd.o: fio_sec_osd.cpp fio/arch/arch.h fio/fio.h fio/optgroup.h fio_headers.h object_id.h osd_id.h osd_ops.h rw_blocking.h
	g++ $(CXXFLAGS) -c -o $@ $<
http_client.o: http_client.cpp http_client.h json11/json11.hpp timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
messenger.o: messenger.cpp json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
msgr_receive.o: msgr_receive.cpp json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
msgr_send.o: msgr_send.cpp json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
nbd_proxy.o: nbd_proxy.cpp cluster_client.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd.o: osd.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_cluster.o: osd_cluster.cpp base64.h blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_flush.o: osd_flush.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_main.o: osd_main.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_ops.o: osd_ops.cpp object_id.h osd_id.h osd_ops.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_peering.o: osd_peering.cpp base64.h blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_peering_pg.o: osd_peering_pg.cpp cpp-btree/btree_map.h object_id.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_peering_pg_test.o: osd_peering_pg_test.cpp cpp-btree/btree_map.h object_id.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_primary.o: osd_primary.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h osd_primary.h osd_rmw.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_primary_subops.o: osd_primary_subops.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h osd_primary.h osd_rmw.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_rmw.o: osd_rmw.cpp malloc_or_die.h object_id.h osd_id.h osd_rmw.h xor.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_rmw_test.o: osd_rmw_test.cpp malloc_or_die.h object_id.h osd_id.h osd_rmw.cpp osd_rmw.h test_pattern.h xor.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_secondary.o: osd_secondary.cpp blockstore.h cpp-btree/btree_map.h epoll_manager.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
osd_test.o: osd_test.cpp object_id.h osd_id.h osd_ops.h rw_blocking.h test_pattern.h
	g++ $(CXXFLAGS) -c -o $@ $<
pg_states.o: pg_states.cpp pg_states.h
	g++ $(CXXFLAGS) -c -o $@ $<
qemu_proxy.o: qemu_proxy.cpp cluster_client.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h qemu_proxy.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
ringloop.o: ringloop.cpp ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
rm_inode.o: rm_inode.cpp cluster_client.h etcd_state_client.h http_client.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
rw_blocking.o: rw_blocking.cpp rw_blocking.h
	g++ $(CXXFLAGS) -c -o $@ $<
stub_bench.o: stub_bench.cpp object_id.h osd_id.h osd_ops.h rw_blocking.h
	g++ $(CXXFLAGS) -c -o $@ $<
stub_osd.o: stub_osd.cpp object_id.h osd_id.h osd_ops.h rw_blocking.h
	g++ $(CXXFLAGS) -c -o $@ $<
stub_uring_osd.o: stub_uring_osd.cpp epoll_manager.h json11/json11.hpp malloc_or_die.h messenger.h object_id.h osd_id.h osd_ops.h ringloop.h timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
test_allocator.o: test_allocator.cpp allocator.h
	g++ $(CXXFLAGS) -c -o $@ $<
test_blockstore.o: test_blockstore.cpp blockstore.h object_id.h ringloop.h timerfd_interval.h
	g++ $(CXXFLAGS) -c -o $@ $<
test_shit.o: test_shit.cpp allocator.h blockstore.h blockstore_flush.h blockstore_impl.h blockstore_init.h blockstore_journal.h cpp-btree/btree_map.h crc32c.h malloc_or_die.h object_id.h osd_id.h osd_ops.h osd_peering_pg.h pg_states.h ringloop.h
	g++ $(CXXFLAGS) -c -o $@ $<
timerfd_interval.o: timerfd_interval.cpp ringloop.h timerfd_interval.h
	g++ $(CXXFLAGS) -c -o $@ $<
timerfd_manager.o: timerfd_manager.cpp timerfd_manager.h
	g++ $(CXXFLAGS) -c -o $@ $<
