#include "EvoViewerSetupPlugin.h"

#include <CoreInterface.h>
#include <Plugin.h>
#include <actions/OptionAction.h>
#include <actions/OptionsAction.h>
#include <actions/StringAction.h>

#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QStringList>
#include <util/DockArea.h>

using namespace mv;
using namespace mv::plugin;
using namespace mv::gui;

namespace
{
struct ViewRequest
{
    const char* kind;
    const char* guiName;
    ViewPlugin* dockTarget;
    DockAreaFlag dockArea;
};

bool containsView(QWidget* branch, ViewPlugin* view)
{
    return branch && view && (branch == &view->getWidget() || branch->isAncestorOf(&view->getWidget()));
}

void applyEvoViewerSplitSizes(QWidget* workspace, ViewPlugin* cluster, ViewPlugin* scatter,
                              ViewPlugin* genes, ViewPlugin* tree)
{
    if (!workspace || !cluster || !scatter || !genes || !tree)
        return;

    for (auto* splitter : workspace->findChildren<QSplitter*>()) {
        const auto count = splitter->count();
        if (count < 2)
            continue;

        QList<bool> hasCluster(count), hasScatter(count), hasGenes(count), hasTree(count);
        for (int i = 0; i < count; ++i) {
            auto* branch = splitter->widget(i);
            hasCluster[i] = containsView(branch, cluster);
            hasScatter[i] = containsView(branch, scatter);
            hasGenes[i] = containsView(branch, genes);
            hasTree[i] = containsView(branch, tree);
        }

        QList<int> weights(count, 1);
        bool apply = false;

        if (splitter->orientation() == Qt::Vertical) {
            int clusterIndex = -1;
            int lowerIndex = -1;
            for (int i = 0; i < count; ++i) {
                if (hasCluster[i])
                    clusterIndex = i;
                if (hasScatter[i] || hasGenes[i] || hasTree[i])
                    lowerIndex = i;
            }
            if (clusterIndex >= 0 && lowerIndex >= 0 && clusterIndex != lowerIndex) {
                weights[clusterIndex] = 11;
                weights[lowerIndex] = 89;
                apply = true;
            }
        } else {
            int scatterIndex = -1;
            int genesIndex = -1;
            int treeIndex = -1;
            for (int i = 0; i < count; ++i) {
                if (hasScatter[i]) scatterIndex = i;
                if (hasGenes[i]) genesIndex = i;
                if (hasTree[i]) treeIndex = i;
            }

            if (scatterIndex >= 0 && genesIndex >= 0 && treeIndex >= 0 &&
                scatterIndex != genesIndex && scatterIndex != treeIndex && genesIndex != treeIndex) {
                weights[scatterIndex] = 3;
                weights[genesIndex] = 4;
                weights[treeIndex] = 3;
                apply = true;
            } else if (scatterIndex >= 0 && (genesIndex >= 0 || treeIndex >= 0)) {
                const auto otherIndex = genesIndex >= 0 ? genesIndex : treeIndex;
                if (scatterIndex != otherIndex) {
                    weights[scatterIndex] = 3;
                    weights[otherIndex] = 7;
                    apply = true;
                }
            } else if (genesIndex >= 0 && treeIndex >= 0 && genesIndex != treeIndex) {
                weights[genesIndex] = 4;
                weights[treeIndex] = 3;
                apply = true;
            }
        }

        if (apply)
            splitter->setSizes(weights);
    }
}
}

EvoViewerSetupPlugin::EvoViewerSetupPlugin(const PluginFactory* factory) :
    ViewPlugin(factory)
{
}

void EvoViewerSetupPlugin::init()
{
    auto* layout = new QVBoxLayout();
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* description = new QLabel(
        "Open the EvoViewer views and apply the names used to link their controls.",
        &getWidget());
    description->setWordWrap(true);

    _setupButton = new QPushButton("Set up EvoViewer", &getWidget());
    _statusLabel = new QLabel("Ready", &getWidget());
    _statusLabel->setWordWrap(true);

    layout->addWidget(description);
    layout->addWidget(_setupButton);
    layout->addWidget(_statusLabel);
    layout->addStretch(1);
    getWidget().setLayout(layout);

    connect(_setupButton, &QPushButton::clicked, this, &EvoViewerSetupPlugin::setupWorkspace);
}

void EvoViewerSetupPlugin::setupWorkspace()
{
    QStringList failures;

    const auto ensureView = [&failures](const ViewRequest& request) -> ViewPlugin* {
        auto* factory = mv::plugins().getPluginFactory(QString::fromUtf8(request.kind));
        if (!factory) {
            failures << QString::fromUtf8(request.kind);
            return nullptr;
        }

        ViewPlugin* view = nullptr;
        for (auto* instance : mv::plugins().getPluginsByFactory(factory)) {
            auto* candidate = dynamic_cast<ViewPlugin*>(instance);
            if (!candidate)
                continue;
            if (candidate->getGuiName() == QString::fromUtf8(request.guiName)) {
                view = candidate;
                break;
            }
            if (!view)
                view = candidate;
        }

        if (!view)
            view = mv::plugins().requestViewPlugin(QString::fromUtf8(request.kind), request.dockTarget, request.dockArea);

        if (!view) {
            failures << QString::fromUtf8(request.kind);
            return nullptr;
        }

        if (request.guiName[0] != '\0')
            view->getGuiNameAction().setString(QString::fromUtf8(request.guiName));
        return view;
    };

    // Add the legend as a tab behind the hierarchy, then split the remaining
    // area into the same top-and-three-column layout shown in the reference.
    auto* legend = ensureView({ "ChartLegend View", "Legend View", nullptr, DockAreaFlag::Right });
    auto* cluster = ensureView({ "Cross-Species Comparison Cluster Rank View", "Cluster Hierarchy View", legend, DockAreaFlag::Center });
    auto* scatterplot = ensureView({ "Scatterplot View", "Scatterplot Embedding View", cluster, DockAreaFlag::Bottom });
    auto* genes = ensureView({ "XSCGeneDetect", "Gene Identification View", scatterplot, DockAreaFlag::Right });
    auto* tree = ensureView({ "XSCPhyTVu", "Phylogenetic Tree View", genes, DockAreaFlag::Right });
    ensureView({ "Tutorial", "", scatterplot, DockAreaFlag::Bottom });

    // Reapply the current trait and species selections in case an older saved
    // project restored the views before their EvoViewer GUI names were applied.
    ViewPlugin* phylogeneticView = nullptr;
    ViewPlugin* geneView = nullptr;
    if (auto* factory = mv::plugins().getPluginFactory(QStringLiteral("XSCPhyTVu"))) {
        for (auto* instance : mv::plugins().getPluginsByFactory(factory)) {
            if (instance->getGuiName() == QStringLiteral("Phylogenetic Tree View")) {
                phylogeneticView = dynamic_cast<ViewPlugin*>(instance);
                break;
            }
        }
    }
    if (auto* factory = mv::plugins().getPluginFactory(QStringLiteral("XSCGeneDetect"))) {
        for (auto* instance : mv::plugins().getPluginsByFactory(factory)) {
            if (instance->getGuiName() == QStringLiteral("Gene Identification View")) {
                geneView = dynamic_cast<ViewPlugin*>(instance);
                break;
            }
        }
    }

    bool linksReady = false;
    if (phylogeneticView && geneView) {
        auto* traitSource = dynamic_cast<OptionAction*>(phylogeneticView->findChildByPath(
            "XSCPhyTVuPlugin Chart/Update Options/Trait"));
        auto* speciesSource = dynamic_cast<OptionsAction*>(phylogeneticView->findChildByPath(
            "XSCPhyTVuPlugin Chart/Linker Options/Selected Leaf Values"));
        auto* traitTarget = dynamic_cast<StringAction*>(geneView->findChildByPath(
            "XSCGeneDetectPlugin Settings/Reverse Search Trait Selection String"));
        auto* speciesTarget = dynamic_cast<StringAction*>(geneView->findChildByPath(
            "XSCGeneDetectPlugin Settings/Selected Species Vals"));

        if (traitSource && speciesSource && traitTarget && speciesTarget) {
            traitTarget->setString(traitSource->getCurrentText());
            speciesTarget->setString(speciesSource->getSelectedOptions().join(QStringLiteral(" @%$,$%@ ")));
            linksReady = true;
        }
    }

    if (!failures.isEmpty())
        _statusLabel->setText(QStringLiteral("Missing: %1").arg(failures.join(QStringLiteral(", "))));
    else
        _statusLabel->setText(linksReady ? QStringLiteral("EvoViewer ready: 6 views")
                                        : QStringLiteral("Views ready; links pending"));

    if (failures.isEmpty()) {
        QTimer::singleShot(150, this, [this, cluster, scatterplot, genes, tree]() {
            applyEvoViewerSplitSizes(mv::workspaces().getWidget(), cluster, scatterplot, genes, tree);
            getVisibleAction().setChecked(false);
        });
    }
}
EvoViewerSetupPluginFactory::EvoViewerSetupPluginFactory()
{
    setIconByName("diagram-project");
    getPluginMetadata().setDescription("Set up EvoViewer views and their linked controls.");
    getPluginMetadata().setSummary("Opens the EvoViewer views and assigns the names used for plugin links.");
}

ViewPlugin* EvoViewerSetupPluginFactory::produce()
{
    return new EvoViewerSetupPlugin(this);
}

DataTypes EvoViewerSetupPluginFactory::supportedDataTypes() const
{
    return {};
}

PluginTriggerActions EvoViewerSetupPluginFactory::getPluginTriggerActions(const Datasets&) const
{
    return {};
}
