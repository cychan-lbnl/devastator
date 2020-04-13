#ifndef _82ccee80dbae49ffbe66af1ed1ede46f
#define _82ccee80dbae49ffbe66af1ed1ede46f

namespace deva {
  struct rng_xoshiro256ss {
    std::uint64_t s[4];

    rng_xoshiro256ss(std::uint64_t seed=0) {
      for(int i=0; i < 4; i++) {
        seed += 0x9E3779B97f4A7C15;
        std::uint64_t x = seed;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EB;
        s[i] = x ^ (x >> 31);
      }
    }

    static std::uint64_t rol64(std::uint64_t x, int k) {
      return (x << k) | (x >> (64 - k));
    }
    
    std::uint64_t operator()() {
      std::uint64_t const result = rol64(s[1] * 5, 7) * 9;
      std::uint64_t const t = s[1] << 17;
      
      s[2] ^= s[0];
      s[3] ^= s[1];
      s[1] ^= s[2];
      s[0] ^= s[3];
      
      s[2] ^= t;
      s[3] = rol64(s[3], 45);
      
      return result;
    }
  };
}
#endif
