#include "EvoViewerSetupPlugin.h"
#include "EvoViewerSetupRequirements.h"
#include "EvoViewerSetupData.h"

#include <CoreInterface.h>
#include <Application.h>
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

constexpr auto configuredViewsProperty = "EvoViewerSetupViewIds";

ViewPlugin* findConfiguredView(const char* kind, const QStringList& configuredIds)
{
    auto* factory = configuredIds.isEmpty() ? nullptr : mv::plugins().getPluginFactory(QString::fromUtf8(kind));
    if (!factory)
        return nullptr;

    for (auto* instance : mv::plugins().getPluginsByFactory(factory))
        if (configuredIds.contains(instance->getId()))
            return dynamic_cast<ViewPlugin*>(instance);
    return nullptr;
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
        const QString restoredName = QStringLiteral("%1 [%2]").arg(publicName, action->getId());
        const QString uniqueName = QStringLiteral("%1 - %2").arg(publicName, action->getId());
        // Workspace restore can leave the public endpoint registered before its
        // private action is linked. Reuse the endpoint to preserve other links.
        const auto publicActions = mv::actions().getPublicActions();
        for (const auto& name : {restoredName, uniqueName, publicName}) {
            WidgetAction* endpoint = nullptr;
            for (auto* candidate : publicActions) {
                if (candidate->text() != name)
                    continue;
                if (endpoint) {
                    endpoint = nullptr;
                    break;
                }
                endpoint = candidate;
            }
            if (endpoint && endpoint->getTypeString() == action->getTypeString() &&
                (name != publicName || endpoint->getConnectedActions().isEmpty())) {
                mv::actions().connectPrivateActionToPublicAction(action, endpoint, true);
                break;
            }
        }
        if (!action->isPublished() && !action->publish(publicName) && !action->publish(uniqueName)) {
            qWarning() << "EvoViewer setup: core rejected action publication:" << action->getLocation(true)
                       << "as" << publicName << "and" << uniqueName;
            return false;
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

    if (privateAction->getPublicAction() == publicAction)
        return true;

    if (!mv::actions().getPublicActions().contains(publicAction))
        return false;

    if (privateAction->isConnectionPermissionFlagSet(WidgetAction::ConnectionPermissionFlag::ForceNone)) {
        qWarning() << "EvoViewer setup: connection is not permitted:"
                   << privateAction->getLocation(true) << "->" << sourceAction->getLocation(true);
        return false;
    }

    privateAction->setConnectionPermissionsToAll();
    if (privateAction->isConnected())
        mv::actions().disconnectPrivateActionFromPublicAction(privateAction, false);
    mv::actions().connectPrivateActionToPublicAction(privateAction, publicAction, false);
    return privateAction->isConnected() && privateAction->getPublicAction() == publicAction;
}

void applyEvoViewerSplitSizes(QWidget* workspace, ViewPlugin* cluster, ViewPlugin* scatter,
                              ViewPlugin* legend, ViewPlugin* genes, ViewPlugin* tree,
                              ViewPlugin* tutorial)
{
    if (!workspace)
        return;

    for (auto* splitter : workspace->findChildren<QSplitter*>()) {
        const bool vertical = splitter->orientation() == Qt::Vertical;
        const auto views = { std::pair{cluster, vertical ? 16 : 100}, std::pair{scatter, vertical ? 84 : 25},
                             std::pair{legend, vertical ? 84 : 10}, std::pair{genes, vertical ? 84 : 40},
                             std::pair{tree, vertical ? 84 : 25}, std::pair{tutorial, vertical ? 84 : 24} };
        auto sizes = splitter->sizes();
        QList<int> weights(sizes.size(), 0);
        int available = 0;
        int totalWeight = 0;
        int branches = 0;
        for (int i = 0; i < sizes.size(); ++i) {
            auto* branch = splitter->widget(i);
            for (const auto& [view, weight] : views) {
                if (!view || !view->getVisibleAction().isChecked() ||
                    (branch != &view->getWidget() && !branch->isAncestorOf(&view->getWidget())))
                    continue;
                if (vertical)
                    weights[i] = std::max(weights[i], weight);
                else
                    weights[i] += weight;
            }
            if (weights[i] > 0) {
                ++branches;
                available += sizes[i];
                totalWeight += weights[i];
            }
        }
        if (branches < 2 || available <= 0)
            continue;

        for (int i = 0; i < sizes.size(); ++i)
            if (weights[i] > 0)
                sizes[i] = std::max(1, static_cast<int>(qint64(available) * weights[i] / totalWeight));

        splitter->setSizes(sizes);
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

void selectEvoViewerTutorial(QObject* context, ViewPlugin* tutorial, const std::function<void(bool)>& onFinished)
{
    if (!tutorial) {
        onFinished(false);
        return;
    }

    auto* retryTimer = new QTimer(context);
    retryTimer->setInterval(100);
    QObject::connect(retryTimer, &QTimer::timeout, context,
        [tutorial = QPointer<ViewPlugin>(tutorial), retryTimer = QPointer<QTimer>(retryTimer),
         attempts = 0, onFinished]() mutable {
        if (!tutorial) {
            retryTimer->stop();
            retryTimer->deleteLater();
            onFinished(false);
            return;
        }
        ++attempts;
        QPointer<OptionAction> tutorialPicker = dynamic_cast<OptionAction*>(
            tutorial->findChildByPath(QStringLiteral("Pick tutorial")));
        if (!tutorialPicker)
            tutorialPicker = findActionByPaths<OptionAction>(tutorial, { QStringLiteral("Pick tutorial") });

        if (tutorialPicker) {
            // Tutorial uses a filtered custom model. Ask the model directly;
            // getOptions() can be temporarily empty while its source loads.
            tutorialPicker->setCurrentText(QStringLiteral("Cytosplore EvoViewer"));
        }

        if (!retryTimer)
            return;
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

EvoViewerSetupPlugin::~EvoViewerSetupPlugin()
{
    finishStudioMode();
}

void EvoViewerSetupPlugin::finishStudioMode()
{
    const auto project = _setupProject;
    _setupProject.clear();
    if (project && mv::Application::core() && !mv::Application::core()->isAboutToBeDestroyed() &&
        project == mv::projects().getCurrentProject() && !setProjectStudioMode(false))
        qWarning() << "EvoViewer setup: could not disable Studio Mode.";
}

void EvoViewerSetupPlugin::setupWorkspace()
{
    if (_setupStarted)
        return;
    _setupStarted = true;

    // Completed views belong to the workspace and survive the setup panel.
    const QPointer<Workspace> workspace(mv::workspaces().getCurrentWorkspace());
    const auto configuredIds = workspace ? workspace->property(configuredViewsProperty).toStringList() : QStringList{};
    QPointer<ViewPlugin> cluster = findConfiguredView("XSC Cluster Rank", configuredIds);
    QPointer<ViewPlugin> scatter = findConfiguredView("Scatterplot View", configuredIds);
    QPointer<ViewPlugin> legend = findConfiguredView("ChartLegend View", configuredIds);
    QPointer<ViewPlugin> genes = findConfiguredView("XSCGeneDetect", configuredIds);
    QPointer<ViewPlugin> tree = findConfiguredView("XSCPhyTVu", configuredIds);
    const bool reconnecting = cluster && scatter && legend && genes && tree;

    const QPointer<EvoViewerSetupPlugin> setup(this);

    if (!reconnecting && (!EvoViewerSetup::requiredPluginsAvailable() ||
        !EvoViewerSetup::requiredTreeDatasetsAvailable() ||
        EvoViewerSetup::eligibleMainPointDatasets().isEmpty())) {
        qWarning() << "EvoViewer setup: required plugins or datasets are missing.";
        getVisibleAction().setChecked(false);
        return;
    }

    // Studio Mode permits setup to create and link views even when the project
    // has restricted editing. Setup always leaves it disabled when finished.
    auto* project = mv::projects().getCurrentProject();
    if (!reconnecting || (project && !project->getStudioModeAction().isChecked()))
        _setupProject = project;
    const bool studioModeEnabled = setProjectStudioMode(true);
    if (!setup)
        return;
    if (!studioModeEnabled) {
        _setupProject.clear();
        qWarning() << "EvoViewer setup: could not enable Studio Mode.";
        getVisibleAction().setChecked(false);
        return;
    }
    const auto fail = [setup](const QString& reason) {
        qWarning() << "EvoViewer setup:" << reason;
        if (setup)
            setup->finishStudioMode();
        if (setup)
            setup->getVisibleAction().setChecked(false);
    };

    QStringList failures;
    if (!cluster)
        cluster = findOrCreateView({ "XSC Cluster Rank", "Cluster Hierarchy View", nullptr, DockAreaFlag::Right }, failures);
    if (!setup)
        return;
    if (!scatter)
        scatter = findOrCreateView({ "Scatterplot View", "Scatterplot Embedding View", cluster, DockAreaFlag::Bottom }, failures);
    if (!setup)
        return;
    if (!legend)
        legend = findOrCreateView({ "ChartLegend View", "Legend View", scatter, DockAreaFlag::Right }, failures);
    if (!setup)
        return;
    if (!genes)
        genes = findOrCreateView({ "XSCGeneDetect", "Gene Identification View", legend, DockAreaFlag::Right }, failures);
    if (!setup)
        return;
    if (!tree)
        tree = findOrCreateView({ "XSCPhyTVu", "Phylogenetic Tree View", genes, DockAreaFlag::Right }, failures);
    if (!setup)
        return;
    QCoreApplication::processEvents();
    if (!setup)
        return;

    if (!failures.isEmpty() || !cluster || !scatter || !legend || !genes || !tree) {
        fail(QStringLiteral("required view unavailable"));
        return;
    }

    const auto viewsStillAvailable = [&]() {
        if (!setup)
            return false;
        if (cluster && scatter && legend && genes && tree &&
            workspace == mv::workspaces().getCurrentWorkspace())
            return true;
        fail(QStringLiteral("workspace changed during setup"));
        return false;
    };

    // Use the exact instances just opened or reused above. Path-based action
    // lookup then stays bound to these views if the project has other copies.
    auto* geneView = genes.data();
    auto* phyView = tree.data();
    auto* clusterView = cluster.data();

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
    if (!viewsStillAvailable())
        return;
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
        if (!viewsStillAvailable())
            return;
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

    auto* scatterColor = findActionByPaths<DatasetPickerAction>(scatter, {
        QStringLiteral("Settings/Datasets/Color")
    });
    auto* scatterSelectionColor = findActionByPaths<ColorAction>(scatter, {
        QStringLiteral("Settings/Selection/Point Selection/Overlay color"),
        QStringLiteral("Settings/Selection/Point Selection/Color")
    });
    auto* legendClusters = findActionByPaths<DatasetPickerAction>(legend, {
        QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Cluster dataset")
    });
    auto* legendSelectionColor = findActionByPaths<ColorAction>(legend, {
        QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Selection color")
    });
    if (reconnecting) {
        const bool colorLinked = scatterColor && legendClusters &&
            publishAction(scatterColor, QStringLiteral("EvoViewer:Scatterplot:ColorDataset")) &&
            connectToPublishedAction(legendClusters, scatterColor);
        const bool selectionColorLinked = scatterSelectionColor && legendSelectionColor &&
            publishAction(scatterSelectionColor, QStringLiteral("EvoViewer:Scatterplot:SelectionColor")) &&
            connectToPublishedAction(legendSelectionColor, scatterSelectionColor);
        if (colorLinked && selectionColorLinked)
            finishStudioMode();
        else
            fail(QStringLiteral("could not restore all Scatterplot and Legend View connections"));
        return;
    }

    // Resolve all links before asking for project-specific data. The tree and
    // metadata datasets must already exist; this setup never invokes a loader.
    EvoViewerSetup::DatasetChoices choices;
    const bool datasetsSelected = EvoViewerSetup::chooseDatasets(
        EvoViewerSetup::eligibleMainPointDatasets(), choices, this);
    if (!viewsStillAvailable())
        return;
    if (!datasetsSelected) {
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
    auto* scatterPointSize = findActionByPaths<DecimalAction>(scatter, {
        QStringLiteral("Settings/Plot/Point/Point size/Point size"),
        QStringLiteral("Settings/Plot/Point size/Point size")
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
    if (!viewsStillAvailable())
        return;
    datasetsApplied &= setDataset(phyReferenceTree, choices.referenceTree);
    QCoreApplication::processEvents();
    if (!viewsStillAvailable())
        return;

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
    if (!viewsStillAvailable())
        return;
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
    if (!viewsStillAvailable())
        return;
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

    if (!viewsStillAvailable())
        return;
    if (workspace)
        workspace->setProperty(configuredViewsProperty, QStringList{
            cluster->getId(), scatter->getId(), legend->getId(), genes->getId(), tree->getId()
        });
    if (!setup)
        return;

    // Open the tutorial after the linked views are ready.
    QTimer::singleShot(250, this, [setup, workspace, cluster, scatter, legend, genes, tree,
                                   scatterColor = QPointer<DatasetPickerAction>(scatterColor),
                                   legendClusters = QPointer<DatasetPickerAction>(legendClusters), fail]() {
        if (workspace != mv::workspaces().getCurrentWorkspace() || !cluster || !scatter || !legend || !genes || !tree) {
            fail(QStringLiteral("workspace changed before opening the tutorial"));
            return;
        }
        QStringList ignoredFailures;
        const QPointer<ViewPlugin> tutorial = findOrCreateView({ "Tutorial", "", nullptr, DockAreaFlag::Right }, ignoredFailures);

        hideSystemView(QStringLiteral("Data properties"));
        hideSystemView(QStringLiteral("Data hierarchy"));
        if (!setup)
            return;
        selectEvoViewerTutorial(setup, tutorial, [setup, workspace, tutorial, scatter, scatterColor, legendClusters,
                                           cluster, legend, genes, tree, fail](bool tutorialSelected) {
            if (!setup)
                return;
            if (workspace != mv::workspaces().getCurrentWorkspace() || !cluster || !scatter || !legend || !genes || !tree) {
                fail(QStringLiteral("workspace changed before finalizing setup"));
                return;
            }
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
            if (!connectToPublishedAction(legendClusters, scatterColor)) {
                fail(QStringLiteral("Legend View color-dataset connection failed after layout"));
                return;
            }
            auto* scatterColorAction = findActionByPaths<ColorAction>(scatter, {
                QStringLiteral("Settings/Selection/Point Selection/Overlay color"),
                QStringLiteral("Settings/Selection/Point Selection/Color")
            });
            auto* legendSelectionAction = findActionByPaths<ColorAction>(legend, {
                QStringLiteral("ChartLegendViewPlugin Chart/Color Options/Selection color")
            });
            if (!connectToPublishedAction(legendSelectionAction, scatterColorAction)) {
                fail(QStringLiteral("selection-color connection failed after layout"));
                return;
            }
            hideSystemView(QStringLiteral("Data properties"));
            hideSystemView(QStringLiteral("Data hierarchy"));
            if (setup)
                setup->getVisibleAction().setChecked(false);
            if (!setup)
                return;
            setup->finishStudioMode();
            if (workspace && workspace == mv::workspaces().getCurrentWorkspace()) {
                workspace->getLockingAction().getLockedAction().setChecked(true);
                if (workspace && !workspace->getLockingAction().getLockedAction().isChecked())
                    qWarning() << "EvoViewer setup: workspace layout could not be locked.";
            }
            if (tutorial) {
                auto allowTutorialClose = [tutorial]() { tutorial->getMayCloseAction().setChecked(true); };
                QObject::connect(&tutorial->getLockingAction().getLockedAction(), &ToggleAction::toggled,
                        tutorial, allowTutorialClose);
                allowTutorialClose();
            }
            if (!setup)
                return;
            QTimer::singleShot(0, setup, [workspace, cluster, scatter, legend, genes, tree, tutorial]() {
                if (workspace && workspace == mv::workspaces().getCurrentWorkspace())
                    applyEvoViewerSplitSizes(mv::workspaces().getWidget(), cluster, scatter, legend, genes, tree, tutorial);
            });
        });
    });
}
