#include "report.hxx"

#include <devastator/diagnostic.hxx>

#include <cstring>
#include <ctime>
#include <fstream>

deva::bench::report::report(const char *appstr) {
  std::string app = std::string(appstr);
  
  std::size_t pos = app.rfind("/");
  pos = pos == std::string::npos ? 0 : pos+1;
  app = app.substr(pos);
  
  if(app.size() > 4 && (
     app.substr(app.size()-4, 4) == ".cpp" ||
     app.substr(app.size()-4, 4) == ".cxx"
    )) {
    app = app.substr(0, app.size()-4);
  }
  
  ambient = deva::datarow::x("app", app);
  ambient &= deva::datarow::from_python_kwargs(deva::os_env<std::string>("report_kwargs", ""));
  
  filename = deva::os_env<std::string>("report_file", "report.out");

  ambient &= deva::describe();

  if(filename == "-")
    f = &std::cout;
  else {
    f = new std::ofstream(filename, std::ofstream::app);
    
    // write the current time as a comment
    std::time_t t = std::time(0);
    std::tm now = *std::localtime(&t);
    char buf[128];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %X", &now);
    *f << "# Opened for append at "<<buf<<std::endl;
  }
}

deva::bench::report::~report() {
  if(f != &std::cout) {
    std::cerr << "Report written to '" << filename << "'." << std::endl;
    delete f;
  }
}

void deva::bench::report::emit(deva::datarow row) {
  row &= ambient;
  *f << "row(xs=dict(";
  row.xs_to_python_kwargs(*f, false);
  *f << "),\n    ys=dict(";
  row.ys_to_python_kwargs(*f, false);
  *f << "))\n";
}
