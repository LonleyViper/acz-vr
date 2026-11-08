#pragma once
#include "probe_core.h"
#include <memory>
namespace acz::probe {
// Factory itself performs no loader/runtime/device work.
std::unique_ptr<Backend> make_real_backend(Clock&, Log, Mode mode = Mode::zero_layers);
}
