from conans import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain
from conans.tools import save, load
import os
import pathlib
import subprocess
from rules_support import PluginBranchInfo


class EvoViewerSetupPluginConan(ConanFile):
    """Package EvoViewerSetupPlugin using Conan.

    Uses rules_support (github.com/ManiVaultStudio/rulessupport) to derive
    version information from the branch naming convention.
    """

    name = "EvoViewerSetupPlugin"
    description = "Sets up a ManiVault workspace for the Cytosplore EvoViewer workflow."
    topics = ("manivault", "plugin", "setup", "EvoViewerSetupPlugin")
    url = "https://github.com/ManiVaultStudio/EvoViewerSetupPlugin"
    author = "ManiVaultStudio"
    license = "LGPL 3.0"

    short_paths = True
    generators = "CMakeDeps"

    settings = {"os": None, "build_type": None, "compiler": None, "arch": None}
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": True, "fPIC": True}

    scm = {
        "type": "git",
        "subfolder": "hdps/EvoViewerSetupPlugin",
        "url": "auto",
        "revision": "auto",
    }

    def __get_git_path(self):
        path = load(
            pathlib.Path(pathlib.Path(__file__).parent.resolve(), "__gitpath.txt")
        )
        print(f"git info from {path}")
        return path

    def export(self):
        save(
            pathlib.Path(self.export_folder, "__gitpath.txt"),
            str(pathlib.Path(__file__).parent.resolve()),
        )

    def set_version(self):
        branch_info = PluginBranchInfo(self.recipe_folder)
        self.version = branch_info.version

    def _dependency_channel(self):
        branch = os.getenv("GITHUB_HEAD_REF") or os.getenv("GITHUB_REF_NAME")

        if not branch:
            branch = subprocess.check_output(
                [
                    "git",
                    "-C",
                    self.__get_git_path(),
                    "rev-parse",
                    "--abbrev-ref",
                    "HEAD",
                ],
                text=True,
            ).strip()

        self.output.info(f"Detected branch: {branch}")
        if branch in ("main", "master", "HEAD"):
            return "latest"

        return branch.rsplit("/", 1)[-1]

    def requirements(self):
        channel = self._dependency_channel()
        self.output.info(f"Using XSCTreeData channel: {channel}")
        self.requires(f"XSCTreeData/{channel}@lkeb/stable")
        self.requires(f"XSCTreeMetaData/{channel}@lkeb/stable")

        branch_info = PluginBranchInfo(self.__get_git_path())
        self.output.info(f"Core requirement {branch_info.core_requirement}")
        self.requires(branch_info.core_requirement)

    def configure(self):
        pass

    def system_requirements(self):
        pass

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def generate(self):
        generator = None
        if self.settings.os == "Macos":
            generator = "Xcode"
        if self.settings.os == "Linux":
            generator = "Ninja Multi-Config"

        tc = CMakeToolchain(self, generator=generator)
        tc.variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"

        qt_path = pathlib.Path(self.deps_cpp_info["qt"].rootpath)
        qt_cfg = list(qt_path.glob("**/Qt6Config.cmake"))[0]
        tc.variables["Qt6_DIR"] = qt_cfg.parents[0].as_posix()

        mv_core_root = self.deps_cpp_info["hdps-core"].rootpath
        manivault_dir = pathlib.Path(mv_core_root, "cmake", "mv").as_posix()
        tc.variables["ManiVault_DIR"] = manivault_dir
        tc.generate()

    def _configure_cmake(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder="hdps/EvoViewerSetupPlugin")
        cmake.verbose = True
        return cmake

    def build(self):
        print("Build OS is: ", self.settings.os)
        cmake = self._configure_cmake()
        cmake.build(build_type="RelWithDebInfo")

    def package(self):
        package_dir = pathlib.Path(self.build_folder, "package")
        rel_with_deb_info_dir = package_dir / "RelWithDebInfo"
        subprocess.run(
            [
                "cmake",
                "--install",
                self.build_folder,
                "--config",
                "RelWithDebInfo",
                "--prefix",
                rel_with_deb_info_dir,
            ],
            check=True,
        )
        self.copy(pattern="*", src=package_dir)

    def package_info(self):
        self.cpp_info.relwithdebinfo.libdirs = ["RelWithDebInfo/lib"]
        self.cpp_info.relwithdebinfo.bindirs = ["RelWithDebInfo/Plugins", "RelWithDebInfo"]
        self.cpp_info.relwithdebinfo.includedirs = ["RelWithDebInfo/include", "RelWithDebInfo"]
