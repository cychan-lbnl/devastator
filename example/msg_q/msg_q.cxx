#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>

#include <cstdint>
#include <deque>
#include <iostream>

using namespace std;

using deva::process_n;
using deva::rank_n;
using deva::rank_me;
using deva::rank_me_local;
constexpr int proc_rank_n = rank_n / process_n;

struct Msg
{
  int idx;
  double val;

  friend ostream & operator<< (ostream & os, const Msg & msg) {
    os << "(" << msg.idx << ", " << msg.val << ")";
    return os;
  }
};

array<deque<Msg>, proc_rank_n> queues;

deque<Msg> & myQ ()
{
  return queues[rank_me_local()];
}

void qPut (int dest_rank, Msg msg)
{
  cout << "Rank " << rank_me() << " sending data " << msg << " to " << dest_rank << endl;
  deva::send(dest_rank,
    // this lambda is executed at the receiving rank
    [] (Msg m) {
      cout << "Rank " << rank_me() << " received data " << m << endl;
      myQ().push_back(std::move(m));
    },
    std::move(msg)
  );
}

bool qGet (Msg & msg)
{
  auto & q = myQ();
  if (q.size()) {
    msg = std::move(q.front());
    q.pop_front();
    return true;
  }
  return false;
}

int qSize ()
{
  return myQ().size();
}

int main ()
{
  deva::run(
    // this lambda is executed on every rank
    [] () {
      int dest_rank = (rank_me() + 1) % rank_n;
      int rounds = 10;

      if (rank_me() == 0) {
        qPut(dest_rank, {1, 1});
        rounds--;
      }

      while (rounds > 0) {
        Msg msg;
        if (qGet(msg)) {
          cout << "Rank " << rank_me() << " processing data " << msg << endl;
          rounds--;
          if (!(rounds == 0 && dest_rank == 0)) {
            qPut(dest_rank, {msg.idx + 1, msg.val + 1});
          }
        }
        while (rounds > 0 && qSize() == 0) {
          deva::progress();
        }
      }
    });

  cout << "SUCCESS" << endl;

  return 0;
}
