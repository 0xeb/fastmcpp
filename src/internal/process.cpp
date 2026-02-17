// Process dispatcher - includes platform-specific implementation
// This file is added to CMakeLists.txt as a single source; it pulls in the
// correct platform implementation via the preprocessor.

#ifdef _WIN32
#include "process_win32.cpp"
#else
#include "process_posix.cpp"
#endif
