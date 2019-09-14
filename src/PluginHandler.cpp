/******************************************************************************
 *
 * Project:  OpenCPN
 *
 ***************************************************************************
 *   Copyright (C) 2019 Alec Leamas                                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 ***************************************************************************
 */
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <memory>
#include <ostream>
#include <regex>
#include <stdexcept>
#include <streambuf>
#include <sstream>
#include <unordered_map>

#include <wx/jsonreader.h>
#include <wx/string.h>

#include <archive.h>
#include <archive_entry.h>
typedef __LA_INT64_T la_int64_t;      //  "older" libarchive versions support

#include <expat.h>

#if defined(__MINGW32__) && defined(Yield)
#undef Yield                 // from win.h, conflicts with mingw headers
#endif

#include "Downloader.h"
#include "OCPNPlatform.h"
#include "PluginHandler.h"
#include "PluginPaths.h"
#include "pluginmanager.h"
#include "navutil.h"
#include "ocpn_utils.h"

#ifdef XML_LARGE_SIZE
#  if defined(XML_USE_MSC_EXTENSIONS) && _MSC_VER < 1400
#    define XML_FMT_INT_MOD "I64"
#  else
#    define XML_FMT_INT_MOD "ll"
#  endif
#else
#  define XML_FMT_INT_MOD "l"
#endif

#ifdef XML_UNICODE_WCHAR_T
# include <wchar.h>
# define XML_FMT_STR "ls"
#else
# define XML_FMT_STR "s"
#endif

#ifdef _WIN32
static std::string SEP("\\");
#else
static std::string SEP("/");
#endif

#ifndef F_OK   // windows: missing unistd.h.
#define F_OK 0
#endif


extern OCPNPlatform*  g_Platform;
extern PlugInManager* g_pi_manager;
extern wxString       g_winPluginDir;
extern MyConfig*      pConfig;
extern OCPNPlatform*  g_Platform;

struct parse_ctx {
    std::vector<PluginMetadata> plugins;
    std::unique_ptr<PluginMetadata> plugin;
    std::string buff;
};


/** split s on first occurrence of delim, or return s in first result. */
static std::vector<std::string> split(const std::string& s, const std::string& delim)
{
    std::vector<std::string> result;
    size_t pos = s.find(delim);
    if (pos == std::string::npos) {
        result.push_back(s);
        return result;
    }
    result.push_back(s.substr(0, pos));
    result.push_back(s.substr(pos + delim.length()));
    return result;
}


inline std::string basename(const std::string path)
{
    wxFileName wxFile(path);
    return wxFile.GetFullName().ToStdString();
}


static bool isRegularFile(const char* path)
{
    wxFileName wxFile(path);
    return wxFile.FileExists() && !wxFile.IsDir();
}


static void mkdir(const std::string path)
{
#if defined(_WIN32) && !defined(__MINGW32__)
    _mkdir(path.c_str());
#elif defined(__MINGW32__)
    mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}


/**
 * Return index in ArrayOfPlugins for plugin with given name,
 * or -1 if not found
 */
static ssize_t PlugInIxByName(const std::string name, ArrayOfPlugIns* plugins)
{
    for (unsigned i = 0; i < plugins->GetCount(); i += 1) {
        if (name == plugins->Item(i)->m_common_name.Lower().ToStdString()) {
            return i;
        }
    }
    return -1;
}


/** Return PluginContainer with given name or 0 if not found */
static PlugInContainer*
    PlugInByName(const std::string name, ArrayOfPlugIns* plugins)
{
    auto ix = PlugInIxByName(name, plugins);
    return ix == -1 ? 0 : plugins->Item(ix);
}



static void XMLCALL elementData(void* userData, const XML_Char* s, int len)
{
    parse_ctx* ctx = static_cast<parse_ctx*>(userData);
    ctx->buff.append(s, len);
}


static void XMLCALL
startElement(void* userData, const XML_Char* name, const XML_Char** atts)
{
    parse_ctx* ctx = static_cast<parse_ctx*>(userData);
    ctx->buff = "";
    if (strcmp(name, "plugin") == 0) {
        ctx->plugin = std::unique_ptr<PluginMetadata>(new PluginMetadata);
    }
}


static void XMLCALL endElement(void* userData, const XML_Char* name)
{
    parse_ctx* ctx = static_cast<parse_ctx*>(userData);
    std::string buff = ctx->buff;

    if (strcmp(name, "plugin") == 0) {
        ctx->plugins.push_back(*ctx->plugin);
    } else if (strcmp(name, "name") == 0) {
        ctx->plugin->name = ocpn::trim(buff);
    } else if (strcmp(name, "version") == 0) {
        ctx->plugin->version = ocpn::trim(buff);
    } else if (strcmp(name, "release") == 0) {
        ctx->plugin->release = ocpn::trim(buff);
    } else if (strcmp(name, "summary") == 0) {
        ctx->plugin->summary = ocpn::trim(buff);
    } else if (strcmp(name, "api_version") == 0) {
        ctx->plugin->api_version = ocpn::trim(buff);
    } else if (strcmp(name, "author") == 0) {
        ctx->plugin->author = ocpn::trim(buff);
    } else if (strcmp(name, "description") == 0) {
        ctx->plugin->description = ocpn::trim(buff);
    } else if (strcmp(name, "git-commit") == 0) {
        ctx->plugin->git_commit = ocpn::trim(buff);
    } else if (strcmp(name, "git-date") == 0) {
        ctx->plugin->git_date = ocpn::trim(buff);
    } else if (strcmp(name, "source") == 0) {
        ctx->plugin->source = ocpn::trim(buff);
    } else if (strcmp(name, "tarball-url") == 0) {
        ctx->plugin->tarball_url = ocpn::trim(buff);
    } else if (strcmp(name, "info-url") == 0) {
        ctx->plugin->info_url = ocpn::trim(buff);
    } else if (strcmp(name, "target") == 0) {
        ctx->plugin->target = ocpn::trim(buff);
    } else if (strcmp(name, "target-version") == 0) {
        ctx->plugin->target_version = ocpn::trim(buff);
    } else if (strcmp(name, "open-source") == 0) {
        ctx->plugin->openSource = ocpn::trim(buff) == "yes";
    }
}


static void readXml(std::string xml, parse_ctx& ctx)
{
    XML_Parser parser = XML_ParserCreate(NULL);

    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, elementData);

    if (XML_Parse(parser, xml.c_str(), xml.size(), true) == XML_STATUS_ERROR) {
        wxLogWarning("%" XML_FMT_STR " at line %" XML_FMT_INT_MOD "u\n",
                     XML_ErrorString(XML_GetErrorCode(parser)),
                     XML_GetCurrentLineNumber(parser));
    }
    XML_ParserFree(parser);
}


static std::string pluginsConfigDir()
{
    std::string pluginDataDir = g_Platform->GetPrivateDataDir().ToStdString();
    pluginDataDir += SEP + "plugins";
    if (!ocpn::exists(pluginDataDir)) {
        mkdir(pluginDataDir);
    }
    pluginDataDir += SEP + "install_data";
    if (!ocpn::exists(pluginDataDir)) {
        mkdir(pluginDataDir);
    }
    return pluginDataDir;
}


static std::string getMetadataPath()
{
    std::string path = g_Platform->GetPrivateDataDir().ToStdString();
    path += SEP;
    path += "ocpn-plugins.xml";
    if (ocpn::exists(path)) {
        return path;
    }
    path = g_Platform->GetSharedDataDir().ToStdString();
    path += SEP ;
    path += "ocpn-plugins.xml";
    return path;
}


static std::string fileListPath(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return pluginsConfigDir() + SEP + name + ".files";
}


static std::string dirListPath(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return pluginsConfigDir() + SEP + name + ".dirs";
}

static std::string versionPath(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return pluginsConfigDir() + SEP + name + ".version";
}

typedef std::unordered_map<std::string, std::string> pathmap_t;

/**
 * Platform-specific mapping of tarball paths to user-writable
 * installation directories.
 */
static pathmap_t getInstallPaths()
{
    using namespace std;

    pathmap_t pathmap;
    PluginPaths* paths = PluginPaths::getInstance();
    pathmap["bin"] =  paths->UserBindir();
    pathmap["lib"] =  paths->UserLibdir();
    pathmap["lib64"] = paths->UserLibdir();
    pathmap["share"] =  paths->UserDatadir();
    pathmap["unknown"] = paths->UserUnknownPrefixDir();
    return pathmap;
}


static void saveFilelist(std::string filelist, std::string name)
{
    using namespace std;
    string listpath = fileListPath(name);
    ofstream diskfiles(listpath);
    if (!diskfiles.is_open()) {
        wxLogWarning("Cannot create installed files list.");
        return;
    }
    diskfiles << filelist;
    diskfiles.close();
}


static void saveDirlist(std::string name)
{
    using namespace std;
    string path = dirListPath(name);
    ofstream dirs(path);
    if (!dirs.is_open()) {
        wxLogWarning("Cannot create installed files list.");
        return;
    }
    pathmap_t pathmap = getInstallPaths();
    unordered_map<string, string>::iterator it;
    for (it = pathmap.begin(); it != pathmap.end(); it++) {
        dirs << it->first << ": " << it->second << endl;
    }
    dirs.close();
}

static void saveVersion(const std::string& name, const std::string& version)
{
    using namespace std;
    string path = versionPath(name);
    ofstream stream(path);
    if (!stream.is_open()) {
        wxLogWarning("Cannot create version file.");
        return;
    }
    stream << version << endl;
    stream.close();
}


static int copy_data(struct archive* ar, struct archive* aw)
{
    int r;
    const void* buff;
    size_t size;
    la_int64_t offset;

    while (true) {
        r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) return (ARCHIVE_OK);
        if (r < ARCHIVE_OK) return (r);
        r = archive_write_data_block(aw, buff, size, offset);
        if (r < ARCHIVE_OK) {
            wxLogWarning("Error copying install data: %s",
                         archive_error_string(aw));
            return (r);
        }
    }
}


static void win_entry_set_install_path(struct archive_entry* entry,
                                       pathmap_t installPaths)
{
    using namespace std;

    string path = archive_entry_pathname(entry);

    // Check # components, drop the single top-level path
    int slashes = count(path.begin(), path.end(), '/');
    if (slashes < 1) {
        archive_entry_set_pathname(entry, "");
        return;
    }

    // Remove top-level directory part
    int slashpos = path.find_first_of('/', 1);
    string prefix = path.substr(0, slashpos);
    path = path.substr(prefix.size() + 1);

    // Map remaining path to installation directory
    if (ocpn::endswith(path, ".dll") || ocpn::endswith(path, ".exe")) {
        path = installPaths["bin"] + "\\" + basename(path);
    } else if (ocpn::startswith(path, "share")) {
        path = installPaths["share"] + "\\" + path;
    } else if (ocpn::startswith(path, "plugins")) {
        path = installPaths["share"] + "\\" + path;
    } else if (archive_entry_filetype(entry) == AE_IFREG) {
        path = installPaths["unknown"] + "\\" + path;
    }
    wxString s(path);
    s.Replace("/", "\\");      // std::regex_replace FTBS on gcc 4.8.4
    s.Replace("\\\\", "\\");
    archive_entry_set_pathname(entry, s.c_str());
}


static void flatpak_entry_set_install_path(struct archive_entry* entry,
                                           pathmap_t installPaths)
{
    using namespace std;

    string path = archive_entry_pathname(entry);
    int slashes = count(path.begin(), path.end(), '/');
    if (slashes < 2) {
        archive_entry_set_pathname(entry, "");
        return;
    }
    int slashpos = path.find_first_of('/', 1);
    string prefix = path.substr(0, slashpos);
    path = path.substr(prefix.size() + 1);
    slashpos = path.find_first_of('/');
    string location = path.substr(0, slashpos);
    string suffix = path.substr(slashpos + 1);
    if (installPaths.find(location) == installPaths.end()
        && archive_entry_filetype(entry) == AE_IFREG
    ) {
        location = "unknown";
    }
    string dest = installPaths[location] + "/" + suffix;
    archive_entry_set_pathname(entry, dest.c_str());
}



static void linux_entry_set_install_path(struct archive_entry* entry,
                                         pathmap_t installPaths)
{
    using namespace std;

    string path = archive_entry_pathname(entry);
    int slashes = count(path.begin(), path.end(), '/');
    if (slashes < 3) {
        archive_entry_set_pathname(entry, "");
        return;
    }
    int slashpos = path.find_first_of('/', 1);
    string prefix = path.substr(0, slashpos);
    path = path.substr(prefix.size() + 1);
    if (ocpn::startswith(path, "usr/")) {
        path = path.substr(strlen("usr/"));
    }
    if (ocpn::startswith(path, "local/")) {
        path = path.substr(strlen("local/"));
    }
    slashpos = path.find_first_of('/');
    string location = path.substr(0, slashpos);
    string suffix = path.substr(slashpos + 1);
    if (installPaths.find(location) == installPaths.end()
        && archive_entry_filetype(entry) == AE_IFREG
    ){
        location = "unknown";
    }
    string dest = installPaths[location] + "/" + suffix;
    archive_entry_set_pathname(entry, dest.c_str());
}


static void apple_entry_set_install_path(struct archive_entry* entry,
                                         pathmap_t installPaths)
{
    using namespace std;

    const string base = PluginPaths::getInstance()->Homedir()
        + "/Library/Application Support/OpenCPN";

    const string path = archive_entry_pathname(entry);
    string dest("");
    size_t slashes = count(path.begin(), path.end(), '/');
    if (slashes < 3) {
        archive_entry_set_pathname(entry, "");
        return;
    }
    auto parts = split(path, "Contents/Resources");
    if (parts.size() >= 2) {
        dest = base  + "/Contents/Resources" + parts[1];
    }
    if (dest == "") {
        parts = split(path, "Contents/SharedSupport");
        if (parts.size() >= 2) {
            dest = base + "/Contents/SharedSupport"  + parts[1];
        }
    }
    if (dest == "") {
        parts = split(path, "Contents/PlugIns");
        if (parts.size() >= 2) {
            dest = base +  "/Contents/PlugIns" + parts[1];
        }
    }
    if (dest == "" && archive_entry_filetype(entry) == AE_IFREG) {
        // Drop uninstalled directories.
        dest = installPaths["unknown"] + "/" + path;
    }
    archive_entry_set_pathname(entry, dest.c_str());
}



static void entry_set_install_path(struct archive_entry* entry,
                                   pathmap_t installPaths)
{
    const auto osSystemId = wxPlatformInfo::Get().GetOperatingSystemId();
    const std::string src = archive_entry_pathname(entry);
    if (g_Platform->isFlatpacked()) {
        flatpak_entry_set_install_path(entry, installPaths);
    }
    else if (osSystemId & wxOS_UNIX_LINUX) {
        linux_entry_set_install_path(entry, installPaths);
    }
    else if (osSystemId & wxOS_WINDOWS) {
        win_entry_set_install_path(entry, installPaths);
    }
    else if (osSystemId & wxOS_MAC) {
        apple_entry_set_install_path(entry, installPaths);
    }
    else {
        wxLogMessage("set_install_path() invoked, unsupported platform %s",
                     wxPlatformInfo::Get().GetOperatingSystemDescription());
    }
    const std::string dest = archive_entry_pathname(entry);
    std::cout << "Installing " << src << " into " << dest << std::endl;
}


static bool archive_check(int r, const char* msg, struct archive* a)
{
    if (r < ARCHIVE_OK) {
        wxLogMessage(msg, archive_error_string(a));
    }
    return r >= ARCHIVE_WARN;
}


static bool explodeTarball(struct archive* src,
                           struct archive* dest,
                           std::string& filelist)
{
    struct archive_entry* entry = 0;
    pathmap_t pathmap = getInstallPaths();
    while (true) {
        int r = archive_read_next_header(src, &entry);
        if (r == ARCHIVE_EOF) {
            return true;
        }
        if (!archive_check(r, "Error reading install archive: %s", src)) {
            return false;
        }
        entry_set_install_path(entry, pathmap);
        if (strlen(archive_entry_pathname(entry)) == 0) {
            continue;
        }
        filelist.append(std::string(archive_entry_pathname(entry)) + "\n");
        r = archive_write_header(dest, entry);
        archive_check(r, "Error writing install header: %s", dest);
        if (r >= ARCHIVE_OK && archive_entry_size(entry) > 0) {
            r = copy_data(src, dest);
            if (!archive_check(r, "Error writing install archive: %s", dest)) {
                return false;
            }
        }
        r = archive_write_finish_entry(dest);
        if (!archive_check(r, "Error writing on finish %s", dest)) {
            return false;
        }
    }
    return false; // notreached
}


/*
 * Extract tarball into platform-specific user directories.
 *
 * The installed tarball has paths like topdir/dest/suffix_path... e. g.
 * oesenc_pi_ubuntu_10_64/usr/local/share/opencpn/plugins/oesenc_pi/README.
 * In this path, the topdir part must exist but is discarded. Next parts
 * being being standard prefixes like /usr/local or /usr are also
 * discarded. The remaining path (here share) is mapped to a user
 * directory. On linux, it ends up in ~/.local/share. The suffix
 * part is then installed as-is into this directory.
 *
 * Windows tarballs has dll and binary files in the top directory. They
 * go to winInstallDir/Program Files. Message catalogs exists under a
 * share/ toplevel directory, they go in winInstallDir/share. The
 * plugin data is installed under winInstallDir/plugins/<plugin name>,
 * and must be looked up by the plugins using GetPluginDataDir(plugin);
 * Windows requires that PATH is set to include the binary dir and tha
 * a bindtextdomain call is invoked to define the message catalog paths.
 *
 * For linux, the expected destinations are bin, lib and share.
 *
 * Parameters:
 *   - src: Readable libarchive source instance.
 *   - dest: Writable libarchive disk-writer instance.
 *   - filelist: On exit, list of installed files.
 *
 */
static bool extractTarball(const std::string path, std::string& filelist)
{
    struct archive* src = archive_read_new();
    archive_read_support_filter_gzip(src);
    archive_read_support_format_tar(src);
    int r = archive_read_open_filename(src, path.c_str(), 10240);
    if (r != ARCHIVE_OK) {
        wxLogWarning("Cannot read installation tarball: %s", path);
        return false;
    }
    struct archive* dest = archive_write_disk_new();
    archive_write_disk_set_options(dest, ARCHIVE_EXTRACT_TIME);
    bool ok = explodeTarball(src, dest, filelist);
    archive_read_free(src);
    archive_write_free(dest);
    return ok;
}


static bool
pluginByIndex(const char* pluginIx, PluginMetadata& plugin, bool useAvailable)
{
    using namespace std;

    unsigned ix;
    stringstream iss(pluginIx);
    iss >> ix;
    if (iss.fail()) {
        wxLogWarning("Illegal numeric input");
        return false;
    }
    auto handler = PluginHandler::getInstance();
    std::vector<PluginMetadata> plugins;
    if  (useAvailable) {
        plugins = PluginHandler::getInstance()->getAvailable();
    }
    else {
        plugins = PluginHandler::getInstance()->getInstalled();
    }
    if (ix >= plugins.size()) {
        wxLogWarning("Numeric value out of range");
        return false;
    }
    plugin = plugins[ix];
    plugin.ix = ix;
    return true;
}


/** Given index, try to return plugin from list of available. */
static bool availableByIndex(const char* pluginIx, PluginMetadata& plugin)
{
    return pluginByIndex(pluginIx, plugin, true);
}


/** Given index, try to return plugin from list of installed. */
static bool installedByIndex(const char* pluginIx, PluginMetadata& plugin)
{
    return pluginByIndex(pluginIx, plugin, false);
}


static void listAvailablePlugins() {
    using namespace std;

    auto plugins = PluginHandler::getInstance()->getAvailable();
    int i = 0;
    for (PluginMetadata p: plugins) {
        cout << setw(4) <<left << i
            << setw(24) << left << p.name << setw(12) << p.version
            << setw(12) << p.target << p.target_version << endl;
        i += 1;
    }
}


static void listInstalledPlugins()
{
    using namespace std;

    auto plugins = PluginHandler::getInstance()->getInstalled();
    int ix = 0;
    for (PluginMetadata p: plugins) {
        cout << setw(4) << left << ix
            << setw(3) << (p.readonly ? "ro" : "rw")
            << setw(24) << p.name << p.version << endl;
        ix += 1;
    }
}


static bool install(const char* pluginIx)
{
    using namespace std;

    PluginMetadata plugin;
    if (!availableByIndex(pluginIx, plugin)) {
        return false;
    }
    return PluginHandler::getInstance()->install(plugin);
}


static bool uninstall(const char* pluginIx)
{
    using namespace std;

    PluginMetadata plugin;
    if (!installedByIndex(pluginIx, plugin)) {
        wxLogMessage("uninstall: Cannot lookup plugin with ix: %s", pluginIx);
        return false;
    }
    if (!g_pi_manager->UnLoadPlugIn(plugin.ix)) {
        wxLogMessage("uninstall: Cannot unload, plugin ix: %d", plugin.ix);
        return false;
    }
    bool ok = PluginHandler::getInstance()->uninstall(plugin.name);
    if (!ok) {
        wxLogMessage("uninstall: Cannot uninstall, plugin: %s",
                     plugin.name.c_str());
        return false;
    }
    return true;
}


std::string getLastErrorMsg()
{
    return "";
}



PluginHandler* PluginHandler::getInstance() {
    static PluginHandler* instance = 0;
    if (!instance) {
        instance = new(PluginHandler);
    }
    return instance;
}

bool PluginHandler::isPluginWritable(std::string name)
{

    if (isRegularFile(fileListPath(name).c_str())) {
        return true;
    }
    if (!g_pi_manager) {
        return false;
    }
    return PlugInIxByName(name, g_pi_manager->GetPlugInArray()) == -1;
}


std::string PluginHandler::getMetadataPath()
{
    if( metadataPath.size() > 0) {
        return metadataPath;
    }
    std::string path = g_Platform->GetPrivateDataDir().ToStdString();
    path += SEP;
    path += "ocpn-plugins.xml";
    if (ocpn::exists(path)) {
        metadataPath = path;
        return path;
    }
    metadataPath = g_Platform->GetSharedDataDir().ToStdString();
    metadataPath += SEP ;
    metadataPath += "ocpn-plugins.xml";
    if (!ocpn::exists(metadataPath)) {
        wxLogWarning("Non-existing plugins file: %s", metadataPath);
    }
    return metadataPath;
}



const std::vector<PluginMetadata> PluginHandler::getAvailable()
{
    using namespace std;

    string path = getMetadataPath();
    wxLogMessage("PluginHandler: using metadata path: %s", path);
    parse_ctx ctx;
    if (!ocpn::exists(path)) {
        wxLogWarning("Non-existing plugins metadata file: %s", path.c_str());
        return ctx.plugins;
    }
    ifstream ifpath(path);
    string xml((istreambuf_iterator<char>(ifpath)),
            istreambuf_iterator<char>());
    readXml(xml, ctx);
    return ctx.plugins;
}


const std::vector<PluginMetadata> PluginHandler::getInstalled()
{
    using namespace std;
    vector<PluginMetadata> plugins;

    if (g_pi_manager) {
        ArrayOfPlugIns* mgr_plugins = g_pi_manager->GetPlugInArray();
        for (unsigned int i = 0; i < mgr_plugins->GetCount(); i += 1) {
            PlugInContainer* p = mgr_plugins->Item(i);
            PluginMetadata plugin;
            auto name = string(p->m_common_name);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            plugin.name = name;
            std::stringstream ss;
            ss << p->m_version_major << "." << p->m_version_minor;
            plugin.version = ss.str();
            plugin.readonly = !isPluginWritable(plugin.name);
            string path = versionPath(plugin.name);
            if (path != "" && wxFileName::IsFileReadable(path)) {
                std::ifstream stream;
                stream.open(path, ifstream::in);
                stream >> plugin.version;
            }
            plugins.push_back(plugin);
        }
    }
    return plugins;
}


bool PluginHandler::install(PluginMetadata plugin, std::string path)
{
    std::string filelist;
    if ( !extractTarball(path, filelist)) {
        std::cout << "Cannot unpack plugin: " << plugin.name  << " at "
            << path << std::endl;
        return false;
    }
    remove(path.c_str());
    saveFilelist(filelist, plugin.name);
    saveDirlist(plugin.name);
    saveVersion(plugin.name, plugin.version);

    int before = g_pi_manager->GetPlugInArray()->GetCount();
    g_pi_manager->LoadAllPlugIns(false);
    int after = g_pi_manager->GetPlugInArray()->GetCount();
    wxLogMessage("install: Reloading plugins, before: %d, after:  %d",
                 before, after);
    std::cout << "Installed: " << plugin.name << std::endl;

    return true;
}


bool PluginHandler::install(PluginMetadata plugin)
{
    std::string path;
    char fname[4096];

    if (tmpnam(fname) == NULL) {
        wxLogWarning("Cannot create temporary file");
        path = "";
        return false;
    }
    path = std::string(fname);
    std::ofstream stream;
    stream.open(path.c_str(), std::ios::out|std::ios::binary|std::ios::trunc);
    std::cout << "Downloading: " << plugin.name << std::endl;
    auto downloader = Downloader(plugin.tarball_url);
    downloader.download(&stream);
    stream.close();

    return install(plugin, path);
}


bool PluginHandler::uninstall(const std::string plugin_name)
{
    using namespace std;

    auto ix = PlugInIxByName(plugin_name, g_pi_manager->GetPlugInArray());
    auto pic = g_pi_manager->GetPlugInArray()->Item(ix);
    g_pi_manager->ClosePlugInPanel(pic, wxID_OK);
    g_pi_manager->UnLoadPlugIn(ix);
    string path = fileListPath(plugin_name);
    if (!ocpn::exists(path)) {
        wxLogWarning("Cannot find installation data for %s (%s)",
                     plugin_name.c_str(), path);
        return false;
    }
    ifstream files(path);
    while (!files.eof()) {
        char line[256];
        files.getline(line, sizeof(line));
        if (isRegularFile(line)) {
            int r = remove(line);
            if (r != 0) {
                wxLogWarning("Cannot remove file %s: %s", line, strerror(r));
            }
        }
    }
    files.close();
    int r = remove(path.c_str());
    if (r != 0) {
        wxLogWarning("Cannot remove file %s: %s", path.c_str(), strerror(r));
    }
    remove(dirListPath(plugin_name).c_str());  // A best effort try, failures
    remove(versionPath(plugin_name).c_str());  // are actually OK.

    return true;
}


PluginCommandHandler* PluginCommandHandler::getInstance() {
    static PluginCommandHandler* instance = 0;
    if (!instance) {
        instance = new(PluginCommandHandler);
    }
    return instance;
}


void show_paths()
{
    using namespace std;
    auto paths = PluginPaths::getInstance();

    cout << "libdir: " <<  paths->UserLibdir() << endl;
    cout << "bindir: " <<  paths->UserBindir() << endl;
    cout << "datadir: " <<  paths->UserDatadir() << endl;
    cout << "lib directories:" << endl;
    for (string p: paths->Libdirs()) {
        cout << "    " << p << endl;
    }

    cout << "bin directories:" << endl;
    for (string p: paths->Bindirs()) {
        cout << "    " << p << endl;
    }
    cout << "data directories:" << endl;
    for (string p: paths->Datadirs()) {
        cout << "    " << p << endl;
    }
    cout << "--> Using windows" << endl;
    cout << "libdir: " <<  paths->UserLibdir() << endl;
    cout << "bindir: " <<  paths->UserBindir() << endl;
    cout << "datadir: " <<  paths->UserDatadir() << endl;
    cout << "lib directories:" << endl;
    for (string p: paths->Libdirs()) {
        cout << "    " << p << endl;
    }

    cout << "bin directories:" << endl;
    for (string p: paths->Bindirs()) {
        cout << "    " << p << endl;
    }
    cout << "data directories:" << endl;
    for (string p: paths->Datadirs()) {
        cout << "    " << p << endl;
    }
 }



void PluginCommandHandler::setParser(wxCmdLineParser* parser)
{
    wxString plugin;
    wxString repo;
    wxString dir;

    if( parser->Found( _T("plugin_list_installed"))) {
        this->command = "plugin_list_installed";
    } else if (parser->Found( _T("plugin_list_available"))) {
        this->command = "plugin_list_available";
    } else if (parser->Found( _T("plugin_show_repo"))) {
        this->command = "plugin_show_repo";
    } else if (parser->Found( "plugin_install", &plugin )) {
        this->command = "plugin_install";
        this->option = plugin.ToStdString();
    } else if (parser->Found( _T("plugin_uninstall"), &plugin)) {
        this->command = "plugin_uninstall";
        this->option = plugin.ToStdString();
    } else if (parser->Found( _T("plugin_set_repo"), &repo)) {
        this->command = "plugin_set_repo";
        this->option = repo.ToStdString();
    } else if (parser->Found( _T("plugin_set_windir"), &dir)) {
        this->command = "plugin_set_windir";
        this->option = dir.ToStdString();
    } else if (parser->Found( _T("plugin_show_windir"))) {
        this->command = "plugin_show_windir";
    } else if (parser->Found( _T("plugin_show_paths"))) {
        this->command = "plugin_show_paths";
    }
}

void PluginCommandHandler::runParserCommands()
{
    using namespace std;

    if (command == "plugin_set_repo") {
        PluginHandler::getInstance()->setMetadata(option.c_str());
        exit(0);
    }
    if (command == "plugin_list_installed") {
        listInstalledPlugins();
        exit(0);
    }
    if (command == "plugin_list_available") {
        listAvailablePlugins();
        exit(0);
    }
    if (command == "plugin_install") {
        install(option.c_str());
        exit(0);
    }
    if (command == "plugin_uninstall") {
        uninstall(option.c_str());
        exit(0);
    }
    if (command == "plugin_show_repo") {
        cout << PluginHandler::getInstance()->getMetadataPath() << endl;
        exit(0);
    }
    if (command == "plugin_set_windir") {
        g_winPluginDir = option.c_str();
        pConfig->UpdateSettings();
    }
    if (command == "plugin_show_windir") {
        cout << "windir: " << g_Platform->GetWinPluginBaseDir() << endl;
    }
    if (command == "plugin_show_paths") {
        show_paths();
        exit(0);
    }
}
