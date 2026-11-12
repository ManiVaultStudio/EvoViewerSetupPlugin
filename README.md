# EvoViewer Setup Plugin

## Purpose

The EvoViewer Setup Plugin prepares one ManiVault workspace for the Cytosplore EvoViewer workflow. It opens the required views, connects shared actions, asks the user to identify the project datasets, applies those datasets, and then opens the EvoViewer tutorial. Setup runs automatically when the plugin view is opened; the setup view closes itself at the end without destroying the view instance.

The plugin is intentionally the only place where this workspace orchestration is implemented. Other plugins keep ownership of their own views, controls, and data behavior.

## Handing off a configured project

Destroy the EvoViewer Setup instance before saving or publishing the project. The workspace keeps the configured views, datasets, and action connections, which are saved through ManiVault's existing project serialization. Hiding the setup view alone leaves an instance in the project.

For a published project, enable **Allowed plugins only** and exclude **EvoViewer Setup Plugin** from that list. Destroy the instance before publishing because ManiVault adds plugin kinds currently in use to the allowed list. If you distribute a separate ManiVault build, you can omit the setup plugin from that build.

## Source layout

- `EvoViewerSetupPlugin.cpp` owns plugin creation, startup, and the debounced menu-availability refresh.
- `EvoViewerSetupRequirements.cpp` checks the required plugin factories and existing project datasets.
- `EvoViewerSetupData.cpp` builds the constrained dataset-selection dialog.
- `EvoViewerSetupWorkflow.cpp` arranges the views, links their actions, applies the selections, and finishes setup.

## Required plugins

The setup entry is available only when all of these plugin factories are present:

| Plugin factory | Role in setup |
| --- | --- |
| `Scatterplot View` | Embedding and cluster-color display |
| `Cross-Species Comparison Cluster Rank View` | Cluster hierarchy view |
| `XSCGeneDetect` | Publishes the shared EvoViewer actions and displays gene analysis |
| `XSCPhyTVu` | Displays the species tree and selected trait |
| `ChartLegend View` | Displays the scatterplot cluster legend |
| `Tutorial` | Opens the `Cytosplore EvoViewer` tutorial after setup |

The project must already contain `XSCTree` and `XSCTreeMeta` datasets, and the plugin manager must provide the `Points` and `Cluster` data types. The setup entry is hidden from the standard plugin menu until the required plugin factories and project datasets are available.

## Required project data

An eligible main points dataset must have, as direct children:

- At least three cluster datasets (ManiVault data type `Clusters`), for the species names and top, middle, and bottom cluster levels.
- At least one `Points` dataset, for a point embedding.

The user selects these existing project datasets in the setup dialog. Dataset pickers are constrained by type and parent-child relationship:

| Setup input | Requirement |
| --- | --- |
| Main points | `Points` dataset with at least three direct `Clusters` children and one direct `Points` child |
| Embedding | `Points` child of the selected main points dataset |
| Scatterplot UMAP points | `Points` child of the selected main points dataset |
| Species names | `Clusters` child of the selected main points dataset |
| Bottom cluster names | `Clusters` child of the selected main points dataset |
| Middle cluster names | `Clusters` child of the selected main points dataset |
| Top cluster names | `Clusters` child of the selected main points dataset |
| Reference tree | `XSCTree` dataset |
| Metadata / trait | `XSCTreeMeta` dataset |

All nine inputs are mandatory. The three hierarchy levels and species names are independent choices because projects can store them as separate cluster datasets.

The setup does not load or create tree data. Both an `XSCTree` dataset and an `XSCTreeMeta` dataset must already be present in the project data hierarchy before the setup entry becomes available.

## Workspace layout

Views are found by plugin kind and requested display name, or created if missing. The setup arranges them as follows:

1. `Cluster Hierarchy View` spans the top and receives one tenth of the vertical space.
2. The bottom row contains, from left to right, `Scatterplot Embedding View`, `Legend View`, `Gene Identification View`, and `Phylogenetic Tree View`.
3. `Data properties` and `Data hierarchy` are hidden when present; their views are not destroyed.
4. The setup view is hidden when setup finishes; it is not destroyed.
5. The Tutorial view is opened last and selects `Cytosplore EvoViewer`.

## Action connections

### Gene Identification View public actions

The setup plugin publishes these GeneDetect actions so other views can connect to the same state:

- Reference tree dataset
- Main points dataset
- Embedding dataset
- Species names dataset
- Bottom, middle, and top cluster names datasets
- Scatterplot UMAP points option
- Species explorer option
- Selected species values
- Status color
- Cluster order hierarchy
- Right-clicked cluster and clear-right-clicked-cluster actions
- Top-selected-hierarchy status

The actions are located by their paths inside the GeneDetect view. Publishing keeps the action owned by GeneDetect while making it available to the other views.

### Phylogenetic Tree View

The reference tree, selected species values, species-name dataset, and species-explorer selection are connected to the corresponding published GeneDetect actions. The selected `XSCTreeMeta` dataset is assigned to the trait dataset picker.

### Cluster Hierarchy View

The top, middle, and bottom hierarchy datasets, species names, main points, embedding, status color, cluster order, right-clicked cluster, clear-right-clicked-cluster, and top-selected-hierarchy status are connected to their matching published GeneDetect actions.

### Scatterplot and Legend

The scatterplot position dataset is set to the selected UMAP points dataset. Its color dataset is set to the selected bottom-cluster dataset and published by the setup plugin. The Legend view's `Cluster dataset` color option is connected to that published scatterplot color action, so the legend follows the same cluster selection. The scatterplot selection tool starts in Lasso mode, and its Microsoft blue selection color (`RGB 53, 126, 199`) is published and connected to the Legend view's selection color.

## Execution and responsiveness

ManiVault view creation, action linking, and dataset selection are performed on the application thread because those APIs and widgets are UI-owned. Setup does not show a separate progress/status popup.

## Failure handling

Setup checks for required plugin factories, eligible main point data, an `XSCTree` dataset, and an `XSCTreeMeta` dataset before it becomes available. Missing views or actions are written to the application log, and the setup view closes without destroying the view instance. A failed step does not open the tutorial as though setup succeeded.
