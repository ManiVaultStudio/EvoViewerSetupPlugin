#pragma once

#include <Dataset.h>

namespace EvoViewerSetup
{
bool requiredPluginsAvailable();
bool requiredTreeDatasetsAvailable();
mv::Datasets eligibleMainPointDatasets();
}
