#ifndef TMA_H
#define TMA_H

// Compatibility include for historical GTSim code. New architecture-specific
// code must name BulkCopyUnit and BulkCopyMechanism explicitly; these aliases
// keep sealed Hopper TMA callers source-compatible.
#include "bulk_copy.h"

namespace GTSim {

using TMARequest = BulkCopyRequest;
using TMAUnit = BulkCopyUnit;

}  // namespace GTSim

#endif  // TMA_H
