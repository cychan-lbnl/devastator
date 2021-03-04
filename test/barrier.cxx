#include <devastator/diagnostic.hxx>
#include <devastator/world.hxx>
#include <devastator/pdes.hxx>

#include <iostream>
#include <chrono>

using namespace std;

void spin ()
{
  double a = 1.1, b = 1.2;
  for (int i = 0; i < 1000000; ++i) {
    a = b / a;
    b = a / b;
  }
}

int main ()
{
  deva::run([] () {
    if (deva::process_me() == 1 && deva::rank_me_local() == 0) {
      cout << "Process 1 spinning for " << 1 << " second ..." << endl;
      auto start_time = chrono::steady_clock::now();
      while (std::chrono::duration<double>(chrono::steady_clock::now() - start_time).count() < 1) {
        spin();
      }
    }
  });

  return 0;
}
