#pragma once

#include <ViewPlugin.h>

class EvoViewerSetupPlugin final : public mv::plugin::ViewPlugin
{
    Q_OBJECT

public:
    explicit EvoViewerSetupPlugin(const mv::plugin::PluginFactory* factory);
    void init() override;

private:
    void setupWorkspace();

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
    bool _setupAvailable = false;
};
