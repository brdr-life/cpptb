// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compatibility header for code written before verification components moved
// into the independently packageable cpptb_vc include tree.
#include "cpptb_vc/cpptb_vc.hpp"

namespace cpptb {

using vc::AnalysisBuffer;
using vc::AnalysisOverflowPolicy;
using vc::AnalysisPort;
using vc::AnalysisSubscriber;
using vc::GetPort;
using vc::InOrderScoreboard;
using vc::PutPort;
using vc::ReadyValidDriver;
using vc::ReadyValidMonitor;
using vc::ReadyValidSampleEdge;

}  // namespace cpptb
