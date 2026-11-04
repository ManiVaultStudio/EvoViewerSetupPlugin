#include "EvoViewerSetupRequirements.h"

#include <AbstractPluginManager.h>
#include <CoreInterface.h>
#include <PointData/PointData.h>
#include <ClusterData/ClusterData.h>

#include <array>
#include <vector>

namespace EvoViewerSetup
{
bool requiredPluginsAvailable()
{
    static const std::array<const char*, 6> requiredKinds{
        "Scatterplot View",
        "XSC Cluster Rank",
        "XSCGeneDetect",
        "XSCPhyTVu",
        "ChartLegend View",
        "Tutorial"
    };

    for (const auto* kind : requiredKinds)
        if (!mv::plugins().getPluginFactory(QString::fromUtf8(kind)))
            return false;

    return true;
}

bool requiredTreeDatasetsAvailable()
{
    bool hasTree = false;
    bool hasMetadata = false;
    for (const auto& dataset : mv::data().getAllDatasets()) {
        if (!dataset.isValid())
            continue;

        const auto type = dataset->getDataType().getTypeString();
        hasTree |= type == QStringLiteral("XSCTree");
        hasMetadata |= type == QStringLiteral("XSCTreeMeta");
        if (hasTree && hasMetadata)
            return true;
    }

    return false;
}

mv::Datasets eligibleMainPointDatasets()
{
    mv::Datasets eligible;
    const auto mainPointDatasets = mv::data().getAllDatasets(std::vector<mv::DataType>{ ::PointType });

    for (const auto& mainPoints : mainPointDatasets) {
        if (!mainPoints.isValid())
            continue;

        std::size_t clusterChildren = 0;
        bool hasPointChild = false;
        for (const auto& child : mainPoints->getChildren()) {
            if (!child.isValid())
                continue;

            const auto& childType = child->getDataType();
            if (childType == ::ClusterType)
                ++clusterChildren;
            else if (childType == ::PointType)
                hasPointChild = true;

            if (clusterChildren >= 3 && hasPointChild)
                break;
        }

        if (clusterChildren >= 3 && hasPointChild)
            eligible << mainPoints;
    }

    return eligible;
}
}
