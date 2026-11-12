#pragma once

#include <ViewPlugin.h>
#include <QPointer>

namespace mv { class Project; }

class EvoViewerSetupPlugin final : public mv::plugin::ViewPlugin
{
    Q_OBJECT

public:
    explicit EvoViewerSetupPlugin(const mv::plugin::PluginFactory* factory);
    ~EvoViewerSetupPlugin() override;
    void init() override;

private:
    void setupWorkspace();
    void initializeExperiments();

    void finishStudioMode();

    QPointer<mv::Project> _setupProject;
    bool _setupStarted = false;
};

class EvoViewerSetupPluginFactory final : public mv::plugin::ViewPluginFactory
{
    Q_OBJECT
    Q_INTERFACES(mv::plugin::ViewPluginFactory mv::plugin::PluginFactory)
    Q_PLUGIN_METADATA(IID "studio.manivault.EvoViewerSetupPlugin" FILE "../PluginInfo.json")

public:
    EvoViewerSetupPluginFactory();
    mv::plugin::ViewPlugin* produce() override;
    mv::DataTypes supportedDataTypes() const override;
    mv::gui::PluginTriggerActions getPluginTriggerActions(const mv::Datasets& datasets) const override;

private:
    void updateAvailability();
    void scheduleAvailabilityUpdate();

    bool _availabilityUpdateScheduled = false;
};
