## ⛔[DEPRECATED] Use this instead [MultiMapPackerAndUploader](https://github.com/Bradasparky/MultiMapPackerAndUploader)

# Multi-Map Packer & Uploader
Inspired by [map_batch_updater](https://github.com/ficool2/map_batch_updater) by ficool2

This program is a configurable CLI wrapper for bspzip.exe that can handle multiple maps in one go with the option of uploading them to the workshop if they already exist. 

This tool has only been tested on Team Fortress 2 maps, however it should work for all other Source 1 games as well. 

## Configuration Settings
**All keys must be specified unless (optional)**
* Within `settings`
  * `bspzip_path` - The absolute path to the bspzip.exe file
  * `bsp_output_path` - The location of bsps will be placed after operations
  * `force_map_compression` - Force all maps to be compressed
  * `upload_maps_to_workshop` - All maps with their workshop settings properly configured will go through the upload process
  * `verbose_logging` - If `true`, print and log extra information about assets to console
  * `extension_whitelist` - File extensions that aren't specified in this array will be ignored

* Within `maps`
  *  `name` - The name of the outputted map (`"name" : "example"` will output `example.bsp`)
  *  `enabled` - If `true`, operations will be performed on this map
  *  `compress` - If `true`, the map will be compressed. This option can be overridden by `force_map_compression` in `settings`
  *  `source_path` - The absolute path to the map which operations will be performed on
  *  (optional) `ignore_assets` - `false` By default, if `true`, ignores all assets in the `assets` and `shared_assets` arrays
     * This option can be used to solely upload maps to the workshop without packing assets
  *  (optional) `assets` - An array of absolute asset paths
     * You must include a pair of either `//` or `\\` to denote which files/folders you want to pack into the map
     * This example will pack the `materials` folder `C:/dir//materials`
     * This example will pack all files/folders within the `materials` folder `C:/dir/materials//`
     * This example will pack `asset.txt` into the map without a folder `C:/dir//asset.txt`
  *  (optional) `workshop`  - An object for configuring workshop upload settings
     * `id` - The map's ugc id on the workshop (can be found in the workshop page url)
     * `upload` - If `true`, this map will go through the upload process after all other operations are completed
     * `visibility` - Ranging from [0,3], 0 = Public, 1 = Friends Only, 2 = Private, 3 = Unlisted
     * (optional) `changelog` - Self-explanatory, you may use newlines, will upload a blank changelog by default

* Within `shared_assets`
  * Same rules apply here as with the `assets` array within a map
  * These are assets which will be packed into all maps unless the map's `ignore_assets` is set to `true`
  * This array must exist in the config, but including assets here is (optional)

## Usage
- Set up your `config.json` file using the settings above (There's an example inside)
- Make sure that `steam_api64.dll` and `steam_appid.txt` are both in the same folder as `multi_map_packer_and_uploader.exe`
- If you intend on uploading maps to the workshop, make sure `steam_appid.txt` contains the game app id that your maps were made for
  * The app id for Team Fortress 2 (440) is there by default
- Run `multi_map_packer_and_uploader.exe`
- You'll be asked to confirm that all assets which were found according to your asset paths are correct
- If `"upload_maps_to_workshop" : true`, you'll be asked to confirm the upload of all maps found from your workshop items
  * If maps with `"upload" : true` have an invalid workshop `id`, you'll be asked to confirm and continue the upload of maps that **_were_** found on the workshop

## Build Instructions
1. Download the latest [Steamworks SDK](https://partner.steamgames.com/downloads/list)
2. Inside the Steamworks SDK zip...<br>
  2a. At the path `sdk\public\steam` copy all files except for the `lib` folder to the location `include\steam` in your project<br>
  2b. At the path `sdk\redistributable_bin\win64` copy `steam_api64.lib` to the location `lib\steam` in your project<br>
  2c. At the path `sdk\redistributable_bin\win64` copy `steam_api64.dll` to the root directory of `multi_map_packer_and_uploader`
3. Download the latest [json.hpp](https://github.com/nlohmann/json/blob/develop/single_include/nlohmann/json.hpp) by nlohmann<br>
  3a. Place this file at the location `include\nlohmann` in your project
4. Open the `.sln` file in Visual Studio 2022 and build the project
