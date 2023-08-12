//
// Copyright(C) 2023 David St-Louis
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
// *Source file for interfacing with archipelago*
//

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#ifdef _MSC_VER
#include <direct.h>
#endif
#else
#include <sys/types.h>
#endif


#include "apdoom_def.h"
#include "apdoom2_def.h"
#include "apheretic_def.h"
#include "Archipelago.h"
#include <json/json.h>
#include <memory.h>
#include <chrono>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <set>


#if defined(_WIN32)
static wchar_t *ConvertMultiByteToWide(const char *str, UINT code_page)
{
    wchar_t *wstr = NULL;
    int wlen = 0;

    wlen = MultiByteToWideChar(code_page, 0, str, -1, NULL, 0);

    if (!wlen)
    {
        errno = EINVAL;
        printf("ConvertMultiByteToWide: Failed to convert path to wide encoding.\n");
        return NULL;
    }

    wstr = (wchar_t*)malloc(sizeof(wchar_t) * wlen);

    if (!wstr)
    {
        printf("ERROR: ConvertMultiByteToWide: Failed to allocate new string.");
        return NULL;
    }

    if (MultiByteToWideChar(code_page, 0, str, -1, wstr, wlen) == 0)
    {
        errno = EINVAL;
        printf("ConvertMultiByteToWide: Failed to convert path to wide encoding.\n");
        free(wstr);
        return NULL;
    }

    return wstr;
}

wchar_t *AP_ConvertUtf8ToWide(const char *str)
{
    return ConvertMultiByteToWide(str, CP_UTF8);
}
#endif



//
// Create a directory
//

void AP_MakeDirectory(const char *path)
{
#ifdef _WIN32
    wchar_t *wdir;

    wdir = AP_ConvertUtf8ToWide(path);

    if (!wdir)
    {
        return;
    }

    _wmkdir(wdir);

    free(wdir);
#else
    mkdir(path, 0755);
#endif
}


FILE *AP_fopen(const char *filename, const char *mode)
{
#ifdef _WIN32
    FILE *file;
    wchar_t *wname = NULL;
    wchar_t *wmode = NULL;

    wname = AP_ConvertUtf8ToWide(filename);

    if (!wname)
    {
        return NULL;
    }

    wmode = AP_ConvertUtf8ToWide(mode);

    if (!wmode)
    {
        free(wname);
        return NULL;
    }

    file = _wfopen(wname, wmode);

    free(wname);
    free(wmode);

    return file;
#else
    return fopen(filename, mode);
#endif
}


int AP_FileExists(const char *filename)
{
    FILE *fstream;

    fstream = AP_fopen(filename, "r");

    if (fstream != NULL)
    {
        fclose(fstream);
        return true;
    }
    else
    {
        // If we can't open because the file is a directory, the 
        // "file" exists at least!

        return errno == EISDIR;
    }
}


struct ap_level_index_t
{
	int ep;
	int map;
};

enum class ap_game_t
{
	doom,
	doom2,
	heretic
};


ap_state_t ap_state;
int ap_is_in_game = 0;
ap_game_t ap_game;
int ap_episode_count = -1;
int ap_map_count = -1;
int ap_weapon_count = -1;
int ap_ammo_count = -1;
int ap_powerup_count = -1;
int ap_inventory_count = -1;

static ap_settings_t ap_settings;
static AP_RoomInfo ap_room_info;
static std::vector<int64_t> ap_item_queue; // We queue when we're in the menu.
static bool ap_was_connected = false; // Got connected at least once. That means the state is valid
static std::set<int64_t> ap_progressive_locations;
static bool ap_initialized = false;
static std::vector<std::string> ap_cached_messages;
static std::string ap_save_dir_name;
static std::vector<ap_notification_icon_t> ap_notification_icons;


void f_itemclr();
void f_itemrecv(int64_t item_id, bool notify_player);
void f_locrecv(int64_t loc_id);
void f_locinfo(std::vector<AP_NetworkItem> loc_infos);
void f_difficulty(int);
void f_random_monsters(int);
void f_random_items(int);
void f_flip_levels(int);
void f_episode1(int);
void f_episode2(int);
void f_episode3(int);
void f_episode4(int);
void f_two_ways_keydoors(int);
void load_state();
void save_state();
void APSend(std::string msg);


ap_level_info_t* ap_get_level_info(int ep, int map)
{
	switch (ap_game)
	{
		case ap_game_t::doom: return &ap_doom_level_infos[ep - 1][map - 1];
		case ap_game_t::doom2: return &ap_doom2_level_infos[ep - 1][map - 1];
		case ap_game_t::heretic: return &ap_heretic_level_infos[ep - 1][map - 1];
	}
}


ap_level_state_t* ap_get_level_state(int ep, int map)
{
	return &ap_state.level_states[(ep - 1) * ap_map_count + (map - 1)];
}


static const std::map<int64_t, ap_item_t>& get_item_type_table()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return ap_doom_item_table;
		case ap_game_t::doom2: return ap_doom2_item_table;
		case ap_game_t::heretic: return ap_heretic_item_table;
	}
}


static const std::map<int /* ep */, std::map<int /* map */, std::map<int /* index */, int64_t /* loc id */>>>& get_location_table()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return ap_doom_location_table;
		case ap_game_t::doom2: return ap_doom2_location_table;
		case ap_game_t::heretic: return ap_heretic_location_table;
	}
}


std::string string_to_hex(const char* str)
{
    static const char hex_digits[] = "0123456789ABCDEF";

	std::string out;
	std::string in = str;

    out.reserve(in.length() * 2);
    for (unsigned char c : in)
    {
        out.push_back(hex_digits[c >> 4]);
        out.push_back(hex_digits[c & 15]);
    }

    return out;
}


static const int doom_max_ammos[] = {200, 50, 300, 50};
static const int doom2_max_ammos[] = {200, 50, 300, 50};
static const int heretic_max_ammos[] = {100, 50, 200, 200, 20, 150};


static unsigned long long hash_seed(const char *str)
{
    unsigned long long hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}


const int* get_max_ammos()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return doom_max_ammos;
		case ap_game_t::doom2: return doom2_max_ammos;
		case ap_game_t::heretic: return heretic_max_ammos;
	}
}


int apdoom_init(ap_settings_t* settings)
{
	memset(&ap_state, 0, sizeof(ap_state));

	if (strcmp(settings->game, "DOOM 1993") == 0)
	{
		ap_game = ap_game_t::doom;
		ap_episode_count = 4;
		ap_map_count = 9;
		ap_weapon_count = 9;
		ap_ammo_count = 4;
		ap_powerup_count = 6;
		ap_inventory_count = 0;
	}
	else if (strcmp(settings->game, "DOOM II") == 0)
	{
		ap_game = ap_game_t::doom2;
		ap_episode_count = 1;
		ap_map_count = 32;
		ap_weapon_count = 9;
		ap_ammo_count = 4;
		ap_powerup_count = 6;
		ap_inventory_count = 0;
	}
	else if (strcmp(settings->game, "Heretic") == 0)
	{
		ap_game = ap_game_t::heretic;
		ap_episode_count = 5;
		ap_map_count = 9;
		ap_weapon_count = 9;
		ap_ammo_count = 6;
		ap_powerup_count = 9;
		ap_inventory_count = 14;
	}
	else
	{
		printf("APDOOM: Invalid game: %s\n", settings->game);
		return 0;
	}

	ap_state.level_states = new ap_level_state_t[ap_episode_count * ap_map_count];
	ap_state.episodes = new int[ap_episode_count];
	ap_state.player_state.powers = new int[ap_powerup_count];
	ap_state.player_state.weapon_owned = new int[ap_weapon_count];
	ap_state.player_state.ammo = new int[ap_ammo_count];
	ap_state.player_state.max_ammo = new int[ap_ammo_count];
	ap_state.player_state.inventory = ap_inventory_count ? new ap_inventory_slot_t[ap_inventory_count] : nullptr;

	memset(ap_state.level_states, 0, sizeof(ap_level_state_t) * ap_episode_count * ap_map_count);
	memset(ap_state.episodes, 0, sizeof(int) * ap_episode_count);
	memset(ap_state.player_state.powers, 0, sizeof(int) * ap_powerup_count);
	memset(ap_state.player_state.weapon_owned, 0, sizeof(int) * ap_weapon_count);
	memset(ap_state.player_state.ammo, 0, sizeof(int) * ap_ammo_count);
	memset(ap_state.player_state.max_ammo, 0, sizeof(int) * ap_ammo_count);
	if (ap_inventory_count)
		memset(ap_state.player_state.inventory, 0, sizeof(ap_inventory_slot_t) * ap_inventory_count);

	ap_state.player_state.health = 100;
	ap_state.player_state.ready_weapon = 1;
	ap_state.player_state.weapon_owned[0] = 1; // Fist
	ap_state.player_state.weapon_owned[1] = 1; // Pistol
	ap_state.player_state.ammo[0] = 50; // Clip
	auto max_ammos = get_max_ammos();
	for (int i = 0; i < ap_ammo_count; ++i)
		ap_state.player_state.max_ammo[i] = max_ammos[i];
	for (int ep = 0; ep < ap_episode_count; ++ep)
		for (int map = 0; map < ap_map_count; ++map)
			for (int k = 0; k < AP_CHECK_MAX; ++k)
				ap_state.level_states[ep * ap_map_count + map].checks[k] = -1;

	ap_settings = *settings;

	AP_NetworkVersion version = {0, 4, 1};
	AP_SetClientVersion(&version);
    AP_Init(ap_settings.ip, ap_settings.game, ap_settings.player_name, ap_settings.passwd);
	AP_SetDeathLinkSupported(true);
	AP_SetItemClearCallback(f_itemclr);
	AP_SetItemRecvCallback(f_itemrecv);
	AP_SetLocationCheckedCallback(f_locrecv);
	AP_SetLocationInfoCallback(f_locinfo);
	AP_RegisterSlotDataIntCallback("difficulty", f_difficulty);
	AP_RegisterSlotDataIntCallback("random_monsters", f_random_monsters);
	AP_RegisterSlotDataIntCallback("random_pickups", f_random_items);
	AP_RegisterSlotDataIntCallback("flip_levels", f_flip_levels);
	AP_RegisterSlotDataIntCallback("episode1", f_episode1);
	AP_RegisterSlotDataIntCallback("episode2", f_episode2);
	AP_RegisterSlotDataIntCallback("episode3", f_episode3);
	AP_RegisterSlotDataIntCallback("episode4", f_episode4);
	AP_RegisterSlotDataIntCallback("two_ways_keydoors", f_two_ways_keydoors);
    AP_Start();

	// Block DOOM until connection succeeded or failed
	auto start_time = std::chrono::steady_clock::now();
	while (true)
	{
		bool should_break = false;
		switch (AP_GetConnectionStatus())
		{
			case AP_ConnectionStatus::Authenticated:
			{
				printf("APDOOM: Authenticated\n");
				AP_GetRoomInfo(&ap_room_info);

				ap_was_connected = true;
				ap_save_dir_name = "AP_" + ap_room_info.seed_name + "_" + string_to_hex(ap_settings.player_name);

				// Create a directory where saves will go for this AP seed.
				if (!AP_FileExists(ap_save_dir_name.c_str()))
					AP_MakeDirectory(ap_save_dir_name.c_str());

				load_state();
				should_break = true;
				break;
			}
			case AP_ConnectionStatus::ConnectionRefused:
				printf("APDOOM: Failed to connect, connection refused\n");
				return 0;
		}
		if (should_break) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10))
		{
			printf("APDOOM: Failed to connect, timeout\n");
			return 0;
		}
	}

	// If none episode is selected, select the first one.
	int ep_count = 0;
	for (int i = 0; i < ap_episode_count; ++i)
		if (ap_state.episodes[i])
			ep_count++;
	if (!ep_count)
		ap_state.episodes[0] = 1;

	// Randomly flip levels based on the seed
	if (ap_state.flip_levels == 1)
	{
		for (int ep = 0; ep < ap_episode_count; ++ep)
			for (int map = 0; map < ap_map_count; ++map)
				ap_state.level_states[ep * ap_map_count + map].flipped = 1;
	}
	else if (ap_state.flip_levels == 2)
	{
		auto ap_seed = apdoom_get_seed();
		unsigned long long seed = hash_seed(ap_seed);
		srand(seed);
		for (int ep = 0; ep < ap_episode_count; ++ep)
			for (int map = 0; map < ap_map_count; ++map)
				ap_state.level_states[ep * ap_map_count + map].flipped = rand() % 2;
	}

	// Scout locations to see which are progressive
	if (ap_progressive_locations.empty())
	{
		std::vector<int64_t> location_scouts;

		const auto& loc_table = get_location_table();
		for (const auto& kv1 : loc_table)
		{
			if (!ap_state.episodes[kv1.first - 1])
				continue;
			for (const auto& kv2 : kv1.second)
			{
				for (const auto& kv3 : kv2.second)
				{
					if (kv3.first == -1) continue;
					location_scouts.push_back(kv3.second);
				}
			}
		}

		AP_SendLocationScouts(location_scouts, 0);

		// Wait for location infos
		start_time = std::chrono::steady_clock::now();
		while (ap_progressive_locations.empty())
		{
			apdoom_update();
		
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(10))
			{
				printf("APDOOM: Failed to connect, timeout waiting for LocationScouts\n");
				return 0;
			}
		}
	}

	ap_initialized = true;
	return 1;
}


static bool is_loc_checked(int ep, int map, int index)
{
	auto level_state = ap_get_level_state(ep, map);
	for (int i = 0; i < level_state->check_count; ++i)
	{
		if (level_state->checks[i] == index) return true;
	}
	return false;
}


void apdoom_shutdown()
{
	if (ap_was_connected)
		save_state();
}


void apdoom_save_state()
{
	if (ap_was_connected)
		save_state();
}


static void json_get_int(const Json::Value& json, int& out_or_default)
{
	if (json.isInt())
		out_or_default = json.asInt();
}


static void json_get_bool_or(const Json::Value& json, int& out_or_default)
{
	if (json.isInt())
		out_or_default |= json.asInt();
}


void load_state()
{
	std::string filename = ap_save_dir_name + "/apstate.json";
	std::ifstream f(filename);
	if (!f.is_open())
		return; // Could be no state yet, that's fine
	Json::Value json;
	f >> json;
	f.close();

	// Player state
	json_get_int(json["player"]["health"], ap_state.player_state.health);
	json_get_int(json["player"]["armor_points"], ap_state.player_state.armor_points);
	json_get_int(json["player"]["armor_type"], ap_state.player_state.armor_type);
	json_get_int(json["player"]["backpack"], ap_state.player_state.backpack);
	json_get_int(json["player"]["ready_weapon"], ap_state.player_state.ready_weapon);
	json_get_int(json["player"]["kill_count"], ap_state.player_state.kill_count);
	json_get_int(json["player"]["item_count"], ap_state.player_state.item_count);
	json_get_int(json["player"]["secret_count"], ap_state.player_state.secret_count);
	for (int i = 0; i < ap_powerup_count; ++i)
		json_get_int(json["player"]["powers"][i], ap_state.player_state.powers[i]);
	for (int i = 0; i < ap_weapon_count; ++i)
		json_get_bool_or(json["player"]["weapon_owned"][i], ap_state.player_state.weapon_owned[i]);
	for (int i = 0; i < ap_ammo_count; ++i)
		json_get_int(json["player"]["ammo"][i], ap_state.player_state.ammo[i]);
	for (int i = 0; i < ap_inventory_count; ++i)
	{
		const auto& inventory_slot = json["player"]["inventory"][i];
		json_get_int(inventory_slot["type"], ap_state.player_state.inventory[i].type);
		json_get_int(inventory_slot["count"], ap_state.player_state.inventory[i].count);
	}
	
	if (ap_state.player_state.backpack)
	{
		auto max_ammos = get_max_ammos();
		for (int i = 0; i < ap_ammo_count; ++i)
			ap_state.player_state.max_ammo[i] = max_ammos[i] * (ap_state.player_state.backpack ? 2 : 1);
	}

	// Level states
	for (int i = 0; i < ap_episode_count; ++i)
	{
		for (int j = 0; j < ap_map_count; ++j)
		{
			auto level_state = ap_get_level_state(i + 1, j + 1);
			json_get_bool_or(json["episodes"][i][j]["completed"], level_state->completed);
			json_get_bool_or(json["episodes"][i][j]["keys0"], level_state->keys[0]);
			json_get_bool_or(json["episodes"][i][j]["keys1"], level_state->keys[1]);
			json_get_bool_or(json["episodes"][i][j]["keys2"], level_state->keys[2]);
			//json_get_bool_or(json["episodes"][i][j]["check_count"], level_state->check_count);
			json_get_bool_or(json["episodes"][i][j]["has_map"], level_state->has_map);
			json_get_bool_or(json["episodes"][i][j]["unlocked"], level_state->unlocked);
			json_get_bool_or(json["episodes"][i][j]["special"], level_state->special);

			//int k = 0;
			//for (const auto& json_check : json["episodes"][i][j]["checks"])
			//{
			//	json_get_bool_or(json_check, level_state->checks[k++]);
			//}
		}
	}

	// Item queue
	for (const auto& item_id_json : json["item_queue"])
	{
		ap_item_queue.push_back(item_id_json.asInt64());
	}

	json_get_int(json["ep"], ap_state.ep);
	for (int i = 0; i < ap_episode_count; ++i)
		json_get_int(json["enabled_episodes"][i++], ap_state.episodes[i]);
	json_get_int(json["map"], ap_state.map);

	for (const auto& prog_json : json["progressive_locations"])
	{
		ap_progressive_locations.insert(prog_json.asInt64());
	}
	
	json_get_bool_or(json["victory"], ap_state.victory);
}


Json::Value serialize_level(int ep, int map)
{
	auto level_state = ap_get_level_state(ep, map);

	Json::Value json_level;

	json_level["completed"] = level_state->completed;
	json_level["keys0"] = level_state->keys[0];
	json_level["keys1"] = level_state->keys[1];
	json_level["keys2"] = level_state->keys[2];
	json_level["check_count"] = level_state->check_count;
	json_level["has_map"] = level_state->has_map;
	json_level["unlocked"] = level_state->unlocked;
	json_level["special"] = level_state->special;

	Json::Value json_checks(Json::arrayValue);
	for (int k = 0; k < AP_CHECK_MAX; ++k)
	{
		if (level_state->checks[k] == -1)
			continue;
		json_checks.append(level_state->checks[k]);
	}
	json_level["checks"] = json_checks;

	return json_level;
}


std::vector<ap_level_index_t> get_level_indices()
{
	std::vector<ap_level_index_t> ret;

	for (int i = 0; i < ap_episode_count; ++i)
	{
		for (int j = 0; j < ap_map_count; ++j)
		{
			ret.push_back({i + 1, j + 1});
		}
	}

	return ret;
}


void save_state()
{
	std::string filename = ap_save_dir_name + "/apstate.json";
	std::ofstream f(filename);
	if (!f.is_open())
	{
		printf("Failed to save AP state.\n");
#if WIN32
		MessageBoxA(nullptr, "Failed to save player state. That's bad.", "Error", MB_OK);
#endif
		return; // Ok that's bad. we won't save player state
	}

	// Player state
	Json::Value json;
	Json::Value json_player;
	json_player["health"] = ap_state.player_state.health;
	json_player["armor_points"] = ap_state.player_state.armor_points;
	json_player["armor_type"] = ap_state.player_state.armor_type;
	json_player["backpack"] = ap_state.player_state.backpack;
	json_player["ready_weapon"] = ap_state.player_state.ready_weapon;
	json_player["kill_count"] = ap_state.player_state.kill_count;
	json_player["item_count"] = ap_state.player_state.item_count;
	json_player["secret_count"] = ap_state.player_state.secret_count;

	Json::Value json_powers(Json::arrayValue);
	for (int i = 0; i < ap_powerup_count; ++i)
		json_powers.append(ap_state.player_state.powers[i]);
	json_player["powers"] = json_powers;

	Json::Value json_weapon_owned(Json::arrayValue);
	for (int i = 0; i < ap_weapon_count; ++i)
		json_weapon_owned.append(ap_state.player_state.weapon_owned[i]);
	json_player["weapon_owned"] = json_weapon_owned;

	Json::Value json_ammo(Json::arrayValue);
	for (int i = 0; i < ap_ammo_count; ++i)
		json_ammo.append(ap_state.player_state.ammo[i]);
	json_player["ammo"] = json_ammo;

	Json::Value json_inventory(Json::arrayValue);
	for (int i = 0; i < ap_inventory_count; ++i)
	{
		if (ap_state.player_state.inventory[i].type == 9) // Don't include wings to player inventory, they are per level
			continue;
		Json::Value json_inventory_slot;
		json_inventory_slot["type"] = ap_state.player_state.inventory[i].type;
		json_inventory_slot["count"] = ap_state.player_state.inventory[i].count;
		json_inventory.append(json_inventory_slot);
	}
	json_player["inventory"] = json_inventory;

	json["player"] = json_player;

	// Level states
	Json::Value json_episodes(Json::arrayValue);
	for (int i = 0; i < ap_episode_count; ++i)
	{
		Json::Value json_levels(Json::arrayValue);
		for (int j = 0; j < ap_map_count; ++j)
		{
			json_levels.append(serialize_level(i + 1, j + 1));
		}
		json_episodes.append(json_levels);
	}
	json["episodes"] = json_episodes;

	// Item queue
	Json::Value json_item_queue(Json::arrayValue);
	for (auto item_id : ap_item_queue)
	{
		json_item_queue.append(item_id);
	}
	json["item_queue"] = json_item_queue;

	json["ep"] = ap_state.ep;
	for (int i = 0; i < ap_episode_count; ++i)
		json["enabled_episodes"][i++] = ap_state.episodes[i] ? true : false;
	json["map"] = ap_state.map;

	// Progression items (So we don't scout everytime we connect)
	for (auto loc_id : ap_progressive_locations)
	{
		json["progressive_locations"].append(loc_id);
	}

	json["victory"] = ap_state.victory;

	f << json;
}


void f_itemclr()
{
	// Not sure what use this would have here.
}


static const std::map<int, int> doom_keys_map = {{5, 0}, {40, 0}, {6, 1}, {39, 1}, {13, 2}, {38, 2}};
static const std::map<int, int> doom2_keys_map = {{5, 0}, {40, 0}, {6, 1}, {39, 1}, {13, 2}, {38, 2}};
static const std::map<int, int> heretic_keys_map = {{80, 0}, {73, 1}, {79, 2}};


const std::map<int, int>& get_keys_map()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return doom_keys_map;
		case ap_game_t::doom2: return doom2_keys_map;
		case ap_game_t::heretic: return heretic_keys_map;
	}
}


int get_map_doom_type()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return 2026;
		case ap_game_t::doom2: return 2026;
		case ap_game_t::heretic: return 35;
	}
}


static const std::map<int, int> doom_weapons_map = {{2001, 2}, {2002, 3}, {2003, 4}, {2004, 5}, {2006, 6}, {2005, 7}};
static const std::map<int, int> doom2_weapons_map = {{2001, 2}, {2002, 3}, {2003, 4}, {2004, 5}, {2006, 6}, {2005, 7}, {82, 1}};
static const std::map<int, int> heretic_weapons_map = {{2005, 7}, {2001, 2}, {53, 3}, {2003, 5}, {2002, 6}, {2004, 4}};


const std::map<int, int>& get_weapons_map()
{
	switch (ap_game)
	{
		case ap_game_t::doom: return doom_weapons_map;
		case ap_game_t::doom2: return doom2_weapons_map;
		case ap_game_t::heretic: return heretic_weapons_map;
	}
}


void f_itemrecv(int64_t item_id, bool notify_player)
{
	const auto& item_type_table = get_item_type_table();
	auto it = item_type_table.find(item_id);
	if (it == item_type_table.end())
		return; // Skip
	ap_item_t item = it->second;

	const char* notif_text = 0;

	auto level_state = ap_get_level_state(item.ep, item.map);

	// Key?
	const auto& keys_map = get_keys_map();
	auto key_it = keys_map.find(item.doom_type);
	if (key_it != keys_map.end())
	{
		level_state->keys[key_it->second] = 1;
		notif_text = ap_get_level_info(item.ep, item.map)->name;
	}

	// Map?
	if (item.doom_type == get_map_doom_type())
	{
		level_state->has_map = 1;
		notif_text = ap_get_level_info(item.ep, item.map)->name;
	}

	// Backpack?
	if (item.doom_type == 8)
	{
		ap_state.player_state.backpack = 1;
		auto max_ammos = get_max_ammos();
		for (int i = 0; i < ap_ammo_count; ++i)
			ap_state.player_state.max_ammo[i] = max_ammos[i] * 2;
	}

	// Weapon?
	const auto& weapons_map = get_weapons_map();
	auto weapon_it = weapons_map.find(item.doom_type);
	if (weapon_it != weapons_map.end())
		ap_state.player_state.weapon_owned[weapon_it->second] = 1;

	// Ignore inventory items, the game will add them up

	// Is it a level?
	if (item.doom_type == -1)
	{
		level_state->unlocked = 1;
		notif_text = ap_get_level_info(item.ep, item.map)->name;
	}

	// Level complete?
	if (item.doom_type == -2)
		level_state->completed = 1;


	if (!notify_player) return;

	if (!ap_is_in_game)
	{
		ap_item_queue.push_back(item_id);
		return;
	}

	// Give item to player
	ap_settings.give_item_callback(item.doom_type, item.ep, item.map);

	// Add notification icon
	auto sprite_it = ap_heretic_type_sprites.find(item.doom_type);
	if (sprite_it != ap_heretic_type_sprites.end())
	{
		ap_notification_icon_t notif;
		snprintf(notif.sprite, 9, "%s", sprite_it->second.c_str());
		notif.t = 0;
		notif.text[0] = '\0'; // For now
		if (notif_text)
		{
			sprintf(notif.text, "%s", notif_text);
		}
		notif.xf = AP_NOTIF_SIZE / 2 + AP_NOTIF_PADDING;
		notif.yf = -200.0f + AP_NOTIF_SIZE / 2;
		notif.state = AP_NOTIF_STATE_PENDING;
		notif.velx = 0.0f;
		notif.vely = 0.0f;
		notif.x = (int)notif.xf;
		notif.y = (int)notif.yf;
		ap_notification_icons.push_back(notif);
	}
}


bool find_location(int64_t loc_id, int &ep, int &map, int &index)
{
	ep = -1;
	map = -1;
	index = -1;

	const auto& loc_table = get_location_table();
	for (const auto& loc_map_table : loc_table)
	{
		for (const auto& loc_index_table : loc_map_table.second)
		{
			for (const auto& loc_index : loc_index_table.second)
			{
				if (loc_index.second == loc_id)
				{
					ep = loc_map_table.first;
					map = loc_index_table.first;
					index = loc_index.first;
					break;
				}
			}
			if (ep != -1) break;
		}
		if (ep != -1) break;
	}
	return (ep > 0);
}


void f_locrecv(int64_t loc_id)
{
	// Find where this location is
	int ep = -1;
	int map = -1;
	int index = -1;
	if (!find_location(loc_id, ep, map, index))
	{
		printf("APDOOM: In f_locrecv, loc id not found: %i\n", (int)loc_id);
		return; // Loc not found
	}

	// Make sure we didn't already check it
	if (is_loc_checked(ep, map, index)) return;
	if (index < 0) return;

	auto level_state = ap_get_level_state(ep, map);
	level_state->checks[level_state->check_count] = index;
	level_state->check_count++;
}


void f_locinfo(std::vector<AP_NetworkItem> loc_infos)
{
	for (const auto& loc_info : loc_infos)
	{
		if (loc_info.flags & 1)
			ap_progressive_locations.insert(loc_info.location);
	}
}


void f_difficulty(int difficulty)
{
	ap_state.difficulty = difficulty;
}


void f_random_monsters(int random_monsters)
{
	ap_state.random_monsters = random_monsters;
}


void f_random_items(int random_items)
{
	ap_state.random_items = random_items;
}


void f_flip_levels(int flip_levels)
{
	ap_state.flip_levels = flip_levels;
}


void f_episode1(int ep)
{
	ap_state.episodes[0] = ep;
}


void f_episode2(int ep)
{
	ap_state.episodes[1] = ep;
}


void f_episode3(int ep)
{
	ap_state.episodes[2] = ep;
}


void f_episode4(int ep)
{
	ap_state.episodes[3] = ep;
}


void f_two_ways_keydoors(int two_ways_keydoors)
{
	ap_state.two_ways_keydoors = two_ways_keydoors;
}


const char* apdoom_get_seed()
{
	return ap_save_dir_name.c_str();
}


void apdoom_check_location(int ep, int map, int index)
{
	int64_t id = 0;
	const auto& loc_table = get_location_table();

	auto it1 = loc_table.find(ep);
	if (it1 == loc_table.end()) return;

	auto it2 = it1->second.find(map);
	if (it2 == it1->second.end()) return;

	auto it3 = it2->second.find(index);
	if (it3 == it2->second.end()) return;

	id = it3->second;

	if (index >= 0)
	{
		if (is_loc_checked(ep, map, index))
		{
			printf("APDOOM: Location already checked\n");
		}
		else
		{ // We get back from AP
			//auto level_state = ap_get_level_state(ep, map);
			//level_state->checks[level_state->check_count] = index;
			//level_state->check_count++;
		}
	}
	AP_SendItem(id);
}


int apdoom_is_location_progression(int ep, int map, int index)
{
	const auto& loc_table = get_location_table();

	auto it1 = loc_table.find(ep);
	if (it1 == loc_table.end()) return 0;

	auto it2 = it1->second.find(map);
	if (it2 == it1->second.end()) return 0;

	auto it3 = it2->second.find(index);
	if (it3 == it2->second.end()) return 0;

	int64_t id = it3->second;

	return (ap_progressive_locations.find(id) != ap_progressive_locations.end()) ? 1 : 0;
}


void apdoom_complete_level(int ep, int map)
{
	//if (ap_state.level_states[ep - 1][map - 1].completed) return; // Already completed
    ap_get_level_state(ep, map)->completed = 1;
	apdoom_check_location(ep, map, -1); // -1 is complete location
}


void apdoom_check_victory()
{
	if (ap_state.victory) return;

	for (int ep = 0; ep < ap_episode_count; ++ep)
	{
		if (!ap_state.episodes[ep]) continue;
		for (int map = 0; map < ap_map_count; ++map)
		{
			if (!ap_get_level_state(ep + 1, map + 1)->completed) return;
		}
	}

	ap_state.victory = 1;

	AP_StoryComplete();
	ap_settings.victory_callback();
}


void apdoom_send_message(const char* msg)
{
	Json::Value say_packet;
	say_packet[0]["cmd"] = "Say";
	say_packet[0]["text"] = msg;
	Json::FastWriter writer;
	APSend(writer.write(say_packet));
}


void apdoom_on_death()
{
	AP_DeathLinkSend();
}


void apdoom_clear_death()
{
	AP_DeathLinkClear();
}


int apdoom_should_die()
{
	return AP_DeathLinkPending() ? 1 : 0;
}


const ap_notification_icon_t* ap_get_notification_icons(int* count)
{
	*count = (int)ap_notification_icons.size();
	return ap_notification_icons.data();
}

/*
    black: "000000"
    red: "EE0000"
    green: "00FF7F"  # typically a location
    yellow: "FAFAD2"  # typically other slots/players
    blue: "6495ED"  # typically extra info (such as entrance)
    magenta: "EE00EE"  # typically your slot/player
    cyan: "00EEEE"  # typically regular item
    slateblue: "6D8BE8"  # typically useful item
    plum: "AF99EF"  # typically progression item
    salmon: "FA8072"  # typically trap item
    white: "FFFFFF"  # not used, if you want to change the generic text color change color in Label

    (byte *) &cr_none, // 0 (RED)
    (byte *) &cr_dark, // 1 (DARK RED)
    (byte *) &cr_gray, // 2 (WHITE) normal text
    (byte *) &cr_green, // 3 (GREEN) location
    (byte *) &cr_gold, // 4 (YELLOW) player
    (byte *) &cr_red, // 5 (RED, same as cr_none)
    (byte *) &cr_blue, // 6 (BLUE) extra info such as Entrance
    (byte *) &cr_red2blue, // 7 (BLUE) items
    (byte *) &cr_red2green // 8 (DARK EDGE GREEN)
*/
void apdoom_update()
{
	if (ap_initialized)
	{
		if (!ap_cached_messages.empty())
		{
			for (const auto& cached_msg : ap_cached_messages)
				ap_settings.message_callback(cached_msg.c_str());
			ap_cached_messages.clear();
		}
	}

	while (AP_IsMessagePending())
	{
		AP_Message* msg = AP_GetLatestMessage();

		std::string colored_msg;

		switch (msg->type)
		{
			case AP_MessageType::ItemSend:
			{
				AP_ItemSendMessage* o_msg = static_cast<AP_ItemSendMessage*>(msg);
				colored_msg = "~9" + o_msg->item + "~2 was sent to ~4" + o_msg->recvPlayer;
				break;
			}
			case AP_MessageType::ItemRecv:
			{
				AP_ItemRecvMessage* o_msg = static_cast<AP_ItemRecvMessage*>(msg);
				colored_msg = "~2Received ~9" + o_msg->item + "~2 from ~4" + o_msg->sendPlayer;
				break;
			}
			case AP_MessageType::Hint:
			{
				AP_HintMessage* o_msg = static_cast<AP_HintMessage*>(msg);
				colored_msg = "~9" + o_msg->item + "~2 from ~4" + o_msg->sendPlayer + "~2 to ~4" + o_msg->recvPlayer + "~2 at ~3" + o_msg->location + (o_msg->checked ? " (Checked)" : " (Unchecked)");
				break;
			}
			default:
			{
				colored_msg = "~2" + msg->text;
				break;
			}
		}

		printf("APDOOM: %s\n", msg->text.c_str());

		if (ap_initialized)
			ap_settings.message_callback(colored_msg.c_str());
		else
			ap_cached_messages.push_back(colored_msg);

		AP_ClearLatestMessage();
	}

	// Check if we're in game, then dequeue the items
	if (ap_is_in_game)
	{
		while (!ap_item_queue.empty())
		{
			auto item_id = ap_item_queue.front();
			ap_item_queue.erase(ap_item_queue.begin());
			f_itemrecv(item_id, true);
		}
	}

	// Update notification icons
	float previous_y = 2.0f;
	for (auto it = ap_notification_icons.begin(); it != ap_notification_icons.end();)
	{
		auto& notification_icon = *it;

		if (notification_icon.state == AP_NOTIF_STATE_PENDING && previous_y > -160.0f)
		{
			notification_icon.state = AP_NOTIF_STATE_DROPPING;
		}
		if (notification_icon.state == AP_NOTIF_STATE_PENDING)
		{
			++it;
			continue;
		}

		if (notification_icon.state == AP_NOTIF_STATE_DROPPING)
		{
			notification_icon.vely += 0.15f;
			if (notification_icon.vely > 8.0f) notification_icon.vely = 8.0f;
			notification_icon.yf += notification_icon.vely;
			if (notification_icon.yf >= previous_y - AP_NOTIF_SIZE - AP_NOTIF_PADDING)
			{
				notification_icon.yf = previous_y - AP_NOTIF_SIZE - AP_NOTIF_PADDING;
				notification_icon.vely *= -0.3f;

				notification_icon.t++;
				if (notification_icon.t > 350) // 10sec
				{
					notification_icon.state = AP_NOTIF_STATE_HIDING;
				}
			}
		}

		if (notification_icon.state == AP_NOTIF_STATE_HIDING)
		{
			notification_icon.vely -= 0.14f;
			notification_icon.xf += notification_icon.vely;
			if (notification_icon.xf < -AP_NOTIF_SIZE / 2)
			{
				it = ap_notification_icons.erase(it);
				continue;
			}
		}

		notification_icon.x = (int)notification_icon.xf;
		notification_icon.y = (int)notification_icon.yf;
		previous_y = notification_icon.yf;

		++it;
	}
}
