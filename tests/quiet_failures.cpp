// Linked into every unit test: CRT assertions, errors and aborts are reported on stderr and end the process with a failure code
// instead of opening a modal dialog that would block an unattended run.
#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>
#include <windows.h>

namespace {

const bool kQuietFailures = [] {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  _set_abort_behavior(0, _CALL_REPORTFAULT);
  for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
  return true;
}();

}
#endif
