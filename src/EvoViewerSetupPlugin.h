#pragma once

#include <ViewPlugin.h>

class QLabel;
class QPushButton;

class EvoViewerSetupPlugin final : public mv::plugin::ViewPlugin
{
    Q_OBJECT

public:
    explicit EvoViewerSetupPlugin(const mv::plugin::PluginFactory* factory);
    void init() override;

private:
    void setupWorkspace();

    QLabel* _statusLabel = nullptr;
    QPushButton* _setupButton = nullptr;
};

class EvoViewerSetupPluginFactory final : public mv::plugin::ViewPluginFactory
{
    Q_OBJECT
    Q_INTERFACES(mv::plugin::ViewPluginFactory mv::plugin::PluginFactory)
    Q_PLUGIN_METADATA(IID "studio.manivault.EvoViewerSetupPlugin")

public:
    EvoViewerSetupPluginFactory();
    mv::plugin::ViewPlugin* produce() override;
    mv::DataTypes supportedDataTypes() const override;
    mv::gui::PluginTriggerActions getPluginTriggerActions(const mv::Datasets& datasets) const override;
};