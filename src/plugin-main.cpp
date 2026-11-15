/*
Trackspin
Copyright (C) 2026 Trackspin

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "card-source.hpp"
#include "runtime.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Отображаемое имя модуля (OBS 32+ показывает его в Plugin Manager вместо имени файла).
MODULE_EXPORT const char *obs_module_name(void)
{
	return "Trackspin";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Trackspin: now playing card for Yandex Music, VK Music and Spotify";
}

bool obs_module_load(void)
{
	if (!trackspin::runtime_start()) {
		obs_log(LOG_INFO, "plugin inactive (version %s)", PLUGIN_VERSION);
		return true;
	}
	trackspin::register_card_source();
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	trackspin::runtime_stop();
	obs_log(LOG_INFO, "plugin unloaded");
}
