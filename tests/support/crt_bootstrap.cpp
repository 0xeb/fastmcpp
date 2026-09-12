// Make a failed assertion in a Debug test fail the test, instead of waiting for
// someone to click a button.
//
// The Debug CRT reports a failed `assert()` through a modal dialog and then calls
// abort(), which also wants to show one. In an interactive session that stops the
// run dead: ctest sits there, the suite never finishes, and the only signal is a
// window nobody is watching. On a build agent the dialog most likely cannot be
// shown at all, but the workflows set no ctest timeout, so anything that did block
// would run to the job limit.
//
// Routing the reports to stderr costs nothing and makes the failure behave like
// every other failure: a message and a non-zero exit.
//
// Linked into test targets as an OBJECT library. That matters -- a static library
// only contributes the objects needed to resolve a referenced symbol, and nothing
// references this file, so as a STATIC library it would link cleanly and do
// nothing at all.

#if defined(_MSC_VER) && defined(_DEBUG)

#include <crtdbg.h>
#include <stdlib.h>

namespace
{

struct CrtBootstrap
{
    CrtBootstrap()
    {
        // abort(): no "abnormal program termination" popup, and no Watson report.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

        // assert() and CRT errors: write to stderr rather than open a window.
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    }
};

// Runs before main(). The object is otherwise unused, which is exactly why this
// has to be an OBJECT library.
const CrtBootstrap g_crt_bootstrap;

} // namespace

#endif // defined(_MSC_VER) && defined(_DEBUG)
