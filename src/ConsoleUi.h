#pragma once
#include "Collector.h"

namespace elm
{
    // Runs a collection with live console progress and a summary. With pause set,
    // an interactive console waits for a key before returning the exit code.
    int RunConsoleCollection(const std::wstring &directory, const CollectionOptions &options, bool pause);
}
