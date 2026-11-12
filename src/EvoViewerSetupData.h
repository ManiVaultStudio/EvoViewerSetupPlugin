#pragma once

#include <Dataset.h>

namespace EvoViewerSetup
{
struct DatasetChoices
{
    mv::Dataset<> mainPoints;
    mv::Dataset<> embedding;
    mv::Dataset<> scatterEmbedding;
    mv::Dataset<> speciesNames;
    mv::Dataset<> bottomClusters;
    mv::Dataset<> middleClusters;
    mv::Dataset<> topClusters;
    mv::Dataset<> referenceTree;
    mv::Dataset<> metadata;
};

bool chooseDatasets(const mv::Datasets& mainPointsCandidates, DatasetChoices& choices, QObject* setup);
}
