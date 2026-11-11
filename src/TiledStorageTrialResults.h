#pragma once

#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTabWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace EvoViewerSetup
{
enum TrialColumn {
    TrialNumber, TileHeight, TileWidth, CacheCapacity, SuperTileRows, SuperTileColumns,
    StorageMs, UpdateMs, DifferentialExpressionMs, CorrelationMs, ScoreMs, ColumnCount
};

using Trial = std::array<qint64, ColumnCount>;

inline QString columnName(int column)
{
    static constexpr const char* names[] = {
        "Trial", "Tile height", "Tile width", "Tile cache capacity", "Super-tile rows", "Super-tile columns",
        "Storage rebuild (ms)", "Update (ms)", "Differential expression (ms)", "Trait correlation (ms)", "Score (ms)"
    };
    return QObject::tr(names[column]);
}

class TrialChart final : public QWidget
{
public:
    TrialChart(const std::vector<Trial>& trials, const int& bestTrial, QWidget* parent) :
        QWidget(parent), _trials(trials), _bestTrial(bestTrial)
    {
        setMinimumSize(420, 260);
        setMouseTracking(true);
    }

    void refresh()
    {
        _points.clear();
        update();
    }

    void setAxes(int x, int y)
    {
        _x = x;
        _y = y;
        refresh();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        painter.setRenderHint(QPainter::Antialiasing);
        _points.clear();
        qint64 xMin = std::numeric_limits<qint64>::max(), xMax = 0, yMax = 0;
        for (const auto& trial : _trials) {
            if (trial[_y] < 0)
                continue;
            xMin = std::min(xMin, trial[_x]);
            xMax = std::max(xMax, trial[_x]);
            yMax = std::max(yMax, trial[_y]);
        }
        painter.setPen(palette().text().color());
        if (xMin > xMax) {
            painter.drawText(rect(), Qt::AlignCenter, tr("No completed trials for this timing."));
            return;
        }

        const auto plot = QRectF(rect()).adjusted(76, 24, -28, -60);
        const auto yScale = std::max<qint64>(1, yMax);
        const auto xScale = std::max<qint64>(1, xMax - xMin);
        for (int tick = 0; tick <= 4; ++tick) {
            const double fraction = tick / 4.0;
            const auto y = plot.bottom() - plot.height() * fraction;
            painter.setPen(palette().mid().color());
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().text().color());
            painter.drawText(QRectF(4, y - 10, 66, 20), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(yScale * fraction, 'g', 4));
        }
        painter.drawText(QRectF(plot.left(), plot.bottom() + 7, plot.width(), 20), Qt::AlignLeft,
                         QString::number(xMin));
        if (xMax != xMin)
            painter.drawText(QRectF(plot.left(), plot.bottom() + 7, plot.width(), 20), Qt::AlignRight,
                             QString::number(xMax));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 29, plot.width(), 20), Qt::AlignCenter, columnName(_x));
        painter.drawText(QRectF(plot.left(), 0, plot.width(), 22), Qt::AlignCenter, columnName(_y));

        const auto color = palette().highlight().color();
        for (const auto& trial : _trials) {
            if (trial[_y] < 0)
                continue;
            const double xFraction = xMax == xMin ? 0.5 : double(trial[_x] - xMin) / xScale;
            const auto point = QPointF(plot.left() + plot.width() * xFraction,
                                       plot.bottom() - plot.height() * double(trial[_y]) / yScale);
            if (_x == TrialNumber && !_points.empty()) {
                painter.setPen(QPen(color, 1.5));
                painter.drawLine(_points.back().first, point);
            }
            _points.emplace_back(point, &trial);
        }
        for (const auto& [point, trial] : _points) {
            const bool best = (*trial)[TrialNumber] == _bestTrial;
            painter.setPen(QPen(best ? palette().text().color() : color, best ? 2 : 1));
            painter.setBrush(color);
            painter.drawEllipse(point, best ? 6 : 4, best ? 6 : 4);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const Trial* closest = nullptr;
        qreal distance = 12;
        for (const auto& [point, trial] : _points) {
            const auto candidate = (point - event->position()).manhattanLength();
            if (candidate < distance) {
                distance = candidate;
                closest = trial;
            }
        }
        if (closest)
            QToolTip::showText(event->globalPosition().toPoint(), tr("Trial %1\n%2: %3\n%4: %5")
                .arg((*closest)[TrialNumber]).arg(columnName(_x)).arg((*closest)[_x])
                .arg(columnName(_y)).arg((*closest)[_y]), this);
        else
            QToolTip::hideText();
    }

private:
    const std::vector<Trial>& _trials;
    const int& _bestTrial;
    int _x = TrialNumber;
    int _y = ScoreMs;
    std::vector<std::pair<QPointF, const Trial*>> _points;
};

class TrialResults final : public QDialog
{
public:
    explicit TrialResults(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(tr("Tiled storage experiment results"));
        resize(1000, 580);
        auto* layout = new QVBoxLayout(this);
        _summary = new QLabel(this);
        _summary->setWordWrap(true);
        layout->addWidget(_summary);
        auto* tabs = new QTabWidget(this);
        layout->addWidget(tabs);

        _table = new QTableWidget(0, ColumnCount, tabs);
        QStringList headings;
        for (int column = 0; column < ColumnCount; ++column)
            headings << columnName(column);
        _table->setHorizontalHeaderLabels(headings);
        _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        _table->setSelectionBehavior(QAbstractItemView::SelectRows);
        _table->verticalHeader()->hide();
        _table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        _table->setSortingEnabled(true);
        tabs->addTab(_table, tr("Trial table"));

        auto* chartTab = new QWidget(tabs);
        auto* chartLayout = new QVBoxLayout(chartTab);
        auto* axes = new QFormLayout;
        auto* x = new QComboBox(chartTab);
        auto* y = new QComboBox(chartTab);
        for (int column = TrialNumber; column <= SuperTileColumns; ++column)
            x->addItem(columnName(column), column);
        for (int column = StorageMs; column <= ScoreMs; ++column)
            y->addItem(columnName(column), column);
        y->setCurrentIndex(y->count() - 1);
        axes->addRow(tr("Compare by"), x);
        axes->addRow(tr("Timing"), y);
        chartLayout->addLayout(axes);
        _chart = new TrialChart(_trials, _bestTrial, chartTab);
        chartLayout->addWidget(_chart, 1);
        const auto setAxes = [this, x, y]() { _chart->setAxes(x->currentData().toInt(), y->currentData().toInt()); };
        connect(x, &QComboBox::currentIndexChanged, this, setAxes);
        connect(y, &QComboBox::currentIndexChanged, this, setAxes);
        auto* note = new QLabel(tr("Each point is a trial. The outlined point has the best score. Hover for the exact values."), chartTab);
        note->setWordWrap(true);
        chartLayout->addWidget(note);
        auto* exportChart = new QPushButton(tr("Export chart (.png)…"), chartTab);
        chartLayout->addWidget(exportChart, 0, Qt::AlignLeft);
        connect(exportChart, &QPushButton::clicked, this, [this]() {
            auto path = QFileDialog::getSaveFileName(this, tr("Export timing chart"),
                QStringLiteral("EvoViewer Tiled Storage Chart.png"), tr("PNG images (*.png)"));
            if (path.isEmpty())
                return;
            if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
                path += QStringLiteral(".png");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || !_chart->grab().save(&file, "PNG") || !file.commit())
                QMessageBox::warning(this, tr("Could not export chart"), file.errorString());
        });
        tabs->addTab(chartTab, tr("Timing chart"));

        auto* actions = new QHBoxLayout;
        _exportCsv = new QPushButton(tr("Export trial data (.csv)…"), this);
        _exportPreset = new QPushButton(tr("Export best preset (.mvp)…"), this);
        actions->addWidget(_exportCsv);
        actions->addWidget(_exportPreset);
        actions->addStretch();
        layout->addLayout(actions);
    }

    QPushButton* csvButton() const { return _exportCsv; }
    QPushButton* presetButton() const { return _exportPreset; }

    void reset(const QString& dataset, const QString& clusters, const QString& workloads, bool updateScored)
    {
        _table->setRowCount(0);
        _trials.clear();
        _bestTrial = -1;
        _dataset = dataset;
        _clusters = clusters;
        _workloads = workloads;
        _updateScored = updateScored;
        _table->horizontalHeaderItem(UpdateMs)->setText(updateScored ? columnName(UpdateMs) : tr("Update preparation (ms)"));
        _summary->setText(tr("%1 · %2 · %3").arg(dataset, clusters, workloads));
        _chart->refresh();
    }

    void append(const Trial& trial, bool best)
    {
        _trials.push_back(trial);
        if (best) {
            _bestTrial = static_cast<int>(trial[TrialNumber]);
            _summary->setText(tr("%1 · %2 · %3\nBest trial %4 · score %5 ms")
                .arg(_dataset, _clusters, _workloads).arg(_bestTrial).arg(trial[ScoreMs]));
        }

        _table->setSortingEnabled(false);
        const auto row = _table->rowCount();
        _table->insertRow(row);
        for (int column = 0; column < ColumnCount; ++column) {
            auto* item = new QTableWidgetItem;
            if (trial[column] >= 0)
                item->setData(Qt::DisplayRole, trial[column]);
            else
                item->setText(QStringLiteral("—"));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            _table->setItem(row, column, item);
        }
        _table->setSortingEnabled(true);
        _chart->refresh();
    }

    bool writeCsv(const QString& path, QString& error) const
    {
        const auto quote = [](QString field) {
            field.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(field);
        };
        QStringList headings;
        for (int column = 0; column < ColumnCount; ++column)
            headings << quote(columnName(column));
        headings << QStringLiteral("Best trial,Cluster dataset,Clusters,Workloads,Update included in score");
        QString csv = headings.join(QLatin1Char(',')) + QStringLiteral("\r\n");
        for (const auto& trial : _trials) {
            QStringList fields;
            for (auto value : trial)
                fields << (value < 0 ? QString() : QString::number(value));
            fields << (trial[TrialNumber] == _bestTrial ? QStringLiteral("Yes") : QStringLiteral("No"))
                   << quote(_dataset) << quote(_clusters) << quote(_workloads)
                   << (_updateScored ? QStringLiteral("Yes") : QStringLiteral("No"));
            csv += fields.join(QLatin1Char(',')) + QStringLiteral("\r\n");
        }

        QSaveFile file(path);
        const auto bytes = csv.toUtf8();
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
            return true;
        error = file.errorString();
        return false;
    }

private:
    std::vector<Trial> _trials;
    int _bestTrial = -1;
    bool _updateScored = true;
    QString _dataset, _clusters, _workloads;
    QLabel* _summary = nullptr;
    QTableWidget* _table = nullptr;
    TrialChart* _chart = nullptr;
    QPushButton* _exportCsv = nullptr;
    QPushButton* _exportPreset = nullptr;
};
}
