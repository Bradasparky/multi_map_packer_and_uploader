#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <limits>
#include <ctime>

#include <stdio.h>
#include <direct.h>
#include <Windows.h>

#include "steam/steam_api.h"
#include "nlohmann/json.hpp"

using json = nlohmann::ordered_json;

struct BSPFileInfo
{
    bool upload = false;
    bool compress = false;
    bool ignore_assets = false;
    uint64 workshop_id = 0;
    ERemoteStoragePublishedFileVisibility visibility = k_ERemoteStoragePublishedFileVisibilityUnlisted;
    std::string name;
    std::string source_path;
    std::string output_path;
    std::string changelog;
    std::vector<std::string> assets;
    SteamUGCDetails_t details = SteamUGCDetails_t();
};
using BSPInfoList = std::vector<BSPFileInfo>;

HANDLE g_Console;
std::ofstream log_stream;

enum ConsoleColors
{
    DEFAULT = 7,
    AQUA = 11,
    RED = 12,
    YELLOW = 14,
    WHITE = 15
};

static void ConsolePrintf(ConsoleColors color, const char* format, ...)
{
    SetConsoleTextAttribute(g_Console, color);
    char buffer[1024];
    std::string text;
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    printf(buffer);
    va_end(args);
    SetConsoleTextAttribute(g_Console, DEFAULT);

    if (log_stream.is_open())
        log_stream << buffer;
}

static void ConsolePrintProgress(const ConsoleColors color, size_t processed, size_t total)
{
    ConsolePrintf(color, "Progress: %llu/%llu (%2.0f%%%%)                      \r", processed, total, total > 0 ? ((processed / (float)total) * 100.0) : 0.f);
}

template <typename T>
void SleepUntilCondition(T* obj, const std::invocable<T> auto func, const uint32 delay)
{
    while (true)
    {
        SteamAPI_RunCallbacks();
        if ((obj->*func)())
            break;

        Sleep(delay);
    }
}

static void ConsoleWaitForKey()
{
    std::cout << "Press enter to exit...";
    std::cin.ignore();
    std::cin.ignore();
}

static void FixSlashes(std::string& str)
{
    std::replace(str.begin(), str.end(), '\\', '/');
}

static bool ContainsExtension(const std::string& str, const std::string& ext)
{
    size_t element = str.find_last_of('.');
    std::string sub = str.substr(element, str.length() - 1);
    return !sub.empty() && !sub.compare(ext);
}

static bool IsIntegerInRange(const int& val, const int& min, const int& max)
{
    return val >= min && val <= max;
}

struct Config
{

public:

    bool ParseConfig(const std::string& config_name, BSPInfoList& bsplist)
    {
        // Load json file
        std::ifstream stream(config_name);
        if (stream.fail())
        {
            ConsolePrintf(RED, "Failed to open %s. Does the file exist?\n", config_name);
            return false;
        }

        ConsolePrintf(WHITE, "> Parsing Settings & Maps\n\n");

        json data = json::parse(stream, nullptr, false, true);
        if (data.is_discarded())
        {
            ConsolePrintf(RED, "Found syntax errors within config.json\n");
            return false;
        }
        
        bool bSuccess = true;
        if (!ParseSettings(data))
        {
            stream.close();
            return false;
        }

        if (!ParseMaps(data, bsplist))
        {
            stream.close();
            return false;
        }

        if (!ParseSharedAssets(data))
        {
            stream.close();
            return false;
        }

        stream.close();
        data.clear();

        ConsolePrintf(YELLOW, "- - - - - - - - - - < Settings > - - - - - - - - - -\n\n");
        ConsolePrintf(AQUA, "Found bspzip.exe @: \"%s\"\n", bspzip_path.c_str());
        ConsolePrintf(AQUA, "Outputting Maps @: \"%s\"\n", base_output_path.c_str());
        ConsolePrintf(AQUA, force_map_compression ? "Forced BSP Compression: Enabled\n" : "Forced BSP Compression: Disabled\n");
        ConsolePrintf(AQUA, upload_maps_to_workshop ? "Workshop Uploading: Enabled\n" : "Workshop Uploading: Disabled\n");
        printf("\n");

        ConsolePrintf(WHITE, "Enter \"y\" to confirm these settings. Enter anything else to abort: ");
        std::string input;
        std::cin >> input;
        if (!(!input.compare("y")))
        {
            ConsolePrintf(DEFAULT, "\n");
            return false;
        }

        // Create a temporary asset.txt file for bspzip to read from
        std::string temp_path(std::filesystem::temp_directory_path().string() + "multi_map_packer_and_uploader/");
        FixSlashes(temp_path);
        if (!std::filesystem::is_directory(temp_path) && !std::filesystem::create_directory(temp_path))
        {
            ConsolePrintf(RED, "Failed to create a temp directory at %s\n", temp_path.c_str());
            return false;
        }

        std::string temp_assets_file(temp_path + "assets.txt");
        std::ofstream asset_stream(temp_assets_file);
        if (asset_stream.fail())
        {
            ConsolePrintf(RED, "Failed to create a file at the file path %s\n", temp_path.c_str());
            return false;
        }

        // Copy maps to output directory
        ConsolePrintf(YELLOW, "\n - - - - - - - - - - Packing Maps - - - - - - - - - - \n\n");

        for (BSPFileInfo& info : bsplist)
        {
            if (info.ignore_assets)
            {
                ConsolePrintf(AQUA, "%s (Ignored Assets)                    \n\n", info.name.c_str());
                continue;
            }

            std::string temp_bsp(temp_path + info.name + ".bsp");
            if (!std::filesystem::copy_file(info.source_path, temp_bsp, std::filesystem::copy_options::overwrite_existing))
            {
                ConsolePrintf(RED, "Failed to write a temp bsp at %s", temp_bsp.c_str());
                asset_stream.close();
                return false;
            }
                

            asset_stream.clear();
            asset_stream.seekp(0);

            for (std::string asset : info.assets)
            {
                asset_stream << asset.c_str() << std::endl;
            }
            
            for (std::string asset : shared_assets)
            {
                asset_stream << asset.c_str() << std::endl;
            }

            std::string decompress("-repack \"" + temp_bsp + "\"");
            std::string pack("-addlist \"" + temp_bsp + "\" \"" + temp_assets_file + "\" \"" + info.output_path + "\"");
            std::string compress("-repack -compress \"" + info.output_path + "\"");
            FixSlashes(decompress);
            FixSlashes(pack);
            FixSlashes(compress);

            SHELLEXECUTEINFOA si = SHELLEXECUTEINFOA();
            si.cbSize = sizeof(SHELLEXECUTEINFOA);
            si.fMask = SEE_MASK_NOCLOSEPROCESS;
            si.lpVerb = "open";
            si.nShow = SW_HIDE;
            si.lpFile = bspzip_path.c_str();
            si.lpParameters = decompress.c_str();

            ConsolePrintf(YELLOW, "%s (Decompressing)...            \r", info.name.c_str());
            if (!ShellExecuteExA(&si))
            {
                asset_stream.close();
                ConsolePrintf(RED, "Failed to run command on bspzip.exe \"%s\"\n", decompress.c_str());
                return false;
            }

            if (WaitForSingleObjectEx(si.hProcess, INFINITE, true) != 0)
            {
                asset_stream.close();
                ConsolePrintf(RED, "Failed to wait for bspzip.exe to finish the decompress operation\n");
                return false;
            }

            if (info.assets.size())
            {
                ConsolePrintf(YELLOW, "%s (Packing)...                    \r", info.name.c_str());
                si.lpParameters = pack.c_str();
                if (!ShellExecuteExA(&si))
                {
                    asset_stream.close();
                    ConsolePrintf(RED, "Failed to run command on bspzip.exe \"%s\"\n", pack.c_str());
                    return false;
                }

                if (WaitForSingleObjectEx(si.hProcess, INFINITE, true) != 0)
                {
                    asset_stream.close();
                    ConsolePrintf(RED, "Failed to wait for bspzip.exe to finish the addlist operation\n");
                    return false;
                }
            }

            if (info.compress || force_map_compression)
            {
                ConsolePrintf(YELLOW, "%s (Compressing)...                    \r", info.name.c_str());
                si.lpParameters = compress.c_str();
                if (!ShellExecuteExA(&si))
                {
                    asset_stream.close();
                    ConsolePrintf(RED, "Failed to run command on bspzip.exe \"%s.\"\n", compress.c_str());
                    return false;
                }

                if (WaitForSingleObjectEx(si.hProcess, INFINITE, true) != 0)
                {
                    asset_stream.close();
                    ConsolePrintf(RED, "Failed to wait for bspzip.exe to finish the compress operation\n");
                    return false;
                }
            }

            ConsolePrintf(AQUA, "%s (Completed)                      \n\n", info.name.c_str());
        }

        asset_stream.close();

        if (std::filesystem::is_directory(temp_path))
            std::filesystem::remove_all(temp_path);

        return true;
    }
    
    std::string bspzip_path;
    std::string base_output_path;
    bool upload_maps_to_workshop = false;

private:

    bool ParseSettings(const json& data)
    {
        if (!data.contains("settings"))
        {
            ConsolePrintf(RED, "Failed to find the \"settings\" key\n");
            return false;
        }

        // Path to bspzip.exe
        const json settings = data["settings"];
        if (!settings.contains("bspzip_path"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"bspzip_path\"\n");
            return false;
        }
        
        if (!settings["bspzip_path"].is_string())
        {
            ConsolePrintf(RED, "The value of \"bspzip_path\" must have a string value\n");
            return false;
        }

        bspzip_path = settings["bspzip_path"].get<std::string>();
        if (bspzip_path.empty())
        {
            ConsolePrintf(RED, "The value of \"bspzip_path\" must not be an empty string\n");
            return false;
        }

        FixSlashes(bspzip_path);
        if (!bspzip_path.ends_with("bspzip.exe"))
        {
            ConsolePrintf(RED, "The value of \"bspzip_path\" must contain the absolute path ending in /bspzip.exe\n", bspzip_path.c_str());
            return false;
        }

        if (!std::filesystem::is_regular_file(bspzip_path))
        {
            ConsolePrintf(RED, "The value of \"bspzip_path\" doesn't have a valid path to bspzip.exe\n", bspzip_path.c_str());
            return false;
        }

        // Output path
        if (!settings.contains("bsp_output_path"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"bsp_output_path\"\n");
            return false;
        }

        if (!settings["bsp_output_path"].is_string())
        {
            ConsolePrintf(RED, "The the value of \"bsp_output_path\" must be a string\n");
            return false;
        }

        base_output_path = settings["bsp_output_path"].get<std::string>();
        FixSlashes(base_output_path);
        if (!std::filesystem::is_directory(base_output_path))
        {
            ConsolePrintf(RED, "The value of \"base_output_path\" is not a valid directory\n", bspzip_path.c_str());
            return false;
        }

        if (base_output_path.rfind('/') != base_output_path.size() - 1)
        {
            base_output_path += '/';
        }

        // Force map compression
        if (!settings.contains("force_map_compression"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"force_map_compression\"\n");
            return false;
        }

        if (!settings["force_map_compression"].is_boolean())
        {
            ConsolePrintf(RED, "The value of \"force_map_compression\" must be a boolean\n");
            return false;
        }

        force_map_compression = settings["force_map_compression"].get<bool>();

        // Upload maps to workshop
        if (!settings.contains("upload_maps_to_workshop"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"upload_maps_to_workshop\"\n");
            return false;
        }

        if (!settings["upload_maps_to_workshop"].is_boolean())
        {
            ConsolePrintf(RED, "The value of \"upload_maps_to_workshop\" must have a boolean value\n");
            return false;
        }

        upload_maps_to_workshop = settings["upload_maps_to_workshop"].get<bool>();

        // Verbose logging
        if (!settings.contains("verbose_logging"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"verbose_logging\"\n");
            return false;
        }

        if (!settings["verbose_logging"].is_boolean())
        {
            ConsolePrintf(RED, "The value of \"verbose_logging\" must be a boolean\n");
            return false;
        }

        verbose_logging = settings["verbose_logging"].get<bool>();

        // Extension Whitelist
        if (!settings.contains("extension_whitelist"))
        {
            ConsolePrintf(RED, "Failed to find settings key \"extension_whitelist\"");
            return false;
        }

        if (!settings["extension_whitelist"].is_array())
        {
            ConsolePrintf(RED, "The key \"extension_whitelist\" must be an array of strings\n");
            return false;
        }

        for (auto& [key, value] : settings["extension_whitelist"].items())
        {
            if (!value.is_string())
            {
                ConsolePrintf(RED, "The array values of \"extension_whitelist\" must only be strings\n");
                return false;
            }

            std::string ext = value.get<std::string>();
            if (ext.empty())
            {
                ConsolePrintf(RED, "Found an empty string while searching for whitelisted extensions\n");
                return false;
            }

            if (ext[0] != '.')
                ext = "." + ext;
            
            valid_exts[ext] = nullptr;
        }

        return true;
    }

    bool ParseMaps(const json& data, BSPInfoList& bsplist)
    {
        if (!data.contains("maps"))
        {
            ConsolePrintf(RED, "Failed to find the \"maps\" array\n");
            return false;
        }

        // Iterate through map entries
        const json maps = data["maps"];
        if (!maps.is_array())
        {
            ConsolePrintf(RED, "The key \"maps\" must be an array\n");
            return false;
        }

        for (auto& [map_key, map_obj] : maps.items())
        {
            if (!map_obj.is_structured())
            {
                ConsolePrintf(RED, "Found a non-object item in the \"maps\" array\n");
                return false;
            }

            // Map name
            const json map_entry = map_obj;
            if (!map_entry.contains("name"))
            {
                ConsolePrintf(RED, "Found an object in \"maps\" with a missing \"name\" key\n");
                return false;
            }

            if (!map_entry["name"].is_string())
            {
                ConsolePrintf(RED, "Found an object in \"maps\" where \"name\" isn't a string\n");
                return false;
            }
            
            std::string map_name(map_entry["name"].get<std::string>());
            if (map_name.empty())
            {
                ConsolePrintf(RED, "Found an object in \"maps\" with an empty \"name\" value\n");
                return false;
            }
            
            BSPFileInfo info;
            info.name = map_name;
            info.output_path = base_output_path + map_name + ".bsp";

            // Enabled
            if (!map_entry.contains("enabled"))
            {
                ConsolePrintf(RED, "%s : The \"enabled\" key is missing\n", map_name.c_str());
                return false;
            }

            if (!map_entry["enabled"].is_boolean())
            {
                ConsolePrintf(RED, "%s : The value of \"enabled\" must be a boolean value\n");
                return false;
            }

            if (!map_entry["enabled"].get<bool>())
            {
                continue;
            }

            // Source path
            if (!map_entry.contains("source_path"))
            {
                ConsolePrintf(RED, "%s : The \"source_path\" key is missing\n", map_name.c_str());
                return false;
            }

            if (!map_entry["source_path"].is_string())
            {
                ConsolePrintf(RED, "%s : The \"source_path\" key must have a string value\n", map_name.c_str());
                return false;
            }

            info.source_path = map_entry["source_path"].get<std::string>();
            if (info.source_path.empty())
            {
                ConsolePrintf(RED, "%s : The value of \"source_path\" must not be empty\n");
                continue;
            }
            
            FixSlashes(info.source_path);
            if (info.source_path.empty())
            {
                ConsolePrintf(RED, "%s : The \"source_path\" key must not be an empty string\n", map_name.c_str());
                return false;
            }

            if (!std::filesystem::is_regular_file(info.source_path) || !ContainsExtension(info.source_path, ".bsp"))
            {
                ConsolePrintf(RED, "%s : The \"source_path\" %s is not a valid bsp file\n", map_name.c_str(), info.source_path.c_str());
                return false;
            }

            size_t last_slash_pos = info.source_path.find_last_of('/') + 1;
            if (!base_output_path.compare(info.source_path.substr(0, last_slash_pos)))
            {
                ConsolePrintf(RED, "%s : The \"source_path\" is not allowed to match the output_path\n", map_name.c_str());
                return false;
            }

            // Compress
            if (map_entry.contains("compress"))
            {
                if (!map_entry["compress"].is_boolean())
                {
                    ConsolePrintf(RED, "%s : The \"compress\" key must have a boolean value\n", map_name.c_str());
                    return false;
                }

                info.compress = map_entry["compress"].get<bool>();
            }
            
            // Ignore assets
            if (map_entry.contains("ignore_assets"))
            {
                if (!map_entry["ignore_assets"].is_boolean())
                {
                    ConsolePrintf(RED, "%s : The \"ignore_assets\" key must have a boolean value\n", map_name.c_str());
                    return false;
                }

                info.ignore_assets = map_entry["ignore_assets"].get<bool>();
            }

            // Workshop
            bool contains_workshop = false;
            bool changelog_set = false;
            if (map_entry.contains("workshop"))
            {
                if (!map_entry["workshop"].is_structured())
                {
                    ConsolePrintf(RED, "%s : The value of \"workshop\" key must be an object\n", map_name.c_str());
                    return false;
                }
                
                // ID
                const json workshop = map_entry["workshop"];
                if (!workshop.contains("id"))
                {
                    ConsolePrintf(RED, "%s : The \"id\" key is missing within the \"workshop\" object\n", map_name.c_str());
                    return false;
                }
                
                if (!workshop["id"].is_number_unsigned())
                {
                    ConsolePrintf(RED, "%s : The \"id\" key within the \"workshop\" object must have an unsigned integer value\n", map_name.c_str());
                    return false;
                }

                info.workshop_id = workshop["id"].get<uint64>();

                // Upload
                if (!workshop.contains("upload"))
                {
                    ConsolePrintf(RED, "%s : The \"upload\" key is missing within the \"workshop\" object\n", map_name.c_str());
                    return false;
                }

                if (!workshop["upload"].is_boolean())
                {
                    ConsolePrintf(RED, "%s : The \"upload\" key within the \"workshop\" object must have a boolean value\n", map_name.c_str());
                    return false;
                }

                info.upload = workshop["upload"].get<bool>();

                // Visibility
                if (!workshop.contains("visibility"))
                {
                    ConsolePrintf(RED, "%s : The \"visibility\" key is missing within the \"workshop\" object\n", map_name.c_str());
                    return false;
                }

                if (!workshop["visibility"].is_number_unsigned() && IsIntegerInRange(workshop["visibility"], 0, k_ERemoteStoragePublishedFileVisibilityUnlisted))
                {
                    ConsolePrintf(RED, "%s : The \"visibility\" key within the \"workshop\" object must have an unsigned integer value [0, 3]\n", map_name.c_str());
                    ConsolePrintf(RED, "Public = 0, Friends Only = 1, Private = 2, Unlisted = 3\n");
                    return false;
                }

                info.visibility = workshop["visibility"].get<ERemoteStoragePublishedFileVisibility>();

                // Changelog
                if (workshop.contains("changelog"))
                {
                    if (!workshop["changelog"].is_string())
                    {
                        ConsolePrintf(RED, "%s : The value of \"changelog\" must have a string value\n", map_name.c_str());
                        return false;
                    }
                    
                    info.changelog = workshop["changelog"].get<std::string>();
                    changelog_set = true;
                }
                
                contains_workshop = true;
            }

            ConsolePrintf(YELLOW, " - - - - - - - - - - < %s > - - - - - - - - - -\n\n", map_name.c_str());
            ConsolePrintf(AQUA, "Basic Settings:\n");
            ConsolePrintf(AQUA, "- Source Path: \"%s\"\n", info.source_path.c_str());
            ConsolePrintf(AQUA, "- Compress: %s\n", info.compress ? "true" : "false");
            if (info.ignore_assets)
                ConsolePrintf(AQUA, "- Ignoring Assets\n");

            if (contains_workshop)
            {
                ConsolePrintf(AQUA, "\nWorkshop Settings:\n");
                ConsolePrintf(AQUA, "- ID: %llu\n", info.workshop_id);
                ConsolePrintf(AQUA, "- Upload: %s\n", info.upload ? "true" : "false");

                switch (info.visibility)
                {
                    case k_ERemoteStoragePublishedFileVisibilityPublic:
                    {
                        ConsolePrintf(AQUA, "- Visibility: Public\n");
                        break;
                    }
                    case k_ERemoteStoragePublishedFileVisibilityFriendsOnly:
                    {
                        ConsolePrintf(AQUA, "- Visibility: Friends Only\n");
                        break;
                    }
                    case k_ERemoteStoragePublishedFileVisibilityPrivate:
                    {
                        ConsolePrintf(AQUA, "- Visibility: Private\n");
                        break;
                    }
                    case k_ERemoteStoragePublishedFileVisibilityUnlisted:
                    {
                        ConsolePrintf(AQUA, "- Visibility: Unlisted\n");
                        break;
                    }
                    default: break;
                }

                if (changelog_set)
                    ConsolePrintf(AQUA, "- Changelog: \n%s\n\n", info.changelog.c_str());
            }

            if (!info.ignore_assets)
            {
                std::vector<std::string> asset_list;
                if (map_entry.contains("assets"))
                {
                    for (std::string asset : map_entry["assets"])
                    {
                        FixSlashes(asset);
                        std::string fixed_dir = asset;
                        size_t pos = fixed_dir.find("//");
                        while (pos != std::string::npos)
                        {
                            fixed_dir.replace(pos, 2, "/");
                            pos = fixed_dir.find("//", pos);
                        }

                        bool valid_file = std::filesystem::is_regular_file(fixed_dir);
                        bool valid_folder = std::filesystem::is_directory(fixed_dir);
                        if (!valid_file && !valid_folder)
                        {
                            ConsolePrintf(RED, "Invalid asset path: %s\nThe asset path is not a valid file/folder\n", fixed_dir.c_str(), map_name.c_str());
                            return false;
                        }

                        size_t first_slash = asset.find("//");
                        if (first_slash == std::string::npos)
                        {
                            ConsolePrintf(RED, "Invalid asset path: %s\n%s : The asset path must include a // or \\\\ that precedes a file/folder that you want to pack into the map\n", asset.c_str(), map_name.c_str());
                            return false;
                        }

                        size_t final_slashes = asset.rfind("//");
                        if (first_slash != final_slashes)
                        {
                            ConsolePrintf(RED, "Invalid asset path: %s\n%s : The asset path must not have more than two instances of // or \\\\\n", asset.c_str(), map_name.c_str());
                            return false;
                        }

                        // Erase one of the slashes to form a valid path
                        asset.erase(first_slash, 1);

                        if (valid_file)
                        {
                            // Wherever the double slash existed, write an internal bsp directory
                            std::string internal_path = asset.substr(first_slash + 1, asset.length() - 1);
                            asset_list.push_back(internal_path);
                            asset_list.push_back(asset);
                        }
                        else if (valid_folder)
                            ParseDirectory(asset, first_slash + 1, asset_list);
                        else
                        {
                            ConsolePrintf(RED, "%s : Invalid asset %s\n", map_name.c_str(), asset.c_str());
                            return false;
                        }
                    }
                }

                info.assets = asset_list;

                if (verbose_logging && asset_list.size())
                {
                    ConsolePrintf(AQUA, "\n - - - - - < Asset List > - - - - -\n\n");
                    uint64 count = 0;
                    for (std::string asset : asset_list)
                    {
                        if (count % 2 == 0)
                        {
                            ConsolePrintf(WHITE, "%s", asset.substr(asset.rfind('/') + 1, asset.length()).c_str());
                            ConsolePrintf(AQUA, " >> ");
                            ConsolePrintf(WHITE, "%s\n", asset.c_str());
                        }
                        ++count;
                    }

                    ConsolePrintf(AQUA, "\n%s - Asset Total: %llu\n\n", map_name.c_str(), asset_list.size() / 2);
                }
                else
                    ConsolePrintf(DEFAULT, "\n");
            }

            bsplist.push_back(info);
        }

        if (!bsplist.size())
        {
            ConsolePrintf(RED, "No map objects were found in the \"maps\" object\n");
            return false;
        }

        return true;
    }

    bool ParseSharedAssets(const json& data)
    {
        if (!data.contains("shared_assets"))
        {
            ConsolePrintf(RED, "Failed to find the \"shared_assets\" key\n");
            return false;
        }

        for (std::string asset : data["shared_assets"])
        {
            FixSlashes(asset);
            std::string fixed_dir = asset;
            size_t pos = fixed_dir.find("//");
            while (pos != std::string::npos)
            {
                fixed_dir.replace(pos, 2, "/");
                pos = fixed_dir.find("//", pos);
            }

            bool valid_file = std::filesystem::is_regular_file(fixed_dir);
            bool valid_folder = std::filesystem::is_directory(fixed_dir);
            if (!valid_file && !valid_folder)
            {
                ConsolePrintf(RED, "Invalid shared asset path: %s\nThe asset path is not a valid file/folder\n", fixed_dir.c_str());
                return false;
            }

            size_t first_slash = asset.find("//");
            if (first_slash == std::string::npos)
            {
                ConsolePrintf(RED, "Invalid shared asset path: %s\n%s : The asset path must include a // or \\\\ that precedes a file/folder that you want to pack into the map\n", asset.c_str());
                return false;
            }

            size_t final_slashes = asset.rfind("//");
            if (first_slash != final_slashes)
            {
                ConsolePrintf(RED, "Invalid shared asset path: %s\n%s : The asset path must not have more than two instances of // or \\\\\n", asset.c_str());
                return false;
            }

            // Erase one of the slashes to form a valid path
            asset.erase(first_slash, 1);

            if (valid_file)
            {
                // Write an internal directory in place of the double slash
                std::string internal_path = asset.substr(first_slash + 1, asset.length() - 1);
                shared_assets.push_back(internal_path);
                shared_assets.push_back(asset);
            }
            else if (valid_folder)
                ParseDirectory(asset, first_slash + 1, shared_assets);
            else
            {
                ConsolePrintf(RED, "Invalid shared asset %s\n", asset.c_str());
                return false;
            }
        }

        if (verbose_logging && shared_assets.size())
        {
            ConsolePrintf(YELLOW, " - - - - - - - - - - < Shared Asset List > - - - - - - - - - -\n\n");
            uint64 count = 0;
            for (std::string asset : shared_assets)
            {
                if (count % 2 == 0)
                {
                    ConsolePrintf(WHITE, "%s", asset.substr(asset.rfind('/') + 1, asset.length()).c_str());
                    ConsolePrintf(AQUA, " >> ");
                    ConsolePrintf(WHITE, "%s\n", asset.c_str());
                }
                ++count;
            }

            ConsolePrintf(AQUA, "\nShared Asset Total: %llu\n\n", shared_assets.size() / 2);
        }
        else
            ConsolePrintf(YELLOW, "Shared Asset Total: %llu\n\n", shared_assets.size() ? shared_assets.size() / 2 : 0);

        return true;
    }

    void ParseDirectory(const std::string& dir, const size_t double_slash_pos, std::vector<std::string>& asset_list)
    {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.is_regular_file())
            {
                std::string file = entry.path().string();
                FixSlashes(file);
                if (ContainsValidExtension(file))
                {
                    std::string internal_path = file.substr(double_slash_pos, file.length() - 1);
                    asset_list.push_back(internal_path);
                    asset_list.push_back(file);
                }
            }
            else if (entry.is_directory())
                ParseDirectory(entry.path().string(), double_slash_pos, asset_list);
        }
    }

    bool ContainsValidExtension(const std::string& source)
    {
        size_t element = source.find_last_of('.');
        if (element == -1)
            return false;

        std::string ext = source.substr(element);
        return valid_exts.contains(ext);
    }

    bool force_map_compression = false;
    bool verbose_logging = false;
    std::unordered_map<std::string, void*> valid_exts;
    std::vector<std::string> shared_assets;
};

class Steam
{

public:

    bool SteamInit()
    {
        ConsolePrintf(WHITE, "> Initializing Steam API...\n");

        if (!SteamAPI_Init())
        {
            ConsolePrintf(RED, "Failed to initialized Steam API\n");
            return false;
        }

        SteamUserHandle = SteamUser();
        SteamFriendsHandle = SteamFriends();
        SteamUGCHandle = SteamUGC();

        if (!SteamUserHandle->BLoggedOn())
        {
            SteamAPI_Shutdown();
            ConsolePrintf(RED, "Failed to connect Steam. Are you logged in?\n");
            return false;
        }

        UserSteamID = SteamUserHandle->GetSteamID();
        UserAccountID = UserSteamID.GetAccountID();

        return true;
    }

    bool FindUGCMaps(BSPInfoList& workshop_list)
    {
        ConsolePrintf(WHITE, "\n> Finding owned workshop maps...\n\n");
        EnumerateAll();
        SleepUntilCondition(this, &Steam::IsUGCQueryFinished, 100);
        
        if (UGCFiles.size() == 0)
        {
            ConsolePrintf(YELLOW, "No workshop maps were found\n");
            return false;
        }
        else
            ConsolePrintf(YELLOW, "Found %llu workshop maps!\n", UGCFiles.size());

        ConsolePrintf(YELLOW, "Verifying that maps marked to be uploaded exist on the workshop...\n\n");

        // Store all UGC ids in a map for fast checking between config and ugc ids
        std::unordered_map<PublishedFileId_t, SteamUGCDetails_t> UGCFileIds;
        for (SteamUGCDetails_t& details : UGCFiles)
            if (details.m_eFileType == k_EWorkshopFileTypeCommunity)
                UGCFileIds[details.m_nPublishedFileId] = details;

        // Make sure that all workshop ids match a ugc id
        BSPInfoList confirmed_list;
        for (BSPFileInfo& info : workshop_list)
        {
            if (UGCFileIds.contains(info.workshop_id))
            {
                info.details = UGCFileIds[info.workshop_id];
                ConsolePrintf(AQUA, "Found %s (%llu)\n", info.name.c_str(), info.workshop_id);
                confirmed_list.push_back(info);
            }
            else
                ConsolePrintf(RED, "Failed to find %s (%llu)\n", info.name.c_str(), info.workshop_id);
        }

        if (confirmed_list.size() != workshop_list.size())
        {
            if (!confirmed_list.size())
            {
                ConsolePrintf(YELLOW, "\nNo maps marked to be uploaded were found on the workshop\n");
                return false;
            }
            else
            {
                ConsolePrintf(YELLOW, "Would you still like to upload the ones that were found?\n");
                ConsolePrintf(WHITE, "Enter \"y\" to continue the upload process, enter anything else to abort: ");
                std::string input;
                std::cin >> input;
                if (!(!input.compare("y")))
                {
                    ConsolePrintf(DEFAULT, "\n");
                    return false;
                }
            }
        }

        workshop_list = confirmed_list;
        printf("\n");
        return true;
    }

    bool UploadUGCMaps(const BSPInfoList& workshop_list)
    {
        ConsolePrintf(WHITE, "> The next step will upload all maps found from the previous step to the workshop.\n");
        ConsolePrintf(WHITE, "Before proceeding, ensure you have reviewed the packed bsps in the output path using GCFScape or VPKEdit.\n");
        ConsolePrintf(WHITE, "Copy-paste any bsps from the output path to your TF2 maps folder and check in-game to ensure they work as expected.\n\n");

        ConsolePrintf(YELLOW, "NOTE:\n");
        ConsolePrintf(YELLOW, "TF2 won't launch if it's not currently open because Steam believes it's already running.\n");
        ConsolePrintf(YELLOW, "As a workaround, you can launch tf_win64.exe manually via adding it as a non-steam game,\n");
        ConsolePrintf(YELLOW, "or by creating a .bat file with the parameters tf_win64.exe -game tf -steam -insecure\n\n");

        ConsolePrintf(WHITE, "Enter \"y\" to start the upload process, enter anything else to abort.\n");
        std::string input;
        std::cin >> input;
        if (!(!input.compare("y")))
            return false;

        ConsolePrintf(WHITE, "\n> Uploading modified maps to the workshop...\n");
        UploadList = workshop_list;
        UploadAll();
        SleepUntilCondition(this, &Steam::IsUGCUploadFinished, 100);
        
        return !Error;
    }

private:

    bool IsUGCQueryFinished() const
    {
        return CallbackFinished;
    }

    void EnumerateAll()
    {
        CallbackFinished = false;
        UGCItemsPage = 1;
        Enumerate(UGCItemsPage);
    }

    void Enumerate(const uint32& page)
    {
        QueryHandle = SteamUGCHandle->CreateQueryUserUGCRequest(UserAccountID,
            k_EUserUGCList_Published,
            k_EUGCMatchingUGCType_Items,
            k_EUserUGCListSortOrder_CreationOrderDesc,
            AppID, AppID, page);

        if (QueryHandle == k_UGCQueryHandleInvalid)
        {
            ConsolePrintf(RED, "Failed to fetch Steam Workshop maps\n");
            CallbackFinished = Error = true;
            return;
        }

        SteamAPICall = SteamUGCHandle->SendQueryUGCRequest(QueryHandle);
        if (SteamAPICall == k_uAPICallInvalid)
        {
            ConsolePrintf(RED, "Failed to send Steam Workshop query\n");
            CallbackFinished = Error = true;
            return;
        }

        QueryCallback.Set(SteamAPICall, this, &Steam::CallbackQuery);
    }

    void CallbackQuery(SteamUGCQueryCompleted_t* result, bool error)
    {
        if (error || result->m_eResult != k_EResultOK)
        {
            ConsolePrintf(RED, "Failed to query Steam Workshop maps, result: %d\n", result->m_eResult);
            CallbackFinished = Error = true;
            SteamUGCHandle->ReleaseQueryUGCRequest(QueryHandle);
            return;
        }

        uint32 item_count = result->m_unNumResultsReturned;
        for (uint32_t i = 0; i < item_count; i++)
        {
            SteamUGCDetails_t details = {};
            SteamUGCHandle->GetQueryUGCResult(result->m_handle, i, &details);
            UGCFiles.push_back(details);
        }

        uint32 matching_results = result->m_unTotalMatchingResults;
        SteamUGCHandle->ReleaseQueryUGCRequest(QueryHandle);

        if (item_count == 0 || UGCFiles.size() >= matching_results)
            CallbackFinished = true;
        else
            Enumerate(++UGCItemsPage);
    }

    bool IsUGCUploadFinished()
    {
        if (CallbackFinished)
            return true;

        uint64 bytes_uploaded = 0;
        uint64 bytes_total = 0;
        if (SteamUGCHandle->GetItemUpdateProgress(UploadHandle, &bytes_uploaded, &bytes_total))
            ConsolePrintProgress(AQUA, bytes_uploaded, bytes_total);

        return false;
    }

    void UploadAll()
    {
        CallbackFinished = false;
        UploadHandle = k_UGCUpdateHandleInvalid;
        Uploaded = 0;
        Upload(UploadList[0].details.m_nPublishedFileId);
    }

    void Upload(const PublishedFileId_t& id)
    {
        ConsolePrintf(YELLOW, "Uploading %s (%llu)...\n", UploadList[Uploaded].name.c_str(), id);

        if (!std::filesystem::is_regular_file(UploadList[Uploaded].output_path))
        {
            ConsolePrintf(RED, "The file path %s is no longer valid. Was the output path deleted?\n", UploadList[Uploaded].output_path.c_str());
            CallbackFinished = Error = true;
            return;
        }

        UploadHandle = SteamUGCHandle->StartItemUpdate(AppID, id);
        if (UploadHandle == k_UGCUpdateHandleInvalid)
        {
            ConsolePrintf(RED, "Failed to begin update for %llu\n", id);
            CallbackFinished = Error = true;
            return;
        }

        if (!SteamUGCHandle->SetItemContent(UploadHandle, UploadList[Uploaded].output_path.c_str()))
        {
            ConsolePrintf(RED, "Failed to set map data for %llu (%s)\n", id, UploadList[Uploaded].output_path.c_str());
            CallbackFinished = Error = true;
            return;
        }

        if (!SteamUGCHandle->SetItemVisibility(UploadHandle, k_ERemoteStoragePublishedFileVisibilityUnlisted))
        {
            ConsolePrintf(RED, "Failed to set map visibility for %llu (%s)\n", id, UploadList[Uploaded].source_path.c_str());
            CallbackFinished = Error = true;
            return;
        }
        
        SteamAPICall = SteamUGCHandle->SubmitItemUpdate(UploadHandle, UploadList[Uploaded].changelog.c_str());
        if (SteamAPICall == k_uAPICallInvalid)
        {
            ConsolePrintf(RED, "Failed to send Steam Upload message\n");
            CallbackFinished = Error = true;
            return;
        }

        UploadCallback.Set(SteamAPICall, this, &Steam::CallbackUpload);
    }

    void CallbackUpload(SubmitItemUpdateResult_t* result, bool error)
    {
        if (result->m_bUserNeedsToAcceptWorkshopLegalAgreement)
        {
            ConsolePrintf(RED, "Failed to upload map. User needs to agree to the workshop legal agreement\n");
            CallbackFinished = Error = true;
            return;
        }

        if (error || result->m_eResult != k_EResultOK)
        {
            ConsolePrintf(RED, "Failed to upload map. Result: %d\n", result->m_eResult);
            CallbackFinished = Error = true;
            return;
        }

        ConsolePrintf(AQUA, "Successfully uploaded %s (%llu)!                                                   \n", 
            UploadList[Uploaded].name.c_str(), UploadList[Uploaded].workshop_id);

        if (++Uploaded >= UploadList.size())
            CallbackFinished = true;
        else
        {
            for (int i = 5; i; i--)
            {
                ConsolePrintf(WHITE, "Waiting a moment to avoid tripping spam filters (%d)...\r", i);
                Sleep(1000);
            }

            ConsolePrintf(WHITE, "                                                                                                  \r");
            Upload(UploadList[Uploaded].details.m_nPublishedFileId);
        }
    }

    const AppId_t AppID = 440;
    CSteamID UserSteamID;
    AccountID_t UserAccountID = 0;
    ISteamUser* SteamUserHandle = nullptr;
    ISteamFriends* SteamFriendsHandle = nullptr;
    ISteamUGC* SteamUGCHandle = nullptr;

    SteamAPICall_t SteamAPICall = 0;
    CCallResult<Steam, SteamUGCQueryCompleted_t> QueryCallback;
    CCallResult<Steam, SubmitItemUpdateResult_t> UploadCallback;
    std::vector<SteamUGCDetails_t> UGCFiles;
    
    UGCQueryHandle_t QueryHandle = 0;
    uint32_t UGCItemsPage = 0;

    UGCUpdateHandle_t UploadHandle = 0;
    size_t Uploaded = 0;

    BSPInfoList UploadList;
    bool CallbackFinished = false;
    bool Error = false;
};

int main()
{
    g_Console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_Console)
    {
        std::cout << "Failed to get console handle\n";
        return 0;
    }

    // Set up logging
    std::string logs_path = std::filesystem::current_path().string() + "\\logs\\";
    if (!std::filesystem::is_directory(logs_path) && !std::filesystem::create_directory(logs_path))
    {
        ConsolePrintf(RED, "Failed to create a the directory %s\n", logs_path);
        return 0;
    }
    
    time_t now = time(0);
    tm* localTime = localtime(&now);
    char timeString[80];
    std::strftime(timeString, sizeof(timeString), "%Y%m%d_%H%M%S.txt", localTime);
    log_stream.open(logs_path + timeString);
    if (log_stream.fail())
    {
        ConsolePrintf(RED, "Failed to create a log file %s\n", logs_path.c_str());
        return 0;
    }

    // Get settings and maps from the config
    Config config;
    BSPInfoList bsplist;
    if (!config.ParseConfig("config.json", bsplist))
    {
        ConsolePrintf(WHITE, "Exiting.\n");
        ConsoleWaitForKey();
        return 1;
    }

    ShellExecuteA(NULL, "open", config.base_output_path.c_str(), NULL, NULL, SW_SHOWDEFAULT);
    
    if (!config.upload_maps_to_workshop)
    {
        ConsoleWaitForKey();
        return 0;
    }
    
    // Start the upload process
    Steam* steam = new Steam();
    while (true)
    {
        if (!steam->SteamInit())
        {
            ConsolePrintf(RED, "Failed to initialize a connection to Steam\n");
            ConsolePrintf(WHITE, "Exiting.\n");
            return 0;
        }

        BSPInfoList workshop_list;
        for (BSPFileInfo& info : bsplist)
        {
            if (info.upload)
            {
                if (!info.workshop_id)
                {
                    ConsolePrintf(YELLOW, "%s : WARNING: %s's upload is set to true with a workshop id of 0. Discarding map from upload list.", info.name.c_str());
                    info.upload = false;
                    continue;
                }
                
                workshop_list.push_back(info);
            }
        }

        if (workshop_list.empty())
        {
            ConsolePrintf(WHITE, "No maps were found to be uploaded to the workshop\n");
            ConsolePrintf(WHITE, "Exiting.\n");
            break;
        }

        if (!steam->FindUGCMaps(workshop_list) || !steam->UploadUGCMaps(workshop_list))
        {
            ConsolePrintf(WHITE, "Exiting.\n");
            break;
        }

        break;
    }
    
    SteamAPI_Shutdown();
    delete steam;

	return 0;
}

