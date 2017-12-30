#include "world_gasnet.hxx"

#include <gasnetex.h>
#include <gasnet.h>

#include <atomic>
#include <thread>

#include <sched.h>

using world::worker_n;

thread_local int world::thread_me_ = 0xdeadbeef;
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
tmsg::active_channels_r<1+worker_n> world::lchans_r[1+worker_n];
tmsg::active_channels_w<1+worker_n> world::lchans_w[1+worker_n];

tmsg::channels_r<worker_n> world::outgoing_rchans_r;
tmsg::channels_w<1> world::outgoing_rchans_w[worker_n];
tmsg::channels_r<1> world::incoming_rchans_r[worker_n];
tmsg::channels_w<worker_n> world::incoming_rchans_w;

namespace {
  enum {
    id_am_master = GEX_AM_INDEX_BASE,
    id_am_worker
  };
  
  void am_master(gex_Token_t, void *buf, size_t buf_size);
  void am_worker(gex_Token_t, void *buf, size_t buf_size, gex_AM_Arg_t worker_n);

  void init_gasnet();
  void master_pump();

  struct remote_in_messages: tmsg::message {
    int count;
  };
}

void world::progress() {
  bool did_something;

  did_something =  lchans_w[thread_me_].cleanup();
  did_something |= lchans_r[thread_me_].receive();

  did_something |= outgoing_rchans_w[thread_me_-1].cleanup();
  did_something |= incoming_rchans_r[thread_me_-1].receive(
    [](message *m) {
      upcxx::parcel_reader r{m};
      remote_in_messages *rm = r.pop_trivial_aligned<remote_in_messages>();
      int n = rm->count;
      while(n--)
        upcxx::command_execute(r);
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
    world::lchans_w.send(0, [=]() {
      if(GASNET_OK == gasnet_barrier_try(0, GASNET_BARRIERFLAG_ANONYMOUS))
        done.store(done_value, std::memory_order_release);
      else
        barrier_defer_try(done_value);
    });
  }
}

void world::barrier() {
  static std::atomic<int> c[2]{{0}, {0}};
  static thread_local unsigned epoch = 0;
  
  int end = epoch & 2 ? 0 : rank_n;
  int bump = epoch & 2 ? -1 : 1;
  
  if((c[epoch & 1] += bump) != end) {
    while(c[epoch & 1].load(std::memory_order_acquire) != end)
      progress();
  }

  if(thread_me_ == 1) {
    lchans_w.send(0, [=]() {
      gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
      barrier_defer_try(epoch+1);
    });
  }

  while(barrier_done.load(std::memory_order_acquire) != epoch+1)
    progress();
  
  epoch += 1;
}

void world::run(const std::function<void()> &fn) {
  std::thread *workers[worker_n];
  
  auto tmain = [=](int me) {
    thread_me_ = me;
    
    for(int t=0; t < worker_n; t++)
      lchans_w[me].connect(t, lchans_r[t]);
    
    if(me == 0) {
      for(int t=0; t < worker_n; t++)
        incoming_rchans_w.connect(t, incoming_rchans_r[t]);
      
      init_gasnet();
      process_rank_lo_ = process_me*worker_n;
      process_rank_hi_ = (process_me+1)*worker_n;
    }
    else {
      for(int t=0; t < worker_n; t++)
        outgoing_rchans_w[me].connect(0, outgoing_rchans_r);
    }

    barrier();
    
    if(me == 0) {
      rank_me_ = 0xdeadbeef;
      master_pump();
    }
    else {
      rank_me_ = process_rank_lo_ + me-1;
      fn();

      static std::atomic<int> bar{0};
      
      if(worker_n != (bar += 1)) {
        while(bar.load(std::memory_order_acquire) != worker_n)
          sched_yield();
      }
      else
        terminating.store(true, std::memory_order_release);
    }
  };
  
  for(int t=0; t < worker_n; t++)
    workers[t] = new std::thread(tmain, t);
  tmain(0);
  
  for(int t=0; t < worker_n; t++) {
    threads[t]->join();
    delete threads[t];
  }
}

namespace {
  void am_master(gex_Token_t, void *buf, size_t buf_size) {
    upcxx::parcel_reader r{buf};
    upcxx::command_execute(r);
  }
  
  void am_worker(gex_Token_t, void *buf, size_t buf_size, gex_AM_Arg_t rank_n) {
    upcxx::parcel_reader r{buf};

    for(int r=0; r < rank_n; r++) {
      std::uint16_t worker = r.pop_trivial_aligned<std::uint16_t>();
      std::uint16_t msg_n = r.pop_trivial_aligned<std::uint16_t>();
      std::uint32_t size8 = r.pop_trivial_aligned<std::uint32_t>();
      r.pop(0, 8);
      
      std::size_t size = 8*size8;
      
      upcxx::parcel_layout ub;
      ub.add_trivial_aligned<remote_in_messages>({});
      ub.add_bytes(size, 8);
      
      void *buf = operator new(ub.size());
      upcxx::parcel_writer w{buf};
      remote_in_messages *rm = w.put_trivial_aligned<remote_in_messages>({});
      rm->count = msg_n;
      
      std::memcpy(w.put(size, 8), r.pop(size, 8), size);
      
      world::incoming_rchans_w.send(worker, rm);
    }
  }

  void master_pump() {
    struct bundle {
      int next = -2;
      std::size_t size8 = 0;
      int workers_present = 0;
      remote_out_message *head[world::worker_n] = {/*nullptr...*/};
    };
    
    std::unique_ptr<bundle[]> bun_table{ new bundle[world::process_n] };
    int bun_head = -1;
    
    while(!terminating.load(std::memory_order_relaxed)) {
      bool did_something;

      did_something =  lchans_w[/*thread_me*/0].cleanup();
      did_something |= lchans_r[/*thread_me*/0].receive();
      
      did_something |= incoming_rchans_w.cleanup();

      did_something |= outgoing_rchans_r.receive_batch(
        [&](message *m) {
          auto *rm = static_cast<remote_out_message*>(m);
          int p = rm->rank / world::worker_n;
          int w = rm->rank % world::worker_n;

          bun_table[p].size8 += rm->size8;
          
          if(bun_table[p].head[w] == nullptr)
            bun_table[p].workers_present += 1;

          rm->bundle_next = bun_table[p].head[w];
          bun_table[p].head[w] = rm;
          
          if(bun_table[p].next == -2) {
            bun_table[p].next = bun_head;
            bun_head = p;
          }
        },
        [&]() {
          while(bun_head != -1) {
            int proc = bun_head;
            int bun_head1 = -1;
            
            do {
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
                  remote_out_message *rm = bun_table[proc].head[worker];
                  if(rm != nullptr) {
                    potential_workers += 1;
                    
                    upcxx::parcel_layout laytmp;
                    laytmp = w.layout();
                    laytmp.add_trivial_aligned<std::uint16_t>();
                    laytmp.add_trivial_aligned<std::uint16_t>();
                    laytmp.add_trivial_aligned<std::uint32_t>();
                    if(laytmp.size() > amlen) goto am_full;
                    
                    w.put_trivial_aligned<std::uint16_t>(worker);
                    std::uint16_t *msg_n = w.put_trivial_aligned<std::uint16_t>(0);
                    std::uint32_t *size8 = w.put_trivial_aligned<std::uint32_t>(0);
                    w.put(0, 8);
                    
                    std::size_t w_offset = w.size();
                    
                    do {
                      laytmp = w.layout();
                      laytmp.add_bytes(8*rm->size8, 8);
                      if(laytmp.size() > amlen) {
                        ASSERT(*msg_n > 0); // message size exceeds AM-Medium!
                        goto am_full;
                      }
                      
                      committed_size = lay.size();
                      committed_workers = potential_workers;
                      *msg_n += 1;
                      *size8 += rm->size8;
                      bun_table[proc].size8 -= rm->size8;
                      
                      std::memcpy(w.put(8*rm->size8, 8), rm + 1, 8*rm->size8);
                      
                      rm = rm->bundle_next;
                      bun_table[proc].head[worker] = rm;
                    } while(rm != nullptr);

                    // depleted all messages to worker
                    bun_table[proc].workers_present -= 1;
                  }
                }

                if(true) { // depleted all messages to proc
                  bun_table[proc].next = -2;
                }
                else { // messages still remain to proc
                am_full:
                  bun_table[proc].next = bun_head1;
                  bun_head1 = proc;
                }
                
                gex_AM_CommitRequestMedium1(sd, id_am_worker, committed_size, committed_workers);
              }
            } while(bun_head != -1);

            bun_head = bun_head1;
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
  }

  void init_gasnet() {
    int ok;
    gex_Client_t client;
    gex_EP_t endpoint;
    gex_Segment_t segment;
    
    ok = gex_Client_Init(
      &client, &endpoint, &the_team, "pdes", nullptr, nullptr, 0
    );
    ASSERT_ALWAYS(ok == GASNET_OK);
    
    ASSERT_ALWAYS(world::process_n == gex_TM_QuerySize(the_team));
    process_me = gex_TM_QueryRank(the_team);

    gex_AM_Entry_t am_table[] = {
      {id_am_master, (void(*)())am_master, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 0, nullptr, "am_master"},
      {id_am_worker, (void(*)())am_worker, GEX_FLAG_AM_MEDIUM | GEX_FLAG_AM_REQUEST, 1, nullptr, "am_worker"},
    };
    ok = gex_EP_RegisterHandlers(endpoint, am_table, sizeof(am_table)/sizeof(am_table[0]));
    ASSERT_ALWAYS(ok == GASNET_OK);

    gasnet_barrier_notify(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ok = gasnet_barrier_wait(0, GASNET_BARRIERFLAG_ANONYMOUS);
    ASSERT_ALWAYS(ok == GASNET_OK);
  }
}
