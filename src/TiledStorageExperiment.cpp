#include "EvoViewerSetupPlugin.h"

#include <AbstractPluginManager.h>
#include <AbstractProjectManager.h>
#include <AbstractTaskManager.h>
#include <Task.h>
#include <CoreInterface.h>
#include <ClusterData/ClusterData.h>
#include <PointData/PointData.h>
#include <PointData/storage/tiled/PointDataTiledStorage.h>
#include <PointData/storage/tiled/TiledPointsStorageBackend.h>
#include <PointData/storage/tiled/TiledStorageBackendConfigurationAction.h>
#include <actions/DatasetPickerAction.h>
#include <actions/OptionsAction.h>
#include <actions/StringAction.h>
#include <actions/TriggerAction.h>

#include <QElapsedTimer>
#include <QFileDialog>
#include <QGroupBox>
#include <QJsonDocument>
#include <QSaveFile>
#include <QShowEvent>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace
{
using namespace mv::gui;
using namespace mv::pointdata::storage;
using Storage = mv::pointdata::tiled::PointDataTiledStorage;
using Candidate = std::array<std::size_t, 5>;

template<class T>
T* action(mv::plugin::ViewPlugin* view, const QString& title)
{
    T* match = nullptr;
    for (auto* candidate : view->getChildren<WidgetAction>(true)) {
        auto* typed = dynamic_cast<T*>(candidate);
        if (!typed || typed->text() != title)
            continue;
        if (match && match != typed)
            return nullptr;
        match = typed;
    }
    return match;
}

class Experiment final : public QWidget
{
public:
    explicit Experiment(QWidget* parent) :
        QWidget(parent),
        _clusterPicker(this, tr("Cluster dataset")),
        _clusterOptions(this, tr("Clusters")),
        _workloads(this, tr("Workloads"), {updateName(), deName(), correlationName()}, {updateName()})
    {
        auto* layout = new QVBoxLayout(this);
        auto* group = new QGroupBox(tr("Experiments"), this);
        layout->addWidget(group);
        auto* controls = new QVBoxLayout(group);
        auto* selection = new QFormLayout;
        selection->addRow(tr("Cluster dataset"), _clusterPicker.createWidget(group));
        selection->addRow(tr("Clusters"), _clusterOptions.createWidget(group));
        selection->addRow(tr("Workloads"), _workloads.createWidget(group));
        controls->addLayout(selection);
        _clusterPicker.setFilterFunction([](const mv::Dataset<>& dataset) {
            if (!dataset.isValid() || dataset->getDataType() != ::ClusterType)
                return false;
            const auto parent = dataset->getParent();
            return parent.isValid() && parent->getDataType() == ::PointType &&
                mv::Dataset<Points>(parent)->getStorageBackendType() == StorageBackendType::Tiled;
        });
        connect(&_clusterPicker, &DatasetPickerAction::currentIndexChanged, this, [this]() {
            updateClusterOptions();
        });
        updateClusterOptions();

        auto* budget = new QHBoxLayout;
        budget->addWidget(new QLabel(tr("Time budget (minutes)"), group));
        _budget = new QSpinBox(group);
        _budget->setRange(1, 240);
        _budget->setValue(15);
        _budget->setToolTip(tr("The search can finish early when improvements level off. An operation already in progress runs to completion."));
        budget->addWidget(_budget);
        budget->addStretch();
        controls->addLayout(budget);

        _output = new QPushButton(tr("Choose preset output…"), group);
        controls->addWidget(_output);
        connect(_output, &QPushButton::clicked, this, [this]() {
            const auto path = choosePresetPath();
            if (!path.isEmpty()) {
                _outputPath = path;
                _output->setText(tr("Change preset output…"));
                _output->setToolTip(path);
            }
        });

        auto* buttons = new QHBoxLayout;
        _start = new QPushButton(tr("Start"), group);
        _pause = new QPushButton(tr("Pause"), group);
        _stop = new QPushButton(tr("Stop"), group);
        for (auto* button : {_start, _pause, _stop})
            buttons->addWidget(button);
        controls->addLayout(buttons);

        _status = new QLabel(tr("Choose a cluster dataset and one or more clusters to benchmark."), group);
        _status->setWordWrap(true);
        controls->addWidget(_status);
        _progress = new QProgressBar(group);
        controls->addWidget(_progress);
        _log = new QPlainTextEdit(group);
        _log->setReadOnly(true);
        controls->addWidget(_log);
        _save = new QPushButton(tr("Save best HDF5 Loader preset…"), group);
        controls->addWidget(_save);

        _poll.setInterval(50);
        connect(&_poll, &QTimer::timeout, this, [this]() { pollUpdate(); });
        connect(_start, &QPushButton::clicked, this, [this]() { start(); });
        connect(_pause, &QPushButton::clicked, this, [this]() {
            _paused = !_paused;
            if (!_paused && _pauseClock.isValid()) {
                _pausedMs += _pauseClock.elapsed();
                _pauseClock.invalidate();
            }
            updateControls();
            _status->setText(_paused ? tr("Pausing after the current operation.") : tr("Resuming…"));
            if (!_poll.isActive())
                scheduleNext();
        });
        connect(_stop, &QPushButton::clicked, this, [this]() {
            _stopped = true;
            _status->setText(tr("Stopping after the current operation."));
            updateControls();
            if (!_poll.isActive())
                scheduleNext();
        });
        connect(_save, &QPushButton::clicked, this, [this]() { savePreset(); });
        updateControls();
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        if (_defaultChecked || _active)
            return;
        for (const auto& dataset : _clusterPicker.getDatasets()) {
            auto name = dataset->getGuiName();
            name.replace(QLatin1Char('_'), QLatin1Char(' '));
            if (name.simplified().compare(QStringLiteral("Consensus Subclass"), Qt::CaseInsensitive) == 0) {
                _clusterPicker.setCurrentDataset(dataset);
                break;
            }
        }
        updateClusterOptions();
        _defaultChecked = !_clusterPicker.getDatasets().isEmpty();
    }

private:
    void updateClusterOptions()
    {
        QStringList names;
        const auto dataset = _clusterPicker.getCurrentDataset<Clusters>();
        if (dataset.isValid())
            for (const auto& cluster : dataset->getClusters())
                names << cluster.getName();
        names.removeDuplicates();

        const auto previous = _clusterOptions.getSelectedOptions();
        const bool sameDataset = _optionsDatasetId == _clusterPicker.getCurrentDatasetId();
        _optionsDatasetId = _clusterPicker.getCurrentDatasetId();
        _clusterOptions.setOptions(names, true);
        QStringList selected;
        if (sameDataset)
            for (const auto& name : previous)
                if (names.contains(name))
                    selected << name;
        if (!sameDataset && names.contains(QStringLiteral("L5 IT")))
            selected << QStringLiteral("L5 IT");
        _clusterOptions.setSelectedOptions(selected);
    }

    enum class Phase { Next, Convert, Measure, Analyze };

    static QString updateName() { return tr("Update"); }
    static QString deName() { return tr("Differential expression"); }
    static QString correlationName() { return tr("Trait correlation"); }

    QWidget* analysisWindow(bool correlation) const
    {
        if (!_genes)
            return nullptr;
        for (auto* widget : _genes->getWidget().findChildren<QWidget*>()) {
            const auto title = widget->windowTitle();
            if (widget->isWindow() && (correlation
                ? title.endsWith(QStringLiteral("trait plots"), Qt::CaseInsensitive)
                : title == QStringLiteral("Species differential expression")))
                return widget;
        }
        return nullptr;
    }

    bool validateWorkloads(mv::plugin::ViewPlugin* genes)
    {
        _selectedWorkloads = _workloads.getSelectedOptions();
        if (_selectedWorkloads.isEmpty()) {
            _status->setText(tr("Select at least one workload."));
            return false;
        }
        _genes = genes;
        if (analysisWindow(false) || analysisWindow(true)) {
            _status->setText(tr("Close the Differential Expression and Trait Analysis windows before starting."));
            return false;
        }
        _de = action<TriggerAction>(genes, QStringLiteral("Species Differential Expression..."));
        _correlation = action<TriggerAction>(genes, QStringLiteral("Trait Analysis"));
        if ((_selectedWorkloads.contains(deName()) && !_de) ||
            (_selectedWorkloads.contains(correlationName()) && !_correlation)) {
            _status->setText(tr("A selected Gene Identification workload is unavailable."));
            return false;
        }
        if (_selectedWorkloads.contains(correlationName())) {
            auto* trait = action<StringAction>(genes, QStringLiteral("Reverse Search Trait Selection String"));
            if (!trait || trait->getString().trimmed().isEmpty()) {
                _status->setText(tr("Choose a trait in Gene Identification before benchmarking correlation."));
                return false;
            }
        }
        return true;
    }

    const TiledPointsStorageBackend* backend() const
    {
        return _points.isValid()
            ? dynamic_cast<const TiledPointsStorageBackend*>(_points->getStorageBackend())
            : nullptr;
    }

    void updateControls()
    {
        _start->setEnabled(!_active);
        _budget->setEnabled(!_active);
        _output->setEnabled(!_active);
        _clusterPicker.setEnabled(!_active);
        _clusterOptions.setEnabled(!_active);
        _workloads.setEnabled(!_active);
        _pause->setEnabled(_active && !_stopped);
        _pause->setText(_paused ? tr("Resume") : tr("Pause"));
        _stop->setEnabled(_active && !_stopped);
        _save->setEnabled(!_active && _bestMs >= 0);
    }

    void start()
    {
        if (!mv::projects().getCurrentProject() || mv::projects().isOpeningProject()) {
            _status->setText(tr("Open a project before starting."));
            return;
        }

        auto* factory = mv::plugins().getPluginFactory(QStringLiteral("XSCGeneDetect"));
        if (!factory) {
            _status->setText(tr("Gene Identification is unavailable."));
            return;
        }
        const auto instances = mv::plugins().getPluginsByFactory(factory);
        if (instances.size() != 1) {
            _status->setText(tr("Open exactly one Gene Identification view."));
            return;
        }
        auto* genes = dynamic_cast<mv::plugin::ViewPlugin*>(instances.front());
        if (!genes || !validateWorkloads(genes))
            return;

        for (const auto* title : {"Main Points Dataset", "Reference Tree Dataset", "Embedding Dataset",
                                  "Species Names", "Bottom Cluster Names", "Middle Cluster Names", "Top Cluster Names"}) {
            auto* picker = action<DatasetPickerAction>(genes, QString::fromUtf8(title));
            if (!picker || !picker->getCurrentDataset().isValid()) {
                _status->setText(tr("Configure the Gene Identification dataset “%1”.").arg(QString::fromUtf8(title)));
                return;
            }
        }

        _picker = action<DatasetPickerAction>(genes, QStringLiteral("Main Points Dataset"));
        _update = action<TriggerAction>(genes, QStringLiteral("Update"));
        _geneStatus = action<StringAction>(genes, QStringLiteral("Status color"));
        if (!_update || !_geneStatus || _geneStatus->getString() == QStringLiteral("R") ||
            (!_update->isEnabled() && _geneStatus->getString() != QStringLiteral("C"))) {
            _status->setText(tr("Gene Identification must be available and idle."));
            return;
        }

        if (_outputPath.isEmpty()) {
            _status->setText(tr("Choose where to save the best HDF5 Loader preset."));
            return;
        }

        QSaveFile output(_outputPath);
        if (!output.open(QIODevice::WriteOnly)) {
            _status->setText(tr("Cannot write the preset (%1).").arg(output.errorString()));
            return;
        }
        output.cancelWriting();

        _points = _picker->getCurrentDataset<Points>();
        if (!_points.isValid() || !_points->isFull() || _points->isProxy() || _points->isLocked() || !backend()) {
            _status->setText(tr("Choose a full, unlocked main dataset with tiled storage."));
            return;
        }
        auto original = backend()->getStorage();
        _clusters = _clusterPicker.getCurrentDataset<Clusters>();
        const auto names = _clusterOptions.getSelectedOptions();
        if (!_clusters.isValid() || _clusters->getParent().get() != mv::Dataset<>(_points).get() || names.isEmpty()) {
            _status->setText(tr("Choose a cluster dataset belonging to the Gene Identification main points and select at least one cluster."));
            return;
        }

        _selection.clear();
        _clusterIndices.clear();
        const auto& clusters = _clusters->getClusters();
        for (qsizetype index = 0; index < clusters.size(); ++index) {
            const auto& cluster = clusters.at(index);
            if (!names.contains(cluster.getName()))
                continue;
            _clusterIndices.push_back(static_cast<std::uint32_t>(index));
            _selection.insert(_selection.end(), cluster.getIndices().begin(), cluster.getIndices().end());
        }
        std::sort(_selection.begin(), _selection.end());
        _selection.erase(std::unique(_selection.begin(), _selection.end()), _selection.end());
        if (_selection.size() < 2 || original->rows() == 0 || original->columns() == 0 ||
            _selection.back() >= original->rows() || _points->getDimensionNames().empty()) {
            _status->setText(tr("The chosen clusters need at least two valid cells in a non-empty tiled dataset with gene names."));
            return;
        }

        _installed = std::move(original);
        _converted = false;
        _rows = _installed->rows();
        _columns = _installed->columns();
        _elementSize = _installed->elementSize();
        _lower = {1, 1, 1, 1, 1};
        _upper = {std::min<std::size_t>(_rows, 65536), std::min<std::size_t>(_columns, 4096), 8192, 32, 32};
        const auto current = backend()->createConversionAction(StorageBackendType::Tiled, nullptr);
        _initial = static_cast<TiledStorageBackendConfigurationAction*>(current.get())->getConfiguration();
        const auto& initial = _initial;
        _cacheBytes = initial.cacheCapacityBytes;
        _candidate = {initial.rowTileSize, initial.columnTileSize, initial.cacheCapacity,
                      initial.superTileRows, initial.superTileColumns};
        for (std::size_t axis = 0; axis < _candidate.size(); ++axis)
            _candidate[axis] = std::clamp(_candidate[axis], _lower[axis], _upper[axis]);
        if (!validCandidate(_candidate)) {
            _candidate[0] = std::min<std::size_t>(256, _rows);
            _candidate[1] = std::min<std::size_t>(8, _columns);
            _candidate[3] = _candidate[4] = 1;
        }
        _primer.assign(_selection.begin(), _selection.begin() + std::min<std::size_t>(32, _selection.size() / 2));
        _previousSelection = _points->getSelectionIndices();
        _expected = _previousSelection;
        _previousClusterIndices = _clusters->getSelectionIndices();
        _appliedClusterSelection = false;
        _best = _candidate;
        _center = _candidate;
        _bestMs = -1;
        _trial = 0;
        _direction = 0;
        _radius = 1.0;
        _sweepBestMs = -1;
        _lastTrialMs = 0;
        _estimatedTrialMs = 0;
        _estimatedRestoreMs = 0;
        _pausedMs = 0;
        _pauseClock.invalidate();
        _visited.clear();
        _visited.insert(_candidate);
        _phase = Phase::Next;
        _active = true;
        _stopped = _paused = false;
        _log->clear();
        _log->appendPlainText(tr("Benchmark selection: %1 / %2 (%3 cells)").arg(_clusters->getGuiName(), names.join(QStringLiteral(", "))).arg(_selection.size()));
        _elementTypeIndex = backend()->getElementTypeIndex();
        _total.start();
        updateControls();
        scheduleNext();
    }

    bool validCandidate(const Candidate& c) const
    {
        // Limit the decoded payload of an individual super-tile.
        return quint64(c[0]) * c[1] * c[3] * c[4] * _elementSize <= 64ULL * 1024 * 1024
            && c[3] <= (_rows + c[0] - 1) / c[0]
            && c[4] <= (_columns + c[1] - 1) / c[1];
    }

    bool selectCandidate()
    {
        while (_radius >= 0.125) {
            if (_direction == 10) {
                // Reduce the search radius when a complete sweep fails to improve by 2%.
                if (_sweepBestMs >= 0 && _bestMs >= 0.98 * _sweepBestMs)
                    _radius /= 2;
                _sweepBestMs = _bestMs;
                _center = _best;
                _direction = 0;
                continue;
            }
            const int direction = _direction++;
            const int axis = direction / 2;
            auto candidate = _center;
            const auto value = static_cast<qint64>(candidate[axis]);
            const auto stride = std::max<qint64>(1, std::llround(value * _radius));
            candidate[axis] = std::clamp(value + (direction % 2 ? stride : -stride),
                                         static_cast<qint64>(_lower[axis]), static_cast<qint64>(_upper[axis]));
            if (validCandidate(candidate) && _visited.insert(candidate).second) {
                _candidate = candidate;
                return true;
            }
        }
        return false;
    }

    StorageBackendConfiguration configuration(const Candidate& candidate) const
    {
        StorageBackendConfiguration config;
        config.rowTileSize = candidate[0];
        config.columnTileSize = candidate[1];
        config.cacheCapacity = candidate[2];
        config.cacheCapacityBytes = _cacheBytes;
        config.packedTiles = true;
        config.superTileRows = candidate[3];
        config.superTileColumns = candidate[4];
        return config;
    }

    void scheduleNext()
    {
        QTimer::singleShot(0, this, [this]() { advance(); });
    }

    bool inputsMatch()
    {
        return _picker && _update && _geneStatus && _points.isValid() && _clusters.isValid() &&
            _clusters->getSelectionIndices() == (_appliedClusterSelection ? _clusterIndices : _previousClusterIndices) &&
            _picker->getCurrentDataset().get() == mv::Dataset<>(_points).get() &&
            backend() && backend()->getStorage() == _installed &&
            _points->getSelectionIndices() == _expected;
    }

    void advance()
    {
        if (!_active || _poll.isActive())
            return;
        if (_stopped) {
            finish(tr("Stopped"));
            return;
        }
        if (!inputsMatch() || _geneStatus->getString() == QStringLiteral("R")) {
            finish(tr("Dataset, selection, or Gene Identification changed during the experiment"));
            return;
        }
        if (_paused) {
            if (!_pauseClock.isValid())
                _pauseClock.start();
            _status->setText(tr("Paused"));
            return;
        }
        if (_phase == Phase::Next) {
            const auto budgetMs = qint64(_budget->value()) * 60 * 1000;
            const auto elapsedMs = _total.elapsed() - _pausedMs;
            const auto nextTrialMs = std::max(_lastTrialMs, _estimatedTrialMs);
            if (_trial > 0 && (elapsedMs >= budgetMs || nextTrialMs + _estimatedRestoreMs > budgetMs - elapsedMs)) {
                finish(tr("Time budget reached or another trial is unlikely to fit"));
                return;
            }
            if (_trial > 0 && !selectCandidate()) {
                finish(tr("Search converged"));
                return;
            }
            ++_trial;
            _trialStartedMs = elapsedMs;
            // Replace Gene Identification's selection cache before rebuilding storage.
            // Every timed update then uses the same full selection on newly built tiles.
            runUpdate(_primer, tr("Preparing a fresh Gene Identification run"));
            return;
        }
        if (_phase == Phase::Analyze) {
            runAnalysis();
            return;
        }
        if (_phase == Phase::Convert) {
            convert();
            return;
        }
        runUpdate(_selection, tr("Timing Gene Identification"));
    }

    void convert()
    {
        _status->setText(tr("Trial %1 · Rebuilding tiled storage").arg(_trial));
        _status->repaint();
        _progress->setRange(0, 100);
        _progress->setValue(0);
        QElapsedTimer timer;
        timer.start();
        try {
            const StorageBackendProgressCallback progress = [this](float fraction) {
                const auto percent = qRound(fraction * 100);
                if (percent != _progress->value()) {
                    _progress->setValue(percent);
                    _progress->repaint();
                }
            };
            const auto config = configuration(_candidate);
            if (_converted && _installed->rowTileSize() == config.rowTileSize &&
                _installed->columnTileSize() == config.columnTileSize &&
                _installed->superTileRows() == config.superTileRows &&
                _installed->superTileColumns() == config.superTileColumns &&
                _elementSize > 1) {
                // Evict the decoded tiles before timing another capacity on the same layout.
                auto cold = std::make_shared<StorageBackendConfiguration>(config);
                cold->cacheCapacityBytes = 1;
                _points->configureStorageBackend(cold);
                _points->configureStorageBackend(std::make_shared<StorageBackendConfiguration>(config));
                progress(1.0f);
            } else {
                _points->convertToStorageBackend(StorageBackendType::Tiled, config, progress);
                _installed = backend()->getStorage();
                _converted = true;
                _estimatedRestoreMs = std::max(_estimatedRestoreMs, timer.elapsed());
            }
        } catch (const std::exception& error) {
            finish(tr("Storage update failed (%1)").arg(QString::fromUtf8(error.what())));
            return;
        }
        _conversionMs = timer.elapsed();
        _phase = Phase::Measure;
        scheduleNext();
    }

    void runUpdate(const std::vector<std::uint32_t>& selection, const QString& label)
    {
        _status->setText(tr("Trial %1 · %2").arg(_trial).arg(label));
        _progress->setRange(0, 0);
        if (_clusterIndices.back() >= static_cast<std::size_t>(_clusters->getClusters().size())) {
            finish(tr("The cluster dataset changed during the experiment"));
            return;
        }
        _clusters->setSelectionIndices(_clusterIndices);
        _appliedClusterSelection = true;
        _expected = _selection;
        if (_points->getSelectionIndices() != _expected) {
            finish(tr("The selected clusters no longer match the benchmark cells"));
            return;
        }
        if (_phase != Phase::Measure) {
            // Only the untimed cache preparation uses a subset of the chosen cells.
            _expected = selection;
            _points->setSelectionIndices(_expected);
        }
        // The completed status normally disables Update for an unchanged selection.
        _update->setEnabled(true);
        _completionMs = -1;
        _observedRunning = false;
        _completionConnection = connect(_geneStatus, &StringAction::stringChanged, this, [this](const QString& status) {
            if (status == QStringLiteral("R"))
                _observedRunning = true;
            else if (_observedRunning && _completionMs < 0)
                _completionMs = _operation.elapsed();
        });
        _operation.start();
        _poll.start();
        _launching = true;
        _update->trigger();
        _launching = false;
    }

    void pollUpdate()
    {
        if (_launching)
            return;
        if (_phase == Phase::Analyze) {
            pollAnalysis();
            return;
        }
        if (!_geneStatus) {
            finish(tr("Gene Identification was closed"));
            return;
        }
        const auto status = _geneStatus->getString();
        if (status == QStringLiteral("R")) {
            _status->setText(tr("Trial %1 · %2 · %3 s").arg(_trial)
                .arg(_stopped ? tr("Stopping after Update") : _paused ? tr("Pausing after Update") :
                     _phase == Phase::Measure ? tr("Timing Gene Identification") : tr("Preparing Gene Identification"))
                .arg(_operation.elapsed() / 1000));
            return;
        }

        _poll.stop();
        disconnect(_completionConnection);
        if (!_observedRunning || status != QStringLiteral("C") || !inputsMatch()) {
            finish(tr("Gene Identification did not complete with the benchmark selection"));
            return;
        }
        if (_phase == Phase::Measure) {
            _updateMs = _completionMs >= 0 ? _completionMs : _operation.elapsed();
            _trialScore = _selectedWorkloads.contains(updateName()) ? _updateMs : 0;
            _analyses.clear();
            for (const auto& workload : {deName(), correlationName()})
                if (_selectedWorkloads.contains(workload))
                    _analyses << workload;
            _analysisTimings.clear();
            if (_analyses.isEmpty())
                recordTrial();
            else
                _phase = Phase::Analyze;
        } else {
            _phase = Phase::Convert;
        }
        scheduleNext();
    }

    void watchAnalysisTask(mv::Task* task)
    {
        if (!task || task->parent() != _analysisOwner)
            return;
        _analysisConnections.push_back(connect(task, &mv::Task::statusChanged, this,
            [this, task, generation = _analysisGeneration](mv::Task::Status, mv::Task::Status status) {
                if (!_poll.isActive() || _phase != Phase::Analyze || generation != _analysisGeneration)
                    return;
                const auto name = task->getName();
                const bool matches = _analyses.front() == correlationName()
                    ? name.startsWith(QStringLiteral("Trait correlation"))
                    : name == QStringLiteral("Differential expression");
                if (matches && (status == mv::Task::Status::Running || status == mv::Task::Status::RunningIndeterminate))
                    _analysisTask = task;
                if (task == _analysisTask && (status == mv::Task::Status::Finished || status == mv::Task::Status::Aborted))
                    _analysisMs = _operation.elapsed();
            }));
    }

    void runAnalysis()
    {
        const auto name = _analyses.front();
        const bool correlation = name == correlationName();
        auto* trigger = correlation ? _correlation.data() : _de.data();
        if (!trigger || !trigger->isEnabled() || analysisWindow(correlation)) {
            finish(tr("%1 is unavailable or its window is already open").arg(name));
            return;
        }

        _clusters->setSelectionIndices(_clusterIndices);
        if (!inputsMatch()) {
            finish(tr("The selected clusters no longer match the benchmark cells"));
            return;
        }

        ++_analysisGeneration;
        _analysisOwner = trigger->parent();
        _analysisTask.clear();
        _analysisMs = -1;
        _taskAdded = connect(&mv::tasks(), &mv::AbstractTaskManager::taskAdded,
                             this, [this](mv::Task* task) { watchAnalysisTask(task); });
        for (auto* task : _analysisOwner->findChildren<mv::Task*>(QString(), Qt::FindDirectChildrenOnly))
            watchAnalysisTask(task);

        _status->setText(tr("Trial %1 · %2").arg(_trial).arg(name));
        _progress->setRange(0, 0);
        _operation.start();
        _launching = true;
        _poll.start();
        trigger->trigger();
        _analysisWindow = analysisWindow(correlation);
        _launching = false;
    }

    void closeAnalysis()
    {
        ++_analysisGeneration;
        disconnect(_taskAdded);
        for (const auto& connection : _analysisConnections)
            disconnect(connection);
        _analysisConnections.clear();
        if (_analysisWindow)
            _analysisWindow->close();
        _analysisWindow.clear();
        _analysisTask.clear();
    }

    void pollAnalysis()
    {
        const auto name = _analyses.front();
        if (!_genes || !_analysisWindow || (!_analysisTask && _operation.elapsed() >= 5000)) {
            finish(tr("%1 did not start. Check the Gene Identification settings and selected cells").arg(name));
            return;
        }
        if (!_analysisTask || (_analysisTask->getStatus() != mv::Task::Status::Finished &&
                              _analysisTask->getStatus() != mv::Task::Status::Aborted)) {
            _status->setText(tr("Trial %1 · %2 · %3 s").arg(_trial).arg(name).arg(_operation.elapsed() / 1000));
            if (_analysisTask) {
                _progress->setRange(0, 100);
                _progress->setValue(qRound(100 * _analysisTask->getProgress()));
            }
            return;
        }

        _poll.stop();
        if (_analysisTask->getStatus() != mv::Task::Status::Finished || !inputsMatch()) {
            finish(tr("%1 did not complete with the benchmark selection").arg(name));
            return;
        }
        const auto elapsed = _analysisMs >= 0 ? _analysisMs : _operation.elapsed();
        _trialScore += elapsed;
        _analysisTimings << tr("%1 %2 ms").arg(name).arg(elapsed);
        closeAnalysis();
        _analyses.removeFirst();
        if (_analyses.isEmpty())
            recordTrial();
        scheduleNext();
    }

    void recordTrial()
    {
        _lastTrialMs = _total.elapsed() - _pausedMs - _trialStartedMs;
        _estimatedTrialMs = _trial == 1 ? _lastTrialMs : (_estimatedTrialMs + _lastTrialMs) / 2;
        if (_sweepBestMs < 0)
            _sweepBestMs = _trialScore;
        if (_bestMs < 0 || _trialScore < _bestMs) {
            _bestMs = _trialScore;
            _best = _candidate;
        }
        const auto updateTiming = _selectedWorkloads.contains(updateName()) ? tr("Update %1 ms").arg(_updateMs)
            : tr("Update preparation %1 ms (excluded)").arg(_updateMs);
        _analysisTimings.prepend(updateTiming);
        _log->appendPlainText(tr("%1. Tile %2 × %3, cache %4, super-tile %5 × %6 · storage %7 ms · %8 · score %9 ms · best %10 ms")
            .arg(_trial).arg(_candidate[0]).arg(_candidate[1]).arg(_candidate[2])
            .arg(_candidate[3]).arg(_candidate[4]).arg(_conversionMs).arg(_analysisTimings.join(QStringLiteral(" · ")))
            .arg(_trialScore).arg(_bestMs));
        _phase = Phase::Next;
    }
    void finish(const QString& message)
    {
        _poll.stop();
        disconnect(_completionConnection);
        closeAnalysis();
        if (backend() && backend()->getStorage() == _installed && _converted) {
            _status->setText(tr("Restoring the original tiled storage settings…"));
            _status->repaint();
            _progress->setRange(0, 100);
            try {
                _points->convertToStorageBackend(StorageBackendType::Tiled, _initial, [this](float fraction) {
                    _progress->setValue(qRound(fraction * 100));
                    _progress->repaint();
                });
            } catch (const std::exception& error) {
                _log->appendPlainText(tr("Could not restore the original settings (%1)").arg(QString::fromUtf8(error.what())));
            }
        }
        if (_points.isValid() && _points->getSelectionIndices() == _expected) {
            if (_clusters.isValid() && _appliedClusterSelection &&
                _clusters->getSelectionIndices() == _clusterIndices &&
                std::all_of(_previousClusterIndices.begin(), _previousClusterIndices.end(), [this](auto index) {
                    return index < static_cast<std::size_t>(_clusters->getClusters().size());
                }))
                _clusters->setSelectionIndices(_previousClusterIndices);
            if (_points->getSelectionIndices() != _previousSelection)
                _points->setSelectionIndices(_previousSelection);
        }

        _installed.reset();
        _active = _paused = _stopped = false;
        _progress->setRange(0, 1);
        _progress->setValue(1);
        _status->setText(tr("%1 · %2 trials · %3 s elapsed").arg(message).arg(_trial).arg(_total.elapsed() / 1000));
        updateControls();
        if (_bestMs >= 0)
            writePreset(_outputPath);
    }

    QString choosePresetPath()
    {
        auto path = QFileDialog::getSaveFileName(this, tr("Save tiled storage preset"),
            QStringLiteral("EvoViewer Tiled Storage.mvp"), tr("ManiVault View Preset (*.mvp)"));
        if (!path.isEmpty() && !path.endsWith(QStringLiteral(".mvp"), Qt::CaseInsensitive))
            path += QStringLiteral(".mvp");
        return path;
    }

    void savePreset()
    {
        const auto path = choosePresetPath();
        if (!path.isEmpty())
            writePreset(path);
    }

    void writePreset(const QString& path)
    {
        try {
            TiledStorageBackendConfigurationAction preset(nullptr, _rows, _columns, _elementSize,
                configuration(_best), true, _elementTypeIndex, true);
            const auto json = preset.toJsonDocument().toJson();
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
                throw std::runtime_error(file.errorString().toStdString());
            _log->appendPlainText(tr("Saved preset to %1").arg(path));
        } catch (const std::exception& error) {
            _log->appendPlainText(tr("Could not save preset (%1)").arg(QString::fromUtf8(error.what())));
        }
    }

    DatasetPickerAction _clusterPicker;
    OptionsAction _clusterOptions;
    OptionsAction _workloads;
    QStringList _selectedWorkloads, _analyses, _analysisTimings;
    QPointer<mv::plugin::ViewPlugin> _genes;
    QPointer<TriggerAction> _de, _correlation;
    QPointer<QWidget> _analysisWindow;
    QPointer<QObject> _analysisOwner;
    QPointer<mv::Task> _analysisTask;
    QMetaObject::Connection _taskAdded;
    std::vector<QMetaObject::Connection> _analysisConnections;
    quint64 _analysisGeneration = 0;
    qint64 _analysisMs = -1, _updateMs = 0, _trialScore = 0;
    QString _optionsDatasetId;
    bool _defaultChecked = false;
    QSpinBox* _budget = nullptr;
    QPushButton* _output = nullptr;
    QString _outputPath;
    QPushButton* _start = nullptr;
    QPushButton* _pause = nullptr;
    QPushButton* _stop = nullptr;
    QPushButton* _save = nullptr;
    QLabel* _status = nullptr;
    QProgressBar* _progress = nullptr;
    QPlainTextEdit* _log = nullptr;
    QTimer _poll;
    QElapsedTimer _operation;
    QElapsedTimer _total;
    QElapsedTimer _pauseClock;

    QPointer<DatasetPickerAction> _picker;
    QPointer<TriggerAction> _update;
    QPointer<StringAction> _geneStatus;
    mv::Dataset<Points> _points;
    mv::Dataset<Clusters> _clusters;
    std::vector<std::uint32_t> _clusterIndices;
    std::vector<std::uint32_t> _previousClusterIndices;
    bool _appliedClusterSelection = false;
    std::shared_ptr<Storage> _installed;
    std::vector<std::uint32_t> _selection;
    std::vector<std::uint32_t> _previousSelection;
    std::vector<std::uint32_t> _primer;
    std::vector<std::uint32_t> _expected;
    std::set<Candidate> _visited;
    Candidate _lower{}, _upper{}, _candidate{}, _center{}, _best{};
    std::size_t _rows = 0, _columns = 0, _elementSize = 0;
    std::size_t _cacheBytes = defaultTileCacheCapacityBytes;
    StorageBackendConfiguration _initial;
    qint64 _bestMs = -1;
    qint64 _sweepBestMs = -1;
    qint64 _pausedMs = 0;
    qint64 _trialStartedMs = 0;
    qint64 _lastTrialMs = 0;
    qint64 _estimatedTrialMs = 0;
    qint64 _estimatedRestoreMs = 0;
    qint64 _conversionMs = 0;
    qint64 _completionMs = -1;
    std::uint32_t _elementTypeIndex = 0;
    QMetaObject::Connection _completionConnection;
    bool _observedRunning = false;
    double _radius = 1.0;
    int _trial = 0;
    int _direction = 0;
    Phase _phase = Phase::Next;
    bool _converted = false;
    bool _active = false;
    bool _paused = false;
    bool _stopped = false;
    bool _launching = false;
};
}

void EvoViewerSetupPlugin::initializeExperiments()
{
    auto* layout = new QVBoxLayout(&getWidget());
    auto* settings = new TriggerAction(this, QStringLiteral("Settings"));
    auto* experiment = new Experiment(&getWidget());
    layout->addWidget(experiment);
    addTitleBarMenuAction(settings);
    connect(settings, &TriggerAction::triggered, experiment, [experiment]() {
        experiment->setHidden(!experiment->isHidden());
    });
}
