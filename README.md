# CTAnalyzerX

A comprehensive toolkit for segmentation and analysis of high-resolution micro-computed tomography (uCT) images of mouse bone microarchitecture.

## Overview

CTAnalyzerX provides a suite of specialized tools for processing and analyzing volumetric uCT datasets, with a focus on bone morphometry in mouse models.

### Primary Use Cases

- **Bone Microarchitecture Analysis**: Quantify trabecular thickness, spacing, bone volume fraction (BV/TV), and other morphometric parameters
- **Region of Interest (ROI) Definition**: Define and extract specific anatomical regions for targeted analysis
- **3D Visualization**: Interactive slice-based and GPU-accelerated volume rendering of uCT datasets
- **Batch Processing**: Automated pipeline execution for high-throughput
- **Multi-format Support**: Load DICOM, NIfTI, and Scanco ISQ formats

## Applications

The toolkit includes several specialized applications, each designed for a specific stage of the analysis pipeline:

### CTAnalyzerX (Main Application)
**Purpose**: Full-featured analysis workstation  
**Location**: `Users/XXX/AppData/Local/CTAnalyzerX/`  
**Features**:
- Complete workflow state machine (Load -> Crop -> Segment -> Analyze)
- Advanced cropping tools with live 3D outline preview and Save functionality
- Semi-automatic segmentation with threshold-based and region-growing tools
- Morphometric analysis with export to CSV or JSON
- Project state persistence (JSON-based session files)
- logging and error reporting with VTK integration (see Location /logs)

**Use Case**: Production analysis of bone uCT datasets with repeatable workflows, state persistence, and comprehensive measurement capabilities.

---

### CTAXViewer
**Purpose**: Lightweight standalone 3D image viewer  
**Location**: `tools/Viewer/`  
**Features**:
- Three orthogonal slice views (Sagittal/YZ, Coronal/XZ, Axial/XY)
- GPU-accelerated volume rendering with interactive rotation
- Real-time window/level adjustment
- ROI plane visualization (non-destructive clipping planes)
- Drag-and-drop file loading with recent files menu
- Screenshot capture (PNG/JPEG)
- Cursor-driven voxel value display in status bar

**Use Case**: Quick inspection of datasets, quality control, and preliminary visualization without the overhead of the full analysis workflow. Ideal for rapid dataset triage.

---

### CTAXPrototype
**Purpose**: Experimental algorithm development and testing workbench  
**Location**: `tools/Prototype/`  
**Features**:
- Prototyping and tuning of mouse bone image segmentation
- Threshold connectivity-based segmentation
- **Optional**: Graph-cut segmentation for bone island detection (requires build with ITK + [ImageGraphCut3DSegmentation](https://github.com/Bonelab/ITKBoneMorphometry))
- Direct pipeline manipulation without state machine constraints
- Export intermediate results for validation

**Build Option**: `CTAXPROTOTYPE_ENABLE_GRAPH_CUT=ON` enables advanced graph-cut segmentation  
**Dependencies (when enabled)**: ITK 5.2+, ImageGraphCut3DSegmentation, GridCut (optional multi-threaded backend)

**Use Case**: Algorithm development, parameter tuning, and validation of segmentation approaches before integration into the main application.

---

### CTAXMorphometry
**Purpose**: Command-line morphometric analysis tool  
**Location**: `tools/Morphometry/`  
**Features**:
- Headless (CLI) or GUI-based batch morphometry computation
- ITK-based compute trabecular thickness (Tb.Th), spacing (Tb.Sp), number (Tb.N)
- VTK-based compute bone volume fraction (BV/TV) and structure model index (SMI)
- CSV or JSON export for integration with statistical analysis pipelines
- Progress dialog with real-time status updates

**Use Case**: High-throughput morphometry for large studies where interactive visualization is unnecessary.

---

### CTAXBatchProcessor
**Purpose**: Automated batch processing for multi-subject studies  
**Location**: `tools/BatchProcessor/`  
**Features**:
- Queue-based job execution with configurable pipelines
- Parallel processing using Qt Concurrent framework
- Automatic retry and error logging
- JSON-based job configuration files
- Progress tracking with estimated time remaining

**Use Case**: Batch processing of 10-1000+ datasets with consistent parameters, eliminating manual intervention.

## Architecture

### Library Architecture

#### **CTAXCore** (`lib/core/`)
Core image processing and rendering components:

- **Image I/O**: `ImageLoader` (DICOM via vtk-dicom, NIfTI, Scanco ISQ)
- **VTK Custom Classes**:
  - `vtkImageOrthoPlanes`: Three-plane reslicing with synchronized cursors
  - `vtkSliceOutlineSource`: 3D crop outline visualization
  - `vtkLandmarkActor`: Spherical landmark markers with labels
  - `vtkTransformWidget`: Interactive 3D transformation handles
  - `vtkImageCoordinateWidget`: Crosshair cursor with voxel coordinate display
  - `vtkFillFullyEnclosedVoxelFilter`: Flood-fill for closed regions
- **ITK Integration**: `FillFullyEnclosedVoxelImageFilter` (ITK-style wrapper)
- **Utilities**: 
  - `OtsuThresholdWorker`: Background thread Otsu computation
  - `VoxelLineIterator`: 3D Bresenham line traversal
  - `ProcessHelpers`: Subprocess management for external tools
  - `VtkQtOutputWindow`: Redirect VTK warnings to Qt logging

**VTK Modules Used**:  
`ImagingCore`, `ImagingGeneral`, `RenderingCore`, `RenderingOpenGL2`, `InteractionStyle`, `RenderingVolume`, `RenderingVolumeOpenGL2`, `DICOM`

**ITK Components**: Core containers, image I/O, basic filters (filtered to exclude `itkdouble-conversion` on MSVC)

---

#### **CTAXGui** (`lib/gui/`)
Reusable Qt widgets and UI components:

- **Widgets**:
  - `CropWidget`: Dual-mode (Cropping/Visualization) ROI definition with range sliders
  - `WindowLevelWidget`: Window/level presets and manual adjustment
  - `ScalarOpacityFunctionWidget`: Transfer function editor for volume rendering
  - `LandmarkWidget`: Table-based landmark management (add/delete/rename)
  - `LightboxWidget`: Three-slice + volume view container with synchronized interaction
  - `SliceView`: Single orthogonal slice with VTK render window integration
  - `VolumeView`: GPU volume ray-casting with crop planes and rotation controls
  - `RangeSlider`: Dual-handle range selector (Qt custom widget)
  - `CollapsibleGroupBox`: Expandable panel container
  - `ImageInfoWidget`: Display image dimensions, spacing, scalar range, metadata
- **Helpers**:
  - `WindowLevelBridge`: Mediates window/level changes between widget and views
  - `JsonSettings`/`JsonUtils`: JSON-based configuration persistence
  - `CropExporter`: Write cropped subvolumes to disk
  - `LandmarkHelper`: Coordinate system transformations for landmarks
- **Styles**: Custom Qt stylesheets (`ProxyStyle`, `SunkenSliderStyle`)

**VTK Modules Used**:  
`ImagingStatistics` (histogram computation), `GUISupportQt` (QVTKOpenGLNativeWidget)

**Dependencies**: Qt5/Qt6 (Core, Gui, Widgets, Charts), Eigen3 (matrix operations)

---

## Build Requirements

### Prerequisites

| Dependency | Minimum Version | Purpose | Install Command (vcpkg) |
|------------|-----------------|---------|-------------------------|
| **CMake** | 3.16+ | Build system generator | `choco install cmake` (Windows) |
| **C++ Compiler** | C++17 | MSVC 2019+, GCC 9+, Clang 10+ | Visual Studio 2019+ |
| **Qt** | 5.15+ | GUI framework | `vcpkg install qt5-base qt5-widgets qt5-charts` |
| **VTK** | 9.3+ | Visualization and rendering | `vcpkg install vtk[qt,dicom]` |
| **vtk-dicom** | 0.8.15+ | DICOM reading | `vcpkg install vtk-dicom` |
| **ITK** | 5.2+ | Image processing algorithms | `vcpkg install itk` |
| **Eigen3** | 3.3+ | Linear algebra (required by ITK) | `vcpkg install eigen3` |

### Optional Dependencies

| Dependency | Purpose | CMake Flag | Install |
|------------|---------|------------|---------|
| **ImageGraphCut3DSegmentation** | Graph-cut bone island segmentation | `CTAXPROTOTYPE_ENABLE_GRAPH_CUT=ON` | Build from [source](https://github.com/Bonelab/ITKBoneMorphometry) |
| **GridCut** | Multi-threaded graph-cut backend | Auto-detected by ImageGraphCut3D | [Download](https://github.com/ptrNine/gridcut) |


see '/docs/build_notes.txt' for detailed build instructions and troubleshooting tips.

---

### Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| **Windows 10/11** | [YES] Primary | MSVC 2019/2022, vcpkg recommended |

---

## Building from Source

### 1. Install Dependencies

#### **Windows (vcpkg - Recommended)**

````````
# Install Visual Studio 2019+ with Desktop development with C++ workload
# Install CMake 3.16+ (e.g., via Chocolatey: choco install cmake)
# Install vcpkg (follow https://github.com/microsoft/vcpkg#getting-started)

# Install required packages using vcpkg
vcpkg install qt5-base qt5-widgets qt5-charts vtk[qt,dicom] vtk-dicom itk eigen3

# Optional: Install Graph-cut segmentation dependencies
# Build ITK with ImageGraphCut3DSegmentation support from source
# Install GridCut from https://github.com/ptrNine/gridcut
````````

### 2. Configure CMake

Clone repository
git clone https://github.com/your-org/CTAnalyzerX.git cd CTAnalyzerX
Create build directory (REQUIRED - out-of-source only)
mkdir build && cd build
Configure with CMake
cmake .. 
-DCMAKE_BUILD_TYPE=Release 
-DCMAKE_PREFIX_PATH="C:/vcpkg/installed/x64-windows" 
-DCTAX_BUILD_TOOLS=ON 
-DCTAX_BUILD_TESTS=OFF
Optional: Enable graph-cut segmentation in Prototype
cmake .. 
-DCTAXPROTOTYPE_ENABLE_GRAPH_CUT=ON 
-DCMAKE_PREFIX_PATH="/path/to/ImageGraphCut3D/install;C:/vcpkg/installed/x64-windows"

**Important CMake Variables**:

| Variable | Default | Description |
|----------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Debug` | `Debug` or `Release` |
| `CMAKE_PREFIX_PATH` | — | Semicolon-separated paths to Qt, VTK, ITK, etc. |
| `CTAX_BUILD_TOOLS` | `ON` | Build Viewer, Prototype, Morphometry, BatchProcessor |
| `CTAX_BUILD_TESTS` | `OFF` | Build unit tests |
| `CTAXPROTOTYPE_ENABLE_GRAPH_CUT` | `OFF` | Enable ITK graph-cut segmentation in Prototype |
| `BUILD_SHARED_LIBS` | `OFF` | Build shared libraries (not recommended) |

---

### 3. Build

Multi-core build (adjust -j based on CPU cores)
cmake --build . --config Release -j8
Single-configuration generators (Ninja, Make):
cmake --build . -j8
Multi-configuration generators (Visual Studio):
cmake --build . --config Release -j8

**Build Outputs** (in `build/bin/`):
- `CTAnalyzerX[.exe]` - Main application
- `CTAXViewer[.exe]` - Standalone viewer
- `CTAXPrototype[.exe]` - Algorithm workbench
- `CTAXMorphometry[.exe]` - Morphometry tool
- `CTAXBatchProcessor[.exe]` - Batch processor

---

### 4. Run

From build directory
.\bin\CTAXViewer
.\bin\CTAnalyzerX
etc.

### 5. Workflow

CTANalyzerX implements a state machine workflow for the main application, guiding users through the analysis pipeline. The primary states are:
1. **Load Image**: Drag-and-drop DICOM/NIfTI/ISQ files or use File → Open
2. **Crop Image**: Define ROI using interactive cropping tools with live 3D preview
3. Save Cropped Image: Export cropped subvolume to disk
1. json project file with metadata and crop parameters are saved in /AppData/CTAnalyzerX/projects

Either proceed to:
Batch Processing:
CTAXBatchProcessor CLI for automated processing of multiple datasets with consistent parameters
1. run CTAXBatchProcessor --help for usage instructions

or
Manual Segmentation:
CTAXPrototype for interactive segmentation
1. Open json configuration file with desired parameters
2. Fine-tune segmentation parameters (thresholds, connectivity)
3. Export segmentation results for analysis

CTAXMorphometry for morphometric analysis of segmented volumes
1. Run CTAXMorphometry with segmented image as input or path to folder of json configuration files for batch processing
2. run CTAXMorphometry --help for usage instructions

Viewer Utility:
Use CTAXViewer for quick inspection of datasets, quality control, and preliminary visualization without the overhead of the full analysis workflow. Ideal for rapid dataset triage.

## Acknowledgments

This project uses the following open-source libraries:

- **[Qt](https://www.qt.io/)** (LGPL-3.0): Cross-platform GUI framework
- **[VTK](https://vtk.org/)** (BSD-3-Clause): 3D visualization and imageprocessing
- **[ITK](https://itk.org/)** (Apache-2.0): Medical image segmentation and registration
- **[vtk-dicom](https://github.com/dgobbi/vtk-dicom)** (BSD-3-Clause): DICOM file I/O
- **[Eigen](https://eigen.tuxfamily.org/)** (MPL-2.0): Linear algebra library
- **[ImageGraphCut3DSegmentation](https://github.com/Bonelab/ITKBoneMorphometry)** (Apache-2.0): Graph-cut segmentation (optional)

---

## Version History

### v0.1.1 (Current)
- Initial public release
- Core viewer and analysis tools functional
- DICOM, NIfTI, Scanco ISQ support
- Basic morphometry and batch processing

### v0.1.0
- Internal development release
- Prototype algorithms validated