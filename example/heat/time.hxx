#pragma once

#include <devastator/diagnostic.hxx>

using Time = unsigned;

constexpr int priority_bit_n = 1;
uint64_t get_timestamp (Time t, unsigned priority)
{
  DEVA_ASSERT(priority < 1 << priority_bit_n);
  return static_cast<uint64_t>(t << priority_bit_n | priority);
}

int get_time (uint64_t timestamp)
{
  return static_cast<int>(timestamp >> priority_bit_n);
}
