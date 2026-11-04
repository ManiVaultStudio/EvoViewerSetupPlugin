#include "EvoViewerSetupPlugin.h"

#include <CoreInterface.h>
#include <Plugin.h>
#include <actions/OptionAction.h>
#include <actions/OptionsAction.h>
#include <actions/StringAction.h>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QStringList>
#include <util/DockArea.h>

#include <array>

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

    const ViewRequest scatterRequest{
        "Scatterplot View", "Scatterplot Embedding View", nullptr, DockAreaFlag::Right
    };
    auto* scatterplot = ensureView(scatterRequest);

    const std::array<ViewRequest, 5> requests{{
        { "Cross-Species Comparison Cluster Rank View", "Cluster Hierarchy View", scatterplot, DockAreaFlag::Left },
        { "ChartLegend View", "Legend View", scatterplot, DockAreaFlag::Bottom },
        { "XSCPhyTVu", "Phylogenetic Tree View", scatterplot, DockAreaFlag::Right },
        { "XSCGeneDetect", "Gene Identification View", scatterplot, DockAreaFlag::Center },
        { "Tutorial", "", scatterplot, DockAreaFlag::Bottom }
    }};

    for (const auto& request : requests)
        ensureView(request);

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
