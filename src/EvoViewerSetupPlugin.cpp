#include "EvoViewerSetupPlugin.h"

#include "EvoViewerSetupRequirements.h"

#include <AbstractDataManager.h>
#include <AbstractPluginManager.h>
#include <AbstractProjectManager.h>
#include <CoreInterface.h>
#include <Plugin.h>
#include <Project.h>

#include <QTimer>

EvoViewerSetupPlugin::EvoViewerSetupPlugin(const mv::plugin::PluginFactory* factory) :
    ViewPlugin(factory)
{
}

void EvoViewerSetupPlugin::init()
{
    initializeExperiments();
    _setupStarted = false;
    QTimer::singleShot(0, this, &EvoViewerSetupPlugin::setupWorkspace);
}

EvoViewerSetupPluginFactory::EvoViewerSetupPluginFactory()
{
    setIconByName("compass");
    getPluginMetadata().setDescription("Set up EvoViewer views and their linked controls.");
    getPluginMetadata().setSummary("Opens the EvoViewer views and configures their shared datasets.");
    setAllowPluginCreationFromStandardGui(false);

    const auto scheduleUpdate = [this]() { scheduleAvailabilityUpdate(); };
    connect(&mv::data(), &mv::AbstractDataManager::datasetAdded, this,
        [scheduleUpdate](mv::Dataset<mv::DatasetImpl>, mv::Dataset<mv::DatasetImpl>, bool) { scheduleUpdate(); });
    connect(&mv::data(), &mv::AbstractDataManager::datasetRemoved, this,
        [scheduleUpdate](const QString&) { scheduleUpdate(); });
    connect(&mv::plugins(), &mv::AbstractPluginManager::pluginAdded, this,
        [scheduleUpdate](mv::plugin::Plugin*) { scheduleUpdate(); });
    connect(&mv::plugins(), &mv::AbstractPluginManager::pluginAboutToBeDestroyed, this,
        [scheduleUpdate](mv::plugin::Plugin*) { scheduleUpdate(); });
    connect(&mv::projects(), &mv::AbstractProjectManager::projectOpened, this,
        [scheduleUpdate](const mv::Project&) { scheduleUpdate(); });

    scheduleAvailabilityUpdate();
}

void EvoViewerSetupPluginFactory::scheduleAvailabilityUpdate()
{
    if (_availabilityUpdateScheduled)
        return;

    _availabilityUpdateScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        _availabilityUpdateScheduled = false;
        updateAvailability();
    });
}

void EvoViewerSetupPluginFactory::updateAvailability()
{
    const auto* project = mv::projects().getCurrentProject();
    const bool allowedByProject = !project || !project->getReadOnlyAction().isChecked() ||
        !project->getAllowedPluginsOnlyAction().isChecked() ||
        project->getAllowedPluginsAction().getStrings().contains(getKind());

    const bool setupAvailable = allowedByProject && EvoViewerSetup::requiredPluginsAvailable() &&
        EvoViewerSetup::requiredTreeDatasetsAvailable() &&
        !EvoViewerSetup::eligibleMainPointDatasets().isEmpty();

    setAllowPluginCreationFromStandardGui(setupAvailable);
}

mv::plugin::ViewPlugin* EvoViewerSetupPluginFactory::produce()
{
    return new EvoViewerSetupPlugin(this);
}

mv::DataTypes EvoViewerSetupPluginFactory::supportedDataTypes() const
{
    return {};
}

mv::gui::PluginTriggerActions EvoViewerSetupPluginFactory::getPluginTriggerActions(const mv::Datasets& datasets) const
{
    Q_UNUSED(datasets);
    return {};
}
