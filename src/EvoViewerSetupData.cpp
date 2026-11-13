#include "EvoViewerSetupData.h"

#include <CoreInterface.h>
#include <PointData/PointData.h>
#include <ClusterData/ClusterData.h>
#include <actions/DatasetPickerAction.h>
#include <actions/OptionAction.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

using namespace mv;
using namespace mv::plugin;
using namespace mv::gui;

namespace EvoViewerSetup
{
constexpr auto TREE_TYPE = "XSCTree";
constexpr auto TREE_METADATA_TYPE = "XSCTreeMeta";
QString datasetType(const Dataset<>& dataset)
{
    return dataset.isValid() ? dataset->getDataType().getTypeString() : QString{};
}

Datasets directChildrenOfType(const Dataset<>& parent, const DataType& type)
{
    Datasets matches;
    if (!parent.isValid())
        return matches;

    for (const auto& dataset : parent->getChildren()) {
        if (dataset.isValid() && dataset->getDataType() == type)
            matches << dataset;
    }

    return matches;
}

Datasets datasetsOfType(const QString& type)
{
    Datasets matches;
    for (const auto& dataset : mv::data().getAllDatasets())
        if (dataset.isValid() && datasetType(dataset) == type)
            matches << dataset;
    return matches;
}

bool chooseDatasets(const Datasets& mainPointsCandidates, DatasetChoices& choices, QObject* setup)
{
    const auto allTrees = datasetsOfType(QLatin1String(TREE_TYPE));
    const auto allMetadata = datasetsOfType(QLatin1String(TREE_METADATA_TYPE));
    if (mainPointsCandidates.isEmpty() || allTrees.isEmpty() || allMetadata.isEmpty())
        return false;

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Select EvoViewer datasets"));
    dialog.setModal(true);

    DatasetPickerAction mainPoints(&dialog, QStringLiteral("Main points dataset"));
    DatasetPickerAction embedding(&dialog, QStringLiteral("Embedding dataset"));
    DatasetPickerAction scatterEmbedding(&dialog, QStringLiteral("Scatterplot UMAP points"));
    DatasetPickerAction speciesNames(&dialog, QStringLiteral("Species names"));
    DatasetPickerAction bottomClusters(&dialog, QStringLiteral("Bottom cluster names"));
    DatasetPickerAction middleClusters(&dialog, QStringLiteral("Middle cluster names"));
    DatasetPickerAction topClusters(&dialog, QStringLiteral("Top cluster names"));
    DatasetPickerAction referenceTree(&dialog, QStringLiteral("Reference tree"));
    DatasetPickerAction metadata(&dialog, QStringLiteral("Trait metadata"));

    mainPoints.setDatasets(mainPointsCandidates);
    mainPoints.setCurrentDataset(mainPointsCandidates.first());
    referenceTree.setDatasets(allTrees);
    referenceTree.setCurrentDataset(allTrees.first());
    metadata.setDatasets(allMetadata);
    metadata.setCurrentDataset(allMetadata.first());

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Choose datasets for the linked views."), &dialog));

    const auto addPicker = [layout, &dialog](DatasetPickerAction& picker) {
        auto* row = new QHBoxLayout();
        row->addWidget(picker.createLabelWidget(&dialog));
        row->addWidget(picker.createWidget(&dialog), 1);
        layout->addLayout(row);
    };

    addPicker(mainPoints);
    addPicker(embedding);
    addPicker(scatterEmbedding);
    addPicker(speciesNames);
    addPicker(bottomClusters);
    addPicker(middleClusters);
    addPicker(topClusters);
    addPicker(referenceTree);
    addPicker(metadata);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto* okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setEnabled(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto updateDependentDatasets = [&]() {
        const Dataset<> selectedMain = mainPoints.getCurrentDataset();
        const auto pointChildren = directChildrenOfType(selectedMain, ::PointType);
        const auto clusterChildren = directChildrenOfType(selectedMain, ::ClusterType);

        const auto keepSelectionOrFirst = [](DatasetPickerAction& picker, const Datasets& candidates) {
            const QString previousId = picker.getCurrentDatasetId();
            picker.setDatasets(candidates);
            if (!previousId.isEmpty() && picker.getCurrentDatasetIds().contains(previousId))
                picker.setCurrentDataset(previousId);
            else if (!candidates.isEmpty())
                picker.setCurrentDataset(candidates.first());
        };

        keepSelectionOrFirst(embedding, pointChildren);
        keepSelectionOrFirst(scatterEmbedding, pointChildren);
        keepSelectionOrFirst(speciesNames, clusterChildren);
        keepSelectionOrFirst(bottomClusters, clusterChildren);
        keepSelectionOrFirst(middleClusters, clusterChildren);
        keepSelectionOrFirst(topClusters, clusterChildren);

        embedding.setEnabled(selectedMain.isValid() && !pointChildren.isEmpty());
        scatterEmbedding.setEnabled(selectedMain.isValid() && !pointChildren.isEmpty());
        speciesNames.setEnabled(selectedMain.isValid() && !clusterChildren.isEmpty());
        bottomClusters.setEnabled(selectedMain.isValid() && !clusterChildren.isEmpty());
        middleClusters.setEnabled(selectedMain.isValid() && !clusterChildren.isEmpty());
        topClusters.setEnabled(selectedMain.isValid() && !clusterChildren.isEmpty());
    };

    const auto updateAcceptEnabled = [&]() {
        const bool ready = mainPoints.getCurrentDataset().isValid() &&
            embedding.getCurrentDataset().isValid() &&
            scatterEmbedding.getCurrentDataset().isValid() &&
            speciesNames.getCurrentDataset().isValid() &&
            bottomClusters.getCurrentDataset().isValid() &&
            middleClusters.getCurrentDataset().isValid() &&
            topClusters.getCurrentDataset().isValid() &&
            referenceTree.getCurrentDataset().isValid() &&
            metadata.getCurrentDataset().isValid();
        okButton->setEnabled(ready);
    };

    QObject::connect(&mainPoints, &OptionAction::currentIndexChanged, &dialog, [&]() {
        updateDependentDatasets();
        updateAcceptEnabled();
    });
    for (auto* picker : { &embedding, &scatterEmbedding, &speciesNames, &bottomClusters,
                          &middleClusters, &topClusters, &referenceTree, &metadata })
        QObject::connect(picker, &OptionAction::currentIndexChanged, &dialog, updateAcceptEnabled);

    updateDependentDatasets();
    updateAcceptEnabled();

    QObject::connect(setup, &QObject::destroyed, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    choices = {
        mainPoints.getCurrentDataset(), embedding.getCurrentDataset(), scatterEmbedding.getCurrentDataset(),
        speciesNames.getCurrentDataset(), bottomClusters.getCurrentDataset(), middleClusters.getCurrentDataset(),
        topClusters.getCurrentDataset(), referenceTree.getCurrentDataset(), metadata.getCurrentDataset()
    };
    return true;
}

}
