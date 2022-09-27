// system headers
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <vector>

// library headers
#include <boost/filesystem.hpp>
#include <CImg.h>
#include <fnmatch.h>


// local headers
#include "linuxdeploy/core/appdir.h"
#include "linuxdeploy/core/elf_file.h"
#include "linuxdeploy/core/log.h"
#include "linuxdeploy/desktopfile/desktopfileentry.h"
#include "linuxdeploy/util/util.h"
#include "linuxdeploy/subprocess/subprocess.h"
#include "copyright.h"

// auto-generated headers
#include "excludelist.h"
#include "appdir_root_setup.h"

using namespace linuxdeploy::core;
using namespace linuxdeploy::desktopfile;
using namespace linuxdeploy::core::log;

using namespace cimg_library;
namespace bf = boost::filesystem;

namespace {
    // equivalent to 0644
    constexpr bf::perms DEFAULT_PERMS = bf::owner_write | bf::owner_read | bf::group_read | bf::others_read;
    // equivalent to 0755
    constexpr bf::perms EXECUTABLE_PERMS = DEFAULT_PERMS | bf::owner_exe | bf::group_exe | bf::others_exe;

    class CopyOperation {
    public:
        bf::path fromPath;
        bf::path toPath;
        bf::perms addedPermissions;
    };

    typedef std::map<bf::path, CopyOperation> CopyOperationsMap;

    /**
     * Stores copy operations.
     * This way, the storage logic does not have to be known to the using class.
     */
    class CopyOperationsStorage {
    private:
        // using a map to make sure every target path is there only once
        CopyOperationsMap _storedOperations;

    public:
        CopyOperationsStorage() = default;

        /**
         * Add copy operation.
         * @param fromPath path to copy from
         * @param toPath path to copy to
         * @param addedPermissions permissions to add to the file's permissions
         */
        void addOperation(const bf::path& fromPath, const bf::path& toPath, const bf::perms addedPermissions) {
             CopyOperation operation{fromPath, toPath, addedPermissions};
            _storedOperations[fromPath] = operation;
        }

        /**
         * Export operations.
         * @return vector containing all operations (random order).
         */
        std::vector<CopyOperation> getOperations() {
            std::vector<CopyOperation> operations;
            operations.reserve(_storedOperations.size());

            for (const auto& operationsPair : _storedOperations) {
                operations.emplace_back(operationsPair.second);
            }

            return operations;
        }

        /**
         * Clear internal storage.
         */
        void clear() {
            _storedOperations.clear();
        }
    };
}

namespace linuxdeploy {
    namespace core {
        namespace appdir {
            class AppDir::PrivateData {
                public:
                    bf::path appDirPath;
                    std::vector<std::string> excludeLibraryPatterns;

                    // store deferred operations
                    // these can be executed by calling excuteDeferredOperations
                    CopyOperationsStorage copyOperationsStorage;
                    std::set<bf::path> stripOperations;
                    std::map<bf::path, std::string> setElfRPathOperations;

                    // stores all files that have been visited by the deploy functions, e.g., when they're blacklisted,
                    // have been added to the deferred operations already, etc.
                    // lookups in a single container are a lot faster than having to look up in several ones, therefore
                    // the little amount of additional memory is worth it, considering the improved performance
                    std::set<bf::path> visitedFiles;

                    // used to automatically rename resources to improve the UX, e.g. icons
                    std::string appName;

                    // platform dependent implementation of copyright files deployment
                    std::shared_ptr<copyright::ICopyrightFilesManager> copyrightFilesManager;

                    // decides whether copyright files deployment is performed
                    bool disableCopyrightFilesDeployment = false;

                public:
                PrivateData() : copyOperationsStorage(), stripOperations(), setElfRPathOperations(), visitedFiles(), appDirPath(), excludeLibraryPatterns() {
                        copyrightFilesManager = copyright::ICopyrightFilesManager::getInstance();
                    };

                public:
                    // calculate library directory name for given ELF file, taking system architecture into account
                    static std::string getLibraryDirName(const bf::path& path) {
                        const auto systemElfClass = elf_file::ElfFile::getSystemElfClass();
                        const auto elfClass = elf_file::ElfFile(path).getElfClass();

                        std::string libDirName = "lib";

                        if (systemElfClass != elfClass) {
                            if (elfClass == ELFCLASS32)
                                libDirName += "32";
                            else
                                libDirName += "64";
                        }

                         return libDirName;
                    }

                    // actually copy file
                    // mimics cp command behavior
                    // also adds minimum file permissions (by default adds 0644 to existing permissions)
                    static bool copyFile(const bf::path& from, bf::path to, bf::perms addedPerms, bool overwrite = false) {
                        ldLog() << "Copying file" << from << "to" << to << std::endl;

                        try {
                            if (!to.parent_path().empty() && !bf::is_directory(to.parent_path()) && !bf::create_directories(to.parent_path())) {
                                ldLog() << LD_ERROR << "Failed to create parent directory" << to.parent_path() << "for path" << to << std::endl;
                                return false;
                            }

                            if (*(to.string().end() - 1) == '/' || bf::is_directory(to))
                                to /= from.filename();

                            if (!overwrite && bf::exists(to)) {
                                ldLog() << LD_DEBUG << "File exists, skipping:" << to << std::endl;
                                return true;
                            }

                            ldLog() << LD_DEBUG << "Copying file" << from << "to" << to << std::endl;
                            bf::copy_file(from, to, bf::copy_option::overwrite_if_exists);

                            {
                                std::stringstream addedPermsStr;
                                addedPermsStr << std::oct << std::setfill('0') << std::setw(2) << addedPerms;
                                ldLog() << LD_DEBUG << "Adding permissions 0o" << LD_NO_SPACE << addedPermsStr.str() << "to" << to << std::endl;
                            }
                            bf::permissions(to, addedPerms | bf::add_perms);
                        } catch (const bf::filesystem_error& e) {
                            ldLog() << LD_ERROR << "Failed to copy file" << from << "to" << to << LD_NO_SPACE << ":" << e.what() << std::endl;
                            return false;
                        }

                        return true;
                    }

                    // create symlink
                    static bool symlinkFile(const bf::path& target, bf::path symlink, const bool useRelativePath = true) {
                        ldLog() << "Creating symlink for file" << target << "in/as" << symlink << std::endl;

                        if (!useRelativePath) {
                            ldLog() << LD_ERROR << "Not implemented" << std::endl;
                            return false;
                        }

                        bf::path relativeTargetPath;

                        // cannot use ln's --relative option any more since we want to support old distros as well
                        // (looking at you, CentOS 6!)
                        {
                            auto symlinkBase = symlink;

                            if (!bf::is_directory(symlinkBase))
                                symlinkBase = symlinkBase.parent_path();

                            relativeTargetPath = bf::relative(target, symlinkBase);
                        }

                        // if a directory is passed as path to create the symlink as/in, we need to complete it with
                        // the filename of the source file to mimic ln's behavior
                        if (bf::is_directory(symlink))
                            symlink /= target.filename();

                        // override existing target (similar to ln's -f flag)
                        if (bf::exists(symlink))
                            bf::remove(symlink);

                        // actually perform symlink creation
                        try {
                            bf::create_symlink(relativeTargetPath, symlink);
                        } catch (const bf::filesystem_error& e) {
                            ldLog() << LD_ERROR << "symlink creation failed:" << e.what() << std::endl;
                            return false;
                        }

                        return true;
                    }

                    bool hasBeenVisitedAlready(const bf::path& path) {
                        return visitedFiles.find(path) != visitedFiles.end();
                    }

                    // execute deferred copy operations registered with the deploy* functions
                    bool executeDeferredOperations() {
                        bool success = true;

                        const auto copyOperations = copyOperationsStorage.getOperations();
                        std::for_each(copyOperations.begin(), copyOperations.end(), [&success](const CopyOperation& operation) {
                            if (!copyFile(operation.fromPath, operation.toPath, operation.addedPermissions)) {
                                success = false;
                            }
                        });
                        copyOperationsStorage.clear();

                        if (!success)
                            return false;

                        if (getenv("NO_STRIP") != nullptr) {
                            ldLog() << LD_WARNING << "$NO_STRIP environment variable detected, not stripping binaries" << std::endl;
                            stripOperations.clear();
                        } else {
                            const auto stripPath = getStripPath();

                            while (!stripOperations.empty()) {
                                const auto& filePath = *(stripOperations.begin());

                                if (util::stringStartsWith(elf_file::ElfFile(filePath).getRPath(), "$")) {
                                    ldLog() << LD_WARNING << "Not calling strip on binary" << filePath << LD_NO_SPACE
                                            << ": rpath starts with $" << std::endl;
                                } else {
                                    ldLog() << "Calling strip on library" << filePath << std::endl;

                                    auto env = subprocess::get_environment();
                                    env["LC_ALL"] = "C";

                                    subprocess::subprocess proc({stripPath, filePath.string()}, env);

                                    const auto result = proc.run();
                                    const auto& err = result.stderr_string();

                                    if (result.exit_code() != 0 &&
                                        !util::stringContains(err, "Not enough room for program headers")) {
                                        ldLog() << LD_ERROR << "Strip call failed:" << err << std::endl;
                                        success = false;
                                    }
                                }

                                stripOperations.erase(stripOperations.begin());
                            }
                        }

                        if (!success)
                            return false;

                        while (!setElfRPathOperations.empty()) {
                            const auto& currentEntry = *(setElfRPathOperations.begin());
                            const auto& filePath = currentEntry.first;
                            const auto& rpath = currentEntry.second;

                            elf_file::ElfFile elfFile(filePath);

                            // no need to set rpath in debug symbols files
                            // also, patchelf crashes on such symbols
                            if (isInDebugSymbolsLocation(filePath) || elfFile.isDebugSymbolsFile()) {
                                ldLog() << LD_WARNING << "Not setting rpath in debug symbols file:" << filePath
                                        << std::endl;
                            } else if (!elfFile.isDynamicallyLinked()) {
                                ldLog() << LD_WARNING << "Not setting rpath in statically-linked file: " << filePath
                                        << std::endl;
                            } else {
                                ldLog() << "Setting rpath in ELF file" << filePath << "to" << rpath << std::endl;
                                if (!elf_file::ElfFile(filePath).setRPath(rpath)) {
                                    ldLog() << LD_ERROR << "Failed to set rpath in ELF file:" << filePath << std::endl;
                                    success = false;
                                }
                            }

                            setElfRPathOperations.erase(setElfRPathOperations.begin());
                        }

                        return true;
                    }

                    // search for copyright file for file and deploy it to AppDir
                    bool deployCopyrightFiles(const bf::path& from) {
                        if (disableCopyrightFilesDeployment)
                            return true;

                        if (copyrightFilesManager == nullptr)
                            return false;

                        auto copyrightFiles = copyrightFilesManager->getCopyrightFilesForPath(from);

                        if (copyrightFiles.empty())
                            return false;

                        ldLog() << "Deploying copyright files for file" << from << std::endl;

                        for (const auto& file : copyrightFiles) {
                            std::string targetDir = file.string();
                            targetDir.erase(0, 1);
                            deployFile(file, appDirPath / targetDir, DEFAULT_PERMS);
                        }

                        return true;
                    }

                    // register copy operation that will be executed later
                    // by compiling a list of files to copy instead of just copying everything, one can ensure that
                    // the files are touched once only
                    // returns the full path of the deployment destination (useful if to is a directory
                    bf::path deployFile(const bf::path& from, bf::path to, bf::perms addedPerms, bool verbose = false) {
                        // not sure whether this is 100% bullet proof, but it simulates the cp command behavior
                        if (to.string().back() == '/' || bf::is_directory(to)) {
                            to /= from.filename();
                        }

                        if (verbose)
                            ldLog() << "Deploying file" << from << "to" << to << std::endl;

                        copyOperationsStorage.addOperation(from, to, addedPerms);

                        // mark file as visited
                        visitedFiles.insert(from);

                        return to;
                    }

                    bool deployElfDependencies(const bf::path& path) {
                        ldLog() << "Deploying dependencies for ELF file" << path << std::endl;
                        try {
                            for (const auto &dependencyPath : elf_file::ElfFile(path).traceDynamicDependencies())
                                if (!deployLibrary(dependencyPath, false, false))
                                    return false;
                        } catch (const elf_file::DependencyNotFoundError& e) {
                            ldLog() << LD_ERROR << e.what() << std::endl;
                            return false;
                        }

                        return true;
                    }

                    static std::string getStripPath() {
                        // by default, try to use a strip next to the linuxdeploy binary
                        // if that isn't available, fall back to searching for strip in the PATH
                        std::string stripPath = "strip";

                        auto binDirPath = bf::path(util::getOwnExecutablePath()).parent_path();
                        auto localStripPath = binDirPath / "strip";

                        if (bf::exists(localStripPath))
                            stripPath = localStripPath.string();

                        ldLog() << LD_DEBUG << "Using strip:" << stripPath << std::endl;

                        return stripPath;
                    }

                    static std::string calculateRelativeRPath(const bf::path& originDir, const bf::path& dependencyLibrariesDir) {
                        auto relPath = bf::relative(bf::absolute(dependencyLibrariesDir), bf::absolute(originDir));
                        std::string rpath = "$ORIGIN/" + relPath.string() + ":$ORIGIN";
                        return rpath;
                    }

                    bool deployLibrary(const bf::path& path, bool forceDeploy = false, bool deployDependencies = true, const bf::path& destination = bf::path()) {
                        if (!forceDeploy && hasBeenVisitedAlready(path)) {
                            ldLog() << LD_DEBUG << "File has been visited already:" << path << std::endl;
                            return true;
                        }

                        if (!bf::exists(path)) {
                            ldLog() << LD_ERROR << "Cannot deploy non-existing library file:" << path << std::endl;
                            return false;
                        }

                        static auto isInExcludelist = [](const bf::path& fileName, const std::vector<std::string> &excludeList) {
                            for (const auto& excludePattern : excludeList) {
                                // simple string match is faster than using fnmatch
                                if (excludePattern == fileName)
                                    return true;

                                auto fnmatchResult = fnmatch(excludePattern.c_str(), fileName.string().c_str(), FNM_PATHNAME);
                                switch (fnmatchResult) {
                                    case 0:
                                        return true;
                                    case FNM_NOMATCH:
                                        break;
                                    default:
                                        ldLog() << LD_ERROR << "fnmatch() reported error:" << fnmatchResult << std::endl;
                                        return false;
                                }
                            }

                            return false;
                        };

                        if (!forceDeploy && (isInExcludelist(path.filename(), generatedExcludelist) || isInExcludelist(path.filename(), excludeLibraryPatterns))) {
                            ldLog() << "Skipping deployment of blacklisted library" << path << std::endl;

                            // mark file as visited
                            visitedFiles.insert(path);

                            return true;
                        }

                        // note for self: make sure to have a trailing slash in libraryDir, otherwise copyFile won't
                        // create a directory
                        bf::path libraryDir = appDirPath / "usr" / (getLibraryDirName(path) + "/");

                        ldLog() << "Deploying shared library" << path;
                        if (!destination.empty())
                            ldLog() << " (destination:" << destination << LD_NO_SPACE << ")";
                        ldLog() << std::endl;

                        auto actualDestination = destination.empty() ? libraryDir : destination;

                        // not sure whether this is 100% bullet proof, but it simulates the cp command behavior
                        if (actualDestination.string().back() == '/' || bf::is_directory(actualDestination)) {
                            actualDestination /= path.filename();
                        }

                        // in case destinationPath is a directory, deployFile will give us the deployed file's path
                        actualDestination = deployFile(path, actualDestination, DEFAULT_PERMS);
                        deployCopyrightFiles(path);

                        std::string rpath = "$ORIGIN";

                        if (!destination.empty()) {
                            // destination is the place where to deploy this file
                            // therefore, rpathDestination means
                            std::string rpathOriginDir = destination.string();

                            if (destination.string().back() == '/') {
                                rpathOriginDir = destination.string();

                                while (rpathOriginDir.back() == '/')
                                    rpathOriginDir.erase(rpathOriginDir.end() - 1, rpathOriginDir.end());
                            } else {
                                rpathOriginDir = destination.parent_path().string();
                            }

                            rpath = calculateRelativeRPath(rpathOriginDir, libraryDir);
                        }

                        // no need to set rpath in debug symbols files
                        // also, patchelf crashes on such symbols
                        if (!isInDebugSymbolsLocation(actualDestination)) {
                            setElfRPathOperations[actualDestination] = rpath;
                        }

                        stripOperations.insert(actualDestination);

                        if (!deployDependencies)
                            return true;

                        return deployElfDependencies(path);
                    }

                    bool deployExecutable(const bf::path& path, const boost::filesystem::path& destination) {
                        if (hasBeenVisitedAlready(path)) {
                            ldLog() << LD_DEBUG << "File has been visited already:" << path << std::endl;
                            return true;
                        }

                        ldLog() << "Deploying executable" << path << std::endl;

                        // FIXME: make executables executable

                        auto destinationPath = destination.empty() ? appDirPath / "usr/bin/" : destination;

                        deployFile(path, destinationPath, EXECUTABLE_PERMS);
                        deployCopyrightFiles(path);

                        std::string rpath = "$ORIGIN/../" + getLibraryDirName(path);

                        if (!destination.empty()) {
                            std::string rpathDestination = destination.string();

                            if (destination.string().back() == '/') {
                                rpathDestination = destination.string();

                                while (rpathDestination.back() == '/')
                                    rpathDestination.erase(rpathDestination.end() - 1, rpathDestination.end());
                            } else {
                                rpathDestination = destination.parent_path().string();
                            }

                            auto relPath = bf::relative(bf::absolute(appDirPath) / "usr" / getLibraryDirName(path), bf::absolute(rpathDestination));
                            rpath = "$ORIGIN/" + relPath.string();
                        }

                        setElfRPathOperations[destinationPath / path.filename()] = rpath;
                        stripOperations.insert(destinationPath / path.filename());

                        if (!deployElfDependencies(path))
                            return false;

                        return true;
                    }

                    bool deployDesktopFile(const DesktopFile& desktopFile) {
                        if (hasBeenVisitedAlready(desktopFile.path())) {
                            ldLog() << LD_DEBUG << "File has been visited already:" << desktopFile.path() << std::endl;
                            return true;
                        }

                        if (!desktopFile.validate()) {
                            ldLog() << LD_ERROR << "Failed to validate desktop file:" << desktopFile.path() << std::endl;
                        }

                        ldLog() << "Deploying desktop file" << desktopFile.path() << std::endl;

                        deployFile(desktopFile.path(), appDirPath / "usr/share/applications/", DEFAULT_PERMS);

                        return true;
                    }

                    bool deployIcon(const bf::path& path, const std::string targetFilename = "") {
                        if (hasBeenVisitedAlready(path)) {
                            ldLog() << LD_DEBUG << "File has been visited already:" << path << std::endl;
                            return true;
                        }

                        ldLog() << "Deploying icon" << path << std::endl;

                        std::string resolution;

                        // if file is a vector image, use "scalable" directory
                        if (util::strLower(path.filename().extension().string()) == ".svg") {
                            resolution = "scalable";
                        } else {
                            try {
                                CImg<unsigned char> image(path.c_str());

                                auto xRes = image.width();
                                auto yRes = image.height();

                                if (xRes != yRes) {
                                    ldLog() << LD_WARNING << "x and y resolution of icon are not equal:" << path;
                                }

                                resolution = std::to_string(xRes) + "x" + std::to_string(yRes);

                                // otherwise, test resolution against "known good" values, and reject invalid ones
                                const auto knownResolutions = {8, 16, 20, 22, 24, 28, 32, 36, 42, 48, 64, 72, 96, 128, 160, 192, 256, 384, 480, 512};

                                // assume invalid
                                bool invalidXRes = true, invalidYRes = true;

                                for (const auto res : knownResolutions) {
                                    if (xRes == res)
                                        invalidXRes = false;
                                    if (yRes == res)
                                        invalidYRes = false;
                                }

                                auto printIconHint = [&knownResolutions]() {
                                    std::stringstream ss;
                                    for (const auto res : knownResolutions) {
                                        ss << res << "x" << res;

                                        if (res != *(knownResolutions.end() - 1))
                                            ss << ", ";
                                    }

                                    ldLog() << LD_ERROR << "Valid resolutions for icons are:" << ss.str() << std::endl;
                                };

                                if (invalidXRes) {
                                    ldLog() << LD_ERROR << "Icon" << path << "has invalid x resolution:" << xRes << std::endl;
                                    printIconHint();
                                    return false;
                                }

                                if (invalidYRes) {
                                    ldLog() << LD_ERROR << "Icon" << path << "has invalid y resolution:" << yRes << std::endl;
                                    printIconHint();
                                    return false;
                                }
                            } catch (const CImgException& e) {
                                ldLog() << LD_ERROR << "CImg error: " << e.what() << std::endl;
                                return false;
                            }
                        }

                        auto filename = path.filename().string();

                        // if the user wants us to automatically rename icon files, we can do so
                        // this is useful when passing multiple icons via -i in different resolutions
                        if (!targetFilename.empty()) {
                            auto newFilename = targetFilename + path.extension().string();
                            if (newFilename != filename) {
                                ldLog() << LD_WARNING << "Changing name of icon" << path << "to target filename" << newFilename << std::endl;
                                filename = newFilename;
                            }
                        }

                        deployFile(path, appDirPath / "usr/share/icons/hicolor" / resolution / "apps" / filename, DEFAULT_PERMS);
                        deployCopyrightFiles(path);

                        return true;
                    }

                    static bool isInDebugSymbolsLocation(const bf::path& path) {
                        // TODO: check if there's more potential locations for debug symbol files
                        for (const std::string& dbgSymbolsPrefix : {".debug/"}) {
                            if (path.string().substr(0, dbgSymbolsPrefix.size()) == dbgSymbolsPrefix)
                                return true;
                        }

                        return false;
                    }
            };

            AppDir::AppDir(const bf::path& path) {
                d = std::make_shared<PrivateData>();

                d->appDirPath = path;
            }

            AppDir::AppDir(const std::string& path) : AppDir(bf::path(path)) {}

            void AppDir::setExcludeLibraryPatterns(const std::vector<std::string> &excludeLibraryPatterns) {
                d->excludeLibraryPatterns = excludeLibraryPatterns;
            }

            bool AppDir::createBasicStructure() const {
                std::vector<std::string> dirPaths = {
                    "usr/bin/",
                    "usr/lib/",
                    "usr/share/applications/",
                    "usr/share/icons/hicolor/",
                };

                for (const std::string& resolution : {"16x16", "32x32", "64x64", "128x128", "256x256", "scalable"}) {
                    auto iconPath = "usr/share/icons/hicolor/" + resolution + "/apps/";
                    dirPaths.push_back(iconPath);
                }

                for (const auto& dirPath : dirPaths) {
                    auto fullDirPath = d->appDirPath / dirPath;

                    ldLog() << "Creating directory" << fullDirPath << std::endl;

                    // skip directory if it exists
                    if (bf::is_directory(fullDirPath))
                        continue;

                    try {
                        bf::create_directories(fullDirPath);
                    } catch (const bf::filesystem_error&) {
                        ldLog() << LD_ERROR << "Failed to create directory" << fullDirPath;
                        return false;
                    }
                }

                return true;
            }

            bool AppDir::deployLibrary(const bf::path& path, const bf::path& destination) {
                return d->deployLibrary(path, false, true, destination);
            }

            bool AppDir::forceDeployLibrary(const bf::path& path, const bf::path& destination) {
                return d->deployLibrary(path, true, true, destination);
            }

            bool AppDir::deployExecutable(const bf::path& path, const boost::filesystem::path& destination) {
                return d->deployExecutable(path, destination);
            }

            bool AppDir::deployDesktopFile(const DesktopFile& desktopFile) {
                return d->deployDesktopFile(desktopFile);
            }

            bool AppDir::deployIcon(const bf::path& path) {
                return d->deployIcon(path);
            }

            bool AppDir::deployIcon(const bf::path& path, const std::string& targetFilename) {
                return d->deployIcon(path, targetFilename);
            }

            bool AppDir::executeDeferredOperations() {
                return d->executeDeferredOperations();
            }

            boost::filesystem::path AppDir::path() const {
                return d->appDirPath;
            }

            static std::vector<bf::path> listFilesInDirectory(const bf::path& path, const bool recursive = true) {
                std::vector<bf::path> foundPaths;

                // directory_iterators throw exceptions if the directory doesn't exist
                if (!bf::is_directory(path)) {
                    ldLog() << LD_DEBUG << "No such directory:" << path << std::endl;
                    return {};
                }

                if (recursive) {
                    for (bf::recursive_directory_iterator i(path); i != bf::recursive_directory_iterator(); ++i) {
                        if (bf::is_regular_file(*i)) {
                            foundPaths.push_back((*i).path());
                        }
                    }
                } else {
                    for (bf::directory_iterator i(path); i != bf::directory_iterator(); ++i) {
                        if (bf::is_regular_file(*i)) {
                            foundPaths.push_back((*i).path());
                        }
                    }
                }

                return foundPaths;
            }

            std::vector<bf::path> AppDir::deployedIconPaths() const {
                auto icons = listFilesInDirectory(path() / "usr/share/icons/");
                auto pixmaps = listFilesInDirectory(path() / "usr/share/pixmaps/", false);
                icons.reserve(pixmaps.size());
                std::copy(pixmaps.begin(), pixmaps.end(), std::back_inserter(icons));
                return icons;
            }

            std::vector<bf::path> AppDir::deployedExecutablePaths() const {
                return listFilesInDirectory(path() / "usr/bin/", false);
            }

            std::vector<DesktopFile> AppDir::deployedDesktopFiles() const {
                std::vector<DesktopFile> desktopFiles;

                auto paths = listFilesInDirectory(path() / "usr/share/applications/", false);
                paths.erase(std::remove_if(paths.begin(), paths.end(), [](const bf::path& path) {
                    return path.extension() != ".desktop";
                }), paths.end());

                for (const auto& path : paths) {
                    desktopFiles.emplace_back(path.string());
                }

                return desktopFiles;
            }

            bool AppDir::setUpAppDirRoot(const DesktopFile& desktopFile, boost::filesystem::path customAppRunPath) {
                AppDirRootSetup setup(*this);
                return setup.run(desktopFile, customAppRunPath);
            }

            bf::path AppDir::deployFile(const boost::filesystem::path& from, const boost::filesystem::path& to) {
                return d->deployFile(from, to, DEFAULT_PERMS, true);
            }

            bool AppDir::copyFile(const bf::path& from, const bf::path& to, bool overwrite) const {
                return d->copyFile(from, to, DEFAULT_PERMS, overwrite);
            }

            bool AppDir::createRelativeSymlink(const bf::path& target, const bf::path& symlink) const {
                return d->symlinkFile(target, symlink, true);
            }

            std::vector<bf::path> AppDir::listExecutables() const {
                std::vector<bf::path> executables;

                for (const auto& file : listFilesInDirectory(path() / "usr" / "bin", false)) {
                    // make sure it's an ELF file
                    try {
                        elf_file::ElfFile elfFile(file);
                    } catch (const elf_file::ElfFileParseError&) {
                        // FIXME: remove this workaround once the MIME check below works as intended
                        continue;
                    }

                    executables.push_back(file);
                }

                return executables;
            }

            std::vector<bf::path> AppDir::listSharedLibraries() const {
                std::vector<bf::path> sharedLibraries;

                for (const auto& file : listFilesInDirectory(path() / "usr" / "lib", true)) {
                    // exclude debug symbols
                    if (d->isInDebugSymbolsLocation(file))
                        continue;

                    // make sure it's an ELF file
                    try {
                        elf_file::ElfFile elfFile(file);
                    } catch (const elf_file::ElfFileParseError&) {
                        // FIXME: remove this workaround once the MIME check below works as intended
                        continue;
                    }

                    sharedLibraries.push_back(file);
                }

                return sharedLibraries;
            }

            bool AppDir::deployDependenciesForExistingFiles() const {
                for (const auto& executable : listExecutables()) {
                    if (bf::is_symlink(executable))
                        continue;

                    if (!d->deployElfDependencies(executable))
                        return false;

                    std::string rpath = "$ORIGIN/../" + PrivateData::getLibraryDirName(executable);

                    d->setElfRPathOperations[executable] = rpath;
                }

                for (const auto& sharedLibrary : listSharedLibraries()) {
                    if (bf::is_symlink(sharedLibrary))
                        continue;

                    if (!d->deployElfDependencies(sharedLibrary))
                        return false;

                    const auto rpath = elf_file::ElfFile(sharedLibrary).getRPath();
                    auto rpathList = util::split(rpath, ':');
                    if (std::find(rpathList.begin(), rpathList.end(), "$ORIGIN") == rpathList.end()) {
                        rpathList.push_back("$ORIGIN");
                        d->setElfRPathOperations[sharedLibrary] = util::join(rpathList, ":");
                    } else {
                        d->setElfRPathOperations[sharedLibrary] = rpath;
                    }
                }

                // used to bundle dependencies of executables or libraries in the AppDir without moving them
                // useful e.g., for plugin systems, etc.
                {
                    constexpr auto VAR_NAME = "ADDITIONAL_BIN_DIRS";

                    const auto additionalBinDirs = getenv(VAR_NAME);

                    if (additionalBinDirs != nullptr) {
                        ldLog() << LD_DEBUG << "Read value of" << VAR_NAME << LD_NO_SPACE << ":" << additionalBinDirs << std::endl;

                        auto additionalBinaryDirs = util::split(getenv(VAR_NAME));

                        for (const auto& additionalBinaryDir : additionalBinaryDirs) {
                            ldLog() << "Deploying additional executables in directory:" << additionalBinaryDir << std::endl;

                            if (!bf::is_directory(additionalBinaryDir)) {
                                ldLog() << LD_ERROR << "Could not find additional binary dir, skipping:" << additionalBinaryDir;
                            }

                            for (bf::directory_iterator it(additionalBinaryDir); it != bf::directory_iterator(); ++it) {
                                const auto entry = *it;
                                const auto& path = entry.path();

                                // can't bundle directories
                                if (!bf::is_regular_file(entry)) {
                                    ldLog() << LD_DEBUG << "Skipping non-file directory entry:" << entry.path() << std::endl;
                                    continue;
                                }

                                // make sure we have an ELF file
                                try {
                                    elf_file::ElfFile(entry.path().string());
                                } catch (const elf_file::ElfFileParseError& e) {
                                    ldLog() << LD_DEBUG << "Skipping non-ELF directory entry:" << entry.path() << std::endl;
                                }

                                ldLog() << "Deploying additional executable:" << entry.path().string() << std::endl;

                                // bundle dependencies
                                if (!d->deployElfDependencies(path))
                                    return false;

                                // set rpath correctly
                                const auto rpathDestination = this->path() / "usr/lib";

                                const auto rpath = PrivateData::calculateRelativeRPath(additionalBinaryDir, rpathDestination);
                                ldLog() << LD_DEBUG << "Calculated rpath:" << rpath << std::endl;

                                d->setElfRPathOperations[path] = rpath;
                            }
                        }
                    }
                }

                return true;
            }

            // TODO: quite similar to deployDependenciesForExistingFiles... maybe they should be merged or use each other
            bool AppDir::deployDependenciesOnlyForElfFile(const boost::filesystem::path& elfFilePath, bool failSilentForNonElfFile) {
                // preconditions: file must be an ELF one, and file must be contained in the AppDir
                const auto canonicalElfFilePath = bf::canonical(elfFilePath);

                // can't bundle directories
                if (!bf::is_regular_file(canonicalElfFilePath)) {
                    ldLog() << LD_DEBUG << "Skipping non-file directory entry:" << canonicalElfFilePath << std::endl;
                    return false;
                }

                // to do a proper prefix check, we need a proper absolute canonical path for the AppDir
                const auto canonicalAppDirPath = bf::canonical(this->path());
                ldLog() << LD_DEBUG << "absolute canonical AppDir path:" << canonicalAppDirPath << std::endl;

                // a fancy way to check STL strings for prefixes is to "ab"use rfind
                if (canonicalElfFilePath.string().rfind(canonicalAppDirPath.string()) != 0) {
                    ldLog() << LD_ERROR << "File" << canonicalElfFilePath << "is not contained in AppDir, its dependencies cannot be deployed into the AppDir" << std::endl;
                    return false;
                }

                // make sure we have an ELF file
                try {
                    elf_file::ElfFile(canonicalElfFilePath.string());
                } catch (const elf_file::ElfFileParseError& e) {
                    auto level = LD_ERROR;

                    if (failSilentForNonElfFile) {
                        level = LD_WARNING;
                    }

                    ldLog() << level << "Not an ELF file:" << canonicalElfFilePath << std::endl;

                    return failSilentForNonElfFile;
                }

                // relative path makes for a nicer and more consistent log
                ldLog() << "Deploying dependencies for ELF file in AppDir:" << elfFilePath << std::endl;

                // bundle dependencies
                if (!d->deployElfDependencies(canonicalElfFilePath))
                    return false;

                // set rpath correctly
                const auto rpathDestination = this->path() / "usr/lib";
                ldLog() << LD_DEBUG << "rpath destination:" << rpathDestination << std::endl;

                const auto rpath = PrivateData::calculateRelativeRPath(elfFilePath.parent_path(), rpathDestination);
                ldLog() << LD_DEBUG << "Calculated rpath:" << rpath << std::endl;

                d->setElfRPathOperations[canonicalElfFilePath] = rpath;

                return true;
            }

            void AppDir::setDisableCopyrightFilesDeployment(bool disable) {
                d->disableCopyrightFilesDeployment = disable;
            }
        }
    }
}
