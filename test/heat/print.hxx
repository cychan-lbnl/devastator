#pragma once

#include <iostream>
#include <sstream>

// Print class modified from AMReX_Print.H

class Print
{
public:

  static constexpr const int AllRanks = -1;

  Print (int rank = 0, std::ostream & os_ = std::cout) : os(os_)
  {
    flag_enabled = (rank == AllRanks || deva::rank_me() == rank);
    ss.precision(os.precision());
  }

  ~Print () {
    if (flag_enabled) {
      os.flush();
      os << ss.str();
      os.flush();
    }
  }

  Print & SetPrecision (int p) {
    if (flag_enabled) {
      ss.precision(p);
    }
    return *this;
  }

  template <typename T>
  Print & operator<< (const T & x) {
    if (flag_enabled) {
      ss << x;
    }
    return *this;
  }

  Print & operator<< (std::basic_ostream<char, std::char_traits<char>> &
              (*func)(std::basic_ostream<char, std::char_traits<char>> &))
  {
    if (flag_enabled) {
      ss << func;
    }
    return *this;
  }

private:
  bool flag_enabled;
  std::ostream & os;
  std::ostringstream ss;
};

class AllPrint : public Print
{
public:
  AllPrint (std::ostream & os_ = std::cout) : Print(Print::AllRanks, os_) {}
};
