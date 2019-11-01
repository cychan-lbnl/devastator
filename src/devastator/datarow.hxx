#ifndef _ffc73fcff3e04beb87c9dfbf32721185
#define _ffc73fcff3e04beb87c9dfbf32721185

#include <functional>
#include <map>
#include <string>
#include <sstream>
#include <utility>

namespace deva {
  // A datarow is a conjunction of name=value assignments. All names are strings
  // and values are either doubles or strings. Datarow's are equality comparable
  // and hashable.
  
  // To build singleton rows use either `datarow::x(name,val)` or `datarow::y(name,val)`
  // for independent (x) vs depenent (y) variables. To build composite rows
  // glue these together with `operator&`. Ex:
  //   datarow::x("name", "john") & datarow::y("programming_skill", -1.e9);

  class datarow;

  //////////////////////////////////////////////////////////////////////////////
  
  class datarow {
    template<typename> friend struct std::hash;
    
    struct variant {
      bool y_not_x;
      bool num_not_str;
      union { std::string str; double num; };

      variant(): y_not_x(false), num_not_str(true), num(0.0) {}
      variant(int num, bool y_not_x):
        y_not_x(y_not_x),
        num_not_str(true),
        num(num) {
      }
      variant(double num, bool y_not_x):
        y_not_x(y_not_x),
        num_not_str(true),
        num(num) {
      }
      variant(std::string str, bool y_not_x):
        y_not_x(y_not_x),
        num_not_str(false),
        str(str) {
      }
      
      variant(variant const &that):
        y_not_x(that.y_not_x),
        num_not_str(that.num_not_str) {
        if(num_not_str)
          ::new(&num) double(that.num);
        else
          ::new(&str) std::string(that.str);
      }
      
      variant(variant &&that):
        y_not_x(that.y_not_x),
        num_not_str(that.num_not_str) {
        if(num_not_str)
          ::new(&num) double(that.num);
        else
          ::new(&str) std::string(std::move(that.str));
      }
      
      ~variant() {
        if(!num_not_str) {
          using std::string;
          str.~string();
        }
      }

      variant& operator=(variant const &that) {
        if(this == &that) return *this;
        this->~variant();
        return *::new(this) variant(that);
      }
      variant& operator=(variant &&that) {
        this->~variant();
        return *::new(this) variant(std::move(that));
      }

      friend bool operator==(variant const &a, variant const &b) {
        return a.y_not_x == b.y_not_x &&
               a.num_not_str == b.num_not_str &&
               (a.num_not_str ? a.num == b.num : a.str == b.str);
      }
      friend bool operator!=(variant const &a, variant const &b) {
        return !(a == b);
      }

      void to_python(std::ostream &o) const;
      static variant from_python(std::istream &s);
    };
    
    std::map<std::string, variant> m_;
    
  public:
    datarow() = default;

    template<typename T>
    datarow(std::string name, T val, bool y_not_x) {
      m_.insert({std::move(name), variant(std::move(val), y_not_x)});
    }
    
    bool operator==(datarow const &that) const {
      return this->m_ == that.m_;
    }
    bool operator!=(datarow const &that) const {
      return this->m_ != that.m_;
    }

    void to_python_kwargs(bool x_not_y, std::ostream &o, bool leading_comma);
    void xs_to_python_kwargs(std::ostream &o, bool leading_comma) { to_python_kwargs(true, o, leading_comma); }
    void ys_to_python_kwargs(std::ostream &o, bool leading_comma) { to_python_kwargs(false, o, leading_comma); }
    
    static datarow from_python_kwargs(std::istream &i);
    static datarow from_python_kwargs(std::string const &s) {
      std::istringstream iss(s);
      return from_python_kwargs(iss);
    }

    // Build a row consisting of a single key=val pair. Combine multiple of these
    // with operator&.
    template<typename T>
    static datarow x(std::string name, T val) {
      return datarow(std::move(name), std::move(val), false);
    }
    template<typename T>
    static datarow y(std::string name, T val) {
      return datarow(std::move(name), std::move(val), true);
    }

    friend datarow& operator&=(datarow &a, datarow b);
    friend datarow operator&(datarow a, datarow b);

    // return this datarow but with all names prefixed by `prefix`
    datarow prefix_all(std::string prefix);
  };

  datarow& operator&=(datarow &a, datarow b);
  datarow operator&(datarow a, datarow b);
}

namespace std {
  template<>
  struct hash<deva::datarow> {
    std::size_t operator()(deva::datarow const &row) const;
  };
}

#endif
