#include <devastator/datarow.hxx>
#include <devastator/diagnostic.hxx>

#include <cstring>
#include <vector>

using namespace std;
using namespace deva;

void datarow::variant::to_python(std::ostream &o) const {
  if(num_not_str)
    o << num;
  else {
    o << '"';
    for(char c: str) {
      if(c == '"' || c == '\\')
        o << '\\';
      o << c;
    }
    o << '"';
  }
}

datarow::variant datarow::variant::from_python(std::istream &s) {
  char c = s.peek();
  if(('0' <= c && c <= '9') || c == '.') {
    double num;
    s >> num;
    return variant(num, false);
  }
  else {
    std::vector<char> buf;
    bool quote;
    char const *delims;

    if(c == '"' || c == '\'') {
      quote = true;
      delims = c == '"' ? "\"" : "'";
      s.get();
      c = s.peek();
    }
    else {
      quote = false;
      delims = "=:, \t\n";
    }
    
    while(c != std::istream::traits_type::eof() && std::strchr(delims, c) == nullptr) {
      if(c == '\\') s.get();
      buf.push_back(s.get());
      c = s.peek();
    }

    if(quote)
      s.get();

    return variant(std::string(&buf[0], buf.size()), false);
  }
}

void datarow::to_python_kwargs(bool x_not_y, std::ostream &o, bool leading_comma) {
  for(auto const &xy: m_) {
    if(x_not_y == !xy.second.y_not_x) {
      if(leading_comma) o << ", ";
      leading_comma = true;
      o << xy.first << '=';
      xy.second.to_python(o);
    }
  }
}

datarow datarow::from_python_kwargs(std::istream &i) {
  auto eat = [&](const char *white) {
    char c = i.peek();
    while(std::strchr(white, c) != nullptr) {
      i.get();
      c = i.peek();
    }
  };

  datarow ans;

  eat(" \t\n");
  while(i.peek() != std::istream::traits_type::eof()) {
    eat(" \t\n");
    variant name = variant::from_python(i);
    eat(" \t\n");
    DEVA_ASSERT_ALWAYS(std::strchr("=:", i.get()) != nullptr);
    eat(" \t\n");
    variant val = variant::from_python(i);
    ans.m_.insert({std::move(name.str), std::move(val)});
    eat(", \t\n");
  }
  
  return ans;
}

datarow datarow::prefix_all(std::string prefix) {
  datarow ans;
  for(auto const &xy: m_)
    ans.m_.insert({prefix + xy.first, xy.second});
  return ans;
}

datarow& deva::operator&=(datarow &a, datarow b) {
  for(auto &xy: b.m_)
    a.m_.insert({std::move(xy.first), std::move(xy.second)});
  return a;
}

datarow deva::operator&(datarow a, datarow b) {
  a &= b;
  return a;
}

std::size_t std::hash<datarow>::operator()(datarow const &row) const {
  std::size_t h = 0xdeadbeef;
  for(auto const &xy: row.m_) {
    h += std::hash<std::string>()(xy.first);
    h *= 31;
    h ^= h >> 11;
    h += (int)xy.second.y_not_x<<20 | (int)xy.second.num_not_str;
    h *= 31;
    if(xy.second.num_not_str)
      h += std::hash<double>()(xy.second.num);
    else
      h += std::hash<std::string>()(xy.second.str);
    h ^= h >> 11;
  }
  return h;
}
