#include "world_gasnet.hxx"

#include <gasnetex.h>
//#include <gasnet.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

#include <sched.h>
#include <fcntl.h>

using world::worker_n;
using world::remote_out_message;

thread_local int world::rank_me_ = 0xdeadbeef;

alignas(64)
int world::process_rank_lo_ = 0xdeadbeef;
int world::process_rank_hi_ = 0xdeadbeef;

namespace {
  int process_me;
  
  gex_TM_t the_team;
  
  std::atomic<bool> terminating{false};
}

alignas(64)
tmsg::channels_r<worker_n> world::remote_send_chan_r;
tmsg::channels_w<1> world::remote_send_chan_w[worker_n];
tmsg::channels_r<1> world::remote_recv_chan_r[worker_n];
tmsg::channels_w<worker_n> world::remote_recv_chan_w;

namespace {
  enum {
    id_am_master = GEX_AM_INDEX_BASE,
    id_am_worker,
  };
  
  void am_master(gex_Token_t, void *buf, size_t buf_size);
  void am_worker(gex_Token_t, void *buf, size_t buf_size, gex_AM_Arg_t worker_n);
  
  void init_gasnet();
  void master_pump();

  struct remote_in_messages: tmsg::message {
    int count;
  };
}

void world::run_and_die(const std::function<void()> &fn) {
  init_gasnet();
  process_rank_lo_ = process_me*worker_n;
  process_rank_hi_ = (process_me+1)*worker_n;
  
  tmsg::run_and_die([&]() {
    int tme = tmsg::thread_me();

    if(tme == 0) {
      for(int w=0; w < worker_n; w++)
        remote_recv_chan_w.connect(w, remote_recv_chan_r[w]);
    }
    else
      remote_send_chan_w[tme-1].connect(0, remote_send_chan_r);
    
    tmsg::barrier();
    
    if(tme == 0) {
      rank_me_ = -process_me - 1;
      master_pump();
    }
    else {
      rank_me_ = process_rank_lo_ + tme-1;
      fn();
      
      world::barrier();
      
      terminating.store(true, std::memory_order_release);
    }
  });
}

namespace {
  void init_gasnet() {
    #if GASNET_CONDUIT_SMP
      setenv("GASNET_PSHM_NODES", std::to_string(world::process_n).c_str(), 1);
    #endif
    
    int ok;
    gex_Client_t client;
    gex_EP_t endpoint;
    gex_Segment_t segment;
    
    ok = gex_Client_Init(
      &client, &endpoint, &the_team, "devastator", nullptr, nullptr, 0
    );
    ASSERT_ALWAYS(ok == GASNET_OK);

    ASSERT_ALWAYS(world::process_n == gex_TM_QuerySize(the_team));
    process_me = gex_TM_QueryRank(the_team);

    if(0) {
      int fd = open(("err."+std::to_string(process_me)).c_str(), O_CREAT|O_TRUNC|O_RDWR, 0666);
      ASSERT(fd >= 0);
      dup2(fd, 2);
    }
    
    gex_AM_Entry_t am_table[] = {
      {id_am_master, (void(*)())am_master, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 0, nullptr, "am_master"},
      {id_am_worker, (void(*)())am_worker, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 1, nullptr, "am_worker"}
    };
    ok = gex_EP_RegisterHandlers(endpoint, am_table, sizeof(am_table)/sizeof(am_table[0]));
    ASSERT_ALWAYS(ok == GASNET_OK);

    gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ok = gasnet_barrier_wait(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ASSERT_ALWAYS(ok == GASNET_OK);
  }
}

void world::progress() {
  bool did_something = tmsg::progress_noyield();

  int wme = tmsg::thread_me() - 1;
  did_something |= remote_send_chan_w[wme].cleanup();
  did_something |= remote_recv_chan_r[wme].receive(
    [](tmsg::message *m) {
      upcxx::parcel_reader r{m};
      remote_in_messages const *rm = &r.pop_trivial_aligned<remote_in_messages>();
      int n = rm->count;
      while(n--) {
        r.pop(0, 8);
        upcxx::command_execute(r);
      }
    }
  );
  
  static thread_local int consecutive_nothings = 0;

  if(did_something)
    consecutive_nothings = 0;
  else if(++consecutive_nothings == 10) {
    consecutive_nothings = 0;
    sched_yield();
  }
}

namespace {
  std::atomic<unsigned> barrier_done{0};
  
  void barrier_defer_try(unsigned done_value) {
    tmsg::send(0, [=]() {
      if(GASNET_OK == gasnet_barrier_try(0, GASNET_BARRIERFLAG_ANONYMOUS))
        barrier_done.store(done_value, std::memory_order_release);
      else
        barrier_defer_try(done_value);
    });
  }
}

void world::barrier() {
  static std::atomic<int> c[2]{{0}, {0}};
  static thread_local unsigned epoch = 0;
  
  int bump = epoch & 2 ? -1 : 1;
  int end = epoch & 2 ? 0 : worker_n;
  
  if(c[epoch & 1].fetch_add(bump) + bump != end) {
    while(c[epoch & 1].load(std::memory_order_acquire) != end)
      world::progress();
  }
  else {
    unsigned epoch1 = epoch + 1;
    tmsg::send(0, [=]() {
      gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
      barrier_defer_try(epoch1);
    });
  }

  while(barrier_done.load(std::memory_order_acquire) != epoch+1)
    world::progress();

  epoch += 1;
}

namespace {
  void am_master(gex_Token_t, void *buf, size_t buf_size) {
    upcxx::parcel_reader r{buf};
    upcxx::command_execute(r);
  }
  
  void am_worker(gex_Token_t, void *buf, size_t buf_size, gex_AM_Arg_t rank_n) {
    upcxx::parcel_reader r{buf};
    
    while(rank_n--) {
      std::uint16_t worker = r.pop_trivial_aligned<std::uint16_t>();
      std::uint16_t msg_n = r.pop_trivial_aligned<std::uint16_t>();
      std::uint32_t size8 = r.pop_trivial_aligned<std::uint32_t>();
      r.pop(0, 8);

      std::size_t size = 8*size8;
      
      upcxx::parcel_layout ub;
      ub.add_trivial_aligned<remote_in_messages>();
      ub.add_bytes(size, 8);
      
      void *buf = operator new(ub.size());
      upcxx::parcel_writer w{buf};
      remote_in_messages *rm = w.put_trivial_aligned<remote_in_messages>({});
      rm->count = msg_n;
      
      std::memcpy(w.put(size, 8), r.pop(size, 8), size);

      //say()<<"rrecv send w="<<worker<<" mn="<<msg_n;
      world::remote_recv_chan_w.send(worker, rm);
    }
  }

  void master_pump() {
    struct bundle {
      int next = -2; // -2=not in list, -1=none, 0 <= table index
      int workers_present = 0;
      std::size_t size8 = 0;
      // messages are in a singly-linked (using remote_out_message::bundle_next) circular list.
      remote_out_message *tail[world::worker_n] = {/*nullptr...*/};
    };
    
    std::unique_ptr<bundle[]> bun_table{ new bundle[world::process_n] };
    int bun_head = -1;
    
    while(!terminating.load(std::memory_order_relaxed)) {
      gasnet_AMPoll();
      
      bool did_something = tmsg::progress_noyield();
      
      did_something |= world::remote_recv_chan_w.cleanup();

      // Non-bundling algorithm. One message from a worker = one AM.
      #if 0
        did_something |= world::remote_send_chan_r.receive(
          [&](tmsg::message *m) {
            auto *rm = static_cast<remote_out_message*>(m);
            int proc = rm->rank / world::worker_n;
            int wrkr = rm->rank % world::worker_n;

            alignas(8) char buf[16<<10];
            upcxx::parcel_writer w{buf};
            w.put_trivial_aligned<uint16_t>(wrkr);
            w.put_trivial_aligned<uint16_t>(1);
            w.put_trivial_aligned<uint32_t>(rm->size8);
            std::memcpy(w.put(8*rm->size8, 8), rm+1, 8*rm->size8);
            
            //say()<<"gex_AM_RequestMedium1";
            gex_AM_RequestMedium1(
              the_team, proc,
              id_am_worker, buf, w.size(),
              GEX_EVENT_NOW, /*flags*/0,
              /*rank_n*/1
            );
          }
        );
      // Bundling algorithm.
      #else 
        did_something |= world::remote_send_chan_r.receive_batch(
          [&](tmsg::message *m) {
            auto *rm = static_cast<remote_out_message*>(m);
            int p = rm->rank / world::worker_n;
            int w = rm->rank % world::worker_n;
            //say()<<"rsend to p="<<p<<" w="<<w<<" sz="<<rm->size8;

            bundle *bun = &bun_table[p];
            bun->size8 += rm->size8;
            
            if(bun->tail[w] == nullptr) {
              bun->workers_present += 1;

              rm->bundle_next = rm;
              bun->tail[w] = rm;
            }
            else {
              rm->bundle_next = bun->tail[w]->bundle_next;
              bun->tail[w]->bundle_next = rm;
              bun->tail[w] = rm;
            }
            
            if(bun->next == -2) { // not in list
              bun->next = bun_head;
              bun_head = p;
            }
          },
          [&]() {
            while(bun_head != -1) {
              int proc = bun_head;
              bun_head = -1;
              
              do {
                bundle *bun = &bun_table[proc];
                int proc_next = bun->next;
                
                gex_AM_SrcDesc_t sd = gex_AM_PrepareRequestMedium(
                  the_team, /*rank*/proc,
                  /*client_buf*/nullptr,
                  /*min_length*/0,
                  /*max_length*/(8*bun->workers_present + 8*bun->size8),
                  /*lc_opt*/nullptr,
                  /*flags*/GEX_FLAG_IMMEDIATE,
                  /*numargs*/1);

                if(sd != GEX_AM_SRCDESC_NO_OP) {
                  void *ambuf = gex_AM_SrcDescAddr(sd);
                  std::size_t amlen = gex_AM_SrcDescSize(sd);

                  upcxx::parcel_writer w{ambuf};
                  std::size_t committed_size = 0;
                  int committed_workers = 0;
                  int potential_workers = 0;
                  
                  for(int worker=0; worker < world::worker_n; worker++) {
                    remote_out_message *rm_tail = bun->tail[worker];
                    
                    if(rm_tail != nullptr) {
                      remote_out_message *rm = rm_tail->bundle_next;
                      
                      potential_workers += 1;
                      
                      upcxx::parcel_layout laytmp;
                      laytmp = w.layout();
                      laytmp.add_trivial_aligned<uint16_t>();
                      laytmp.add_trivial_aligned<uint16_t>();
                      laytmp.add_trivial_aligned<uint32_t>();
                      if(laytmp.size() > amlen) goto am_full;
                      
                      w.put_trivial_aligned<uint16_t>(worker);
                      uint16_t *msg_n = w.put_trivial_aligned<uint16_t>(0);
                      uint32_t *size8 = w.put_trivial_aligned<uint32_t>(0);
                      w.put(0, 8);
                      
                      std::size_t w_offset = w.size();
                      
                      while(true) {
                        laytmp = w.layout();
                        laytmp.add_bytes(8*rm->size8, 8);
                        if(uint16_t(*msg_n + 1) == 0 || laytmp.size() > amlen) {
                          ASSERT(*msg_n > 0); // message size exceeds AM-Medium!
                          goto am_full;
                        }
                        
                        committed_size = laytmp.size();
                        committed_workers = potential_workers;
                        *msg_n += 1;
                        *size8 += rm->size8;
                        bun->size8 -= rm->size8;
                        
                        std::memcpy(w.put(8*rm->size8, 8), rm + 1, 8*rm->size8);

                        if(rm == rm_tail) break;
                        rm = rm->bundle_next;
                        rm_tail->bundle_next = rm;
                      }
                      
                      // depleted all messages to worker
                      bun->tail[worker] = nullptr;
                      bun->workers_present -= 1;
                    }
                  }

                  if(true) { // depleted all messages to proc
                    bun->next = -2; // not in list
                  }
                  else { // messages still remain to proc
                  am_full:
                    bun->next = bun_head;
                    bun_head = proc;
                  }

                  //say()<<"sending AM to "<<proc<<" sz="<<committed_size/8<<" wn="<<committed_workers;
                  gex_AM_CommitRequestMedium1(sd, id_am_worker, committed_size, committed_workers);

                  proc = proc_next;
                }
              } while(proc != -1);
            }
          }
        );
      #endif
      
      static thread_local int consecutive_nothings = 0;

      if(did_something)
        consecutive_nothings = 0;
      else if(++consecutive_nothings == 10) {
        consecutive_nothings = 0;
        sched_yield();
      }
    }
  }
}
