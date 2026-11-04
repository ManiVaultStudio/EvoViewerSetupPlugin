#include "EvoViewerSetupPlugin.h"
#include "EvoViewerSetupRequirements.h"
#include "EvoViewerSetupData.h"

#include <CoreInterface.h>
#include <Dataset.h>
#include <Project.h>
#include <Workspace.h>
#include <Plugin.h>
#include <actions/ColorAction.h>
#include <actions/DecimalAction.h>
#include <actions/DatasetPickerAction.h>
#include <actions/OptionAction.h>
#include <actions/OptionsAction.h>
#include <actions/StringAction.h>
#include <actions/TriggerAction.h>
#include <util/DockArea.h>

#include <QDebug>
#include <QCoreApplication>
#include <QColor>
#include <QList>
#include <QMap>
#include <QSplitter>
#include <QTimer>

#include <algorithm>
#include <array>
#include <functional>

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


WidgetAction* findActionByPaths(ViewPlugin* plugin, const QStringList& paths)
{
    if (!plugin)
        return nullptr;

    for (const auto& path : paths) {
        if (auto* action = plugin->findChildByPath(path))
            return action;

        // findChildByPath requires the path to match the owning plugin's
        // current text exactly. Setup renames view tabs, and some plugins
        // keep their action groups under an additional settings group. Match
        // the full descendant location by suffix as a fallback while still
        // requiring every requested group and action name to match.
        const auto suffix = QStringLiteral("/") + path;
        for (auto* action : plugin->getChildren<WidgetAction>(true)) {
            const auto location = action->getLocation(true);
            if (location == path || location.endsWith(suffix, Qt::CaseSensitive))
                return action;
        }
    }

    return nullptr;
}

template<typename ActionType>
ActionType* findActionByPaths(ViewPlugin* plugin, const QStringList& paths)
{
    return dynamic_cast<ActionType*>(findActionByPaths(plugin, paths));
}

WidgetAction* findUniqueActionByTitle(ViewPlugin* plugin, const QString& title)
{
    if (!plugin)
        return nullptr;

    WidgetAction* match = nullptr;
    for (auto* action : plugin->getChildren<WidgetAction>(true)) {
        if (action->text() != title)
            continue;
        if (match)
            return nullptr;
        match = action;
    }
    return match;
}

ViewPlugin* findOrCreateView(const ViewRequest& request, QStringList& failures)
{
    const QString kind = QString::fromUtf8(request.kind);
    auto* factory = mv::plugins().getPluginFactory(kind);
    if (!factory) {
        failures << kind;
        return nullptr;
    }

    ViewPlugin* view = nullptr;
    for (auto* instance : mv::plugins().getPluginsByFactory(factory)) {
        auto* candidate = dynamic_cast<ViewPlugin*>(instance);
        if (!candidate)
            continue;

        if (request.guiName[0] != '\0' && candidate->getGuiName() == QString::fromUtf8(request.guiName)) {
            view = candidate;
            break;
        }
        if (!view)
            view = candidate;
    }

    if (!view)
        view = mv::plugins().requestViewPlugin(kind, request.dockTarget, request.dockArea);

    if (!view) {
        failures << kind;
        return nullptr;
    }

    if (request.guiName[0] != '\0')
        view->getGuiNameAction().setString(QString::fromUtf8(request.guiName));
    view->getVisibleAction().setChecked(true);

    return view;
}

bool setDataset(DatasetPickerAction* picker, const Dataset<>& dataset)
{
    if (!picker || !dataset.isValid()) {
        qWarning() << "EvoViewer setup: cannot assign dataset; picker or dataset is invalid."
                   << (picker ? picker->getLocation(true) : QStringLiteral("<picker missing>"));
        return false;
    }

    picker->setCurrentDataset(dataset);
    const bool assigned = picker->getCurrentDatasetId() == dataset.getDatasetId();
    if (!assigned)
        qWarning() << "EvoViewer setup: dataset assignment rejected by picker:" << picker->getLocation(true)
                   << "requested" << dataset.getDatasetId() << "current" << picker->getCurrentDatasetId();
    return assigned;
}

bool publishAction(WidgetAction* action, const QString& publicName)
{
    if (!action)
        return false;

    action->setConnectionPermissionsToAll();

    if (!action->isPublished()) {
        if (!action->publish(publicName)) {
            // A previous attempt or another view may already own the preferred
            // shared name. Retry with this action's stable ID so name-based
            // linking remains unambiguous and setup stays repeatable.
            const QString uniqueName = QStringLiteral("%1 [%2]").arg(publicName, action->getId());
            if (!action->publish(uniqueName)) {
                qWarning() << "EvoViewer setup: core rejected action publication:" << action->getLocation(true)
                           << "as" << publicName << "and" << uniqueName;
                return false;
            }
        }
    }

    const bool published = action->isPublished() && action->getPublicAction();
    if (!published)
        qWarning() << "EvoViewer setup: action has no public endpoint:" << action->getLocation(true)
                   << "as" << publicName;
    return published;
}

bool setProjectStudioMode(bool enabled)
{
    auto* project = mv::projects().getCurrentProject();
    if (!project)
        return false;

    auto& studioModeAction = project->getStudioModeAction();
    if (studioModeAction.isChecked() == enabled)
        return true;

    studioModeAction.setChecked(enabled);
    return studioModeAction.isChecked() == enabled;
}

bool connectToPublishedAction(WidgetAction* privateAction, WidgetAction* sourceAction)
{
    if (!privateAction || !sourceAction || !sourceAction->isPublished()) {
        qWarning() << "EvoViewer setup: action path or publication is missing."
                   << (privateAction ? privateAction->getLocation(true) : QStringLiteral("<private action missing>"))
                   << (sourceAction ? sourceAction->getLocation(true) : QStringLiteral("<source action missing>"));
        return false;
    }

    auto* publicAction = sourceAction->getPublicAction();
    if (!publicAction) {
        qWarning() << "EvoViewer setup: published action has no public endpoint:"
                   << sourceAction->getLocation(true);
        return false;
    }

    if (privateAction->isConnected()) {
        if (privateAction->getPublicAction() == publicAction)
            return true;
        mv::actions().disconnectPrivateActionFromPublicAction(privateAction, false);
    }

    if (privateAction->isConnectionPermissionFlagSet(WidgetAction::ConnectionPermissionFlag::ForceNone)) {
        qWarning() << "EvoViewer setup: connection is not permitted:"
                   << privateAction->getLocation(true) << "->" << sourceAction->getLocation(true);
        return false;
    }

    // Resolve by the published name, as the existing plugin linker pattern
    // does. connectToPublicActionByName is protected in this core version, so
    // use the manager's public connector after resolving the unique endpoint.
    privateAction->setConnectionPermissionsToAll();
    WidgetAction* namedPublicAction = nullptr;
    for (auto* candidate : mv::actions().getPublicActions()) {
        if (candidate->text() != publicAction->text())
            continue;
        if (namedPublicAction) {
            qWarning() << "EvoViewer setup: published action name is ambiguous:" << publicAction->text();
            return false;
        }
        namedPublicAction = candidate;
    }
    if (!namedPublicAction || namedPublicAction != publicAction) {
        qWarning() << "EvoViewer setup: named public action was not found:" << publicAction->text();
        return false;
    }

    mv::actions().connectPrivateActionToPublicAction(privateAction, namedPublicAction, false);
    return privateAction->isConnected() && privateAction->getPublicAction() == publicAction;
}

void applyEvoViewerSplitSizes(QWidget* workspace, ViewPlugin* cluster, ViewPlugin* scatter,
                              ViewPlugin* legend, ViewPlugin* genes, ViewPlugin* tree)
{
    if (!workspace || !cluster || !scatter || !legend || !genes || !tree)
        return;

    const auto branchContains = [](QWidget* branch, ViewPlugin* view) {
        return branch && view && (branch == &view->getWidget() || branch->isAncestorOf(&view->getWidget()));
    };

    for (auto* splitter : workspace->findChildren<QSplitter*>()) {
        const int count = splitter->count();
        if (count < 2)
            continue;

        QList<bool> hasCluster(count), hasScatter(count), hasLegend(count), hasGenes(count), hasTree(count);
        for (int i = 0; i < count; ++i) {
            auto* branch = splitter->widget(i);
            hasCluster[i] = branchContains(branch, cluster);
            hasScatter[i] = branchContains(branch, scatter);
            hasLegend[i] = branchContains(branch, legend);
            hasGenes[i] = branchContains(branch, genes);
            hasTree[i] = branchContains(branch, tree);
        }

        QList<int> weights(count, 1);
        bool shouldResize = false;
        if (splitter->orientation() == Qt::Vertical) {
            int upper = -1;
            int lower = -1;
            for (int i = 0; i < count; ++i) {
                if (hasCluster[i]) upper = i;
                if (hasScatter[i] || hasLegend[i] || hasGenes[i] || hasTree[i]) lower = i;
            }
            if (upper >= 0 && lower >= 0 && upper != lower) {
                weights[upper] = 16;
                weights[lower] = 84;
                shouldResize = true;
            }
        } else {
            int scatterIndex = -1;
            int legendIndex = -1;
            int genesIndex = -1;
            int treeIndex = -1;
            for (int i = 0; i < count; ++i) {
                if (hasScatter[i]) scatterIndex = i;
                if (hasLegend[i]) legendIndex = i;
                if (hasGenes[i]) genesIndex = i;
                if (hasTree[i]) treeIndex = i;
            }

            if (scatterIndex >= 0 && legendIndex >= 0 && genesIndex >= 0 && treeIndex >= 0 &&
                scatterIndex != legendIndex && scatterIndex != genesIndex && scatterIndex != treeIndex &&
                legendIndex != genesIndex && legendIndex != treeIndex && genesIndex != treeIndex) {
                weights[scatterIndex] = 25;
                weights[legendIndex] = 10;
                weights[genesIndex] = 40;
                weights[treeIndex] = 25;
                shouldResize = true;
            } else if (scatterIndex >= 0 && (legendIndex >= 0 || genesIndex >= 0 || treeIndex >= 0)) {
                const int other = legendIndex >= 0 ? legendIndex : (genesIndex >= 0 ? genesIndex : treeIndex);
                if (other != scatterIndex) {
                    weights[scatterIndex] = 25;
                    weights[other] = 70;
                    shouldResize = true;
                }
            } else if (legendIndex >= 0 && (genesIndex >= 0 || treeIndex >= 0)) {
                const int other = genesIndex >= 0 ? genesIndex : treeIndex;
                if (other != legendIndex) {
                    weights[legendIndex] = 10;
                    weights[other] = 58;
                    shouldResize = true;
                }
            } else if (genesIndex >= 0 && treeIndex >= 0 && genesIndex != treeIndex) {
                weights[genesIndex] = 34;
                weights[treeIndex] = 24;
                shouldResize = true;
            }
        }

        if (shouldResize)
            splitter->setSizes(weights);
    }
}

void hideSystemView(const QString& kind)
{
    auto* factory = mv::plugins().getPluginFactory(kind);
    if (!factory)
        return;

    for (auto* instance : mv::plugins().getPluginsByFactory(factory))
        if (auto* view = dynamic_cast<ViewPlugin*>(instance))
            view->getVisibleAction().setChecked(false);
}

void selectEvoViewerTutorial(ViewPlugin* tutorial, const std::function<void(bool)>& onFinished)
{
    if (!tutorial) {
        onFinished(false);
        return;
    }

    auto* retryTimer = new QTimer(tutorial);
    retryTimer->setInterval(100);
    QObject::connect(retryTimer, &QTimer::timeout, tutorial,
        [tutorial, retryTimer, attempts = 0, onFinished]() mutable {
        ++attempts;
        auto* tutorialPicker = dynamic_cast<OptionAction*>(tutorial->findChildByPath(QStringLiteral("Pick tutorial")));
        if (!tutorialPicker)
            tutorialPicker = findActionByPaths<OptionAction>(tutorial, { QStringLiteral("Pick tutorial") });

        if (tutorialPicker) {
            // Tutorial uses a filtered custom model. Ask the model directly;
            // getOptions() can be temporarily empty while its source loads.
            tutorialPicker->setCurrentText(QStringLiteral("Cytosplore EvoViewer"));
        }

        if (tutorialPicker && tutorialPicker->getCurrentText() == QStringLiteral("Cytosplore EvoViewer")) {
            retryTimer->stop();
            retryTimer->deleteLater();
            onFinished(true);
        } else if (attempts >= 300) {
            retryTimer->stop();
            retryTimer->deleteLater();
            onFinished(false);
        }
    });
    retryTimer->start();
}
}

void EvoViewerSetupPlugin::setupWorkspace()
{
    if (_setupStarted)
        return;
    _setupStarted = true;

    if (!EvoViewerSetup::requiredPluginsAvailable() ||
        !EvoViewerSetup::requiredTreeDatasetsAvailable() ||
        EvoViewerSetup::eligibleMainPointDatasets().isEmpty()) {
        qWarning() << "EvoViewer setup: required plugins or datasets are missing.";
        getVisibleAction().setChecked(false);
        return;
    }

    // Studio Mode permits setup to create and link views even when the project
    // has restricted editing. Setup always leaves it disabled when finished.
    if (!setProjectStudioMode(true)) {
        qWarning() << "EvoViewer setup: could not enable Studio Mode.";
        getVisibleAction().setChecked(false);
        return;
    }
    const auto disableStudioMode = []() {
        if (!setProjectStudioMode(false))
            qWarning() << "EvoViewer setup: could not disable Studio Mode.";
    };
    const auto fail = [this, disableStudioMode](const QString& reason) {
        qWarning() << "EvoViewer setup:" << reason;
        disableStudioMode();
        getVisibleAction().setChecked(false);
    };

    QStringList failures;
    auto* cluster = findOrCreateView({ "XSC Cluster Rank", "Cluster Hierarchy View", nullptr, DockAreaFlag::Right }, failures);
    auto* scatter = findOrCreateView({ "Scatterplot View", "Scatterplot Embedding View", cluster, DockAreaFlag::Bottom }, failures);
    auto* legend = findOrCreateView({ "ChartLegend View", "Legend View", scatter, DockAreaFlag::Right }, failures);
    auto* genes = findOrCreateView({ "XSCGeneDetect", "Gene Identification View", legend, DockAreaFlag::Right }, failures);
    auto* tree = findOrCreateView({ "XSCPhyTVu", "Phylogenetic Tree View", genes, DockAreaFlag::Right }, failures);
    QCoreApplication::processEvents();

    if (!failures.isEmpty() || !cluster || !scatter || !legend || !genes || !tree) {
        fail(QStringLiteral("required view unavailable"));
        return;
    }

    // Use the exact instances just opened or reused above. Path-based action
    // lookup then stays bound to these views if the project has other copies.
    auto* geneView = genes;
    auto* phyView = tree;
    auto* clusterView = cluster;

    const std::array<std::pair<const char*, const char*>, 14> geneActionSpecs{{
        { "Reference Tree Dataset", "EvoViewer:GeneDetect:ReferenceTreeDataset" },
        { "Main Points Dataset", "EvoViewer:GeneDetect:MainPointsDataset" },
        { "Embedding Dataset", "EvoViewer:GeneDetect:EmbeddingDataset" },
        { "Species Names", "EvoViewer:GeneDetect:SpeciesNamesDataset" },
        { "Bottom Cluster Names", "EvoViewer:GeneDetect:BottomClusterNamesDataset" },
        { "Middle Cluster Names", "EvoViewer:GeneDetect:MiddleClusterNamesDataset" },
        { "Top Cluster Names", "EvoViewer:GeneDetect:TopClusterNamesDataset" },
        { "Embedding UMAP Points", "EvoViewer:GeneDetect:ScatterplotEmbeddingPointsUMAPOption" },
        { "Leaves Explorer Options", "EvoViewer:GeneDetect:SpeciesExplorerInMap" },
        { "Selected Species Vals", "EvoViewer:GeneDetect:SelectedSpeciesVals" },
        { "Status color", "EvoViewer:GeneDetect:StatusColor" },
        { "Cluster Order Hierarchy", "EvoViewer:GeneDetect:ClusterOrderHierarchy" },
        { "Right Clicked Cluster", "EvoViewer:GeneDetect:RightClickedCluster" },
        { "Clear Right Clicked Cluster", "EvoViewer:GeneDetect:ClearRightClickedCluster" }
    }};

    QMap<QString, WidgetAction*> geneActions;
    QStringList failedPublications;
    for (const auto& spec : geneActionSpecs) {
        const QString title = QString::fromUtf8(spec.first);
        auto* action = findActionByPaths(geneView, {
            QStringLiteral("XSCGeneDetectPlugin Settings/%1").arg(title),
            QStringLiteral("Dataset and Linker Options/%1").arg(title),
            QStringLiteral("Settings/Dataset and Linker Options/%1").arg(title),
            QStringLiteral("XSCGeneDetectPlugin Settings/Dataset and Linker Options/%1").arg(title)
        });
        if (action && publishAction(action, QString::fromUtf8(spec.second))) {
            geneActions.insert(title, action);
        } else
            failedPublications << title;
    }

    auto* topStatus = findActionByPaths(geneView, {
        QStringLiteral("XSCGeneDetectPlugin Settings/Top Selected Hierarchy Status"),
        QStringLiteral("Dataset and Linker Options/Top Selected Hierarchy Status"),
        QStringLiteral("Settings/Dataset and Linker Options/Top Selected Hierarchy Status")
    });
    if (topStatus && publishAction(topStatus, QStringLiteral("EvoViewer:GeneDetect:TopSelectedHierarchyStatus"))) {
        geneActions.insert(QStringLiteral("Top Selected Hierarchy Status"), topStatus);
    } else
        failedPublications << QStringLiteral("Top Selected Hierarchy Status");

    QCoreApplication::processEvents();
    if (!failedPublications.isEmpty()) {
        for (const auto& spec : geneActionSpecs) {
            const QString title = QString::fromUtf8(spec.first);
            if (!failedPublications.contains(title))
                continue;

            auto* action = findActionByPaths(geneView, {
                QStringLiteral("XSCGeneDetectPlugin Settings/%1").arg(title),
                QStringLiteral("Dataset and Linker Options/%1").arg(title),
                QStringLiteral("Settings/Dataset and Linker Options/%1").arg(title),
                QStringLiteral("XSCGeneDetectPlugin Settings/Dataset and Linker Options/%1").arg(title)
            });
            if (action && publishAction(action, QString::fromUtf8(spec.second))) {
                geneActions.insert(title, action);
                failedPublications.removeAll(title);
            }
        }
        if (failedPublications.contains(QStringLiteral("Top Selected Hierarchy Status"))) {
            auto* action = findActionByPaths(geneView, {
                QStringLiteral("XSCGeneDetectPlugin Settings/Top Selected Hierarchy Status"),
                QStringLiteral("Dataset and Linker Options/Top Selected Hierarchy Status"),
                QStringLiteral("Settings/Dataset and Linker Options/Top Selected Hierarchy Status")
            });
            if (action && publishAction(action, QStringLiteral("EvoViewer:GeneDetect:TopSelectedHierarchyStatus"))) {
                geneActions.insert(QStringLiteral("Top Selected Hierarchy Status"), action);
                failedPublications.removeAll(QStringLiteral("Top Selected Hierarchy Status"));
            }
        }
        if (!failedPublications.isEmpty())
            qWarning() << "EvoViewer setup: GeneDetect action publication failed:" << failedPublications;
    }

    const std::array<const char*, 8> requiredGeneDatasetActions{{
        "Reference Tree Dataset", "Main Points Dataset", "Embedding Dataset", "Species Names",
        "Bottom Cluster Names", "Middle Cluster Names", "Top Cluster Names", "Embedding UMAP Points"
    }};
    const bool geneDatasetsReady = std::all_of(requiredGeneDatasetActions.begin(), requiredGeneDatasetActions.end(),
        [&geneActions](const char* actionName) {
            return dynamic_cast<DatasetPickerAction*>(geneActions.value(QString::fromUtf8(actionName))) != nullptr;
        });
    if (!geneDatasetsReady) {
        fail(QStringLiteral("Gene Identification dataset actions unavailable"));
        return;
    }
    if (geneActions.size() != static_cast<int>(geneActionSpecs.size() + 1)) {
        fail(QStringLiteral("GeneDetect shared actions unavailable"));
        return;
    }

    // Phylogenetic tree actions follow the GeneDetect published actions.
    bool requiredLinksReady = true;
    requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Chart Options/Reference tree")
    }), geneActions.value(QStringLiteral("Reference Tree Dataset")));
    requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Tree Leaf Selection Value")
    }), geneActions.value(QStringLiteral("Selected Species Vals")));
    requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Leaf Dataset Picker")
    }), geneActions.value(QStringLiteral("Species Names")));
    requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Selected Leaf Values")
    }), geneActions.value(QStringLiteral("Leaves Explorer Options")));

    // Cluster hierarchy actions publish their matching values to GeneDetect.
    const std::array<std::pair<const char*, const char*>, 11> clusterLinks{{
        { "Top Hierarchy", "Top Cluster Names" },
        { "Middle Hierarchy", "Middle Cluster Names" },
        { "Bottom Hierarchy", "Bottom Cluster Names" },
        { "Species Names", "Species Names" },
        { "Main Points", "Main Points Dataset" },
        { "Low Dim Embedding", "Embedding Dataset" },
        { "Current hierarchy calculation status.", "Status color" },
        { "Cluster Order", "Cluster Order Hierarchy" },
        { "Right Clicked Cluster", "Right Clicked Cluster" },
        { "Top Selected Hierarchy Status", "Top Selected Hierarchy Status" },
        { "Clear Right Clicked Cluster", "Clear Right Clicked Cluster" }
    }};
    QMap<QString, WidgetAction*> clusterActions;
    for (const auto& link : clusterLinks) {
        auto* privateAction = findActionByPaths(clusterView, {
            QStringLiteral("XSC Cluster Rank Settings/%1").arg(QString::fromUtf8(link.first)),
            QStringLiteral("XSCClusterRankPlugin Settings/%1").arg(QString::fromUtf8(link.first)),
            QStringLiteral("Linking Options/%1").arg(QString::fromUtf8(link.first)),
            QStringLiteral("XSCClusterRankPlugin Settings/Linking Options/%1").arg(QString::fromUtf8(link.first))
        });
        if (!privateAction)
            privateAction = findUniqueActionByTitle(clusterView, QString::fromUtf8(link.first));
        if (privateAction)
            clusterActions.insert(QString::fromUtf8(link.first), privateAction);
        else
            qWarning() << "EvoViewer setup: Cluster Rank action was not found:" << link.first;
        requiredLinksReady &= connectToPublishedAction(privateAction, geneActions.value(QString::fromUtf8(link.second)));
    }
    if (!requiredLinksReady) {
        // Give newly opened views and the action manager one event-loop turn,
        // then retry in case a view completed initialization during linking.
        QCoreApplication::processEvents();
        requiredLinksReady = true;
        requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
            QStringLiteral("XSCPhyTVuPlugin Chart/Chart Options/Reference tree")
        }), geneActions.value(QStringLiteral("Reference Tree Dataset")));
        requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
            QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Tree Leaf Selection Value")
        }), geneActions.value(QStringLiteral("Selected Species Vals")));
        requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
            QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Leaf Dataset Picker")
        }), geneActions.value(QStringLiteral("Species Names")));
        requiredLinksReady &= connectToPublishedAction(findActionByPaths(phyView, {
            QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Selected Leaf Values")
        }), geneActions.value(QStringLiteral("Leaves Explorer Options")));
        for (const auto& link : clusterLinks) {
            auto* privateAction = findActionByPaths(clusterView, {
                QStringLiteral("XSC Cluster Rank Settings/%1").arg(QString::fromUtf8(link.first)),
                QStringLiteral("XSCClusterRankPlugin Settings/%1").arg(QString::fromUtf8(link.first)),
                QStringLiteral("Linking Options/%1").arg(QString::fromUtf8(link.first)),
                QStringLiteral("XSCClusterRankPlugin Settings/Linking Options/%1").arg(QString::fromUtf8(link.first))
            });
            if (!privateAction)
                privateAction = findUniqueActionByTitle(clusterView, QString::fromUtf8(link.first));
            if (privateAction)
                clusterActions.insert(QString::fromUtf8(link.first), privateAction);
            requiredLinksReady &= connectToPublishedAction(privateAction,
                geneActions.value(QString::fromUtf8(link.second)));
        }
    }
    if (!requiredLinksReady) {
        fail(QStringLiteral("cross-view action connections failed"));
        return;
    }

    // Resolve all links before asking for project-specific data. The tree and
    // metadata datasets must already exist; this setup never invokes a loader.
    EvoViewerSetup::DatasetChoices choices;
    if (!EvoViewerSetup::chooseDatasets(EvoViewerSetup::eligibleMainPointDatasets(), choices)) {
        fail(QStringLiteral("dataset selection unavailable or cancelled"));
        return;
    }

    auto* phyReferenceTree = findActionByPaths<DatasetPickerAction>(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Chart Options/Reference tree")
    });
    auto* phySpeciesDataset = findActionByPaths<DatasetPickerAction>(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Linker Options/Leaf Dataset Picker")
    });
    auto* phyTraitDataset = findActionByPaths<DatasetPickerAction>(phyView, {
        QStringLiteral("XSCPhyTVuPlugin Chart/Metadata Options/Trait dataset")
    });
    auto* scatterPosition = findActionByPaths<DatasetPickerAction>(scatter, {
        QStringLiteral("Settings/Datasets/Position")
    });
    auto* scatterColor = findActionByPaths<DatasetPickerAction>(scatter, {
        QStringLiteral("Settings/Datasets/Color")
    });
    auto* scatterPointSize = findActionByPaths<DecimalAction>(scatter, {
        QStringLiteral("Settings/Plot/Point/Point size/Point size"),
        QStringLiteral("Settings/Plot/Point size/Point size")
    });
    auto* scatterSelectionColor = findActionByPaths<ColorAction>(scatter, {
        QStringLiteral("Settings/Selection/Point Selection/Color")
    });
    auto* legendClusters = findActionByPaths<DatasetPickerAction>(legend, {
        QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Cluster dataset")
    });
    auto* legendSelectionColor = findActionByPaths<ColorAction>(legend, {
        QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Selection color")
    });
    auto* legendTitle = findActionByPaths<StringAction>(legend, {
        QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Chart Title")
    });
    if (!phyReferenceTree || !phySpeciesDataset || !phyTraitDataset ||
        !scatterPosition || !scatterColor || !scatterSelectionColor || !scatterPointSize ||
        !legendClusters || !legendSelectionColor || !legendTitle) {
        fail(QStringLiteral("required view actions unavailable"));
        return;
    }
    legendTitle->setString(QStringLiteral("Cell types"));

    // Set the parent and reference datasets first, then let their picker
    // models process the change before assigning dependent selections.
    bool datasetsApplied = setDataset(
        dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Main Points Dataset"))), choices.mainPoints);
    // Force the Phylogenetic Tree picker through a real index change. Its
    // view dataset is updated by currentIndexChanged; setting the same already
    // selected tree again can otherwise skip that callback and leave the chart empty.
    phyReferenceTree->setCurrentIndex(-1);
    QCoreApplication::processEvents();
    datasetsApplied &= setDataset(phyReferenceTree, choices.referenceTree);
    QCoreApplication::processEvents();

    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Reference Tree Dataset"))), choices.referenceTree);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Embedding Dataset"))), choices.embedding);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Species Names"))), choices.speciesNames);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Bottom Cluster Names"))), choices.bottomClusters);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Middle Cluster Names"))), choices.middleClusters);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Top Cluster Names"))), choices.topClusters);
    datasetsApplied &= setDataset(dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Embedding UMAP Points"))), choices.scatterEmbedding);

    // Like the reference picker, the metadata picker can already point at the
    // sole available dataset. Force its change signal so ChartOptions refreshes
    // the metadata model and associates it with the selected reference tree.
    phyTraitDataset->setCurrentIndex(-1);
    QCoreApplication::processEvents();
    datasetsApplied &= setDataset(phyTraitDataset, choices.metadata);

    // Match the scatterplot and legend datasets to the selections the user just made.
    datasetsApplied &= setDataset(scatterPosition, choices.scatterEmbedding);
    datasetsApplied &= setDataset(scatterColor, choices.bottomClusters);
    scatterPointSize->setValue(3.0);
    datasetsApplied &= publishAction(scatterColor, QStringLiteral("EvoViewer:Scatterplot:ColorDataset"));
    if (!datasetsApplied) {
        qWarning() << "EvoViewer setup: one or more selected datasets could not be applied.";
        fail(QStringLiteral("selected datasets could not be applied"));
        return;
    }

    QCoreApplication::processEvents();
    if (!QMetaObject::invokeMethod(tree, "triggerChartJS", Qt::DirectConnection)) {
        fail(QStringLiteral("Phylogenetic Tree refresh could not be triggered"));
        return;
    }

    bool linkedDatasetsMatch = true;
    for (const auto& link : clusterLinks) {
        auto* clusterPicker = dynamic_cast<DatasetPickerAction*>(clusterActions.value(QString::fromUtf8(link.first)));
        auto* genePicker = dynamic_cast<DatasetPickerAction*>(geneActions.value(QString::fromUtf8(link.second)));
        if (!clusterPicker || !genePicker)
            continue;
        if (clusterPicker->getCurrentDatasetId() != genePicker->getCurrentDatasetId()) {
            qWarning() << "EvoViewer setup: Cluster Hierarchy dataset did not follow its GeneDetect action:"
                       << clusterPicker->getLocation(true) << "expected" << genePicker->getCurrentDatasetId()
                       << "current" << clusterPicker->getCurrentDatasetId();
            linkedDatasetsMatch = false;
        }
    }
    const auto geneReferenceTree = dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Reference Tree Dataset")));
    const auto geneSpeciesDataset = dynamic_cast<DatasetPickerAction*>(geneActions.value(QStringLiteral("Species Names")));
    linkedDatasetsMatch &= geneReferenceTree &&
        phyReferenceTree->getCurrentDatasetId() == geneReferenceTree->getCurrentDatasetId();
    linkedDatasetsMatch &= geneSpeciesDataset &&
        phySpeciesDataset->getCurrentDatasetId() == geneSpeciesDataset->getCurrentDatasetId();
    if (!linkedDatasetsMatch) {
        fail(QStringLiteral("linked dataset values did not propagate"));
        return;
    }

    auto* publicColorDataset = scatterColor->getPublicAction();
    if (!publicColorDataset || !connectToPublishedAction(legendClusters, scatterColor)) {
        fail(QStringLiteral("Legend View color-dataset connection failed"));
        return;
    }
    if (legendClusters->getCurrentDatasetId() != scatterColor->getCurrentDatasetId()) {
        fail(QStringLiteral("Legend View did not receive the Scatterplot color dataset"));
        return;
    }

    const QColor microsoftSelectionBlue(53, 126, 199);
    scatterSelectionColor->setColor(microsoftSelectionBlue);
    if (!publishAction(scatterSelectionColor, QStringLiteral("EvoViewer:Scatterplot:SelectionColor")) ||
        !connectToPublishedAction(legendSelectionColor, scatterSelectionColor)) {
        fail(QStringLiteral("selection-color publication or Legend View connection failed"));
        return;
    }
    legendSelectionColor->setColor(microsoftSelectionBlue);

    // Start scatterplot interaction in lasso mode, matching the EvoViewer
    // workflow's point-selection use case.
    auto* scatterSelectionType = findActionByPaths<OptionAction>(scatter, {
        QStringLiteral("Settings/Selection/Point Selection/Type"),
        QStringLiteral("Scatterplot View Settings/Selection/Point Selection/Type")
    });
    if (!scatterSelectionType || !scatterSelectionType->getOptions().contains(QStringLiteral("Lasso"))) {
        fail(QStringLiteral("scatterplot Lasso selection action was not found"));
        return;
    }
    scatterSelectionType->setCurrentText(QStringLiteral("Lasso"));

    hideSystemView(QStringLiteral("Data properties"));
    hideSystemView(QStringLiteral("Data hierarchy"));

    // Close this empty setup view without destroying it, apply the final sizes,
    // and open the requested tutorial only after all setup work has completed.
    getVisibleAction().setChecked(false);
    QTimer::singleShot(250, this, [cluster, scatter, legend, genes, tree, scatterColor,
                                   legendClusters, disableStudioMode]() {
        QStringList ignoredFailures;
        auto* tutorial = findOrCreateView({ "Tutorial", "", nullptr, DockAreaFlag::Right }, ignoredFailures);

        hideSystemView(QStringLiteral("Data properties"));
        hideSystemView(QStringLiteral("Data hierarchy"));
        // Creating the tutorial can change the docking tree; size the final
        // view arrangement after it has joined the workspace.
        applyEvoViewerSplitSizes(mv::workspaces().getWidget(), cluster, scatter, legend, genes, tree);
        selectEvoViewerTutorial(tutorial, [tutorial, scatter, scatterColor, legendClusters,
                                           cluster, legend, genes, tree,
                                           disableStudioMode](bool tutorialSelected) {
            if (!tutorialSelected)
                qWarning() << "EvoViewer setup: tutorial view opened, but its requested tutorial was unavailable.";
            if (tutorial)
                tutorial->getVisibleAction().setChecked(true);

            auto* selectionType = findActionByPaths<OptionAction>(scatter, {
                QStringLiteral("Settings/Selection/Point Selection/Type"),
                QStringLiteral("Scatterplot View Settings/Selection/Point Selection/Type")
            });
            if (selectionType && selectionType->getOptions().contains(QStringLiteral("Lasso")))
                selectionType->setCurrentText(QStringLiteral("Lasso"));
            else
                qWarning() << "EvoViewer setup: scatterplot Lasso selection action was not found after view setup.";
            if (scatterColor && legendClusters && scatterColor->getPublicAction() &&
                !connectToPublishedAction(legendClusters, scatterColor))
                qWarning() << "EvoViewer setup: Legend View color-dataset connection failed after layout.";
            auto* scatterColorAction = findActionByPaths<ColorAction>(scatter, {
                QStringLiteral("Settings/Selection/Point Selection/Color")
            });
            auto* legendSelectionAction = findActionByPaths<ColorAction>(legend, {
                QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Selection color")
            });
            if (scatterColorAction && legendSelectionAction &&
                !connectToPublishedAction(legendSelectionAction, scatterColorAction))
                qWarning() << "EvoViewer setup: selection-color connection failed after layout.";
            hideSystemView(QStringLiteral("Data properties"));
            hideSystemView(QStringLiteral("Data hierarchy"));
            QCoreApplication::processEvents();
            applyEvoViewerSplitSizes(mv::workspaces().getWidget(), cluster, scatter, legend, genes, tree);
            disableStudioMode();
            if (auto* workspace = mv::workspaces().getCurrentWorkspace()) {
                workspace->getLockingAction().getLockedAction().setChecked(true);
                if (!workspace->getLockingAction().getLockedAction().isChecked())
                    qWarning() << "EvoViewer setup: workspace layout could not be locked.";
            }
        });
    });
}
