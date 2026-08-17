// SPDX-License-Identifier: Apache-2.0
#pragma once

// Convenience umbrella: verification components live in the independently
// packageable cpptb_vc include tree; this header forwards to it.
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
