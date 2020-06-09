#include "deva_includes.hpp"

int main() {
  deva::run([&]() {
    std::cout<<"Hello from devastator."<<std::endl;
  });
  return 0;
}
