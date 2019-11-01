#ifndef _1c7c899e519c4bb88e37cf475d78ac5f
#define _1c7c899e519c4bb88e37cf475d78ac5f

#include <devastator/os_env.hxx>
#if DEVA_WORLD
  #include <devastator/world.hxx>
#endif

#include <devastator/datarow.hxx>

#include <iostream>
#include <string>

namespace deva {
namespace bench {
  // Writes a report file consisting of emitted rows. This may not be entered
  // concurrently, so you will need to funnel your report data to a single rank
  // to write the report. Constructor reads env vars:
  //  "report_file": location to append data points, special value "-" indicates
  //    stdout (default="report.out").
  //  "report_kwargs": python formatted string of keyword argument assignments
  //    to be passed as additional independent variable assignments to the `emit`
  //    function.
  class report {
    std::string filename;
    std::ostream *f;
    deva::datarow ambient;
    
  public:
    report(const char *appstr/* = typically pass __FILE__*/);
    ~report();
    
    void blank() { *f << std::endl; }
    
    void emit(deva::datarow row);
  };
}}
#endif
